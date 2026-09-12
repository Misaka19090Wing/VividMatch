#ifndef VIVIDMATCH_VIDEO_BATCH_HPP
#define VIVIDMATCH_VIDEO_BATCH_HPP

// Batch video comparison: fingerprint a folder of clips, find out which of them
// are the same video, and keep one per group.
//
// Shape of the work, which is what the design follows:
//
//   1. Extraction dominates. Decoding every clip costs far more than any amount
//      of comparing, and it is per-file independent, so it is done once per clip
//      on a thread pool. Measured: 5 twenty-second clips extract in ~445 ms
//      while all 10 of their pairwise comparisons take ~1.2 ms.
//   2. Comparing every pair is therefore cheap but not free: the cost grows with
//      the square of the clip count and with the square of the clip length
//      (sampling is once per second, so an hour-long clip is ~3600 samples and a
//      pair is ~13M frame comparisons). A cheap per-clip signature is compared
//      first and only plausible pairs get the full comparison.
//   3. The signature is the majority vote of each sampled frame's block bits:
//      128 bytes per clip regardless of length. Measured separation on a fixture
//      set: same content 0.97-1.00, different content 0.63-0.70, so a threshold
//      in that gap misses nothing.
//
// Audio is deliberately not extracted for the whole batch up front. It costs a
// separate ffmpeg process per clip and is only meaningful over an alignment, so
// it is extracted lazily for the clips that take part in a visually matching
// pair.

#include "parallel_extract.hpp"
#include "video_fingerprint.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <numeric>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace vividmatch {

// Signature similarity below which a pair is not worth the full comparison.
// Sits inside the measured gap between same content (>= 0.97) and different
// content (<= 0.70), so the prefilter discards only clear non-matches.
inline constexpr double kDefaultSignatureThreshold = 0.85;
// Worker threads used to decode clips. Each one decodes video with OpenCV and
// may spawn an ffmpeg process for audio, so this is deliberately modest.
inline constexpr unsigned kDefaultBatchWorkers = 4;

// Compact per-clip summary: 128 bytes, independent of clip length.
struct VideoSignature {
    std::array<std::uint64_t, kBlockCount> blocks{};
    bool valid = false;
};

struct VideoBatchItem {
    std::string path;
    VideoFingerprint fingerprint;
    VideoSignature signature;
    // 0 when the clip could not be read; the item is then reported as failed.
    bool ok = false;
    std::string error;
};

struct VideoBatchGroup {
    // Indices into the items vector, in input order.
    std::vector<int> members;
    bool duplicate = false;     // more than one clip with the same content
    bool montageOnly = false;   // the members match, but only as a 混剪
    double similarity = 0.0;    // best pairwise score inside the group
    // Members whose pictures match but whose soundtracks differ, so the group is
    // a re-cut rather than one video at several resolutions.
    bool audioDiffers = false;
    int preferred = -1;         // index the ranking function picked, or -1
};

struct VideoBatchResult {
    std::vector<VideoBatchItem> items;
    std::vector<VideoBatchGroup> groups;
    int failed = 0;
    // Work actually done, for reporting and for sizing a bigger run.
    int pairCandidates = 0;  // pairs that passed the signature prefilter
    int pairsCompared = 0;   // pairs that got the full comparison
    double extractMs = 0.0;
    double compareMs = 0.0;
};

// Majority vote of every sampled frame's block bits.
inline VideoSignature makeVideoSignature(const VideoFingerprint& video) {
    VideoSignature signature;
    if (video.frames.empty()) {
        return signature;
    }

    // For each block and bit position, count the frames that had it set.
    std::array<std::array<int, kHashBits>, kBlockCount> votes{};
    for (const VideoFrameFingerprint& frame : video.frames) {
        for (int block = 0; block < kBlockCount; ++block) {
            const std::uint64_t value = frame.fingerprint.blocks[static_cast<std::size_t>(block)];
            for (int bit = 0; bit < kHashBits; ++bit) {
                if (((value >> bit) & 1ULL) != 0ULL) {
                    ++votes[static_cast<std::size_t>(block)][static_cast<std::size_t>(bit)];
                }
            }
        }
    }

    const int half = static_cast<int>(video.frames.size()) / 2;
    for (int block = 0; block < kBlockCount; ++block) {
        std::uint64_t value = 0;
        for (int bit = 0; bit < kHashBits; ++bit) {
            if (votes[static_cast<std::size_t>(block)][static_cast<std::size_t>(bit)] > half) {
                value |= std::uint64_t{1} << bit;
            }
        }
        signature.blocks[static_cast<std::size_t>(block)] = value;
    }
    signature.valid = true;
    return signature;
}

