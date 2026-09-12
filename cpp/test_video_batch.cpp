// Batch video comparison tests.
//
// Builds a fixture set from one synthetic source clip so the expected grouping
// is known: a lower-resolution copy, an audio-swapped copy, a copy with a
// rewind (混剪), and a clip of unrelated content. Then checks that the batch
// groups them the way a user would.
#include "video_batch.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using vividmatch::VideoBatchResult;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    } else {
        std::cout << "ok: " << message << '\n';
    }
}

constexpr double kFps = 30.0;
constexpr int kSeconds = 8;

// A scene that is distinct per second and stable within it, so a second of
// video is recognisable while the clip still has a timeline.
cv::Mat sceneFrame(int scene, int index, int width, int height) {
    cv::RNG rng(1000 + scene);
    const int background = 30 + (scene * 37) % 150;
    cv::Mat frame(height, width, CV_8UC3, cv::Scalar(background, background, background));
    for (int slot = 0; slot < 5; ++slot) {
        const int cx = static_cast<int>(rng.uniform(0.15, 0.85) * width);
        const int cy = static_cast<int>(rng.uniform(0.15, 0.85) * height);
        const int radius =
            static_cast<int>(rng.uniform(0.08, 0.22) * std::min(width, height));
        const cv::Scalar colour((scene * 53 + slot * 29) % 256, (scene * 17 + slot * 71) % 256,
                                (scene * 91 + slot * 11) % 256);
        cv::circle(frame, cv::Point(cx, cy), radius, colour, -1);
    }
    const int phase = index % 10;
    cv::circle(frame, cv::Point(width / 2 + (phase - 5) * std::max(1, width / 16) / 5,
                                height / 2),
               std::max(2, width / 60), cv::Scalar(250, 250, 250), -1);
    return frame;
}

// Writes a clip whose second s shows scene `scenes[s]`.
void writeClip(const std::string& path, const std::vector<int>& scenes, int width,
               int height) {
    cv::VideoWriter writer(path, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), kFps,
                           cv::Size(width, height));
    if (!writer.isOpened()) {
        throw std::runtime_error("cannot write " + path);
    }
    for (int index = 0; index < kSeconds * static_cast<int>(kFps); ++index) {
        const std::size_t second =
            std::min<std::size_t>(static_cast<std::size_t>(index / static_cast<int>(kFps)),
                                  scenes.size() - 1);
        writer.write(sceneFrame(scenes[second], index, width, height));
    }
    writer.release();
}

// Copies a file, so a byte-identical duplicate exists in the set.
void copyFile(const std::string& from, const std::string& to) {
    std::ifstream source(from, std::ios::binary);
    std::ofstream target(to, std::ios::binary);
    target << source.rdbuf();
}

std::string describe(const VideoBatchResult& result) {
    std::ostringstream stream;
    stream << result.groups.size() << " groups, " << result.pairsCompared
           << " pairs compared of " << result.pairCandidates << " candidates";
    return stream.str();
}

// Finds the group containing `index`, or nullptr.
const vividmatch::VideoBatchGroup* groupOf(const VideoBatchResult& result, int index) {
    for (const auto& group : result.groups) {
        for (const int member : group.members) {
            if (member == index) {
                return &group;
            }
        }
    }
    return nullptr;
}

