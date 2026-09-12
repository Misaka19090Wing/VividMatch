#include "videocomparepage.h"

#include "formatutils.h"
#include "parallel_extract.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kPreviewWidth = 260;
constexpr int kPreviewHeight = 150;

QStringList videoExtensions()
{
    return {
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("m4v"),
    };
}

bool isSupportedVideoFile(const QString& path)
{
    return videoExtensions().contains(QFileInfo(path).suffix().toLower());
}

QString firstDroppedVideo(const QMimeData* mime)
{
    if (!mime || !mime->hasUrls()) {
        return QString();
    }
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile() && isSupportedVideoFile(url.toLocalFile())) {
            return url.toLocalFile();
        }
    }
    return QString();
}

QString formatDuration(double seconds)
{
    const int total = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Reads one frame near the middle of the clip as a poster image, plus the
// properties shown under each preview.
bool loadVideoSummary(const QString& path, QImage& frame, QString& info)
{
    cv::VideoCapture capture(path.toStdString(), cv::CAP_FFMPEG);
    if (!capture.isOpened()) {
        capture.open(path.toStdString());
    }
    if (!capture.isOpened()) {
        return false;
    }

    const int width = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH));
    const int height = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT));
    const double fps = capture.get(cv::CAP_PROP_FPS);
    const double frames = capture.get(cv::CAP_PROP_FRAME_COUNT);
    const double duration = (fps > 0.0 && frames > 0.0) ? frames / fps : 0.0;

    cv::Mat mat;
    if (duration > 0.0) {
        capture.set(cv::CAP_PROP_POS_MSEC, duration * 500.0);
    }
    capture.read(mat);
    if (mat.empty()) {
        capture.set(cv::CAP_PROP_POS_FRAMES, 0);
        capture.read(mat);
    }
    capture.release();

    if (!mat.empty()) {
        cv::Mat rgb;
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        frame = QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                       QImage::Format_RGB888)
                    .copy();
    } else {
        frame = QImage();
    }

    info = QStringLiteral("%1x%2 · %3 fps · %4")
               .arg(width)
               .arg(height)
               .arg(QString::number(fps, 'f', 2))
               .arg(formatDuration(duration));
    return true;
}

QString verdictLabel(vividmatch::VideoVerdict verdict)
{
    switch (verdict) {
        case vividmatch::VideoVerdict::Identical:
            return VideoComparePage::tr("Verdict: the same video");
        case vividmatch::VideoVerdict::Reencoded:
            return VideoComparePage::tr("Verdict: same picture, different audio (a re-cut)");
        case vividmatch::VideoVerdict::Montage:
            return VideoComparePage::tr("Verdict: spliced together (the timeline jumps)");
        case vividmatch::VideoVerdict::Partial:
            return VideoComparePage::tr("Verdict: partly the same");
        case vividmatch::VideoVerdict::Different:
            return VideoComparePage::tr("Verdict: different videos");
    }
    return VideoComparePage::tr("Verdict: unknown");
}

QString verdictColour(vividmatch::VideoVerdict verdict)
{
    switch (verdict) {
        case vividmatch::VideoVerdict::Identical:
            return QStringLiteral("#15803d");
        case vividmatch::VideoVerdict::Reencoded:
        case vividmatch::VideoVerdict::Partial:
            return QStringLiteral("#b45309");
        case vividmatch::VideoVerdict::Montage:
        case vividmatch::VideoVerdict::Different:
            return QStringLiteral("#b91c1c");
    }
    return QStringLiteral("#334155");
}

QString verdictExplanation(vividmatch::VideoVerdict verdict)
{
    switch (verdict) {
        case vividmatch::VideoVerdict::Identical:
            return VideoComparePage::tr(
                "The shorter video is matched by one monotonic chain, so picture and "
                "timing agree.");
        case vividmatch::VideoVerdict::Reencoded:
            return VideoComparePage::tr(
                "The picture matches closely but the soundtrack does not, so the music "
                "bed was replaced.");
        case vividmatch::VideoVerdict::Montage:
            return VideoComparePage::tr(
                "The matched pictures do not sit on one timeline, so this is spliced "
                "together or played backwards.");
        case vividmatch::VideoVerdict::Partial:
            return VideoComparePage::tr(
                "The matches are in order but do not cover enough of the clip.");
        case vividmatch::VideoVerdict::Different:
            return VideoComparePage::tr("No sampled frame matched.");
    }
    return QString();
}

