#include "parallel_extract.hpp"
#include "video_batch.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Converts a filesystem path to the UTF-8 string the rest of the tool uses.
//
// On Windows the conversion has to go through the wide form. Taking
// path::string() instead returns the path in the ANSI code page, so a Chinese or
// Japanese file name comes back as mojibake and then fails to open.
std::string pathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return vividmatch::detail::narrow(path.wstring());
#else
    return path.string();
#endif
}

std::filesystem::path pathFromUtf8(const std::string& value) {
#ifdef _WIN32
    return std::filesystem::path(vividmatch::detail::widen(value));
#else
    return std::filesystem::path(value);
#endif
}

// Extensions the batch scan accepts, matching what the GUI offers.
bool hasVideoExtension(const std::string& path) {
    static const std::vector<std::string> extensions = {".mp4", ".mov", ".mkv",
                                                       ".avi", ".webm", ".m4v"};
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::any_of(extensions.begin(), extensions.end(),
                       [&lower](const std::string& extension) {
                           return lower.size() > extension.size()
                                  && lower.compare(lower.size() - extension.size(),
                                                   extension.size(), extension) == 0;
                       });
}

// Expands the arguments into a clip list: directories are scanned for video
// files, files are taken as given, and duplicates are dropped while keeping the
// order the user gave.
std::vector<std::string> collectClips(const std::vector<std::string>& arguments) {
    std::vector<std::string> paths;
    for (const std::string& argument : arguments) {
        std::error_code code;
        const std::filesystem::path root = pathFromUtf8(argument);
        if (std::filesystem::is_directory(root, code)) {
            std::vector<std::string> found;
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(root, code)) {
                if (entry.is_regular_file(code)) {
                    const std::string candidate = pathToUtf8(entry.path());
                    if (hasVideoExtension(candidate)) {
                        found.push_back(candidate);
                    }
                }
            }
            std::sort(found.begin(), found.end());
            paths.insert(paths.end(), found.begin(), found.end());
        } else {
            paths.push_back(argument);
        }
    }
    std::vector<std::string> unique;
    for (const std::string& path : paths) {
        if (std::find(unique.begin(), unique.end(), path) == unique.end()) {
            unique.push_back(path);
        }
    }
    return unique;
}

void printComparisonDetails(const vividmatch::VideoComparison& comparison,
                            const vividmatch::VideoFingerprint& left,
                            const vividmatch::VideoFingerprint& right) {
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

    // Extraction and comparison are reported separately: they have different
    // cost profiles, and naming both makes a slow run diagnosable.
    std::cout << std::setprecision(1);
    std::cout << "time_ms_extract_video=" << left.elapsedMs + right.elapsedMs << '\n';
    std::cout << "time_ms_extract_audio=" << left.audio.elapsedMs + right.audio.elapsedMs
              << '\n';
    std::cout << "time_ms_compare=" << comparison.elapsedMs << '\n';
    std::cout << std::setprecision(4);

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
    printComparisonDetails(comparison, left, right);
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

// Batch mode: group a folder of clips by content and report one winner per
// duplicate group. Returns 0 when at least one duplicate group was found.
int batchVideos(const std::vector<std::string>& arguments) {
    const std::vector<std::string> paths = collectClips(arguments);
    if (paths.empty()) {
        std::cerr << "error: no video files found in the given paths\n";
        return 2;
    }

    std::cout << "clips=" << paths.size() << '\n';
    // Progress goes to stderr so stdout stays machine readable.
    const vividmatch::VideoBatchResult result = vividmatch::compareVideoBatch(
        paths, vividmatch::kDefaultFrameThreshold, vividmatch::kDefaultSignatureThreshold,
        vividmatch::kDefaultBatchWorkers, vividmatch::preferHighestResolution,
        [](int done, int total, const std::string& stage) {
            std::cerr << '\r' << stage << ' ' << done << '/' << total << std::flush;
        });
    std::cerr << '\n';

    std::cout.setf(std::ios::fixed);
    std::cout << std::setprecision(1);
    std::cout << "time_ms_extract=" << result.extractMs << '\n';
    std::cout << "time_ms_compare=" << result.compareMs << '\n';
    std::cout << "pairs_compared=" << result.pairsCompared << " of "
              << (paths.size() * (paths.size() - 1) / 2) << " possible\n";
    std::cout << "unreadable=" << result.failed << '\n';
    std::cout << std::setprecision(4);

    int duplicateGroups = 0;
    for (const vividmatch::VideoBatchGroup& group : result.groups) {
        if (!group.duplicate) {
            continue;
        }
        ++duplicateGroups;
        std::cout << "group similarity=" << group.similarity
                  << " members=" << group.members.size()
                  << (group.audioDiffers ? " audio_differs=true" : "") << '\n';
        for (const int member : group.members) {
            const vividmatch::VideoBatchItem& item =
                result.items[static_cast<std::size_t>(member)];
            std::cout << "  " << (member == group.preferred ? "*" : " ") << item.path
                      << " [" << item.fingerprint.width << "x" << item.fingerprint.height
                      << " " << std::setprecision(1) << item.fingerprint.duration << "s]"
                      << std::setprecision(4) << '\n';
        }
    }

    for (const vividmatch::VideoBatchItem& item : result.items) {
        if (!item.ok) {
            std::cout << "failed=" << item.path << " reason=\"" << item.error << "\"\n";
        }
    }
    std::cout << "duplicate_groups=" << duplicateGroups << '\n';
    // A batch that found nothing is not an error, so exit 0 either way; the
    // duplicate_groups line is what a script reads.
    return 0;
}

void usage() {
    std::cout
        << "VividMatch video fingerprint (OpenCV C++, visual + temporal + audio)\n"
        << "usage:\n"
        << "  vividmatch_video compare <first> <second> [frame_threshold] [--no-audio]\n"
        << "  vividmatch_video summary <video> [--no-audio]\n"
        << "  vividmatch_video batch <folder-or-clip> [more...]\n"
        << "notes:\n"
        << "  Frames are sampled once per second. frame_threshold defaults to 0.78\n"
        << "  and is the per-frame similarity above which two frames count as the\n"
        << "  same moment. Exit code 0 means verdict=identical.\n"
        << "  batch scans folders for video files, groups the clips that are the\n"
        << "  same video and marks with '*' the one to keep (highest resolution).\n"
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
    if (args.size() >= 2 && args[0] == "batch") {
        return batchVideos(std::vector<std::string>(args.begin() + 1, args.end()));
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
