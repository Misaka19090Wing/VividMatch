#ifndef BATCHCOMPAREWORKER_H
#define BATCHCOMPAREWORKER_H

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

struct BatchCluster {
    QVector<int> rows;
    double similarity = 0.0;
    bool duplicate = false;
};

Q_DECLARE_METATYPE(BatchCluster)
Q_DECLARE_METATYPE(QVector<BatchCluster>)

class BatchCompareWorker : public QObject
{
    Q_OBJECT

public:
    explicit BatchCompareWorker(const QVector<QString>& paths, double threshold,
                                QObject* parent = nullptr);

public slots:
    void run();

signals:
    void progressChanged(int done, int total);
    // elapsedMs is how long the whole batch took, measured inside the worker so
    // it covers decoding, fingerprinting and the pairwise comparison.
    void finished(QVector<BatchCluster> clusters, qint64 elapsedMs);
    void failed(const QString& message);

private:
    QVector<QString> m_paths;
    double m_threshold;
};

#endif // BATCHCOMPAREWORKER_H