// The audio layer reports why it sat out. "No audio track" is normal and needs
// no action, while a missing ffmpeg does, so they must not read the same.
QString audioLayerNote(const vividmatch::AudioComparison& audio)
{
    if (audio.available) {
        return VideoComparePage::tr("Audio: compared %1 s, similarity %2% (RMS difference "
                                    "%3) -> %4")
            .arg(audio.comparedSeconds)
            .arg(audio.meanSimilarity * 100.0, 0, 'f', 1)
            .arg(audio.meanRmsDifference, 0, 'f', 3)
            .arg(audio.sameSoundtrack ? VideoComparePage::tr("the same soundtrack")
                                      : VideoComparePage::tr("different soundtracks"));
    }

    const QString reason = QString::fromStdString(audio.error);
    QString explained;
    if (reason.contains(QStringLiteral("no audio track"))) {
        explained = VideoComparePage::tr("at least one clip has no audio track, so only "
                                         "the picture was compared");
    } else if (reason.contains(QStringLiteral("ffmpeg not found"))) {
        explained = VideoComparePage::tr("ffmpeg was not found; install it and add it to "
                                         "PATH to enable audio comparison");
    } else if (reason.contains(QStringLiteral("could not be read"))) {
        explained = VideoComparePage::tr("the audio track could not be read (damaged file "
                                         "or unsupported container)");
    } else if (reason.contains(QStringLiteral("no aligned frames"))) {
        explained = VideoComparePage::tr("the pictures never aligned, so there was nothing "
                                         "to compare the audio over");
    } else if (reason.isEmpty()) {
        explained = VideoComparePage::tr("not enabled for this run");
    } else {
        explained = reason;
    }
    return VideoComparePage::tr("Audio: not used (%1)").arg(explained);
}

}  // namespace

// --- worker ----------------------------------------------------------------

VideoCompareWorker::VideoCompareWorker(QString firstPath, QString secondPath,
                                       double frameThreshold, bool includeAudio,
                                       QObject* parent)
    : QObject(parent)
    , m_firstPath(std::move(firstPath))
    , m_secondPath(std::move(secondPath))
    , m_frameThreshold(frameThreshold)
    , m_includeAudio(includeAudio)
{
}

