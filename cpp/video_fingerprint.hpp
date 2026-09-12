#ifndef VIVIDMATCH_VIDEO_FINGERPRINT_HPP
#define VIVIDMATCH_VIDEO_FINGERPRINT_HPP

// Visual and temporal layers of strategy.md for video.
//
// A video is reduced to a short sequence of per-second visual fingerprints.
// Two videos are then compared by matching those sequences frame by frame and
// checking that the matched positions advance monotonically: a same video at a
// different resolution matches in order, while a spliced (混剪) video matches
// individual frames but jumps around on the timeline.
//
// The audio layer described in strategy.md is deliberately not part of this
// header; it stays a separate concern so the visual/temporal verdict can be
// reasoned about (and tested) on its own.

#include "visual_fingerprint.hpp"
#include "audio_fingerprint.hpp"

#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace vividmatch {

// Frames sampled per second of video, per strategy.md ("每秒1帧").
inline constexpr double kDefaultSamplesPerSecond = 1.0;
// Per-frame similarity above which two sampled frames count as the same moment.
inline constexpr double kDefaultFrameThreshold = 0.78;
// A verdict needs at least this much of the shorter video matched in order, so
// that "identical" means the shorter clip is contained in the longer one.
inline constexpr double kVerdictCoverage = 0.80;
// Fraction of the matching samples that must stay in one monotonic chain. A
// rewind or a shuffled edit leaves matches that cannot all be chained, so some
// of the matched frames fall outside the longest run.
inline constexpr double kVerdictRunRatio = 0.90;
// Mean per-frame score required to call two videos the same content.
inline constexpr double kVerdictScore = 0.80;
// How far below a frame's best partner another partner may score and still
// count as a match. Two genuinely similar moments (after de-duplicating an
// essentially static shot) fall inside this slack, while a merely passable
// pairing does not - keeping those out is what stops one row from
// contributing several matches and diluting the monotonic chain.
inline constexpr double kMatchScoreSlack = 0.02;

struct VideoFrameFingerprint {
    double timestamp = 0.0;  // seconds from the start of the video
    Fingerprint fingerprint;
};

struct VideoFingerprint {
    std::vector<VideoFrameFingerprint> frames;
    // Visual-only fingerprint. Kept out of the struct above so a caller can
    // compute the visual and audio halves independently (and in parallel).
    AudioFingerprint audio;
    double duration = 0.0;  // seconds, from frame rate and frame count
    double fps = 0.0;
    int width = 0;
    int height = 0;
    std::int64_t totalFrames = 0;
};

enum class VideoVerdict {
    Identical,   // same content, only resolution/bitrate differ
    Reencoded,   // same picture, different soundtrack: 画面相同但BGM被替换
    Montage,     // matches are out of order: 混剪拼接
    Partial,     // matches but not enough to call it the same video
    Different,
};

inline const char* videoVerdictName(VideoVerdict verdict) {
    switch (verdict) {
        case VideoVerdict::Identical:
            return "identical";
        case VideoVerdict::Reencoded:
            return "reencoded";
        case VideoVerdict::Montage:
            return "montage";
        case VideoVerdict::Partial:
            return "partial";
        case VideoVerdict::Different:
            return "different";
    }
    return "unknown";
}

struct FramePairScore {
    double score = 0.0;      // similarity of the two sampled frames
    bool matched = false;    // score >= frame threshold
};

struct VideoComparison {
    // scores[i][j] compares left.frames[i] with right.frames[j].
    std::vector<std::vector<FramePairScore>> scores;
    // Every (left, right) pair whose score reaches the frame threshold, in
    // left order. Keeping all of them (instead of one partner per frame) is
    // what lets a tie between two identical moments still produce a chain.
    std::vector<std::pair<int, int>> matches;
    int monotonicRun = 0;     // longest strictly increasing chain over matches
    int leftFrames = 0;
    int rightFrames = 0;
    int shorterFrames = 0;
    // Share of the shorter video's sampled frames that the monotonic chain
    // accounts for.
    double coverage = 0.0;
    // Share of the matched left frames that the monotonic chain advances
    // through; below kVerdictRunRatio the timeline jumped.
    double runRatio = 0.0;
    int coveredLeftFrames = 0;  // left samples participating in the chain
    double meanScore = 0.0;   // mean score across the monotonic run
    double firstMatchScore = 0.0;  // score at the start of the monotonic run
    double lastMatchScore = 0.0;   // score at the end of the monotonic run
    double bestMatchScore = 0.0;
    double frameThreshold = kDefaultFrameThreshold;
    // The alignment the visual layer settled on, as (left, right) sample
    // indices in chain order. The audio layer reuses it, so both layers judge
    // the same time correspondence.
    std::vector<std::pair<int, int>> alignment;
    // Audio layer result (strategy.md's 视听融合矩阵 inputs).
    AudioComparison audio;
    VideoVerdict verdict = VideoVerdict::Different;
};

