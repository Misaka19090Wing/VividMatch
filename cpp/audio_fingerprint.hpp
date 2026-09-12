#ifndef VIVIDMATCH_AUDIO_FINGERPRINT_HPP
#define VIVIDMATCH_AUDIO_FINGERPRINT_HPP

// Audio layer of strategy.md.
//
// The audio track is decoded to 16 kHz mono PCM, cut into one-second windows,
// and each window is reduced to a few spectral features: RMS energy, spectral
// centroid and a coarse band-energy vector. Windows are compared with cosine
// similarity, so the same soundtrack matches across bitrates and channel
// layouts while a replaced music bed does not.
//
// strategy.md specifies FFTW for the STFT. This implementation uses a small
// radix-2 FFT written here instead, so the project keeps OpenCV as its only
// library dependency.
//
// Decoding uses the ffmpeg command line, because the OpenCV build this project
// targets exposes no audio decoding API (CAP_PROP_AUDIO_STREAM is not honoured
// by its FFMPEG backend). When ffmpeg is not installed the audio layer reports
// itself unavailable and the visual/temporal verdict stands alone.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vividmatch {

// strategy.md asks for 16 kHz mono PCM.
inline constexpr int kAudioSampleRate = 16000;
// Analysis window for the STFT.
inline constexpr int kAudioFftSize = 1024;
// Number of coarse frequency bands kept per second.
inline constexpr int kAudioBands = 16;
// Cosine similarity above which two seconds of audio count as the same sound.
inline constexpr double kDefaultAudioThreshold = 0.90;

// --- small radix-2 FFT -----------------------------------------------------

// In-place iterative Cooley-Tukey FFT; size must be a power of two.
inline void fftRadix2(std::vector<double>& real, std::vector<double>& imag) {
    const std::size_t n = real.size();
    if (n < 2 || (n & (n - 1)) != 0 || imag.size() != n) {
        return;
    }

    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const double stepReal = std::cos(angle);
        const double stepImag = std::sin(angle);
        for (std::size_t start = 0; start < n; start += len) {
            double wReal = 1.0;
            double wImag = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::size_t a = start + k;
                const std::size_t b = a + len / 2;
                const double uReal = real[a];
                const double uImag = imag[a];
                const double vReal = real[b] * wReal - imag[b] * wImag;
                const double vImag = real[b] * wImag + imag[b] * wReal;
                real[a] = uReal + vReal;
                imag[a] = uImag + vImag;
                real[b] = uReal - vReal;
                imag[b] = uImag - vImag;
                const double nextReal = wReal * stepReal - wImag * stepImag;
                wImag = wReal * stepImag + wImag * stepReal;
                wReal = nextReal;
            }
        }
    }
}

// --- audio fingerprints ----------------------------------------------------

struct AudioSecondFeature {
    double timestamp = 0.0;     // seconds from the start of the track
    double rms = 0.0;           // loudness of the second
    double centroid = 0.0;      // spectral centre of mass, in Hz
    std::vector<double> bands;  // normalised energy per frequency band
};

struct AudioFingerprint {
    bool available = false;
    std::string error;  // why audio is missing, when available == false
    int sampleRate = kAudioSampleRate;
    double duration = 0.0;
    std::vector<AudioSecondFeature> seconds;
};

struct AudioComparison {
    bool available = false;
    std::string error;
    int leftSeconds = 0;
    int rightSeconds = 0;
    int comparedSeconds = 0;  // windows compared along the alignment
    double meanSimilarity = 0.0;
    double meanRmsDifference = 0.0;  // |rmsLeft - rmsRight| averaged over them
    bool sameSoundtrack = false;     // meanSimilarity >= threshold
    double threshold = kDefaultAudioThreshold;
};

// Cosine similarity of two windows' band vectors. Silence is handled
// explicitly: two silent seconds match, a silent and a loud one do not.
inline double audioFeatureSimilarity(const AudioSecondFeature& left,
                                     const AudioSecondFeature& right) {
    const std::size_t count = std::min(left.bands.size(), right.bands.size());
    if (count == 0) {
        return 0.0;
    }
    double dot = 0.0;
    double leftNorm = 0.0;
    double rightNorm = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        dot += left.bands[i] * right.bands[i];
        leftNorm += left.bands[i] * left.bands[i];
        rightNorm += right.bands[i] * right.bands[i];
    }
    if (leftNorm <= 0.0 && rightNorm <= 0.0) {
        return 1.0;
    }
    if (leftNorm <= 0.0 || rightNorm <= 0.0) {
        return 0.0;
    }
    return dot / (std::sqrt(leftNorm) * std::sqrt(rightNorm));
}

