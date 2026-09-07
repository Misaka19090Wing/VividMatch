#include "visual_fingerprint.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using vividmatch::Fingerprint;
using vividmatch::compareFingerprints;
using vividmatch::makeFingerprint;

cv::Mat syntheticImage(int seed, int width = 512, int height = 512) {
    cv::Mat image(height, width, CV_8UC3);
    cv::RNG rng(seed);
    image = cv::Scalar(
        static_cast<int>(rng.uniform(0, 256)),
        static_cast<int>(rng.uniform(0, 256)),
        static_cast<int>(rng.uniform(0, 256)));

    for (int i = 0; i < 14; ++i) {
        const int center_x = static_cast<int>(rng.uniform(0, width));
        const int center_y = static_cast<int>(rng.uniform(0, height));
        const int radius =
            static_cast<int>(rng.uniform(10, std::max(20, std::min(width, height) / 6)));
        const cv::Scalar color(
            static_cast<int>(rng.uniform(0, 256)),
            static_cast<int>(rng.uniform(0, 256)),
            static_cast<int>(rng.uniform(0, 256)));

        switch (i % 4) {
            case 0:
                cv::circle(image, cv::Point(center_x, center_y), radius, color, -1);
                break;
            case 1: {
                const cv::Point second(
                    std::min(width - 1, center_x + radius),
                    std::min(height - 1, center_y + radius));
                cv::rectangle(image, cv::Point(center_x, center_y), second, color, -1);
                break;
            }
            case 2:
                cv::line(
                    image,
                    cv::Point(center_x, center_y),
                    cv::Point(
                        std::max(0, center_x + radius),
                        std::max(0, center_y - radius)),
                    color,
                    3);
                break;
            default: {
                std::vector<cv::Point> polygon_points{
                    cv::Point(center_x, center_y),
                    cv::Point(
                        std::min(width - 1, center_x + radius),
                        std::min(height - 1, center_y + radius / 2)),
                    cv::Point(
                        std::max(0, center_x - radius / 2),
                        std::min(height - 1, center_y + radius))};
                std::vector<std::vector<cv::Point>> polygons{polygon_points};
                cv::fillPoly(image, polygons, color);
                break;
            }
        }
    }
    return image;
}

cv::Mat resized(const cv::Mat& source, int size, int interpolation) {
    cv::Mat output;
    cv::resize(source, output, cv::Size(size, size), 0.0, 0.0, interpolation);
    return output;
}

bool nearlyEqual(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) < tolerance;
}

int expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }
    std::cout << "ok: " << message << '\n';
    return 0;
}

}  // namespace

int main() {
    int failures = 0;

    {
        const cv::Mat base = syntheticImage(11);
        std::vector<cv::Mat> variants{
            base,
            resized(base, 64, cv::INTER_AREA),
            resized(base, 96, cv::INTER_LINEAR),
            resized(base, 256, cv::INTER_AREA),
            resized(base, 720, cv::INTER_LINEAR),
            resized(base, 1280, cv::INTER_CUBIC),
            resized(base, 1920, cv::INTER_CUBIC)};

        std::vector<Fingerprint> fingerprints;
        for (const cv::Mat& variant : variants) {
            fingerprints.push_back(makeFingerprint(variant));
        }

        bool all_match = true;
        for (std::size_t i = 0; i < fingerprints.size(); ++i) {
            for (std::size_t j = 0; j < fingerprints.size(); ++j) {
                const double score = compareFingerprints(fingerprints[i], fingerprints[j]);
                if (score < 0.80) {
                    all_match = false;
                    std::cerr << "same-image score " << score << " below 0.80\n";
                }
            }
        }
        failures += expect(all_match, "same image across resolutions scores >= 0.80");
    }

    {
        const Fingerprint reference = makeFingerprint(syntheticImage(2));
        bool all_different = true;
        for (int seed = 3; seed < 8; ++seed) {
            const double score =
                compareFingerprints(reference, makeFingerprint(syntheticImage(seed, 480, 720)));
            if (score >= 0.70) {
                all_different = false;
                std::cerr << "different-image score " << score << " above 0.70\n";
            }
        }
        failures += expect(all_different, "different images score below 0.70");
    }

    if (failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    return failures;
}