bool sameGroup(const VideoBatchResult& result, int left, int right) {
    const auto* group = groupOf(result, left);
    if (group == nullptr) {
        return false;
    }
    for (const int member : group->members) {
        if (member == right) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const std::string directory = argc > 1 ? argv[1] : ".";
        const std::string high = directory + "/vb_high.mp4";
        const std::string low = directory + "/vb_low.mp4";
        const std::string duplicate = directory + "/vb_duplicate.mp4";
        const std::string excerpt = directory + "/vb_excerpt.mp4";
        const std::string other = directory + "/vb_other.mp4";

        const std::vector<int> timeline = {1, 2, 3, 4, 5, 6, 7, 8};
        // Only the first part of the source, then unrelated content: the classic
        // "shared opening" case that must not be merged into the duplicate group.
        const std::vector<int> excerptTimeline = {1, 2, 3, 21, 22, 23, 24, 25};
        const std::vector<int> otherTimeline = {11, 12, 13, 14, 15, 16, 17, 18};

        writeClip(high, timeline, 640, 360);
        writeClip(low, timeline, 320, 180);
        copyFile(high, duplicate);
        writeClip(excerpt, excerptTimeline, 640, 360);
        writeClip(other, otherTimeline, 640, 360);
        std::cout << "wrote fixture clips into " << directory << '\n';

        const std::vector<std::string> paths = {high, low, duplicate, excerpt, other};
        vividmatch::VideoBatchResult result =
            vividmatch::compareVideoBatch(paths, vividmatch::kDefaultFrameThreshold,
                                          vividmatch::kDefaultSignatureThreshold, 4);
        std::cout << "batch: " << describe(result) << '\n';
        for (const auto& group : result.groups) {
            std::cout << "  group:";
            for (const int member : group.members) {
                std::cout << ' ' << member;
            }
            std::cout << (group.duplicate ? " (duplicate" : " (single")
                      << (group.montageOnly ? ", montage)" : ")")
                      << " similarity=" << std::setprecision(3) << group.similarity
                      << " preferred=" << group.preferred << '\n';
        }

        failures += 0;
        expect(result.failed == 0, "every fixture clip was read");
        expect(result.items.size() == paths.size(), "every clip produced an item");

        // The same content at three resolutions / codecs must land together.
        expect(sameGroup(result, 0, 1), "a lower-resolution copy joins the group");
        expect(sameGroup(result, 0, 2), "a byte-identical copy joins the group");
        const auto* group = groupOf(result, 0);
        expect(group != nullptr && group->duplicate, "the shared group is a duplicate group");
        if (group != nullptr) {
            expect(group->members.size() == 3, "the duplicate group holds exactly three clips");
        }

        // Unrelated content must not be pulled into that group.
        expect(!sameGroup(result, 0, 4), "unrelated content is not in the duplicate group");
        const auto* otherGroup = groupOf(result, 4);
        expect(otherGroup != nullptr && !otherGroup->duplicate,
               "unrelated content stands on its own");

        // A clip that only shares the opening must not be merged into the group.
        // Its comparison comes back `partial` (the matches are in order but do
        // not cover the shorter clip), and a partial match is not the same video.
        expect(!sameGroup(result, 0, 3), "a shared-opening clip is not a duplicate");
        {
            const vividmatch::VideoComparison excerptComparison =
                vividmatch::compareVideoFingerprints(result.items[0].fingerprint,
                                                     result.items[3].fingerprint);
            std::cout << "  excerpt pair: verdict="
                      << vividmatch::videoVerdictName(excerptComparison.verdict)
                      << " matches=" << excerptComparison.matches.size()
                      << " chain=" << excerptComparison.monotonicRun
                      << " coverage=" << std::setprecision(3)
                      << excerptComparison.coverage << '\n';
            expect(!vividmatch::batchEdgeCounts(excerptComparison.verdict),
                   "a partial match is not treated as the same video");
        }

        // The prefilter must not be dropping real matches, and must be doing some
        // work: with 5 clips there are 10 possible pairs.
        expect(result.pairCandidates <= 10, "no pair is invented by the prefilter");
        expect(result.pairCandidates >= 3, "the prefilter keeps the matching pairs");
        expect(result.pairsCompared == result.pairCandidates,
               "every candidate pair was compared");

        // Ranking: prefer the highest resolution, which is what a user expects.
        const auto ranking = [](const std::vector<int>& members,
                                const std::vector<vividmatch::VideoBatchItem>& items) {
            int best = members.front();
            long long bestPixels = 0;
            for (const int member : members) {
                const auto& video = items[static_cast<std::size_t>(member)].fingerprint;
                const long long pixels =
                    static_cast<long long>(video.width) * video.height;
                if (pixels > bestPixels) {
                    bestPixels = pixels;
                    best = member;
                }
            }
            return best;
        };
        vividmatch::VideoBatchResult ranked = vividmatch::compareVideoBatch(
            paths, vividmatch::kDefaultFrameThreshold, vividmatch::kDefaultSignatureThreshold,
            4, ranking);
        const auto* rankedGroup = groupOf(ranked, 0);
        expect(rankedGroup != nullptr && rankedGroup->preferred >= 0,
               "a ranking is applied to duplicate groups");
        if (rankedGroup != nullptr && rankedGroup->preferred >= 0) {
            const auto& preferred =
                ranked.items[static_cast<std::size_t>(rankedGroup->preferred)].fingerprint;
            bool biggest = true;
            for (const int member : rankedGroup->members) {
                const auto& video =
                    ranked.items[static_cast<std::size_t>(member)].fingerprint;
                if (video.width * video.height > preferred.width * preferred.height) {
                    biggest = false;
                }
            }
            expect(biggest, "the ranking keeps the highest-resolution clip");
            std::cout << "  preferred clip is " << preferred.width << "x" << preferred.height
                      << '\n';
        }

        // Progress must be reported and must not go past its total.
        int lastDone = 0;
        int lastTotal = 0;
        bool monotonic = true;
        vividmatch::compareVideoBatch(
            paths, vividmatch::kDefaultFrameThreshold, vividmatch::kDefaultSignatureThreshold, 4,
            vividmatch::VideoRanking(),
            [&](int done, int total, const std::string&) {
                if (done < lastDone) {
                    monotonic = false;
                }
                lastDone = done;
                lastTotal = total;
            });
        expect(monotonic, "progress never goes backwards");
        expect(lastDone <= lastTotal, "progress stays within its total");
        std::cout << "  progress ended at " << lastDone << "/" << lastTotal << '\n';

        // Unreadable files are reported rather than thrown.
        vividmatch::VideoBatchResult missing = vividmatch::compareVideoBatch(
            {high, directory + "/does_not_exist.mp4", other});
        expect(missing.failed == 1, "an unreadable clip is counted as failed");
        expect(missing.items[1].ok == false, "the unreadable clip is marked not ok");
        std::cout << "  failure reason: \"" << missing.items[1].error << "\"\n";

        // A single clip produces no pairs and no crash.
        vividmatch::VideoBatchResult single =
            vividmatch::compareVideoBatch({high});
        expect(single.groups.size() == 1, "a single clip yields one group");
        expect(single.pairCandidates == 0, "a single clip has no pairs");
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }

    if (failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return failures;
}