// --- PCM decoding ----------------------------------------------------------

namespace detail {

#ifdef _WIN32
// Paths arrive as UTF-8 and are handed to the Win32 wide APIs, which is what
// keeps non-ASCII file names working.
inline std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        wide.data(), length);
    return wide;
}

inline std::string narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        utf8.data(), length, nullptr, nullptr);
    return utf8;
}
#endif

inline std::uint32_t readLe32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
           | (static_cast<std::uint32_t>(bytes[1]) << 8)
           | (static_cast<std::uint32_t>(bytes[2]) << 16)
           | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline std::uint16_t readLe16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

// Minimal RIFF/WAVE reader: walks the chunk list and keeps the 16-bit PCM data
// chunk that ffmpeg writes for us. Opening through ifstream(path) would fail on
// a non-ASCII temporary path, so on Windows the wide form is used; this makes
// makeTemporaryPath's UTF-8 result readable no matter where the user's TEMP is.
inline bool readWavPcm(const std::string& path, std::vector<double>& samples,
                       int& sampleRate) {
#ifdef _WIN32
    const std::wstring widePath = widen(path);
    std::ifstream stream(widePath.c_str(), std::ios::binary);
#else
    std::ifstream stream(path, std::ios::binary);
#endif
    if (!stream) {
        return false;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),
                                           std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || std::string(bytes.begin(), bytes.begin() + 4) != "RIFF"
        || std::string(bytes.begin() + 8, bytes.begin() + 12) != "WAVE") {
        return false;
    }

    int channels = 0;
    int bitsPerSample = 0;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;
    sampleRate = 0;

    std::size_t cursor = 12;
    while (cursor + 8 <= bytes.size()) {
        const std::string id(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                             bytes.begin() + static_cast<std::ptrdiff_t>(cursor) + 4);
        const std::uint32_t size = readLe32(&bytes[cursor + 4]);
        const std::size_t body = cursor + 8;
        if (id == "fmt " && size >= 16 && body + 16 <= bytes.size()) {
            channels = readLe16(&bytes[body + 2]);
            sampleRate = static_cast<int>(readLe32(&bytes[body + 4]));
            bitsPerSample = readLe16(&bytes[body + 14]);
        } else if (id == "data") {
            dataOffset = body;
            dataSize = std::min<std::size_t>(size, bytes.size() - body);
        }
        cursor = body + size + (size % 2);
    }

    if (channels <= 0 || bitsPerSample != 16 || sampleRate <= 0 || dataSize == 0) {
        return false;
    }

    // ffmpeg is asked for mono, so the channels are already mixed down.
    const std::size_t frameCount = dataSize / 2;
    samples.resize(frameCount);
    for (std::size_t i = 0; i < frameCount; ++i) {
        const std::int16_t value =
            static_cast<std::int16_t>(readLe16(&bytes[dataOffset + i * 2]));
        samples[i] = static_cast<double>(value) / 32768.0;
    }
    return true;
}

inline bool fileExists(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return static_cast<bool>(stream);
}

inline void removeQuietly(const std::string& path) {
    std::remove(path.c_str());
}

// Locates the ffmpeg executable: an explicit setting first, then PATH.
inline std::string findFfmpeg(const std::string& explicitPath) {
    if (!explicitPath.empty()) {
        return fileExists(explicitPath) ? explicitPath : std::string();
    }
#ifdef _WIN32
    const char* candidate = "ffmpeg.exe";
    char buffer[MAX_PATH] = {0};
    if (SearchPathA(nullptr, candidate, nullptr, MAX_PATH, buffer, nullptr) > 0) {
        return std::string(buffer);
    }
#endif
    return std::string();
}

struct FfmpegResult {
    bool ok = false;
    std::string error;
};

