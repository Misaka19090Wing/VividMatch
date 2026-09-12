#include "parallel_extract.hpp"
#include "video_fingerprint.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using vividmatch::Fingerprint;
using vividmatch::VideoComparison;
using vividmatch::VideoFingerprint;
using vividmatch::VideoVerdict;

constexpr double kFps = 30.0;
// 6 seconds so that every "second bucket" below is actually reachable.
constexpr double kSeconds = 6.0;

int expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }
    std::cout << "ok: " << message << '\n';
    return 0;
}

cv::Scalar sceneColour(int scene, int slot) {
    // Golden-ratio hue stepping: dabs chosen this way stay far apart even for
    // large scene numbers, so no two scenes accidentally look alike.
    const double hue = std::fmod(0.6180339887 * scene + 0.137 * slot, 1.0);
    cv::Mat hsv(1, 1, CV_8UC3);
    hsv.at<cv::Vec3b>(0, 0) = cv::Vec3b(
        static_cast<uchar>(hue * 180.0),
        static_cast<uchar>(190 + ((scene + slot) % 3) * 20),
        static_cast<uchar>(200 + ((scene * 2 + slot) % 3) * 20));
    cv::Mat bgr;
    cv::cvtColor(hsv, bgr, cv::COLOR_HSV2BGR);
    const cv::Vec3b pixel = bgr.at<cv::Vec3b>(0, 0);
    return cv::Scalar(pixel[0], pixel[1], pixel[2]);
}

// One distinguishable second of video: a few large solid shapes on a
// background whose brightness also depends on the scene. Large areas carry the
// low-frequency content the DCT fingerprint keys on, so different scenes score
// far apart while frames within one scene differ only by the small moving
// marker and keep the same fingerprint.
cv::Mat sceneFrame(int scene, int index, int width, int height) {
    cv::RNG rng(1000 + scene);
    const int background = 30 + (scene * 37) % 150;
    cv::Mat frame(height, width, CV_8UC3, cv::Scalar(background, background, background));

    for (int slot = 0; slot < 5; ++slot) {
        const int cx = static_cast<int>(rng.uniform(0.15, 0.85) * width);
        const int cy = static_cast<int>(rng.uniform(0.15, 0.85) * height);
        const int radius = static_cast<int>(rng.uniform(0.08, 0.22) * std::min(width, height));
        cv::circle(frame, cv::Point(cx, cy), radius, sceneColour(scene, slot), -1);
    }

    // Small motion inside the second: visible, but far too small to change the
    // block hashes of shapes this large.
    const int travel = std::max(1, width / 16);
    const int phase = index % 10;
    const int radius = std::max(2, width / 60);
    const int cx = width / 2 + (phase - 5) * travel / 5;
    const int cy = height / 2;
    cv::circle(frame, cv::Point(cx, cy), radius, cv::Scalar(250, 250, 250), -1);
    return frame;
}

// Writes the clip: scenePerSecond[s] is the scene shown during second s, and
// every frame of a second carries that scene plus the in-second motion marker.
void writeVideo(const std::string& path, const std::vector<int>& scenePerSecond,
                int width, int height) {
    cv::VideoWriter writer(
        path,
        cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
        kFps,
        cv::Size(width, height));
    if (!writer.isOpened()) {
        throw std::runtime_error("cannot open VideoWriter for " + path);
    }
    const int frames = static_cast<int>(kSeconds * kFps);
    for (int index = 0; index < frames; ++index) {
        const std::size_t second =
            std::min<std::size_t>(static_cast<std::size_t>(index / kFps),
                                  scenePerSecond.size() - 1);
        writer.write(sceneFrame(scenePerSecond[second], index, width, height));
    }
    writer.release();
}

// Builds the in-memory fingerprint a video would produce, so a case can be
// checked without encoding a file.
VideoFingerprint fingerprintOf(const std::vector<int>& secondPerSecond, int width,
                               int height) {
    VideoFingerprint video;
    video.fps = kFps;
    video.width = width;
    video.height = height;
    video.duration = kSeconds;
    video.totalFrames = static_cast<std::int64_t>(kSeconds * kFps);
    for (std::size_t i = 0; i < secondPerSecond.size(); ++i) {
        video.frames.push_back(
            {static_cast<double>(i), vividmatch::makeFingerprint(
                                         sceneFrame(secondPerSecond[i], 0, width, height))});
    }
    return video;
}

