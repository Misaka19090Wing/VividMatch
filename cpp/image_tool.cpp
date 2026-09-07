#include "visual_fingerprint.hpp"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

int printHash(const std::string& path) {
    const vividmatch::Fingerprint fingerprint = vividmatch::fingerprintFromFile(path);
    std::cout << "path=" << path << '\n';
    std::cout << "source=" << fingerprint.width << "x" << fingerprint.height << '\n';
    std::cout << "hash=";
    for (const std::uint64_t block : fingerprint.blocks) {
        std::cout << std::hex << std::setw(16) << std::setfill('0') << block;
    }
    std::cout << '\n';
    return 0;
}

int compareImages(const std::string& first, const std::string& second, double threshold) {
    const vividmatch::Fingerprint left = vividmatch::fingerprintFromFile(first);
    const vividmatch::Fingerprint right = vividmatch::fingerprintFromFile(second);
    const double score = vividmatch::compareFingerprints(left, right);
    const bool match = score >= threshold;

    std::cout.setf(std::ios::fixed);
    std::cout << "first=" << first << '\n';
    std::cout << "second=" << second << '\n';
    std::cout << "source=" << left.width << "x" << left.height << " vs "
              << right.width << "x" << right.height << '\n';
    std::cout << std::setprecision(4) << "score=" << score << '\n';
    std::cout << std::setprecision(4) << "threshold=" << threshold << '\n';
    std::cout << "match=" << (match ? "true" : "false") << '\n';
    return match ? 0 : 1;
}

void usage() {
    std::cout
        << "VividMatch image fingerprint (OpenCV C++)\n"
        << "usage:\n"
        << "  vividmatch_image compare <first> <second> [threshold]\n"
        << "  vividmatch_image hash <image>\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string> args(argv + 1, argv + argc);
        if (args.size() >= 3 && args[0] == "compare") {
            double threshold = 0.78;
            if (args.size() >= 4) {
                try {
                    threshold = std::stod(args[3]);
                } catch (const std::exception&) {
                    throw std::invalid_argument("invalid threshold: " + args[3]);
                }
            }
            return compareImages(args[1], args[2], threshold);
        }
        if (args.size() == 2 && args[0] == "hash") {
            return printHash(args[1]);
        }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
