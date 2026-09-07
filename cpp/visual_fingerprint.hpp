#ifndef VIVIDMATCH_VISUAL_FINGERPRINT_HPP
#define VIVIDMATCH_VISUAL_FINGERPRINT_HPP

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace vividmatch {

inline constexpr int kImageSize = 64;
inline constexpr int kGridSize = 4;
inline constexpr int kBlockSize = kImageSize / kGridSize;
inline constexpr int kBlockCount = kGridSize * kGridSize;
inline constexpr int kHashBits = 64;
inline constexpr int kDiscardBlocks = 4;

struct Fingerprint {
    std::array<std::uint64_t, kBlockCount> blocks{};
    int width = 0;
    int height = 0;
};

inline int popcount(std::uint64_t value) {
    int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

inline cv::Mat fingerprintCanvas(const cv::Mat& image) {
    if (image.empty()) {
        throw std::invalid_argument("input image is empty");
    }

    cv::Mat gray;
    if (image.type() == CV_8UC1) {
        gray = image;
    } else if (image.type() == CV_8UC3 || image.type() == CV_8UC4) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        throw std::invalid_argument("unsupported image pixel type");
    }

    const int interpolation =
        (gray.rows < kImageSize && gray.cols < kImageSize)
            ? cv::INTER_LINEAR
            : cv::INTER_AREA;
    cv::Mat canvas;
    cv::resize(gray, canvas, cv::Size(kImageSize, kImageSize), 0.0, 0.0, interpolation);
    return canvas;
}

inline std::uint64_t blockHash(const cv::Mat& tile) {
    cv::Mat float_tile;
    tile.convertTo(float_tile, CV_32FC1);
    cv::Mat dct;
    cv::dct(float_tile, dct);

    std::array<float, kHashBits> low_frequency{};
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            low_frequency[row * 8 + col] = dct.at<float>(row, col);
        }
    }

    std::array<float, kHashBits> sorted = low_frequency;
    std::sort(sorted.begin(), sorted.end());
    const float median = (sorted[31] + sorted[32]) * 0.5f;

    std::uint64_t hash = 0;
    for (int i = 0; i < kHashBits; ++i) {
        if (low_frequency[static_cast<std::size_t>(i)] > median) {
            hash |= std::uint64_t{1} << i;
        }
    }
    return hash;
}

inline Fingerprint makeFingerprint(const cv::Mat& image) {
    cv::Mat canvas = fingerprintCanvas(image);
    Fingerprint fingerprint;
    fingerprint.width = image.cols;
    fingerprint.height = image.rows;

    int index = 0;
    for (int row = 0; row < kGridSize; ++row) {
        for (int col = 0; col < kGridSize; ++col) {
            cv::Rect block_rect(
                col * kBlockSize,
                row * kBlockSize,
                kBlockSize,
                kBlockSize);
            fingerprint.blocks[index] = blockHash(canvas(block_rect));
            ++index;
        }
    }
    return fingerprint;
}

inline Fingerprint fingerprintFromFile(const std::string& path) {
    cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
    if (image.empty()) {
        throw std::invalid_argument("cannot decode image: " + path);
    }
    return makeFingerprint(image);
}

inline double compareFingerprints(
    const Fingerprint& left,
    const Fingerprint& right,
    int discard = kDiscardBlocks) {
    std::array<double, kBlockCount> distances{};
    for (int i = 0; i < kBlockCount; ++i) {
        distances[i] = static_cast<double>(
            popcount(left.blocks[i] ^ right.blocks[i]));
    }

    std::sort(distances.begin(), distances.end());
    const int keep_count = std::max(1, kBlockCount - discard);
    double total_distance = 0.0;
    for (int i = 0; i < keep_count; ++i) {
        total_distance += distances[i];
    }
    return 1.0 - total_distance / (static_cast<double>(keep_count) * kHashBits);
}

}  // namespace vividmatch

#endif  // VIVIDMATCH_VISUAL_FINGERPRINT_HPP