void VideoCompareWorker::run()
{
    try {
        // The video and audio stages overlap, and progress() is called from
        // those worker threads, so each update is routed back through the event
        // loop rather than touching widgets from here.
        QElapsedTimer extractionTimer;
        extractionTimer.start();

        vividmatch::VideoFingerprint left;
        vividmatch::VideoFingerprint right;
        vividmatch::fingerprintPair(
            m_firstPath.toStdString(), m_secondPath.toStdString(), left, right,
            m_includeAudio, std::string(), [this](const std::string& stage) {
                const QString message = stage == "extracting audio fingerprints"
                                            ? tr("Extracting the audio fingerprint (ffmpeg)...")
                                            : tr("Extracting the video fingerprint...");
                QMetaObject::invokeMethod(
                    this, [this, message]() { emit progress(message); },
                    Qt::QueuedConnection);
            });

        const qint64 extractionMs = extractionTimer.elapsed();

        emit progress(tr("Comparing..."));
        vividmatch::VideoComparison comparison =
            vividmatch::compareVideoFingerprints(left, right, m_frameThreshold);
        emit finished(comparison, extractionMs);
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

// --- page ------------------------------------------------------------------

VideoComparePage::VideoComparePage(QWidget* parent)
    : QWidget(parent)
    , m_firstPreview(nullptr)
    , m_secondPreview(nullptr)
    , m_firstInfo(nullptr)
    , m_secondInfo(nullptr)
    , m_verdict(nullptr)
    , m_verdictDetail(nullptr)
    , m_metrics(nullptr)
    , m_progress(nullptr)
    , m_compareButton(nullptr)
    , m_threshold(nullptr)
    , m_audioCheck(nullptr)
    , m_worker(nullptr)
    , m_busy(false)
{
    qRegisterMetaType<vividmatch::VideoComparison>("vividmatch::VideoComparison");

    setAcceptDrops(true);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 18, 28, 18);
    root->setSpacing(14);

    QHBoxLayout* header = new QHBoxLayout;
    m_backButton = new QPushButton(this);
    m_backButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_backButton, &QPushButton::released, this, &VideoComparePage::backRequested);
    m_titleLabel = new QLabel(this);
    m_titleLabel->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;color:#1f2937;"));
    header->addWidget(m_backButton);
    header->addSpacing(10);
    header->addWidget(m_titleLabel);
    header->addStretch(1);
    root->addLayout(header);

    auto makePreview = [this]() -> QLabel* {
        QLabel* label = new QLabel(this);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumSize(kPreviewWidth, kPreviewHeight);
        label->setMaximumSize(kPreviewWidth * 2, kPreviewHeight * 2);
        label->setStyleSheet(QStringLiteral(
            "background:#ffffff;border:1px solid #d7dce2;border-radius:6px;color:#9ca3af;"));
        label->setAcceptDrops(true);
        label->installEventFilter(this);
        return label;
    };

    m_firstPreview = makePreview();
    m_secondPreview = makePreview();

    m_firstInfo = new QLabel(this);
    m_secondInfo = new QLabel(this);
    for (QLabel* info : {m_firstInfo, m_secondInfo}) {
        info->setAlignment(Qt::AlignCenter);
        info->setStyleSheet(QStringLiteral("color:#6b7280;"));
    }

    auto makeColumn = [this](QLabel* side, QLabel* preview, QLabel* info,
                             QPushButton* chooseButton) {
        QWidget* column = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(column);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        side->setParent(column);
        side->setStyleSheet(QStringLiteral("font-weight:600;color:#334155;"));
        layout->addWidget(side);
        layout->addWidget(preview, 0, Qt::AlignHCenter);
        layout->addWidget(info, 0, Qt::AlignHCenter);
        layout->addWidget(chooseButton, 0, Qt::AlignHCenter);
        return column;
    };

    m_firstSideLabel = new QLabel(this);
    m_secondSideLabel = new QLabel(this);
    m_firstButton = new QPushButton(this);
    m_firstButton->setObjectName(QStringLiteral("secondaryButton"));
    m_secondButton = new QPushButton(this);
    m_secondButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_firstButton, &QPushButton::released, this, &VideoComparePage::chooseFirstVideo);
    connect(m_secondButton, &QPushButton::released, this, &VideoComparePage::chooseSecondVideo);

    QHBoxLayout* videoRow = new QHBoxLayout;
    videoRow->setSpacing(22);
    videoRow->addStretch(1);
    videoRow->addWidget(
        makeColumn(m_firstSideLabel, m_firstPreview, m_firstInfo, m_firstButton));
    videoRow->addWidget(
        makeColumn(m_secondSideLabel, m_secondPreview, m_secondInfo, m_secondButton));
    videoRow->addStretch(1);
    root->addLayout(videoRow);

    QHBoxLayout* compareRow = new QHBoxLayout;
    compareRow->addStretch(1);
    m_thresholdLabel = new QLabel(this);
    compareRow->addWidget(m_thresholdLabel);
    m_threshold = new QDoubleSpinBox(this);
    m_threshold->setRange(0.50, 0.99);
    m_threshold->setDecimals(2);
    m_threshold->setSingleStep(0.01);
    m_threshold->setValue(vividmatch::kDefaultFrameThreshold);
    compareRow->addWidget(m_threshold);
    compareRow->addSpacing(14);
    m_audioCheck = new QCheckBox(this);
    m_audioCheck->setChecked(true);
    compareRow->addWidget(m_audioCheck);
    compareRow->addSpacing(14);
    m_compareButton = new QPushButton(this);
    m_compareButton->setObjectName(QStringLiteral("primaryButton"));
    m_compareButton->setEnabled(false);
    connect(m_compareButton, &QPushButton::released, this, &VideoComparePage::startCompare);
    compareRow->addWidget(m_compareButton);
    compareRow->addStretch(1);
    root->addLayout(compareRow);

    QFrame* resultPanel = new QFrame(this);
    resultPanel->setFrameShape(QFrame::StyledPanel);
    resultPanel->setStyleSheet(QStringLiteral(
        "QFrame { background:#ffffff; border:1px solid #d7dce2; border-radius:8px; }"
        "QLabel { color:#334155; }"
        "QLabel#verdict { font-size:18px; font-weight:700; }"
        "QLabel#metrics { color:#475569; }"));
    QVBoxLayout* resultLayout = new QVBoxLayout(resultPanel);
    resultLayout->setContentsMargins(18, 14, 18, 14);
    resultLayout->setSpacing(6);
    m_verdict = new QLabel(resultPanel);
    m_verdict->setObjectName(QStringLiteral("verdict"));
    m_verdictDetail = new QLabel(QString(), resultPanel);
    m_metrics = new QLabel(QString(), resultPanel);
    m_metrics->setObjectName(QStringLiteral("metrics"));
    m_metrics->setTextInteractionFlags(Qt::TextSelectableByMouse);
    resultLayout->addWidget(m_verdict);
    resultLayout->addWidget(m_verdictDetail);
    resultLayout->addWidget(m_metrics);
    root->addWidget(resultPanel);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    root->addWidget(m_progress);
    root->addStretch(1);

    retranslate();
}

