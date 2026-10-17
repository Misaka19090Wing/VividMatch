#include "batchcompareworker.h"

#include <opencv2/imgcodecs.hpp>

#include "comparisonlog.h"
#include "formatutils.h"
#include "visual_fingerprint.hpp"

#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QStringList>

#include <algorithm>
#include <numeric>
#include <vector>

namespace {

// An upper bound on per-pair log lines. A thousand images have half a million
// pairs; logging every one would make the panel unusable and the text huge.
// The comparison itself still runs over every pair, and a warning marks where
// the per-pair view stops.
// shortcut: per-pair lines stop at 5000, raise it if a batch needs every pair visible.
constexpr int kMaxPairLogLines = 5000;

bool decodeToMat(const QString& path, cv::Mat& output)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray data = file.readAll();
    std::vector<uchar> buffer(data.constData(), data.constData() + data.size());
    output = cv::imdecode(buffer, cv::IMREAD_COLOR);
    return !output.empty();
}

int findRoot(std::vector<int>& parent, int index)
{
    while (parent[index] != index) {
        parent[index] = parent[parent[index]];
        index = parent[index];
    }
    return index;
}

void unionRoots(std::vector<int>& parent, int left, int right)
{
    const int leftRoot = findRoot(parent, left);
    const int rightRoot = findRoot(parent, right);
    if (leftRoot != rightRoot) {
        parent[rightRoot] = leftRoot;
    }
}

} // namespace

BatchCompareWorker::BatchCompareWorker(const QVector<QString>& paths, double threshold,
                                       QObject* parent)
    : QObject(parent)
    , m_paths(paths)
    , m_threshold(threshold)
{
}

