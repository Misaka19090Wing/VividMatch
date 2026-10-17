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

// Number of set bits; the Hamming distance between two block hashes is the
// popcount of their XOR.
inline int popcount(std::uint64_t value) {
    int count = 0;
    while (value != 0) {
        value &= value - 1;  // clears the lowest set bit
        ++count;
    }
    return count;
}

// Normalises any supported image to the fixed 64x64 grayscale canvas every
// fingerprint is taken from. Scaling to one fixed size is what makes the
// comparison independent of the source resolution.
//
// INTER_AREA averages pixels when shrinking, which is the correct filter for
// downscaling; it degrades when enlarging a very small source, so a source
// smaller than the canvas is grown with INTER_LINEAR instead.
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

// Hashes one block of the 64x64 canvas.
//
// The block's 2D DCT is taken and the low-frequency 8x8 corner kept: those
// coefficients carry the block's coarse structure, which is what survives
// resizing and re-encoding, while the high frequencies are where codec noise
// lives. Each coefficient is then compared against the median of the 64, giving
// one bit per coefficient. Using the median (rather than zero) makes the bits
// invariant to a uniform brightness or contrast change, so a slightly darker
// re-encode still hashes the same.
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

// Fingerprints one image: normalise to the fixed canvas, then hash each of the
// 4x4 blocks in reading order (block index = row * kGridSize + column, which is
// the order compareFingerprints assumes on both sides).
//
// The source width and height are recorded for reporting only; they take no
// part in matching, which is what makes two resolutions of one picture compare
// equal.
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

// Convenience wrapper: decode a file, then fingerprint it.
inline Fingerprint fingerprintFromFile(const std::string& path) {
    cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
    if (image.empty()) {
        throw std::invalid_argument("cannot decode image: " + path);
    }
    return makeFingerprint(image);
}

// Detailed result of comparing two fingerprints block by block. The per-block
// Hamming distances and which blocks were discarded are what a UI needs to
// explain a score; compareFingerprints() only needs the final number.
struct BlockComparison {
    std::array<int, kBlockCount> distances{};
    std::array<bool, kBlockCount> discarded{};
    int keptBlocks = kBlockCount - kDiscardBlocks;
    double score = 0.0;
};

// Compares two fingerprints by block. Blocks are ordered by Hamming distance;
// the `discard` worst are marked discarded and the remaining ones averaged.
// The score is a fraction of the 64 bits per kept block, so it is a claim about
// block hashes, not about pixels: two unrelated but very flat images can share
// several blocks. Callers pair it with a threshold (0.78 by default).
inline BlockComparison compareFingerprintBlocks(
    const Fingerprint& left,
    const Fingerprint& right,
    int discard = kDiscardBlocks) {
    BlockComparison comparison;
    std::array<int, kBlockCount> order{};
    for (int i = 0; i < kBlockCount; ++i) {
        comparison.distances[i] = popcount(left.blocks[i] ^ right.blocks[i]);
        order[i] = i;
    }

    // Stable so ties discard the same block on every run, which keeps a log or
    // a test reproducible.
    std::stable_sort(order.begin(), order.end(), [&comparison](int first, int second) {
        return comparison.distances[first] < comparison.distances[second];
    });

    const int requested = std::max(0, std::min(discard, kBlockCount - 1));
    comparison.keptBlocks = kBlockCount - requested;
    double totalDistance = 0.0;
    for (int i = 0; i < comparison.keptBlocks; ++i) {
        totalDistance += comparison.distances[order[i]];
    }
    for (int i = comparison.keptBlocks; i < kBlockCount; ++i) {
        comparison.discarded[order[i]] = true;
    }
    comparison.score =
        1.0 - totalDistance / (static_cast<double>(comparison.keptBlocks) * kHashBits);
    return comparison;
}

// Similarity in [0, 1], where 1 means every kept block hashes identically.
//
// This is the 抗马赛克 rule: blocks are compared by Hamming
// distance, the `discard` worst blocks are dropped, and the rest are averaged.
// Dropping the worst blocks is what lets a watermark, a black bar or a mosaic
// patch sit over part of the picture without dragging the score down, while a
// genuinely different picture still differs in most blocks.
inline double compareFingerprints(
    const Fingerprint& left,
    const Fingerprint& right,
    int discard = kDiscardBlocks) {
    return compareFingerprintBlocks(left, right, discard).score;
}

}  // namespace vividmatch

#endif  // VIVIDMATCH_VISUAL_FINGERPRINT_HPP