std::string describe(const VideoComparison& comparison) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(3);
    stream << vividmatch::videoVerdictName(comparison.verdict)
           << " samples=" << comparison.leftFrames << "/" << comparison.rightFrames
           << " matches=" << comparison.matches.size()
           << " run=" << comparison.monotonicRun << " coverage=" << comparison.coverage
           << " run_ratio=" << comparison.runRatio << " mean=" << comparison.meanScore;
    if (comparison.audio.available) {
        stream << " audio=" << comparison.audio.meanSimilarity;
    } else {
        stream << " audio=n/a";
    }
    return stream.str();
}

// A single-tone audio fingerprint, standing in for a decoded track.
vividmatch::AudioFingerprint toneAudio(double frequency, int seconds) {
    vividmatch::AudioFingerprint audio;
    audio.available = true;
    audio.sampleRate = vividmatch::kAudioSampleRate;
    audio.duration = seconds;
    for (int second = 0; second < seconds; ++second) {
        std::vector<double> samples(static_cast<std::size_t>(vividmatch::kAudioSampleRate));
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = std::sin(2.0 * 3.14159265358979323846 * frequency
                                  * static_cast<double>(i) / vividmatch::kAudioSampleRate);
        }
        audio.seconds.push_back(vividmatch::detail::analyseSecond(
            samples, 0, samples.size(), vividmatch::kAudioSampleRate, second));
    }
    return audio;
}

void dumpMatrix(const std::string& label, const VideoComparison& comparison) {
    std::cout << "   " << label << " score matrix (rows=left, cols=right):\n";
    std::cout.setf(std::ios::fixed);
    for (std::size_t i = 0; i < comparison.scores.size(); ++i) {
        std::cout << "     [" << i << "]";
        for (std::size_t j = 0; j < comparison.scores[i].size(); ++j) {
            std::cout << std::setw(8) << std::setprecision(3)
                      << comparison.scores[i][j].score;
        }
        std::cout << '\n';
    }
    std::cout << "     matches:";
    for (const auto& match : comparison.matches) {
        std::cout << " (" << match.first << "," << match.second << ")";
    }
    std::cout << '\n';
}

