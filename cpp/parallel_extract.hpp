#ifndef VIVIDMATCH_PARALLEL_EXTRACT_HPP
#define VIVIDMATCH_PARALLEL_EXTRACT_HPP

// Runs the two halves of fingerprinting at the same time.
//
// Measurement on a 3 minute 720p pair: extracting the videos one after another
// and then the audio one after another takes 2373 ms. Running the audio beside
// the video brings that to 1662 ms (1.4x), because the audio stage is mostly a
// separate ffmpeg process waiting on I/O rather than CPU in this process. What
// is left over is the longest single video decode, so this is a constant-factor
// win and not a linear one - but it costs nothing in accuracy and it is exactly
// the structure a batch job needs, where the win is per-file parallelism.
//
// Attention when generalising this to many files: each audio fingerprint spawns
// an ffmpeg process, so a large worker count should bound how many of those run
// at once rather than spawning one per file.

#include "video_fingerprint.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace vividmatch {

// Runs `work(index)` for every index in [0, count) across at most `workers`
// threads and returns when all of them are done. `workers <= 1` runs inline,
// which keeps the single-threaded behaviour available for debugging.
inline void forEachParallel(std::size_t count, unsigned workers,
                            const std::function<void(std::size_t)>& work) {
    if (count == 0) {
        return;
    }
    const unsigned threadCount =
        std::max(1u, std::min<unsigned>(workers, static_cast<unsigned>(count)));
    if (threadCount <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            work(i);
        }
        return;
    }

    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(threadCount);
    for (unsigned t = 0; t < threadCount; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                const std::size_t index = next.fetch_add(1);
                if (index >= count) {
                    return;
                }
                work(index);
            }
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
}

// Default worker count for the stages this module runs concurrently.
inline unsigned defaultExtractionWorkers() {
    const unsigned cores = std::thread::hardware_concurrency();
    return cores == 0 ? 2u : std::min(cores, 4u);
}

// Fingerprints two clips with the video and audio stages overlapping. The two
// fingerprints come out in the same order as the paths, so the result is
// identical to fingerprinting them one after another - only the wall time
// differs. `progress` is called from worker threads, so it must be thread safe.
inline void fingerprintPair(
    const std::string& firstPath,
    const std::string& secondPath,
    VideoFingerprint& first,
    VideoFingerprint& second,
    bool includeAudio = true,
    const std::string& ffmpegPath = std::string(),
    const std::function<void(const std::string&)>& progress = {},
    double* elapsedMs = nullptr) {
    const std::vector<std::string> paths{firstPath, secondPath};
    VideoFingerprint videos[2];
    AudioFingerprint audio[2];
    const auto started = std::chrono::steady_clock::now();

    auto notify = [&progress](const std::string& message) {
        if (progress) {
            progress(message);
        }
    };

    notify("extracting video fingerprints");
    std::thread videoThread([&]() {
        forEachParallel(2, 2, [&](std::size_t i) {
            try {
                videos[i] = fingerprintVideo(paths[i]);
            } catch (...) {
                // Collected below: the comparison reports the missing side.
            }
        });
    });

    std::thread audioThread([&]() {
        if (!includeAudio) {
            return;
        }
        notify("extracting audio fingerprints");
        forEachParallel(2, 2, [&](std::size_t i) {
            try {
                audio[i] = fingerprintAudio(paths[i], ffmpegPath);
            } catch (...) {
            }
        });
    });

    videoThread.join();
    audioThread.join();

    // An empty visual fingerprint means the file could not be read at all, which
    // the caller treats as an error rather than as "no match".
    if (videos[0].frames.empty() && videos[0].totalFrames == 0) {
        throw std::invalid_argument("cannot read video: " + firstPath);
    }
    if (videos[1].frames.empty() && videos[1].totalFrames == 0) {
        throw std::invalid_argument("cannot read video: " + secondPath);
    }

    if (includeAudio) {
        videos[0].audio = std::move(audio[0]);
        videos[1].audio = std::move(audio[1]);
    }
    first = std::move(videos[0]);
    second = std::move(videos[1]);

    // True wall time of the extraction stages. The per-fingerprint times are
    // each measured on their own and overlap, so this is the number to show as
    // "how long the extraction took".
    if (elapsedMs != nullptr) {
        *elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                      - started)
                .count();
    }
}

// One-call form: fingerprint both files concurrently, then compare them. The
// caller measures extraction itself when it needs that split, because the
// comparison result carries only the stages it observed.
inline VideoComparison compareVideoFiles(
    const std::string& leftPath,
    const std::string& rightPath,
    double frameThreshold = kDefaultFrameThreshold,
    bool includeAudio = true,
    const std::string& ffmpegPath = std::string(),
    const std::function<void(const std::string&)>& progress = {}) {
    VideoFingerprint left;
    VideoFingerprint right;
    fingerprintPair(leftPath, rightPath, left, right, includeAudio, ffmpegPath, progress);
    return compareVideoFingerprints(left, right, frameThreshold);
}

}  // namespace vividmatch

#endif  // VIVIDMATCH_PARALLEL_EXTRACT_HPP