// Longest strictly increasing subsequence length over the second coordinate of
// the given (index, index) pairs. This is the 时间轴单调性校验: a run of matches
// whose positions advance in step is a continuous cut, while a shuffled set of
// matches cannot produce a long run.
inline std::vector<int> longestIncreasingRun(const std::vector<std::pair<int, int>>& pairs) {
    const std::size_t count = pairs.size();
    if (count == 0) {
        return {};
    }

    std::vector<int> best(count, 1);
    std::vector<int> previous(count, -1);
    int bestEnd = 0;

    for (std::size_t i = 1; i < count; ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            // Both coordinates must advance: one right frame per left frame,
            // in step. Requiring only the right index would let a single left
            // frame chain to several right frames and hide a rewind.
            if (pairs[j].first < pairs[i].first && pairs[j].second < pairs[i].second
                && best[j] + 1 > best[i]) {
                best[i] = best[j] + 1;
                previous[i] = static_cast<int>(j);
            }
        }
        if (best[i] > best[bestEnd]) {
            bestEnd = static_cast<int>(i);
        }
    }

    std::vector<int> run;
    for (int index = bestEnd; index != -1; index = previous[static_cast<std::size_t>(index)]) {
        run.push_back(index);
    }
    std::reverse(run.begin(), run.end());
    return run;
}

// Sequentially decodes a video and keeps the frame closest to each sampling
// instant. Sequential reading (instead of seeking) keeps the sampled positions
// accurate for every codec, and the sampling instants are identical for both
// videos being compared, so two versions of the same clip line up even when
// their frame rates differ.
//
// Sample instants are derived from the frame counter and the nominal frame
// rate rather than from CAP_PROP_POS_MSEC, which some backends report as a
// stale or zero value. That also lets the frames between two samples be walked
// with grab(): OpenCV still decodes them, but it skips the colour conversion
// and the copy into a cv::Mat, which is most of the work we would otherwise do
// on frames we are about to discard.
inline VideoFingerprint fingerprintVideo(
    const std::string& path,
    double samplesPerSecond = kDefaultSamplesPerSecond,
    double samplingOffset = 0.0) {
    if (samplesPerSecond <= 0.0) {
        throw std::invalid_argument("samplesPerSecond must be positive");
    }

    cv::VideoCapture capture(path, cv::CAP_FFMPEG);
    if (!capture.isOpened()) {
        // Fall back to whatever backend OpenCV picks for this container.
        capture.open(path);
    }
    if (!capture.isOpened()) {
        throw std::invalid_argument("cannot open video: " + path);
    }

    VideoFingerprint video;
    video.fps = capture.get(cv::CAP_PROP_FPS);
    if (!(video.fps > 0.0)) {
        video.fps = 25.0;  // assume a common rate when the container omits it
    }
    video.width = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH));
    video.height = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT));
    const double reportedFrames = capture.get(cv::CAP_PROP_FRAME_COUNT);
    const std::int64_t frameCount = reportedFrames > 0.0
                                        ? static_cast<std::int64_t>(reportedFrames)
                                        : 0;
    const double reportedDuration =
        reportedFrames > 0.0 ? reportedFrames / video.fps : 0.0;

    const double samplePeriod = 1.0 / samplesPerSecond;
    const double framesPerSample = video.fps / samplesPerSecond;
    double nextTarget = samplingOffset;

    if (frameCount > 0 && framesPerSample > 1.5) {
        // Known length and more frames than samples: walk with grab().
        cv::Mat frame;
        for (std::int64_t index = 0; index < frameCount; ++index) {
            const double timestamp = static_cast<double>(index) / video.fps;
            if (timestamp + 1e-9 < nextTarget) {
                if (!capture.grab()) {
                    break;
                }
                continue;
            }
            if (!capture.read(frame) || frame.empty()) {
                break;
            }
            video.frames.push_back({timestamp, makeFingerprint(frame)});
            nextTarget += samplePeriod;
            while (nextTarget <= timestamp) {
                nextTarget += samplePeriod;
            }
        }
    } else {
        // Unknown or short stream: decode everything and pick as we go.
        double lastTimestamp = -1.0;
        double elapsed = 0.0;
        std::int64_t index = 0;
        cv::Mat frame;
        for (;;) {
            if (!capture.read(frame) || frame.empty()) {
                break;
            }
            double timestamp = capture.get(cv::CAP_PROP_POS_MSEC) / 1000.0;
            if (!(timestamp > lastTimestamp)) {
                timestamp = static_cast<double>(index) / video.fps;
            }
            ++index;
            elapsed = timestamp;
            lastTimestamp = timestamp;

            if (timestamp + 1e-9 < nextTarget) {
                continue;
            }
            video.frames.push_back({timestamp, makeFingerprint(frame)});
            nextTarget += samplePeriod;
            while (nextTarget <= timestamp) {
                nextTarget += samplePeriod;
            }
        }
        (void)elapsed;
    }

    video.totalFrames = static_cast<std::int64_t>(capture.get(cv::CAP_PROP_FRAME_COUNT));
    if (video.totalFrames <= 0) {
        video.totalFrames = video.frames.empty()
                                ? 0
                                : static_cast<std::int64_t>(
                                      std::llround(video.frames.back().timestamp * video.fps));
    }
    video.duration = reportedDuration > 0.0
                         ? reportedDuration
                         : (video.totalFrames > 0 ? video.totalFrames / video.fps : 0.0);

    capture.release();
    return video;
}

