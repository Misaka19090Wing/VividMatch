#ifndef VIDEOCOMPAREPAGE_H
#define VIDEOCOMPAREPAGE_H

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "video_fingerprint.hpp"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QThread;

// Carries the whole comparison result back to the UI thread.
Q_DECLARE_METATYPE(vividmatch::VideoComparison)

// Fingerprints both clips and compares them off the UI thread: decoding video
// and shelling out to ffmpeg for audio would freeze the window otherwise.
class VideoCompareWorker : public QObject
{
    Q_OBJECT

public:
    VideoCompareWorker(QString firstPath, QString secondPath, double frameThreshold,
                       bool includeAudio, QObject* parent = nullptr);

public slots:
    void run();

signals:
    void progress(const QString& message);
    void finished(vividmatch::VideoComparison comparison);
    void failed(const QString& message);

private:
    QString m_firstPath;
    QString m_secondPath;
    double m_frameThreshold;
    bool m_includeAudio;
};

class VideoComparePage : public QWidget
{
    Q_OBJECT

public:
    explicit VideoComparePage(QWidget* parent = nullptr);

signals:
    void backRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void chooseFirstVideo();
    void chooseSecondVideo();
    void startCompare();
    void onWorkerProgress(const QString& message);
    void onCompareFinished(vividmatch::VideoComparison comparison);
    void onCompareFailed(const QString& message);
    void onThreadFinished();

private:
    void chooseVideo(int slot);
    void setVideoPath(int slot, const QString& path);
    void refreshPreview(int slot);
    void refreshPreviews();
    void resetResult();
    void showResult(const vividmatch::VideoComparison& comparison);
    void setBusy(bool busy);

    QLabel* m_firstPreview;
    QLabel* m_secondPreview;
    QLabel* m_firstInfo;
    QLabel* m_secondInfo;
    QLabel* m_verdict;
    QLabel* m_verdictDetail;
    QLabel* m_metrics;
    QProgressBar* m_progress;
    QPushButton* m_compareButton;
    QDoubleSpinBox* m_threshold;
    QCheckBox* m_audioCheck;

    QString m_firstPath;
    QString m_secondPath;
    QImage m_firstFrame;
    QImage m_secondFrame;
    QPointer<QThread> m_thread;
    VideoCompareWorker* m_worker;
    bool m_busy;
};

#endif  // VIDEOCOMPAREPAGE_H
