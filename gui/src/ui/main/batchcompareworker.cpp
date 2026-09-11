#include "batchcompareworker.h"

#include <opencv2/imgcodecs.hpp>

#include "visual_fingerprint.hpp"

#include <QFile>

#include <algorithm>
#include <numeric>
#include <vector>

namespace {

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
    const int count = m_paths.size();
    const long long pairCount = count > 1 ? (static_cast<long long>(count) * (count - 1)) / 2 : 0;
    const long long total = count + pairCount;

    std::vector<vividmatch::Fingerprint> fingerprints;
    fingerprints.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        emit progressChanged(i, static_cast<int>(total));
        cv::Mat image;
        if (!decodeToMat(m_paths.at(i), image)) {
            emit failed(QStringLiteral("无法读取图片：%1").arg(m_paths.at(i)));
            return;
        }
        try {
            fingerprints.push_back(vividmatch::makeFingerprint(image));
        } catch (const std::exception& error) {
            emit failed(QStringLiteral("图片指纹计算失败：%1").arg(m_paths.at(i)));
            return;
        }
    }

    std::vector<std::vector<double>> similarity(count, std::vector<double>(count, 0.0));
    std::vector<int> parent(count);
    std::iota(parent.begin(), parent.end(), 0);

    int done = count;
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            const double score = vividmatch::compareFingerprints(fingerprints[i], fingerprints[j]);
            similarity[i][j] = score;
            similarity[j][i] = score;
            if (score >= m_threshold) {
                unionRoots(parent, i, j);
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

    emit progressChanged(static_cast<int>(total), static_cast<int>(total));
    emit finished(clusters);
}