// Same discard-the-worst-blocks rule the frame comparison uses, so the
// prefilter and the full comparison agree on what "similar" means.
inline double signatureSimilarity(const VideoSignature& left, const VideoSignature& right) {
    if (!left.valid || !right.valid) {
        return 0.0;
    }
    std::array<double, kBlockCount> distances{};
    for (int i = 0; i < kBlockCount; ++i) {
        distances[static_cast<std::size_t>(i)] = static_cast<double>(popcount(
            left.blocks[static_cast<std::size_t>(i)]
            ^ right.blocks[static_cast<std::size_t>(i)]));
    }
    std::sort(distances.begin(), distances.end());
    const int keep = std::max(1, kBlockCount - kDiscardBlocks);
    double total = 0.0;
    for (int i = 0; i < keep; ++i) {
        total += distances[static_cast<std::size_t>(i)];
    }
    return 1.0 - total / (static_cast<double>(keep) * kHashBits);
}

namespace detail {

// Union-find over the items, used to collect duplicate groups.
inline int batchFindRoot(std::vector<int>& parent, int index) {
    while (parent[static_cast<std::size_t>(index)] != index) {
        // Path halving keeps the tree shallow without a second pass.
        parent[static_cast<std::size_t>(index)] =
            parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(index)])];
        index = parent[static_cast<std::size_t>(index)];
    }
    return index;
}

inline void batchUnion(std::vector<int>& parent, int left, int right) {
    const int leftRoot = batchFindRoot(parent, left);
    const int rightRoot = batchFindRoot(parent, right);
    if (leftRoot != rightRoot) {
        parent[static_cast<std::size_t>(rightRoot)] = leftRoot;
    }
}

}  // namespace detail

// Ranks the clips of a group and returns the index of the one to keep, or -1 to
// keep every clip checked. The caller supplies this so the batch core stays free
// of policy (the GUI offers resolution / size / date choices, the CLI a default).
using VideoRanking = std::function<int(const std::vector<int>& members,
                                       const std::vector<VideoBatchItem>& items)>;

// Default policy: keep the highest-resolution clip, and on a tie the one with
// the larger file (a higher bitrate at the same resolution is the better copy).
inline int preferHighestResolution(const std::vector<int>& members,
                                   const std::vector<VideoBatchItem>& items) {
    int best = -1;
    long long bestPixels = -1;
    std::uintmax_t bestBytes = 0;
    for (const int member : members) {
        const VideoBatchItem& item = items[static_cast<std::size_t>(member)];
        const long long pixels = static_cast<long long>(item.fingerprint.width)
                                 * item.fingerprint.height;
        std::error_code code;
        const std::uintmax_t bytes =
            std::filesystem::file_size(std::filesystem::u8path(item.path), code);
        if (best < 0 || pixels > bestPixels
            || (pixels == bestPixels && bytes > bestBytes)) {
            best = member;
            bestPixels = pixels;
            bestBytes = code ? 0 : bytes;
        }
    }
    return best;
}

// Whether an edge means the two clips are the same video.
//
// Only `identical` (same pictures, soundtrack agrees) and `reencoded` (same
// pictures, soundtrack swapped) count. `partial` deliberately does not: it means
// the matched part is in order but does not cover the shorter clip, which is
// what a shared opening, a trailer or a clip cut from a longer video looks like.
// Merging those would put genuinely different videos in one duplicate group.
inline bool batchEdgeCounts(VideoVerdict verdict) {
    return verdict == VideoVerdict::Identical || verdict == VideoVerdict::Reencoded;
}

// Fingerprints every clip, then groups the ones that are the same video.
//
// `progress` is called with (done, total, stage label) from worker threads and
// must be thread safe; `total` counts extraction plus comparison work so a UI
// can drive one bar.
inline VideoBatchResult compareVideoBatch(
    const std::vector<std::string>& paths,
    double frameThreshold = kDefaultFrameThreshold,
    double signatureThreshold = kDefaultSignatureThreshold,
    unsigned workers = kDefaultBatchWorkers,
    const VideoRanking& ranking = VideoRanking(),
    const std::function<void(int, int, const std::string&)>& progress = {});

// --- implementation --------------------------------------------------------

