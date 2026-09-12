#include "parallel_extract.hpp"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

void printComparisonDetails(const vividmatch::VideoComparison& comparison) {
    std::cout.setf(std::ios::fixed);
    std::cout << std::setprecision(4);
    std::cout << "samples=" << comparison.leftFrames << " vs " << comparison.rightFrames
              << '\n';
    std::cout << "matches=" << comparison.matches.size() << '\n';
    std::cout << "monotonic_run=" << comparison.monotonicRun << '\n';
    std::cout << "coverage=" << comparison.coverage << '\n';
    std::cout << "run_ratio=" << comparison.runRatio << '\n';
    std::cout << "mean_score=" << comparison.meanScore << '\n';
    std::cout << "best_score=" << comparison.bestMatchScore << '\n';

    const vividmatch::AudioComparison& audio = comparison.audio;
    std::cout << "audio_available=" << (audio.available ? "true" : "false") << '\n';
    if (audio.available) {
        std::cout << "audio_seconds_compared=" << audio.comparedSeconds << '\n';
        std::cout << "audio_similarity=" << audio.meanSimilarity << '\n';
        std::cout << "audio_rms_diff=" << audio.meanRmsDifference << '\n';
        std::cout << "audio_same_soundtrack=" << (audio.sameSoundtrack ? "true" : "false")
                  << '\n';
    } else if (!audio.error.empty()) {
        std::cout << "audio_skipped=" << audio.error << '\n';
    }
}

int compareVideos(const std::string& first, const std::string& second, double threshold,
                  bool withAudio) {
    vividmatch::VideoFingerprint left;
    vividmatch::VideoFingerprint right;
    vividmatch::fingerprintPair(first, second, left, right, withAudio);
    const vividmatch::VideoComparison comparison =
        vividmatch::compareVideoFingerprints(left, right, threshold);

    std::cout << "first=" << first << '\n';
    std::cout << "second=" << second << '\n';
    std::cout.setf(std::ios::fixed);
    std::cout << std::setprecision(3);
    std::cout << "source=" << left.width << "x" << left.height << " " << left.fps
              << "fps " << left.duration << "s vs " << right.width << "x" << right.height
              << " " << right.fps << "fps " << right.duration << "s\n";
    printComparisonDetails(comparison);
    std::cout << "verdict=" << vividmatch::videoVerdictName(comparison.verdict) << '\n';

    if (comparison.verdict != vividmatch::VideoVerdict::Identical) {
        return 1;
    }
    return 0;
}

int printSummary(const std::string& path, bool withAudio) {
    vividmatch::VideoFingerprint video = vividmatch::fingerprintVideo(path);
    if (withAudio) {
        video.audio = vividmatch::fingerprintAudio(path);
    }
    std::cout.setf(std::ios::fixed);
    std::cout << "path=" << path << '\n';
    std::cout << std::setprecision(3);
    std::cout << "source=" << video.width << "x" << video.height << '\n';
    std::cout << "fps=" << video.fps << '\n';
    std::cout << "frames=" << video.totalFrames << '\n';
    std::cout << "duration=" << video.duration << '\n';
    std::cout << "samples=" << video.frames.size() << '\n';

    const vividmatch::AudioFingerprint& audio = video.audio;
    std::cout << "audio_available=" << (audio.available ? "true" : "false") << '\n';
    if (audio.available) {
        std::cout << "audio_seconds=" << audio.seconds.size() << '\n';
        std::cout << "audio_duration=" << audio.duration << '\n';
        std::cout << std::setprecision(1);
        for (const vividmatch::AudioSecondFeature& second : audio.seconds) {
            std::cout << "audio t=" << second.timestamp << " rms=" << std::setprecision(4)
                      << second.rms << " centroid=" << std::setprecision(1)
                      << second.centroid << '\n';
        }
    } else if (!audio.error.empty()) {
        std::cout << "audio_skipped=" << audio.error << '\n';
    }

    for (const vividmatch::VideoFrameFingerprint& frame : video.frames) {
        std::cout << std::setprecision(3) << "sample t=" << frame.timestamp << " hash=";
        std::cout << std::hex << std::setfill('0');
        for (const std::uint64_t block : frame.fingerprint.blocks) {
            std::cout << std::setw(16) << block;
        }
        std::cout << std::dec << std::setfill(' ') << '\n';
    }
    return 0;
}

void usage() {
    std::cout
        << "VividMatch video fingerprint (OpenCV C++, visual + temporal + audio)\n"
        << "usage:\n"
        << "  vividmatch_video compare <first> <second> [frame_threshold] [--no-audio]\n"
        << "  vividmatch_video summary <video> [--no-audio]\n"
        << "notes:\n"
        << "  Frames are sampled once per second. frame_threshold defaults to 0.78\n"
        << "  and is the per-frame similarity above which two frames count as the\n"
        << "  same moment. Exit code 0 means verdict=identical.\n"
        << "  The audio layer needs the ffmpeg command line; without it the\n"
        << "  visual/temporal verdict is reported on its own.\n";
}

}  // namespace

namespace {

int run(const std::vector<std::string>& rawArgs) {
    std::vector<std::string> args = rawArgs;
    bool withAudio = true;
    for (auto it = args.begin(); it != args.end();) {
        if (*it == "--no-audio") {
            withAudio = false;
            it = args.erase(it);
        } else {
            ++it;
        }
    }

    if (args.size() >= 3 && args[0] == "compare") {
        double threshold = vividmatch::kDefaultFrameThreshold;
        if (args.size() >= 4) {
            try {
                threshold = std::stod(args[3]);
            } catch (const std::exception&) {
                throw std::invalid_argument("invalid frame threshold: " + args[3]);
            }
        }
        return compareVideos(args[1], args[2], threshold, withAudio);
    }
    if (args.size() == 2 && args[0] == "summary") {
        return printSummary(args[1], withAudio);
    }
    usage();
    return 2;
}

}  // namespace

#ifdef _WIN32
// Windows hands main() the arguments in the ANSI code page, so a Chinese or
// Japanese file name arrives already mangled. wmain() gives them as UTF-16 and
// they are converted to UTF-8 here, which is what the fingerprint code (and
// ffmpeg) expects.
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 1; i < argc; ++i) {
            args.push_back(vividmatch::detail::narrow(argv[i]));
        }
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
#endif