inline VideoComparison compareVideoFingerprints(
    const VideoFingerprint& left,
    const VideoFingerprint& right,
    double frameThreshold = kDefaultFrameThreshold,
    double audioThreshold = kDefaultAudioThreshold) {
    VideoComparison result;
    result.leftFrames = static_cast<int>(left.frames.size());
    result.rightFrames = static_cast<int>(right.frames.size());
    result.shorterFrames = std::min(result.leftFrames, result.rightFrames);
    result.frameThreshold = frameThreshold;

    if (result.leftFrames == 0 || result.rightFrames == 0) {
        return result;
    }

    result.scores.assign(
        left.frames.size(),
        std::vector<FramePairScore>(right.frames.size()));

    for (std::size_t i = 0; i < left.frames.size(); ++i) {
        for (std::size_t j = 0; j < right.frames.size(); ++j) {
            const double score = compareFingerprints(
                left.frames[i].fingerprint, right.frames[j].fingerprint);
            result.scores[i][j] = {score, score >= frameThreshold};
            result.bestMatchScore = std::max(result.bestMatchScore, score);
        }
    }

    // Collect the matched pairs, then take the longest order-preserving chain
    // through them. Only a frame's best partner - and any partner scoring
    // within the slack of it - counts: keeping every above-threshold pair
    // would let one frame contribute several matches and dilute the chain,
    // while keeping strictly one would make the chain depend on an arbitrary
    // tie-break when two sampled moments look the same.
    for (std::size_t i = 0; i < left.frames.size(); ++i) {
        double bestScore = 0.0;
        for (std::size_t j = 0; j < right.frames.size(); ++j) {
            bestScore = std::max(bestScore, result.scores[i][j].score);
        }
        if (bestScore < frameThreshold) {
            continue;
        }
        for (std::size_t j = 0; j < right.frames.size(); ++j) {
            const FramePairScore& pair = result.scores[i][j];
            if (pair.matched && pair.score >= bestScore - kMatchScoreSlack) {
                result.matches.emplace_back(static_cast<int>(i), static_cast<int>(j));
            }
        }
    }

    const std::vector<int> run = longestIncreasingRun(result.matches);
    result.monotonicRun = static_cast<int>(run.size());

    // Coverage is measured on whichever video has fewer samples: "identical"
    // should mean the shorter clip is contained in the longer one. Counting
    // the longer video's leftovers instead would make a short excerpt of a
    // long video look only partially matched.
    std::vector<bool> leftCovered(left.frames.size(), false);
    std::vector<bool> rightCovered(right.frames.size(), false);
    for (const int index : run) {
        const auto& pair = result.matches[static_cast<std::size_t>(index)];
        leftCovered[static_cast<std::size_t>(pair.first)] = true;
        rightCovered[static_cast<std::size_t>(pair.second)] = true;
    }
    const double leftCoverage =
        result.leftFrames > 0
            ? static_cast<double>(std::count(leftCovered.begin(), leftCovered.end(), true))
                  / result.leftFrames
            : 0.0;
    const double rightCoverage =
        result.rightFrames > 0
            ? static_cast<double>(std::count(rightCovered.begin(), rightCovered.end(), true))
                  / result.rightFrames
            : 0.0;
    const bool leftIsShorter = result.leftFrames <= result.rightFrames;
    result.coverage = leftIsShorter ? leftCoverage : rightCoverage;

    // 时间轴单调性校验, stated precisely: the chain must contain exactly one
    // partner for every left frame that matched at all. A rewind leaves a
    // matched left frame outside the chain, so the chain comes up short.
    std::vector<bool> matchedLeft(left.frames.size(), false);
    for (const auto& match : result.matches) {
        matchedLeft[static_cast<std::size_t>(match.first)] = true;
    }
    const int matchedLeftFrames =
        static_cast<int>(std::count(matchedLeft.begin(), matchedLeft.end(), true));
    result.runRatio = matchedLeftFrames > 0
                          ? static_cast<double>(result.monotonicRun) / matchedLeftFrames
                          : 0.0;
    result.coveredLeftFrames = matchedLeftFrames;

    if (!run.empty()) {
        double total = 0.0;
        for (const int index : run) {
            const auto& pair = result.matches[static_cast<std::size_t>(index)];
            result.alignment.push_back(pair);
            total += result.scores[static_cast<std::size_t>(pair.first)]
                                 [static_cast<std::size_t>(pair.second)]
                                     .score;
        }
        result.meanScore = total / run.size();
        const auto& firstPair = result.matches[static_cast<std::size_t>(run.front())];
        const auto& lastPair = result.matches[static_cast<std::size_t>(run.back())];
        result.firstMatchScore =
            result.scores[static_cast<std::size_t>(firstPair.first)]
                         [static_cast<std::size_t>(firstPair.second)]
                             .score;
        result.lastMatchScore =
            result.scores[static_cast<std::size_t>(lastPair.first)]
                         [static_cast<std::size_t>(lastPair.second)]
                             .score;
    }

    // Audio layer: measured only over the seconds that aligned visually.
    result.audio = compareAudioFingerprints(left.audio, right.audio, result.alignment,
                                            audioThreshold);

    if (result.matches.empty()) {
        result.verdict = VideoVerdict::Different;
        return result;
    }

    // The matches cannot all be laid out on one timeline: a rewind, a jump or
    // a shuffled edit leaves most of them outside the longest chain. That is
    // 混剪拼接, whatever the per-frame scores or the soundtrack say.
    if (result.runRatio < kVerdictRunRatio) {
        result.verdict = VideoVerdict::Montage;
        return result;
    }

    const bool enoughCoverage = result.coverage >= kVerdictCoverage;
    const bool enoughScore = result.meanScore >= kVerdictScore;
    const bool samePictures = enoughCoverage && enoughScore;

    // 视听融合矩阵, restricted to the cases this implementation can actually
    // tell apart. Audio is only consulted when both tracks decoded and there
    // was an alignment to compare them on.
    if (samePictures && result.audio.available && !result.audio.sameSoundtrack) {
        // 画面相同 + 音频不同: the picture is the original but the soundtrack
        // was swapped, so this is a re-cut rather than the same video.
        result.verdict = VideoVerdict::Reencoded;
        return result;
    }
    if (!samePictures && result.audio.available && result.audio.sameSoundtrack) {
        // 画面匹配低 + 音频匹配高: strategy.md's "audio veto" case, where the
        // picture is heavily obscured but the sound gives it away.
        result.verdict = VideoVerdict::Identical;
        return result;
    }
    result.verdict = samePictures ? VideoVerdict::Identical : VideoVerdict::Partial;
    return result;
}

inline VideoComparison compareVideoFiles(
    const std::string& leftPath,
    const std::string& rightPath,
    double samplesPerSecond = kDefaultSamplesPerSecond,
    double frameThreshold = kDefaultFrameThreshold,
    bool includeAudio = true,
    const std::string& ffmpegPath = std::string()) {
    VideoFingerprint left = fingerprintVideo(leftPath, samplesPerSecond);
    VideoFingerprint right = fingerprintVideo(rightPath, samplesPerSecond);
    if (includeAudio) {
        left.audio = fingerprintAudio(leftPath, ffmpegPath);
        right.audio = fingerprintAudio(rightPath, ffmpegPath);
    }
    return compareVideoFingerprints(left, right, frameThreshold);
}

}  // namespace vividmatch

#endif  // VIVIDMATCH_VIDEO_FINGERPRINT_HPP