void VideoComparePage::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void VideoComparePage::retranslate()
{
    m_backButton->setText(tr("Back to mode selection"));
    m_titleLabel->setText(tr("Video comparison mode"));

    m_firstSideLabel->setText(tr("Video one"));
    m_secondSideLabel->setText(tr("Video two"));
    m_firstButton->setText(tr("Choose video one"));
    m_secondButton->setText(tr("Choose video two"));
    m_thresholdLabel->setText(tr("Frame match threshold"));
    m_audioCheck->setText(tr("Compare audio (needs ffmpeg)"));
    m_compareButton->setText(tr("Start comparison"));

    // Before a clip is chosen the info line is the supported-formats hint; after,
    // it carries that clip's own measurements.
    const QString hint = tr("Supports mp4 / mov / mkv / avi / webm");
    if (m_firstPath.isEmpty()) {
        m_firstPreview->setText(tr("No video one chosen"));
        m_firstInfo->setText(hint);
    }
    if (m_secondPath.isEmpty()) {
        m_secondPreview->setText(tr("No video two chosen"));
        m_secondInfo->setText(hint);
    }

    // The state labels are derived from what the page is currently doing rather
    // than stored, so they cannot go stale in the other language.
    if (m_busy) {
        m_verdict->setText(m_progress->format());
    } else if (!m_hasResult) {
        m_verdict->setText(tr("Waiting to compare"));
    }
    if (!m_busy && !m_hasResult) {
        m_progress->setFormat(tr("Waiting to compare"));
    }
    refreshResultText();
}

void VideoComparePage::chooseFirstVideo() { chooseVideo(0); }

void VideoComparePage::chooseSecondVideo() { chooseVideo(1); }

void VideoComparePage::chooseVideo(int slot)
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a video"), QString(),
        tr("Videos (*.mp4 *.mov *.mkv *.avi *.webm *.m4v);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    setVideoPath(slot, path);
}