// Turns ffmpeg's own message into something the user can act on. "No audio
// track" is by far the most common case and is not an error the user needs to
// fix, so it must not read like a broken install.
inline std::string explainFfmpegFailure(const std::string& output) {
    if (output.find("does not contain any stream") != std::string::npos
        || output.find("Could not find audio") != std::string::npos
        || output.find("does not contain stream") != std::string::npos) {
        return "this file has no audio track";
    }
    if (output.find("Invalid data found") != std::string::npos
        || output.find("moov atom not found") != std::string::npos) {
        return "the file could not be read (truncated or unsupported container)";
    }
    if (output.find("No such file or directory") != std::string::npos
        || output.find("Error opening input") != std::string::npos) {
        return "ffmpeg could not find or open the file";
    }
    if (output.find("Permission denied") != std::string::npos) {
        return "ffmpeg was denied access to the file or the temporary folder";
    }
    if (output.find("No space left") != std::string::npos) {
        return "no space left for the temporary audio file";
    }
    return "ffmpeg could not decode an audio track";
}

// Reads the tail of a capture file: ffmpeg puts the actual reason at the end.
inline std::string tailOfFile(const std::string& path, std::size_t maxLength = 500) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::string();
    }
    std::string content((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
    if (content.size() > maxLength) {
        content = content.substr(content.size() - maxLength);
    }
    for (char& character : content) {
        if (character == '\r' || character == '\n' || character == '\t') {
            character = ' ';
        }
    }
    while (!content.empty() && content.back() == ' ') {
        content.pop_back();
    }
    return content;
}

#ifdef _WIN32

// ffmpeg receives the arguments directly as UTF-16, with no cmd.exe in the
// middle. That matters: writing the command to a .cmd file and running it
// through cmd.exe mangles any non-ASCII path, because cmd reads .cmd files in
// the OEM code page rather than UTF-8. A Chinese or Japanese file name then
// arrives at ffmpeg as mojibake and the audio layer looks broken.
// Quotes one argument the way CommandLineToArgvW will parse it back.
inline std::wstring quoteWideArgument(const std::wstring& value) {
    const bool needsQuotes =
        value.empty() || value.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes) {
        return value;
    }
    std::wstring quoted = L"\"";
    for (std::size_t i = 0; i < value.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < value.size() && value[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == value.size()) {
            // Trailing backslashes must be doubled before the closing quote.
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (value[i] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(value[i]);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

// Runs ffmpeg with the log going straight to a file handle, so no shell is
// involved and no intermediate script is written.
inline FfmpegResult runFfmpegToFile(const std::string& ffmpeg, const std::string& video,
                                    const std::string& wavOut) {
    FfmpegResult result;

    std::wstring commandLine = quoteWideArgument(widen(ffmpeg));
    const wchar_t* fixedArguments[] = {
        L" -nostdin -v error -y -i ", L" -vn -ac 1 -ar ",
        L" -c:a pcm_s16le ",         L""};
    commandLine += fixedArguments[0];
    commandLine += quoteWideArgument(widen(video));
    commandLine += fixedArguments[1];
    commandLine += std::to_wstring(kAudioSampleRate);
    commandLine += fixedArguments[2];
    commandLine += quoteWideArgument(widen(wavOut));

    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    const std::wstring logPath = widen(wavOut) + L".log";
    HANDLE logHandle = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) {
        result.error = "cannot create a log file in the temporary folder";
        return result;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = logHandle;
    startup.hStdError = logHandle;

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                        &process);
    CloseHandle(logHandle);
    if (!created) {
        result.error = "cannot start ffmpeg";
        return result;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    if (exitCode != 0) {
        result.error = explainFfmpegFailure(tailOfFile(narrow(logPath)));
        removeQuietly(wavOut + ".log");
        return result;
    }
    removeQuietly(wavOut + ".log");
    result.ok = true;
    return result;
}

#else

inline FfmpegResult runFfmpegToFile(const std::string& ffmpeg, const std::string& video,
                                    const std::string& wavOut) {
    FfmpegResult result;
    const std::string logPath = wavOut + ".log";
    const std::string command = "\"" + ffmpeg + "\" -nostdin -v error -y -i \"" + video
                                + "\" -vn -ac 1 -ar " + std::to_string(kAudioSampleRate)
                                + " -c:a pcm_s16le \"" + wavOut + "\" > \"" + logPath
                                + "\" 2>&1";
    if (std::system(command.c_str()) != 0) {
        result.error = explainFfmpegFailure(tailOfFile(logPath));
        removeQuietly(logPath);
        return result;
    }
    removeQuietly(logPath);
    result.ok = true;
    return result;
}

#endif

// Unique per call, not per process: audio for several files is fingerprinted
// concurrently, and a process-wide name would make them overwrite each other's
// temporary WAV.
inline unsigned long long nextTemporaryIndex() {
    static std::atomic<unsigned long long> counter{0};
    return counter.fetch_add(1);
}

#ifdef _WIN32
inline std::string temporaryPath(const std::string& suffix) {
    wchar_t directory[MAX_PATH] = {0};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    const std::wstring base = length > 0 ? std::wstring(directory) : std::wstring(L".");
    const std::wstring name = L"vividmatch_audio_" + std::to_wstring(GetCurrentProcessId())
                              + L"_" + std::to_wstring(nextTemporaryIndex())
                              + widen(suffix);
    return narrow(base + name);
}
#else
inline std::string temporaryPath(const std::string& suffix) {
    return "/tmp/vividmatch_audio_" + std::to_string(static_cast<long>(::getpid())) + "_"
           + std::to_string(nextTemporaryIndex()) + suffix;
}
#endif

// --- feature extraction ----------------------------------------------------

// One second of PCM reduced to RMS, spectral centroid and band energies.
inline AudioSecondFeature analyseSecond(const std::vector<double>& samples,
                                        std::size_t begin, std::size_t end,
                                        int sampleRate, double timestamp) {
    AudioSecondFeature feature;
    feature.timestamp = timestamp;
    feature.bands.assign(kAudioBands, 0.0);
    if (end <= begin || sampleRate <= 0) {
        return feature;
    }

    double sumSquares = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        sumSquares += samples[i] * samples[i];
    }
    feature.rms = std::sqrt(sumSquares / static_cast<double>(end - begin));

    const int fftSize = kAudioFftSize;
    const int half = fftSize / 2;
    const int hop = fftSize / 2;
    const double binHz = static_cast<double>(sampleRate) / fftSize;

    std::vector<double> real(static_cast<std::size_t>(fftSize));
    std::vector<double> imag(static_cast<std::size_t>(fftSize));
    std::vector<double> spectrum(static_cast<std::size_t>(half + 1), 0.0);
    int windows = 0;
    double centroidSum = 0.0;
    double magnitudeSum = 0.0;

    const std::size_t available = end - begin;
    for (std::size_t offset = 0; offset + static_cast<std::size_t>(fftSize) <= available;
         offset += static_cast<std::size_t>(hop)) {
        for (int i = 0; i < fftSize; ++i) {
            const double window =
                0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / (fftSize - 1));
            real[static_cast<std::size_t>(i)] = samples[begin + offset + static_cast<std::size_t>(i)] * window;
            imag[static_cast<std::size_t>(i)] = 0.0;
        }
        fftRadix2(real, imag);

        for (int bin = 0; bin <= half; ++bin) {
            const double magnitude =
                std::sqrt(real[static_cast<std::size_t>(bin)] * real[static_cast<std::size_t>(bin)]
                          + imag[static_cast<std::size_t>(bin)] * imag[static_cast<std::size_t>(bin)]);
            spectrum[static_cast<std::size_t>(bin)] = magnitude;
            centroidSum += magnitude * bin * binHz;
            magnitudeSum += magnitude;
        }
        ++windows;
    }

    if (windows > 0 && magnitudeSum > 0.0) {
        feature.centroid = centroidSum / magnitudeSum;
    }

    // Fold the spectrum into a logarithmic set of bands, which keeps the
    // low frequencies where speech and music carry most of their energy.
    for (int band = 0; band < kAudioBands; ++band) {
        const double lowFraction = static_cast<double>(band) / kAudioBands;
        const double highFraction = static_cast<double>(band + 1) / kAudioBands;
        const int lowBin = std::max(1, static_cast<int>(lowFraction * lowFraction * half));
        const int highBin = std::max(lowBin + 1,
                                     static_cast<int>(highFraction * highFraction * half) + 1);
        double energy = 0.0;
        for (int bin = lowBin; bin < highBin && bin <= half; ++bin) {
            energy += spectrum[static_cast<std::size_t>(bin)];
        }
        feature.bands[static_cast<std::size_t>(band)] =
            windows > 0 ? energy / windows : 0.0;
    }

    // Normalise the band vector so loudness (RMS) does not dominate the cosine
    // similarity: the shape of the spectrum is what identifies the sound.
    double norm = 0.0;
    for (const double value : feature.bands) {
        norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm > 0.0) {
        for (double& value : feature.bands) {
            value /= norm;
        }
    }
    return feature;
}

}  // namespace detail

// Decodes the audio track of a video and reduces it to one feature per second.
inline AudioFingerprint fingerprintAudio(const std::string& path,
                                         const std::string& ffmpegPath = std::string()) {
    AudioFingerprint fingerprint;

    const std::string ffmpeg = detail::findFfmpeg(ffmpegPath);
    if (ffmpeg.empty()) {
        fingerprint.error =
            "ffmpeg not found on PATH, so audio was not compared";
        return fingerprint;
    }

    const std::string wav = detail::temporaryPath(".wav");
    const detail::FfmpegResult decoded = detail::runFfmpegToFile(ffmpeg, path, wav);
    if (!decoded.ok) {
        detail::removeQuietly(wav);
        fingerprint.error = decoded.error;
        return fingerprint;
    }

    std::vector<double> samples;
    int sampleRate = 0;
    const bool read = detail::readWavPcm(wav, samples, sampleRate);
    detail::removeQuietly(wav);
    if (!read) {
        fingerprint.error = "ffmpeg produced no usable audio data";
        return fingerprint;
    }
    if (samples.empty()) {
        fingerprint.error = "the audio track is empty";
        return fingerprint;
    }

    fingerprint.available = true;
    fingerprint.sampleRate = sampleRate;
    fingerprint.duration =
        static_cast<double>(samples.size()) / static_cast<double>(sampleRate);

    const std::size_t perSecond = static_cast<std::size_t>(sampleRate);
    for (std::size_t start = 0, index = 0; start < samples.size();
         start += perSecond, ++index) {
        const std::size_t end = std::min(samples.size(), start + perSecond);
        fingerprint.seconds.push_back(detail::analyseSecond(
            samples, start, end, sampleRate, static_cast<double>(index)));
    }
    return fingerprint;
}

// Compares the audio of two videos. Only the seconds listed in `alignment`
// (left index, right index) are compared, so the comparison follows the
// time-warping the visual layer already found instead of assuming the two
// tracks start together and run at the same speed.
inline AudioComparison compareAudioFingerprints(
    const AudioFingerprint& left, const AudioFingerprint& right,
    const std::vector<std::pair<int, int>>& alignment,
    double threshold = kDefaultAudioThreshold) {
    AudioComparison comparison;
    comparison.threshold = threshold;
    comparison.leftSeconds = static_cast<int>(left.seconds.size());
    comparison.rightSeconds = static_cast<int>(right.seconds.size());

    if (!left.available || !right.available) {
        comparison.error = !left.available ? left.error : right.error;
        return comparison;
    }
    if (alignment.empty()) {
        comparison.error = "no aligned frames to compare audio on";
        return comparison;
    }

    double similarityTotal = 0.0;
    double rmsTotal = 0.0;
    int compared = 0;
    for (const auto& pair : alignment) {
        if (pair.first < 0 || pair.second < 0
            || pair.first >= comparison.leftSeconds
            || pair.second >= comparison.rightSeconds) {
            continue;
        }
        const AudioSecondFeature& leftSecond =
            left.seconds[static_cast<std::size_t>(pair.first)];
        const AudioSecondFeature& rightSecond =
            right.seconds[static_cast<std::size_t>(pair.second)];
        similarityTotal += audioFeatureSimilarity(leftSecond, rightSecond);
        rmsTotal += std::abs(leftSecond.rms - rightSecond.rms);
        ++compared;
    }

    if (compared == 0) {
        comparison.error = "aligned frames fall outside the audio track";
        return comparison;
    }

    comparison.available = true;
    comparison.comparedSeconds = compared;
    comparison.meanSimilarity = similarityTotal / compared;
    comparison.meanRmsDifference = rmsTotal / compared;
    comparison.sameSoundtrack = comparison.meanSimilarity >= threshold;
    return comparison;
}

}  // namespace vividmatch

#endif  // VIVIDMATCH_AUDIO_FINGERPRINT_HPP