inline VideoBatchResult compareVideoBatch(
    const std::vector<std::string>& paths,
    double frameThreshold,
    double signatureThreshold,
    unsigned workers,
    const VideoRanking& ranking,
    const std::function<void(int, int, const std::string&)>& progress) {
    VideoBatchResult result;
    result.items.resize(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        result.items[i].path = paths[i];
    }
    if (paths.empty()) {
        return result;
    }

    const unsigned threadCount =
        std::max(1u, std::min<unsigned>(workers, static_cast<unsigned>(paths.size())));
    const int itemCount = static_cast<int>(paths.size());
    auto notify = [&progress](int done, int total, const char* stage) {
        if (progress) {
            progress(done, total, stage);
        }
    };

    // --- 1. extraction, one clip per work item -----------------------------
    const auto extractStart = std::chrono::steady_clock::now();
    {
        std::atomic<int> done{0};
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        pool.reserve(threadCount);
        for (unsigned t = 0; t < threadCount; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    const std::size_t i = next.fetch_add(1);
                    if (i >= paths.size()) {
                        return;
                    }
                    VideoBatchItem& item = result.items[i];
                    try {
                        item.fingerprint = fingerprintVideo(paths[i]);
                        item.signature = makeVideoSignature(item.fingerprint);
                        item.ok = !item.fingerprint.frames.empty();
                        if (!item.ok) {
                            item.error = "no frames could be sampled";
                        }
                    } catch (const std::exception& error) {
                        item.ok = false;
                        item.error = error.what();
                    }
                    notify(done.fetch_add(1) + 1, itemCount, "extracting");
                }
            });
        }
        for (std::thread& thread : pool) {
            thread.join();
        }
    }
    result.extractMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                  - extractStart)
            .count();

    for (const VideoBatchItem& item : result.items) {
        if (!item.ok) {
            ++result.failed;
        }
    }

    // --- 2. prefilter, then the full comparison ----------------------------
    struct Edge {
        int left = 0;
        int right = 0;
        VideoComparison comparison;
        bool compared = false;
    };
    std::vector<Edge> edges;
    for (int i = 0; i < itemCount; ++i) {
        if (!result.items[static_cast<std::size_t>(i)].ok) {
            continue;
        }
        for (int j = i + 1; j < itemCount; ++j) {
            if (!result.items[static_cast<std::size_t>(j)].ok) {
                continue;
            }
            const double signature =
                signatureSimilarity(result.items[static_cast<std::size_t>(i)].signature,
                                    result.items[static_cast<std::size_t>(j)].signature);
            if (signature < signatureThreshold) {
                continue;
            }
            edges.push_back({i, j, VideoComparison(), false});
        }
    }
    result.pairCandidates = static_cast<int>(edges.size());
    notify(itemCount, itemCount + static_cast<int>(edges.size()), "comparing");

    const auto compareStart = std::chrono::steady_clock::now();
    if (!edges.empty()) {
        std::atomic<std::size_t> next{0};
        std::atomic<int> done{0};
        std::vector<std::thread> pool;
        const unsigned edgeWorkers =
            std::max(1u, std::min<unsigned>(threadCount, static_cast<unsigned>(edges.size())));
        pool.reserve(edgeWorkers);
        for (unsigned t = 0; t < edgeWorkers; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    const std::size_t index = next.fetch_add(1);
                    if (index >= edges.size()) {
                        return;
                    }
                    Edge& edge = edges[index];
                    edge.comparison = compareVideoFingerprints(
                        result.items[static_cast<std::size_t>(edge.left)].fingerprint,
                        result.items[static_cast<std::size_t>(edge.right)].fingerprint,
                        frameThreshold);
                    edge.compared = true;
                    notify(itemCount + done.fetch_add(1) + 1,
                           itemCount + static_cast<int>(edges.size()), "comparing");
                }
            });
        }
        for (std::thread& thread : pool) {
            thread.join();
        }
    }
    result.compareMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                  - compareStart)
            .count();
    result.pairsCompared = static_cast<int>(edges.size());

    // --- 3. audio, only where the pictures already matched -----------------
    // A re-cut keeps the picture and swaps the soundtrack, and that is worth
    // distinguishing, but extracting audio for the whole batch would cost an
    // ffmpeg process per clip for nothing.
    std::vector<int> audioWanted;
    std::vector<bool> audioNeeded(paths.size(), false);
    for (const Edge& edge : edges) {
        if (!edge.compared || !batchEdgeCounts(edge.comparison.verdict)) {
            continue;
        }
        for (const int index : {edge.left, edge.right}) {
            if (!audioNeeded[static_cast<std::size_t>(index)]) {
                audioNeeded[static_cast<std::size_t>(index)] = true;
                audioWanted.push_back(index);
            }
        }
    }
    if (!audioWanted.empty()) {
        notify(itemCount + static_cast<int>(edges.size()),
               itemCount + static_cast<int>(edges.size()), "audio");
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        const unsigned audioWorkers =
            std::max(1u, std::min<unsigned>(threadCount, static_cast<unsigned>(audioWanted.size())));
        pool.reserve(audioWorkers);
        for (unsigned t = 0; t < audioWorkers; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    const std::size_t index = next.fetch_add(1);
                    if (index >= audioWanted.size()) {
                        return;
                    }
                    VideoBatchItem& item = result.items[static_cast<std::size_t>(audioWanted[index])];
                    item.fingerprint.audio = fingerprintAudio(item.path);
                }
            });
        }
        for (std::thread& thread : pool) {
            thread.join();
        }

        // Re-judge those edges now that the soundtrack is known.
        for (Edge& edge : edges) {
            if (!edge.compared || !batchEdgeCounts(edge.comparison.verdict)) {
                continue;
            }
            const VideoFingerprint& left =
                result.items[static_cast<std::size_t>(edge.left)].fingerprint;
            const VideoFingerprint& right =
                result.items[static_cast<std::size_t>(edge.right)].fingerprint;
            edge.comparison = compareVideoFingerprints(left, right, frameThreshold);
        }
    }

    // --- 4. group by the edges that count ----------------------------------
    std::vector<int> parent(static_cast<std::size_t>(itemCount));
    std::iota(parent.begin(), parent.end(), 0);
    std::vector<int> duplicateDegree(static_cast<std::size_t>(itemCount), 0);
    std::vector<int> montageDegree(static_cast<std::size_t>(itemCount), 0);
    std::vector<double> bestEdge(static_cast<std::size_t>(itemCount), 0.0);
    std::vector<bool> audioDiffers(static_cast<std::size_t>(itemCount), false);

    for (const Edge& edge : edges) {
        if (!edge.compared) {
            continue;
        }
        const VideoVerdict verdict = edge.comparison.verdict;
        if (batchEdgeCounts(verdict)) {
            detail::batchUnion(parent, edge.left, edge.right);
            const double score = std::max(edge.comparison.coverage, edge.comparison.meanScore);
            for (const int index : {edge.left, edge.right}) {
                ++duplicateDegree[static_cast<std::size_t>(index)];
                bestEdge[static_cast<std::size_t>(index)] =
                    std::max(bestEdge[static_cast<std::size_t>(index)], score);
                if (verdict == VideoVerdict::Reencoded) {
                    audioDiffers[static_cast<std::size_t>(index)] = true;
                }
            }
        } else if (verdict == VideoVerdict::Montage) {
            // 混剪: the clips overlap but cannot be laid on one timeline, so they
            // are reported separately from a duplicate group.
            ++montageDegree[static_cast<std::size_t>(edge.left)];
            ++montageDegree[static_cast<std::size_t>(edge.right)];
        }
    }

    std::vector<std::vector<int>> byRoot(static_cast<std::size_t>(itemCount));
    for (int i = 0; i < itemCount; ++i) {
        if (!result.items[static_cast<std::size_t>(i)].ok) {
            continue;
        }
        byRoot[static_cast<std::size_t>(detail::batchFindRoot(parent, i))].push_back(i);
    }

    for (std::vector<int>& members : byRoot) {
        if (members.empty()) {
            continue;
        }
        VideoBatchGroup group;
        group.members = members;
        group.duplicate = members.size() > 1;
        for (const int index : members) {
            group.similarity =
                std::max(group.similarity, bestEdge[static_cast<std::size_t>(index)]);
            group.audioDiffers =
                group.audioDiffers || audioDiffers[static_cast<std::size_t>(index)];
        }

        // A clip of its own whose only relations were 混剪 edges is reported as a
        // montage group rather than as an unrelated single. Duplicate groups
        // never take this label: their members are genuinely the same video.
        if (!group.duplicate) {
            for (const int index : members) {
                if (montageDegree[static_cast<std::size_t>(index)] > 0) {
                    group.montageOnly = true;
                    break;
                }
            }
        }

        if (ranking && group.duplicate) {
            group.preferred = ranking(group.members, result.items);
        }
        result.groups.push_back(std::move(group));
    }

    // Duplicate groups first, then by score, then by input order, so the list is
    // stable to look at and stable to test.
    std::sort(result.groups.begin(), result.groups.end(),
              [](const VideoBatchGroup& left, const VideoBatchGroup& right) {
                  if (left.duplicate != right.duplicate) {
                      return left.duplicate && !right.duplicate;
                  }
                  if (left.duplicate && left.similarity != right.similarity) {
                      return left.similarity > right.similarity;
                  }
                  return left.members.front() < right.members.front();
              });

    notify(itemCount + static_cast<int>(edges.size()),
           itemCount + static_cast<int>(edges.size()), "done");
    return result;
}

}  // namespace vividmatch

#endif  // VIVIDMATCH_VIDEO_BATCH_HPP