void BatchCompareWorker::run()
{
    auto log = [this](ComparisonLogLevel level, const QString& message) {
        emit logMessage(static_cast<int>(level), message);
    };

    // Measured inside the worker so the reported time covers the actual work:
    // decoding, fingerprinting and the pairwise comparison.
    QElapsedTimer timer;
    timer.start();

    const int count = m_paths.size();
    const long long pairCount = count > 1 ? (static_cast<long long>(count) * (count - 1)) / 2 : 0;
    const long long total = count + pairCount;

    log(ComparisonLogLevel::Info,
        tr("Starting batch image comparison: %1 images, match threshold %2")
            .arg(count)
            .arg(m_threshold, 0, 'f', 2));

    std::vector<vividmatch::Fingerprint> fingerprints;
    fingerprints.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        emit progressChanged(i, static_cast<int>(total));

        QElapsedTimer imageTimer;
        imageTimer.start();
        cv::Mat image;
        if (!decodeToMat(m_paths.at(i), image)) {
            const QString message =
                tr("Could not read image %1: %2").arg(i + 1).arg(m_paths.at(i));
            log(ComparisonLogLevel::Error, message);
            emit failed(message);
            return;
        }
        const qint64 decodeMs = imageTimer.restart();

        try {
            fingerprints.push_back(vividmatch::makeFingerprint(image));
        } catch (const std::exception& error) {
            const QString message = tr("Could not fingerprint image %1: %2 (%3)")
                                        .arg(i + 1)
                                        .arg(m_paths.at(i))
                                        .arg(QString::fromUtf8(error.what()));
            log(ComparisonLogLevel::Error, message);
            emit failed(message);
            return;
        }
        const qint64 hashMs = imageTimer.elapsed();
        const vividmatch::Fingerprint& fingerprint = fingerprints.back();
        log(ComparisonLogLevel::Detail,
            tr("Image %1/%2: %3 (%4x%5), decoded in %6, hashed in %7")
                .arg(i + 1)
                .arg(count)
                .arg(QFileInfo(m_paths.at(i)).fileName())
                .arg(fingerprint.width)
                .arg(fingerprint.height)
                .arg(formatutils::durationValue(decodeMs))
                .arg(formatutils::durationValue(hashMs)));
    }

    log(ComparisonLogLevel::Info,
        tr("Fingerprint extraction finished: %1 images in %2")
            .arg(count)
            .arg(formatutils::durationValue(timer.elapsed())));

    std::vector<std::vector<double>> similarity(count, std::vector<double>(count, 0.0));
    std::vector<int> parent(count);
    std::iota(parent.begin(), parent.end(), 0);

    int loggedPairs = 0;
    int done = count;
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            const double score = vividmatch::compareFingerprints(fingerprints[i], fingerprints[j]);
            similarity[i][j] = score;
            similarity[j][i] = score;
            if (score >= m_threshold) {
                unionRoots(parent, i, j);
            }

            if (loggedPairs < kMaxPairLogLines) {
                log(ComparisonLogLevel::Detail,
                    tr("Pair %1-%2 (%3 vs %4): similarity %5% (threshold %6%) -> %7")
                        .arg(i + 1)
                        .arg(j + 1)
                        .arg(QFileInfo(m_paths.at(i)).fileName())
                        .arg(QFileInfo(m_paths.at(j)).fileName())
                        .arg(score * 100.0, 0, 'f', 2)
                        .arg(m_threshold * 100.0, 0, 'f', 2)
                        .arg(score >= m_threshold ? tr("same") : tr("different")));
                ++loggedPairs;
                if (loggedPairs == kMaxPairLogLines) {
                    log(ComparisonLogLevel::Warning,
                        tr("Per-pair detail stopped after %1 pairs; the remaining pairs "
                           "are still compared.")
                            .arg(kMaxPairLogLines));
                }
            }

            ++done;
            emit progressChanged(done, static_cast<int>(total));
        }
    }

    std::vector<std::vector<int>> grouped(count);
    for (int i = 0; i < count; ++i) {
        grouped[findRoot(parent, i)].push_back(i);
    }

    QVector<BatchCluster> clusters;
    for (const std::vector<int>& rows : grouped) {
        if (rows.empty()) {
            continue;
        }
        BatchCluster cluster;
        double maxSimilarity = 0.0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            cluster.rows.append(rows[i]);
            for (std::size_t j = i + 1; j < rows.size(); ++j) {
                maxSimilarity = std::max(maxSimilarity, similarity[rows[i]][rows[j]]);
            }
        }
        cluster.similarity = maxSimilarity;
        cluster.duplicate = rows.size() > 1;
        clusters.append(cluster);
    }

    std::sort(clusters.begin(), clusters.end(),
              [](const BatchCluster& left, const BatchCluster& right) {
                  if (left.duplicate != right.duplicate) {
                      return left.duplicate && !right.duplicate;
                  }
                  if (left.duplicate) {
                      return left.similarity > right.similarity;
                  }
                  return left.rows.first() < right.rows.first();
              });

    int duplicateGroups = 0;
    for (int index = 0; index < static_cast<int>(clusters.size()); ++index) {
        const BatchCluster& cluster = clusters.at(index);
        QStringList names;
        for (const int row : cluster.rows) {
            if (row >= 0 && row < m_paths.size()) {
                names << QFileInfo(m_paths.at(row)).fileName();
            }
        }
        if (cluster.duplicate) {
            ++duplicateGroups;
            log(ComparisonLogLevel::Success,
                tr("Duplicate group %1: %2 images, best similarity %3% - %4")
                    .arg(index + 1)
                    .arg(cluster.rows.size())
                    .arg(cluster.similarity * 100.0, 0, 'f', 2)
                    .arg(names.join(QStringLiteral(", "))));
        } else {
            log(ComparisonLogLevel::Detail,
                tr("Image %1 stands alone: %2").arg(index + 1).arg(names.join(QStringLiteral(", "))));
        }
    }

    log(ComparisonLogLevel::Info,
        tr("Batch image comparison finished: %1 images, %2 groups, %3 duplicate "
           "groups in %4")
            .arg(count)
            .arg(static_cast<int>(clusters.size()))
            .arg(duplicateGroups)
            .arg(formatutils::durationValue(timer.elapsed())));

    emit progressChanged(static_cast<int>(total), static_cast<int>(total));
    emit finished(clusters, timer.elapsed());
}