void VideoComparePage::setVideoPath(int slot, const QString& path)
{
    QImage frame;
    QString info;
    if (!loadVideoSummary(path, frame, info)) {
        QMessageBox::warning(this, tr("Cannot read the video"),
                             tr("Cannot open the video file:\n%1").arg(path));
        return;
    }

    if (slot == 0) {
        m_firstPath = path;
        m_firstFrame = frame;
        m_firstInfo->setText(info);
        m_firstInfo->setToolTip(path);
    } else {
        m_secondPath = path;
        m_secondFrame = frame;
        m_secondInfo->setText(info);
        m_secondInfo->setToolTip(path);
    }
    refreshPreview(slot);
    m_compareButton->setEnabled(!m_firstPath.isEmpty() && !m_secondPath.isEmpty()
                                && !m_busy);
    resetResult();
}

void VideoComparePage::refreshPreview(int slot)
{
    QLabel* preview = slot == 0 ? m_firstPreview : m_secondPreview;
    const QImage& frame = slot == 0 ? m_firstFrame : m_secondFrame;
    if (frame.isNull() || preview->width() <= 0 || preview->height() <= 0) {
        return;
    }
    const int usableWidth = std::max(40, preview->width() - 8);
    const int usableHeight = std::max(40, preview->height() - 8);
    preview->setPixmap(QPixmap::fromImage(frame).scaled(
        usableWidth, usableHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    preview->setText(QString());
}

void VideoComparePage::refreshPreviews()
{
    refreshPreview(0);
    refreshPreview(1);
}

void VideoComparePage::resetResult()
{
    m_hasResult = false;
    m_verdict->setText(tr("Waiting to compare"));
    m_verdict->setStyleSheet(QString());
    m_verdictDetail->clear();
    m_metrics->clear();
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("Waiting to compare"));
}

void VideoComparePage::setBusy(bool busy)
{
    m_busy = busy;
    m_compareButton->setEnabled(!busy && !m_firstPath.isEmpty() && !m_secondPath.isEmpty());
    m_threshold->setEnabled(!busy);
    m_audioCheck->setEnabled(!busy);
}

void VideoComparePage::startCompare()
{
    if (m_busy || m_firstPath.isEmpty() || m_secondPath.isEmpty()) {
        return;
    }
    if (m_thread && m_thread->isRunning()) {
        return;
    }

    setBusy(true);
    m_hasResult = false;
    m_verdict->setText(tr("Comparing..."));
    m_verdict->setStyleSheet(QString());
    m_verdictDetail->clear();
    m_metrics->clear();
    m_progress->setRange(0, 0);
    m_progress->setFormat(tr("Comparing..."));

    m_worker = new VideoCompareWorker(m_firstPath, m_secondPath, m_threshold->value(),
                                      m_audioCheck->isChecked());
    m_thread = new QThread(this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &VideoCompareWorker::run);
    connect(m_worker, &VideoCompareWorker::progress, this,
            &VideoComparePage::onWorkerProgress, Qt::QueuedConnection);
    connect(m_worker, &VideoCompareWorker::finished, this,
            &VideoComparePage::onCompareFinished, Qt::QueuedConnection);
    connect(m_worker, &VideoCompareWorker::failed, this, &VideoComparePage::onCompareFailed,
            Qt::QueuedConnection);
    connect(m_worker, &VideoCompareWorker::finished, m_thread, &QThread::quit);
    connect(m_worker, &VideoCompareWorker::failed, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, this, &VideoComparePage::onThreadFinished,
            Qt::QueuedConnection);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

void VideoComparePage::onWorkerProgress(const QString& message)
{
    m_progress->setFormat(message);
}

void VideoComparePage::onCompareFinished(vividmatch::VideoComparison comparison,
                                        qint64 extractionMs)
{
    showResult(comparison, extractionMs);
}

void VideoComparePage::onCompareFailed(const QString& message)
{
    m_hasResult = false;
    m_verdict->setText(tr("Comparison failed"));
    m_verdict->setStyleSheet(QStringLiteral("color:#b91c1c;font-size:18px;font-weight:700;"));
    m_verdictDetail->setText(message);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("Comparison failed"));
}

void VideoComparePage::onThreadFinished()
{
    setBusy(false);
    if (m_progress->maximum() == 0) {
        m_progress->setRange(0, 1);
        m_progress->setValue(1);
        m_progress->setFormat(tr("Comparison finished"));
    }
}

void VideoComparePage::showResult(const vividmatch::VideoComparison& comparison,
                                  qint64 extractionMs)
{
    // Remembered so a language switch can rebuild this panel: the text embeds
    // numbers, so it is regenerated rather than re-translated.
    m_hasResult = true;
    m_result = comparison;
    m_extractionMs = extractionMs;
    refreshResultText();
}

void VideoComparePage::refreshResultText()
{
    if (!m_hasResult) {
        m_verdict->setText(tr("Waiting to compare"));
        m_verdict->setStyleSheet(QString());
        m_verdictDetail->clear();
        m_metrics->clear();
        return;
    }

    const vividmatch::VideoComparison& comparison = m_result;
    m_verdict->setText(verdictLabel(comparison.verdict));
    m_verdict->setStyleSheet(
        QStringLiteral("color:%1;font-size:18px;font-weight:700;")
            .arg(verdictColour(comparison.verdict)));
    m_verdictDetail->setText(verdictExplanation(comparison.verdict));

    QStringList lines;
    lines << tr("Picture: sampled %1 / %2 frames, %3 matched pairs, chain of %4")
                 .arg(comparison.leftFrames)
                 .arg(comparison.rightFrames)
                 .arg(comparison.matches.size())
                 .arg(comparison.monotonicRun);
    lines << tr("Timing: coverage %1%, chain completeness %2%")
                 .arg(comparison.coverage * 100.0, 0, 'f', 1)
                 .arg(comparison.runRatio * 100.0, 0, 'f', 1);
    lines << tr("Frame similarity: mean %1% (best %2%, threshold %3)")
                 .arg(comparison.meanScore * 100.0, 0, 'f', 1)
                 .arg(comparison.bestMatchScore * 100.0, 0, 'f', 1)
                 .arg(comparison.frameThreshold, 0, 'f', 2);

    lines << audioLayerNote(comparison.audio);

    // Timing is split by stage. Extraction is normally the bulk of the wait, and
    // its stage times overlap each other, so it is reported as the wall time the
    // worker measured rather than as a sum.
    const qint64 overallMs = m_extractionMs + static_cast<qint64>(comparison.elapsedMs);
    lines << tr("Time: %1 in total (fingerprints %2, comparison %3)")
                 .arg(formatutils::durationValue(overallMs))
                 .arg(formatutils::durationValue(m_extractionMs))
                 .arg(formatutils::durationValue(static_cast<qint64>(comparison.elapsedMs)));

    m_metrics->setText(lines.join(QLatin1Char('\n')));
}

bool VideoComparePage::eventFilter(QObject* watched, QEvent* event)
{
    const bool overFirst = watched == m_firstPreview;
    const bool overSecond = watched == m_secondPreview;
    const bool overPage = watched == this;

    if (!overFirst && !overSecond && !overPage) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* dragEvent = static_cast<QDragEnterEvent*>(event);
        if (!firstDroppedVideo(dragEvent->mimeData()).isEmpty()) {
            dragEvent->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        const QString path = firstDroppedVideo(dropEvent->mimeData());
        if (!path.isEmpty()) {
            int slot = 0;
            if (overSecond) {
                slot = 1;
            } else if (overPage) {
                slot = dropEvent->position().toPoint().x() < width() / 2 ? 0 : 1;
            }
            setVideoPath(slot, path);
            dropEvent->acceptProposedAction();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VideoComparePage::dragEnterEvent(QDragEnterEvent* event)
{
    if (!firstDroppedVideo(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void VideoComparePage::dropEvent(QDropEvent* event)
{
    const QString path = firstDroppedVideo(event->mimeData());
    if (path.isEmpty()) {
        return;
    }
    const int slot = event->position().toPoint().x() < width() / 2 ? 0 : 1;
    setVideoPath(slot, path);
    event->acceptProposedAction();
}

void VideoComparePage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    refreshPreviews();
}