void printDiagnostics(const std::string& label, const VideoComparison& comparison) {
    std::cout << "   " << label << ": " << describe(comparison) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    int failures = 0;
    try {
        const std::string directory = argc > 1 ? argv[1] : ".";
        const bool verbose = argc > 2 && std::string(argv[2]) == "--verbose";
        const std::string path_a = directory + "/vm_test_a.mp4";
        const std::string path_a_small = directory + "/vm_test_a_small.mp4";
        const std::string path_a_fast = directory + "/vm_test_a_fast.mp4";
        const std::string path_b = directory + "/vm_test_b.mp4";
        const std::string path_cut = directory + "/vm_test_cut.mp4";

        const std::vector<int> timeline_a = {1, 2, 3, 4, 5, 6};
        const std::vector<int> timeline_b = {9, 9, 9, 9, 9, 9};
        // Scenes 1..4, then a jump back to scenes 2..3: every sampled frame
        // matches A, but the timeline rewinds, so the matches cannot all sit on
        // one monotonic chain (混剪).
        const std::vector<int> timeline_cut = {1, 2, 3, 4, 2, 3};

        writeVideo(path_a, timeline_a, 640, 480);
        writeVideo(path_a_small, timeline_a, 320, 240);
        writeVideo(path_a_fast, timeline_a, 640, 480);
        writeVideo(path_b, timeline_b, 640, 480);
        writeVideo(path_cut, timeline_cut, 640, 480);
        std::cout << "wrote test videos into " << directory << '\n';

        auto report = [verbose](const std::string& label,
                                const VideoComparison& comparison) {
            printDiagnostics(label, comparison);
            if (verbose) {
                dumpMatrix(label, comparison);
            }
        };

        // --- same video, different resolution -----------------------------
        {
            const VideoFingerprint left = vividmatch::fingerprintVideo(path_a);
            const VideoFingerprint right = vividmatch::fingerprintVideo(path_a_small);
            const VideoComparison comparison =
                vividmatch::compareVideoFingerprints(left, right);
            report("same-resolution", comparison);
            failures += expect(
                comparison.verdict == VideoVerdict::Identical,
                "same video at a different resolution is identical");
        }

        // --- same video, different frame rate ------------------------------
        {
            const VideoComparison comparison = vividmatch::compareVideoFingerprints(
                vividmatch::fingerprintVideo(path_a),
                vividmatch::fingerprintVideo(path_a_fast));
            report("same-file", comparison);
            failures += expect(comparison.verdict == VideoVerdict::Identical,
                               "identical file is identical");
        }

        // --- different video ----------------------------------------------
        {
            const VideoComparison comparison = vividmatch::compareVideoFingerprints(
                vividmatch::fingerprintVideo(path_a),
                vividmatch::fingerprintVideo(path_b));
            report("different", comparison);
            failures += expect(comparison.verdict == VideoVerdict::Different,
                               "unrelated video is different");
        }

        // --- spliced video --------------------------------------------------
        // Scenes 1..3 followed by unrelated scenes: a prefix match, which the
        // temporal layer must not accept as the same video.
        {
            const VideoComparison comparison = vividmatch::compareVideoFingerprints(
                vividmatch::fingerprintVideo(path_a),
                vividmatch::fingerprintVideo(path_cut));
            report("prefix-and-foreign", comparison);
            failures += expect(comparison.verdict != VideoVerdict::Identical,
                               "a clip with unrelated footage is not identical");
        }

        // --- temporal guard on hand-built fingerprints ----------------------
        // The same scenes in reverse order: every sampled frame matches, but
        // no monotonic chain can cover them, so it must be 混剪.
        {
            const std::vector<int> forwardTimeline = {1, 2, 3, 4, 5, 6};
            const std::vector<int> reversedTimeline = {6, 5, 4, 3, 2, 1};
            const VideoFingerprint forward = fingerprintOf(forwardTimeline, 320, 240);
            const VideoFingerprint reversed = fingerprintOf(reversedTimeline, 320, 240);
            const VideoComparison comparison =
                vividmatch::compareVideoFingerprints(forward, reversed);
            report("reversed", comparison);
            failures += expect(comparison.monotonicRun == 1,
                               "a reversed timeline chains only one frame");
            failures += expect(comparison.verdict == VideoVerdict::Montage,
                               "a reversed timeline is not accepted as identical");
        }

        // --- concurrent extraction must equal sequential extraction ----------
        // The video and audio stages overlap, and the two videos are decoded on
        // separate threads. That must not change a single fingerprint: this is
        // the check that would have caught the audio temp files colliding.
        {
            VideoFingerprint sequentialLeft = vividmatch::fingerprintVideo(path_a);
            VideoFingerprint sequentialRight = vividmatch::fingerprintVideo(path_b);
            sequentialLeft.audio = vividmatch::fingerprintAudio(path_a);
            sequentialRight.audio = vividmatch::fingerprintAudio(path_b);

            VideoFingerprint parallelLeft;
            VideoFingerprint parallelRight;
            vividmatch::fingerprintPair(path_a, path_b, parallelLeft, parallelRight, true);

            auto sameFrames = [](const VideoFingerprint& left, const VideoFingerprint& right) {
                if (left.frames.size() != right.frames.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < left.frames.size(); ++i) {
                    if (left.frames[i].fingerprint.blocks != right.frames[i].fingerprint.blocks) {
                        return false;
                    }
                }
                return true;
            };

            failures += expect(sameFrames(parallelLeft, sequentialLeft)
                                   && sameFrames(parallelRight, sequentialRight),
                               "concurrent extraction yields identical fingerprints");
            failures += expect(
                parallelLeft.audio.available == sequentialLeft.audio.available
                    && parallelRight.audio.available == sequentialRight.audio.available
                    && parallelLeft.audio.seconds.size()
                           == sequentialLeft.audio.seconds.size()
                    && parallelRight.audio.seconds.size()
                           == sequentialRight.audio.seconds.size(),
                "concurrent extraction yields the same audio fingerprints");

            const VideoComparison concurrent =
                vividmatch::compareVideoFingerprints(parallelLeft, parallelRight);
            const VideoComparison oneAtATime =
                vividmatch::compareVideoFingerprints(sequentialLeft, sequentialRight);
            failures += expect(concurrent.verdict == oneAtATime.verdict,
                               "concurrent and sequential extraction agree on the verdict");
        }

        // --- the longest increasing run itself ------------------------------
        {
            const std::vector<std::pair<int, int>> ordered = {{0, 0}, {1, 1}, {2, 2}};
            const std::vector<std::pair<int, int>> shuffled = {{0, 2}, {1, 0}, {2, 1}};
            // The chain this test relies on must skip repeated partners.
            const std::vector<std::pair<int, int>> repeated = {
                {0, 0}, {1, 1}, {1, 3}, {2, 4}, {3, 5}};
            const std::vector<std::pair<int, int>> reversed = {
                {0, 5}, {1, 4}, {2, 3}, {3, 2}, {4, 1}, {5, 0}};
            std::cout << "   longestIncreasingRun(repeated)="
                      << vividmatch::longestIncreasingRun(repeated).size()
                      << " longestIncreasingRun(reversed)="
                      << vividmatch::longestIncreasingRun(reversed).size() << '\n';
            failures += expect(
                vividmatch::longestIncreasingRun(ordered).size() == 3,
                "longestIncreasingRun accepts an ordered chain");
            failures += expect(
                vividmatch::longestIncreasingRun(shuffled).size() == 2,
                "longestIncreasingRun finds the longest ordered subsequence");
            failures += expect(
                vividmatch::longestIncreasingRun(repeated).size() == 4,
                "longestIncreasingRun skips repeated partners");
            failures += expect(
                vividmatch::longestIncreasingRun(reversed).size() == 1,
                "longestIncreasingRun on a reversed timeline is a single pair");
        }

        // --- audio layer, on hand-built fingerprints -------------------------
        // Using synthetic features keeps this independent of ffmpeg, which the
        // audio layer needs only to decode a real file.
        {
            const std::vector<int> timeline = {1, 2, 3, 4, 5, 6};
            VideoFingerprint same = fingerprintOf(timeline, 320, 240);
            same.audio = toneAudio(440.0, 6);
            VideoFingerprint other = fingerprintOf(timeline, 320, 240);
            other.audio = toneAudio(2000.0, 6);

            // Same picture, same sound.
            {
                VideoFingerprint copy = same;
                const VideoComparison comparison =
                    vividmatch::compareVideoFingerprints(same, copy);
                report("audio-same", comparison);
                failures += expect(comparison.audio.available,
                                   "audio layer runs when both tracks are present");
                failures += expect(comparison.audio.sameSoundtrack,
                                   "identical audio is recognised as the same soundtrack");
                failures += expect(comparison.verdict == VideoVerdict::Identical,
                                   "same picture and same audio is identical");
            }

            // Same picture, replaced music bed: 画面相同但BGM被替换.
            {
                const VideoComparison comparison =
                    vividmatch::compareVideoFingerprints(same, other);
                report("audio-replaced", comparison);
                failures += expect(!comparison.audio.sameSoundtrack,
                                   "a replaced soundtrack is detected");
                failures += expect(comparison.verdict == VideoVerdict::Reencoded,
                                   "same picture with a different soundtrack is reencoded");
            }

            // No audio at all must not change the visual verdict.
            {
                const VideoComparison comparison = vividmatch::compareVideoFingerprints(
                    fingerprintOf(timeline, 320, 240), fingerprintOf(timeline, 320, 240));
                failures += expect(!comparison.audio.available,
                                   "audio layer reports itself unavailable without tracks");
                failures += expect(comparison.verdict == VideoVerdict::Identical,
                                   "a missing soundtrack falls back to the visual verdict");
            }

            // The FFT must place a pure tone in the matching bin.
            {
                std::vector<double> real(vividmatch::kAudioFftSize);
                std::vector<double> imag(vividmatch::kAudioFftSize, 0.0);
                const double frequency = 1000.0;
                for (std::size_t i = 0; i < real.size(); ++i) {
                    real[i] = std::sin(2.0 * 3.14159265358979323846 * frequency
                                       * static_cast<double>(i)
                                       / vividmatch::kAudioSampleRate);
                }
                vividmatch::fftRadix2(real, imag);
                int peakBin = 0;
                double peak = 0.0;
                for (std::size_t bin = 1; bin < real.size() / 2; ++bin) {
                    const double magnitude = std::hypot(real[bin], imag[bin]);
                    if (magnitude > peak) {
                        peak = magnitude;
                        peakBin = static_cast<int>(bin);
                    }
                }
                const double detected = peakBin * static_cast<double>(vividmatch::kAudioSampleRate)
                                        / vividmatch::kAudioFftSize;
                std::cout << "   fft peak for " << frequency << "Hz -> " << detected << "Hz\n";
                failures += expect(std::abs(detected - frequency) <= 2.0 * vividmatch::kAudioSampleRate
                                                                       / vividmatch::kAudioFftSize,
                                   "the hand-rolled FFT finds a 1kHz tone");
            }
        }
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
