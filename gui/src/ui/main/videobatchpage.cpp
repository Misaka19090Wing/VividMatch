#include "videobatchpage.h"

#include "comparisonlog.h"
#include "formatutils.h"
#include "keeppolicydialog.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSettings>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QThread>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <filesystem>

namespace {

constexpr int kRecordIdRole = Qt::UserRole + 1;
// Per-pair detail is useful but can grow with the square of the batch size. The
// comparison still covers every pair; this only bounds what the panel prints.
// shortcut: per-pair lines stop at 5000, raise it if a batch needs every pair visible.
constexpr int kMaxPairLogLines = 5000;
// Marks the group holding clips that are not compared yet, so a rebuild can
// replace it without touching the result groups.
constexpr int kPendingMarker = -1;
constexpr int kGroupBaseRole = Qt::UserRole + 2;
constexpr int kThumbnailImageRole = Qt::UserRole + 3;
constexpr int kDuplicateGroupRole = Qt::UserRole + 4;

// Padding kept between the preview column edges and the picture itself.
constexpr int kThumbnailPadding = 6;
// Width the grabbed frame is stored at. Only a memory cap: the delegate scales
// the stored frame to whatever width the preview column has, so a larger column
// still shows a larger picture.
constexpr int kMaxPreviewWidth = 480;
// A preview column narrower than this is no longer useful, so it stays readable
// even when dragged.
constexpr int kMinPreviewColumnWidth = 96;

QStringList videoWildcards()
{
    return {QStringLiteral("*.mp4"), QStringLiteral("*.mov"), QStringLiteral("*.mkv"),
            QStringLiteral("*.avi"), QStringLiteral("*.webm"), QStringLiteral("*.m4v")};
}

bool isSupportedVideoFile(const QString& path)
{
    static const QStringList extensions = {QStringLiteral("mp4"), QStringLiteral("mov"),
                                           QStringLiteral("mkv"), QStringLiteral("avi"),
                                           QStringLiteral("webm"), QStringLiteral("m4v")};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

QString humanSize(qint64 bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    }
    const double mb = kb / 1024.0;
    if (mb < 1024.0) {
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 2);
    }
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

QString formatDuration(double seconds)
{
    const int total = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Where in the clip the preview frame is taken from, as a percentage of its
// length, unless the user picks another position.
constexpr int kDefaultCapturePercent = 50;

// Everything the list shows about a clip that needs the clip to be opened: a
// preview frame plus its real resolution and duration. Gathered in one pass so
// the resolution and duration columns can be filled as soon as the clip is
// added, rather than only after a comparison.
struct ClipProbe {
    cv::Mat frame;
    int width = 0;
    int height = 0;
    double duration = 0.0;
};

// Opens the clip once, reads its properties, and grabs a preview frame from
// `percent` of its length. The frame is empty if the clip cannot be read as a
// picture; the properties may still be known in that case.
//
// `percent` is how far into the clip to look, which the user controls: the first
// frame of a video is often a title card or a fade from black, so the default is
// the middle of the clip rather than its start.
ClipProbe probeClip(const std::string& path, int percent)
{
    ClipProbe result;
    cv::VideoCapture capture(path, cv::CAP_FFMPEG);
    if (!capture.isOpened()) {
        capture.open(path);
    }
    if (!capture.isOpened()) {
        return result;
    }

    // The coded size, which is what a user compares between two copies.
    result.width = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH));
    result.height = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT));
    const double frames = capture.get(cv::CAP_PROP_FRAME_COUNT);
    const double fps = capture.get(cv::CAP_PROP_FPS);
    if (frames > 0.0 && fps > 0.0) {
        result.duration = frames / fps;
    }

    if (result.duration > 0.0) {
        // Clamp so 100% does not seek past the end and fail outright.
        const double fraction =
            std::min(0.99, std::max(0.0, static_cast<double>(percent) / 100.0));
        // A time-based seek is used rather than a frame index: it lands in the
        // right part of the clip, which is all a preview needs, and frame-exact
        // seeking is slower.
        capture.set(cv::CAP_PROP_POS_MSEC, result.duration * fraction * 1000.0);
    }

    // Seeking can land slightly off; take the first frame that arrives.
    for (int attempt = 0; attempt < 3 && result.frame.empty(); ++attempt) {
        if (!capture.read(result.frame)) {
            break;
        }
    }
    capture.release();

    if (!result.frame.empty() && result.frame.cols > kMaxPreviewWidth) {
        // Cap the stored size to bound memory across a large batch. The aspect
        // ratio is preserved, and the delegate scales the result to the column,
        // so a wider column still shows a wider picture.
        cv::Mat scaled;
        const double scale = static_cast<double>(kMaxPreviewWidth) / result.frame.cols;
        cv::resize(result.frame, scaled,
                   cv::Size(kMaxPreviewWidth,
                            std::max(1, static_cast<int>(result.frame.rows * scale))),
                   0.0, 0.0, cv::INTER_AREA);
        result.frame = scaled;
    }
    return result;
}

QImage matToImage(const cv::Mat& frame)
{
    if (frame.empty()) {
        return QImage();
    }
    // BGR (OpenCV's order) to RGB, which is what QImage wants.
    cv::Mat rgb;
    cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
    return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                  QImage::Format_RGB888)
        .copy();  // copy: the QImage must outlive `rgb`
}

}  // namespace

// Rect the checkbox indicator occupies inside the check column. Left-aligning it
// right after the cell start keeps it hugging the 选中 header instead of drifting
// to the middle of the column when that column is widened.
static QRect checkBoxRect(const QStyleOptionViewItem& option)
{
    QStyleOptionButton indicator;
    indicator.state = QStyle::State_Enabled;
    indicator.rect = option.rect;
    indicator.direction = option.direction;
    indicator.fontMetrics = option.fontMetrics;
    const QRect indicatorRect =
        option.widget->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,
                                               &indicator, option.widget);
    const QSize size = indicatorRect.size();
    const int maxX = option.rect.right() - size.width() + 1;
    const int x = std::min(option.rect.x(), maxX);
    return QRect(x, option.rect.y() + (option.rect.height() - size.height()) / 2,
                 size.width(), size.height());
}

// --- preview delegate ------------------------------------------------------

// Keeps the checkbox left-aligned in its column and scales the preview frame to
// the current width of the preview column, so dragging that column edge really
// resizes the picture instead of just widening empty space.
class VideoBatchPage::VideoBatchItemDelegate : public QStyledItemDelegate
{
public:
    explicit VideoBatchItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void setPreviewColumnWidth(int width) { m_previewColumnWidth = std::max(1, width); }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        // Group rows span the whole row, so their height must not follow the
        // preview column width. Only real clips carry a record id.
        const bool isClip =
            index.sibling(index.row(), CheckColumn).data(kRecordIdRole).toInt() > 0;
        if (isClip && index.column() == PreviewColumn) {
            return QSize(m_previewColumnWidth,
                         previewHeight(index, m_previewColumnWidth) + 2 * kThumbnailPadding);
        }
        return QStyledItemDelegate::sizeHint(option, index);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (index.column() == CheckColumn) {
            paintCheckColumn(painter, option, index);
            return;
        }
        if (index.column() == PreviewColumn) {
            paintPreviewColumn(painter, option, index);
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

    // Height the preview occupies when its column is columnWidth wide.
    static int previewHeight(const QModelIndex& index, int columnWidth)
    {
        const int available = std::max(1, columnWidth - 2 * kThumbnailPadding);
        const QImage image = index.data(kThumbnailImageRole).value<QImage>();
        if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
            return available * 9 / 16;  // a 16:9 placeholder before the grab lands
        }
        return std::max(1, image.height() * available / image.width());
    }

private:
    void paintCheckColumn(QPainter* painter, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const
    {
        if (!(index.flags() & Qt::ItemIsUserCheckable)) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem adjusted(option);
        initStyleOption(&adjusted, index);
        adjusted.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        adjusted.text.clear();
        adjusted.icon = QIcon();

        QStyle* style = option.widget->style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &adjusted, painter,
                             option.widget);

        QStyleOptionButton indicator;
        indicator.state = QStyle::State_Enabled;
        if (option.state & QStyle::State_MouseOver) {
            indicator.state |= QStyle::State_MouseOver;
        }
        indicator.rect = checkBoxRect(option);
        indicator.direction = option.direction;
        indicator.fontMetrics = option.fontMetrics;
        switch (index.data(Qt::CheckStateRole).toInt()) {
            case Qt::Checked:
                indicator.state |= QStyle::State_On;
                break;
            case Qt::PartiallyChecked:
                indicator.state |= QStyle::State_NoChange;
                break;
            default:
                indicator.state |= QStyle::State_Off;
                break;
        }
        style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &indicator, painter,
                             option.widget);
    }

    void paintPreviewColumn(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const
    {
        QStyleOptionViewItem adjusted(option);
        initStyleOption(&adjusted, index);
        adjusted.text.clear();
        adjusted.icon = QIcon();

        QStyle* style = option.widget->style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &adjusted, painter,
                             option.widget);

        const QImage image = index.data(kThumbnailImageRole).value<QImage>();
        if (image.isNull()) {
            return;
        }
        const QRect target =
            option.rect.adjusted(kThumbnailPadding, kThumbnailPadding, -kThumbnailPadding,
                                 -kThumbnailPadding);
        if (target.width() <= 0 || target.height() <= 0) {
            return;
        }
        // Fit inside the cell without distorting: letterbox rather than stretch.
        const QSize scaled = image.size().scaled(target.size(), Qt::KeepAspectRatio);
        const QRect box(target.x() + (target.width() - scaled.width()) / 2,
                        target.y() + (target.height() - scaled.height()) / 2,
                        scaled.width(), scaled.height());
        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->drawImage(box, image);
        painter->restore();
    }

    int m_previewColumnWidth = 160;
};

// Heading for one result group. Built here rather than in the page so the worker
// can label a group before it is handed over.
QString groupLabel(const VideoBatchGroupInfo& group)
{
    if (group.duplicate) {
        QString label =
            VideoBatchPage::tr("Duplicate group - similarity %1% - %2 clips")
                .arg(group.similarity * 100.0, 0, 'f', 1)
                .arg(group.rows.size());
        if (group.audioDiffers) {
            label += VideoBatchPage::tr(" - contains a version with different audio");
        }
        return label;
    }
    if (group.montage) {
        return VideoBatchPage::tr("Possibly spliced - only partly overlapping");
    }
    return VideoBatchPage::tr("No duplicates");
}

// Verdict name for the log. The core's canonical names ("identical",
// "reencoded", ...) are useful in the CLI, but the GUI log is read by the same
// people as the rest of the interface, so it uses the translated wording.
QString verdictText(vividmatch::VideoVerdict verdict)
{
    switch (verdict) {
        case vividmatch::VideoVerdict::Identical:
            return VideoBatchPage::tr("identical");
        case vividmatch::VideoVerdict::Reencoded:
            return VideoBatchPage::tr("reencoded");
        case vividmatch::VideoVerdict::Montage:
            return VideoBatchPage::tr("montage");
        case vividmatch::VideoVerdict::Partial:
            return VideoBatchPage::tr("partial");
        case vividmatch::VideoVerdict::Different:
            return VideoBatchPage::tr("different");
    }
    return VideoBatchPage::tr("unknown");
}

// One preview grab, run on the global thread pool. Pooling keeps the UI
// responsive while a folder of clips is being read, and a plain runnable avoids
// sharing any state beyond the path it was handed.
class ThumbnailTask : public QRunnable
{
public:
    ThumbnailTask(QString path, int percent, VideoBatchPage* page)
        : m_path(std::move(path))
        , m_percent(percent)
        , m_page(page)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        const std::string utf8 = m_path.toStdString();
        const ClipProbe probe = probeClip(utf8, m_percent);
        const QImage image = matToImage(probe.frame);
        // Delivered on the page's thread; an empty image still has to arrive so
        // the page can stop waiting for this clip.
        QMetaObject::invokeMethod(
            m_page,
            [page = m_page, path = m_path, image, probe]() {
                emit page->thumbnailReady(path, image, probe.width, probe.height,
                                          probe.duration);
            },
            Qt::QueuedConnection);
    }

private:
    QString m_path;
    int m_percent;
    VideoBatchPage* m_page;
};

// --- ranking ---------------------------------------------------------------

namespace {

qint64 videoRuleValue(const vividmatch::VideoBatchItem& item, KeepCriterion criterion)
{
    switch (criterion) {
        case KeepCriterion::Resolution:
            return static_cast<qint64>(item.fingerprint.width) * item.fingerprint.height;
        case KeepCriterion::FileSize: {
            std::error_code code;
            const std::uintmax_t bytes =
                std::filesystem::file_size(std::filesystem::u8path(item.path), code);
            return code ? 0 : static_cast<qint64>(bytes);
        }
        case KeepCriterion::Modified:
            return QFileInfo(QString::fromStdString(item.path))
                .lastModified()
                .toMSecsSinceEpoch();
        case KeepCriterion::BitDepth:
            return 0;
    }
    return 0;
}

bool betterVideoItem(const vividmatch::VideoBatchItem& left,
                     const vividmatch::VideoBatchItem& right,
                     const QVector<KeepRule>& rules)
{
    for (const KeepRule& rule : rules) {
        const qint64 leftValue = videoRuleValue(left, rule.criterion);
        const qint64 rightValue = videoRuleValue(right, rule.criterion);
        if (leftValue == rightValue) {
            continue;
        }
        return rule.direction == KeepDirection::Higher ? leftValue > rightValue
                                                       : leftValue < rightValue;
    }
    return false;
}

} // namespace

int rankVideoGroup(const QVector<KeepRule>& rules, bool keepAll,
                   const std::vector<int>& members,
                   const std::vector<vividmatch::VideoBatchItem>& items)
{
    if (keepAll || members.empty()) {
        return -1;
    }

    int best = members.front();
    for (const int member : members) {
        if (member != best
            && betterVideoItem(items[static_cast<std::size_t>(member)],
                               items[static_cast<std::size_t>(best)], rules)) {
            best = member;
        }
    }
    return best;
}

// --- worker ----------------------------------------------------------------

VideoBatchWorker::VideoBatchWorker(QStringList paths, QVector<KeepRule> rules, bool keepAll)
    : m_paths(std::move(paths))
    , m_rules(std::move(rules))
    , m_keepAll(keepAll)
{
}

void VideoBatchWorker::run()
{
    auto log = [this](ComparisonLogLevel level, const QString& message) {
        emit logMessage(static_cast<int>(level), message);
    };

    try {
        std::vector<std::string> paths;
        paths.reserve(static_cast<std::size_t>(m_paths.size()));
        for (const QString& path : m_paths) {
            paths.push_back(path.toStdString());
        }

        log(ComparisonLogLevel::Info,
            VideoBatchPage::tr("Starting batch video comparison: %1 clips, frame "
                               "threshold %2, signature threshold %3, %4 worker threads")
                .arg(static_cast<int>(m_paths.size()))
                .arg(vividmatch::kDefaultFrameThreshold, 0, 'f', 2)
                .arg(vividmatch::kDefaultSignatureThreshold, 0, 'f', 2)
                .arg(vividmatch::kDefaultBatchWorkers));

        // The rules are carried by value, so ranking a group here touches no
        // page state from this thread.
        const QVector<KeepRule> rules = m_rules;
        const bool keepAll = m_keepAll;
        const vividmatch::VideoRanking ranking =
            [rules, keepAll](const std::vector<int>& members,
                             const std::vector<vividmatch::VideoBatchItem>& items) {
                return rankVideoGroup(rules, keepAll, members, items);
            };

        const vividmatch::VideoBatchResult result = vividmatch::compareVideoBatch(
            paths, vividmatch::kDefaultFrameThreshold, vividmatch::kDefaultSignatureThreshold,
            vividmatch::kDefaultBatchWorkers, ranking,
            [this, &log](int done, int total, const std::string& stage) {
                const QString label = stage == "extracting" ? VideoBatchPage::tr("Reading clips")
                                      : stage == "audio"    ? VideoBatchPage::tr("Comparing audio")
                                                            : VideoBatchPage::tr("Comparing");
                emit progress(done, total, label);
                log(ComparisonLogLevel::Detail,
                    VideoBatchPage::tr("%1: %2/%3").arg(label).arg(done).arg(total));
            });

        VideoBatchOutcome outcome;
        outcome.clipCount = static_cast<int>(result.items.size());
        outcome.failedCount = result.failed;
        outcome.extractMs = static_cast<qint64>(result.extractMs);
        outcome.compareMs = static_cast<qint64>(result.compareMs);
        outcome.pairsCompared = result.pairsCompared;
        const int n = outcome.clipCount;
        outcome.pairsPossible = n > 1 ? n * (n - 1) / 2 : 0;

        log(ComparisonLogLevel::Info,
            VideoBatchPage::tr("Signature prefilter: %1 of %2 possible pairs passed "
                               "(threshold %3)")
                .arg(result.pairCandidates)
                .arg(outcome.pairsPossible)
                .arg(vividmatch::kDefaultSignatureThreshold, 0, 'f', 2));
        log(ComparisonLogLevel::Info,
            VideoBatchPage::tr("Fingerprint extraction finished in %1; comparison took %2")
                .arg(formatutils::durationValue(static_cast<qint64>(result.extractMs)))
                .arg(formatutils::durationValue(static_cast<qint64>(result.compareMs))));
        log(ComparisonLogLevel::Detail,
            VideoBatchPage::tr("Extraction runs on %1 worker threads, so per-clip times "
                               "overlap.")
                .arg(vividmatch::kDefaultBatchWorkers));

        // Per clip: what was read, how long it took, and how many pairs the
        // signature prefilter discarded because of it.
        for (int index = 0; index < static_cast<int>(result.items.size()); ++index) {
            const vividmatch::VideoBatchItem& item = result.items[static_cast<std::size_t>(index)];
            const QString name = QFileInfo(QString::fromStdString(item.path)).fileName();
            if (!item.ok) {
                const QString message =
                    VideoBatchPage::tr("Clip %1 could not be read: %2 (%3)")
                        .arg(index + 1)
                        .arg(name)
                        .arg(QString::fromStdString(item.error));
                log(ComparisonLogLevel::Error, message);
                outcome.failures << message;
                continue;
            }

            log(ComparisonLogLevel::Detail,
                VideoBatchPage::tr("Clip %1/%2: %3 - %4x%5, %6 fps, %7 s, %8 sampled "
                                   "frames, fingerprint %9, %10 signature rejections")
                    .arg(index + 1)
                    .arg(static_cast<int>(result.items.size()))
                    .arg(name)
                    .arg(item.fingerprint.width)
                    .arg(item.fingerprint.height)
                    .arg(item.fingerprint.fps, 0, 'f', 2)
                    .arg(item.fingerprint.duration, 0, 'f', 2)
                    .arg(static_cast<int>(item.fingerprint.frames.size()))
                    .arg(formatutils::durationValue(
                        static_cast<qint64>(item.fingerprint.elapsedMs)))
                    .arg(result.signatureRejects[static_cast<std::size_t>(index)]));
        }

        // Per candidate pair: the compact numbers the batch layer copied out.
        int loggedPairs = 0;
        for (const vividmatch::VideoBatchPair& pair : result.pairs) {
            if (loggedPairs >= kMaxPairLogLines) {
                break;
            }
            const QString leftName =
                QFileInfo(QString::fromStdString(
                              result.items[static_cast<std::size_t>(pair.left)].path))
                    .fileName();
            const QString rightName =
                QFileInfo(QString::fromStdString(
                              result.items[static_cast<std::size_t>(pair.right)].path))
                    .fileName();
            log(pair.counts ? ComparisonLogLevel::Success : ComparisonLogLevel::Detail,
                VideoBatchPage::tr("Pair %1-%2 (%3 vs %4): signature %5%, verdict %6, "
                                   "coverage %7%, span %8%, chain %9%, mean %10%")
                    .arg(pair.left + 1)
                    .arg(pair.right + 1)
                    .arg(leftName)
                    .arg(rightName)
                    .arg(pair.signature * 100.0, 0, 'f', 1)
                    .arg(verdictText(pair.verdict))
                    .arg(pair.coverage * 100.0, 0, 'f', 1)
                    .arg(pair.spanCoverage * 100.0, 0, 'f', 1)
                    .arg(pair.runRatio * 100.0, 0, 'f', 1)
                    .arg(pair.meanScore * 100.0, 0, 'f', 1));

            if (pair.audio.available) {
                log(ComparisonLogLevel::Detail,
                    VideoBatchPage::tr("  audio: %1 aligned seconds, similarity %2% "
                                       "(threshold %3%) -> %4")
                        .arg(pair.audio.comparedSeconds)
                        .arg(pair.audio.meanSimilarity * 100.0, 0, 'f', 1)
                        .arg(pair.audio.threshold * 100.0, 0, 'f', 1)
                        .arg(pair.audio.sameSoundtrack
                                 ? VideoBatchPage::tr("the same soundtrack")
                                 : VideoBatchPage::tr("different soundtracks")));
            } else if (!pair.audio.error.empty()) {
                log(ComparisonLogLevel::Detail,
                    VideoBatchPage::tr("  audio: not used (%1)")
                        .arg(QString::fromStdString(pair.audio.error)));
            }

            ++loggedPairs;
            if (loggedPairs == kMaxPairLogLines) {
                log(ComparisonLogLevel::Warning,
                    VideoBatchPage::tr("Per-pair detail stopped after %1 pairs; the "
                                       "remaining pairs are still compared.")
                        .arg(kMaxPairLogLines));
            }
        }

        int duplicateGroups = 0;
        for (int index = 0; index < static_cast<int>(result.groups.size()); ++index) {
            const vividmatch::VideoBatchGroup& group =
                result.groups[static_cast<std::size_t>(index)];
            QStringList names;
            for (const int member : group.members) {
                names << QFileInfo(QString::fromStdString(
                                       result.items[static_cast<std::size_t>(member)].path))
                             .fileName();
            }
            if (group.duplicate) {
                ++duplicateGroups;
            }

            QString detail =
                VideoBatchPage::tr("%1 clips, similarity %2%")
                    .arg(static_cast<int>(group.members.size()))
                    .arg(group.similarity * 100.0, 0, 'f', 1);
            if (group.audioDiffers) {
                detail += VideoBatchPage::tr(", different audio");
            }
            if (group.preferred >= 0) {
                detail += VideoBatchPage::tr(", keep %1")
                              .arg(QFileInfo(QString::fromStdString(
                                                 result.items[static_cast<std::size_t>(
                                                     group.preferred)]
                                                     .path))
                                       .fileName());
            }

            const QString groupName =
                group.duplicate
                    ? VideoBatchPage::tr("Duplicate group %1").arg(index + 1)
                    : group.montageOnly
                          ? VideoBatchPage::tr("Possibly spliced clip %1").arg(index + 1)
                          : VideoBatchPage::tr("Single clip %1").arg(index + 1);
            log(group.duplicate ? ComparisonLogLevel::Success
                                : ComparisonLogLevel::Detail,
                VideoBatchPage::tr("%1: %2 - %3")
                    .arg(groupName)
                    .arg(names.join(QStringLiteral(", ")))
                    .arg(detail));
        }

        log(ComparisonLogLevel::Info,
            VideoBatchPage::tr("Batch video comparison finished: %1 clips, %2 duplicate "
                               "groups, %3 pairs compared in %4")
                .arg(outcome.clipCount)
                .arg(duplicateGroups)
                .arg(outcome.pairsCompared)
                .arg(formatutils::durationValue(outcome.extractMs + outcome.compareMs)));

        for (const vividmatch::VideoBatchGroup& group : result.groups) {
            VideoBatchGroupInfo info;
            info.duplicate = group.duplicate;
            info.montage = group.montageOnly;
            info.audioDiffers = group.audioDiffers;
            info.similarity = group.similarity;
            for (const int member : group.members) {
                const vividmatch::VideoBatchItem& item =
                    result.items[static_cast<std::size_t>(member)];
                const QString path = QString::fromStdString(item.path);
                const QFileInfo file(path);

                VideoBatchRow row;
                row.row = member;
                row.path = path;
                row.name = file.fileName();
                row.width = item.fingerprint.width;
                row.height = item.fingerprint.height;
                row.resolution = QStringLiteral("%1x%2").arg(row.width).arg(row.height);
                row.duration = item.fingerprint.duration;
                row.durationText = formatDuration(row.duration);
                row.fileSize = file.size();
                row.sizeText = humanSize(row.fileSize);
                row.modified = file.lastModified();
                row.modifiedText = row.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                row.preferred = member == group.preferred;
                info.rows.append(row);
            }
            // Largest first inside a group, so the likely keeper reads first.
            std::sort(info.rows.begin(), info.rows.end(),
                      [](const VideoBatchRow& left, const VideoBatchRow& right) {
                          return static_cast<long long>(left.width) * left.height
                                 > static_cast<long long>(right.width) * right.height;
                      });
            info.label = groupLabel(info);
            outcome.groups.append(info);
        }

        emit finished(outcome);
    } catch (const std::exception& error) {
        const QString message = QString::fromUtf8(error.what());
        log(ComparisonLogLevel::Error,
            VideoBatchPage::tr("Batch video comparison failed: %1").arg(message));
        emit failed(message);
    }
}

// --- page ------------------------------------------------------------------

VideoBatchPage::VideoBatchPage(QWidget* parent)
    : QWidget(parent)
    , m_tree(new QTreeWidget(this))
    , m_policyButton(nullptr)
    , m_search(new QLineEdit(this))
    , m_progress(new QProgressBar(this))
    , m_compareButton(nullptr)
    , m_clearButton(nullptr)
    , m_invertButton(nullptr)
    , m_selectAllButton(nullptr)
    , m_removeButton(nullptr)
    , m_deleteButton(nullptr)
    , m_status(new QLabel(this))
    , m_log(new ComparisonLogWidget(this))
    , m_delegate(new VideoBatchItemDelegate(this))
    , m_backButton(nullptr)
    , m_titleLabel(nullptr)
    , m_addFolderButton(nullptr)
    , m_addVideosButton(nullptr)
    , m_policyLabel(nullptr)
    , m_searchLabel(nullptr)
    , m_captureLabel(nullptr)
    , m_regrabButton(nullptr)
    , m_nextRecordId(1)
    , m_lastClipCount(0)
    , m_lastDuplicateGroups(0)
    , m_lastFailedCount(0)
    , m_lastTotalMs(0)
    , m_worker(nullptr)
    , m_busy(false)
{
    qRegisterMetaType<VideoBatchOutcome>("VideoBatchOutcome");

    setAcceptDrops(true);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 16, 24, 16);
    root->setSpacing(10);

    QHBoxLayout* header = new QHBoxLayout;
    m_backButton = new QPushButton(this);
    m_backButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_backButton, &QPushButton::released, this, &VideoBatchPage::backRequested);
    m_titleLabel = new QLabel(this);
    m_titleLabel->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;color:#1f2937;"));
    header->addWidget(m_backButton);
    header->addSpacing(10);
    header->addWidget(m_titleLabel);
    header->addStretch(1);
    root->addLayout(header);

    // Controls mirror the batch image page: add, then act on the checked rows,
    // with the primary action on the right next to the keep policy.
    m_addFolderButton = new QPushButton(this);
    m_addFolderButton->setObjectName(QStringLiteral("secondaryButton"));
    m_addVideosButton = new QPushButton(this);
    m_addVideosButton->setObjectName(QStringLiteral("secondaryButton"));
    m_selectAllButton = new QPushButton(this);
    m_selectAllButton->setObjectName(QStringLiteral("secondaryButton"));
    m_invertButton = new QPushButton(this);
    m_invertButton->setObjectName(QStringLiteral("secondaryButton"));
    m_removeButton = new QPushButton(this);
    m_removeButton->setObjectName(QStringLiteral("secondaryButton"));
    m_deleteButton = new QPushButton(this);
    m_deleteButton->setObjectName(QStringLiteral("secondaryButton"));
    m_clearButton = new QPushButton(this);
    m_clearButton->setObjectName(QStringLiteral("secondaryButton"));
    m_compareButton = new QPushButton(this);
    m_compareButton->setObjectName(QStringLiteral("primaryButton"));

    connect(m_addFolderButton, &QPushButton::released, this, &VideoBatchPage::chooseFolder);
    connect(m_addVideosButton, &QPushButton::released, this, &VideoBatchPage::chooseVideos);
    connect(m_selectAllButton, &QPushButton::released, this, &VideoBatchPage::selectAllChecked);
    connect(m_invertButton, &QPushButton::released, this, &VideoBatchPage::invertChecked);
    connect(m_removeButton, &QPushButton::released, this, &VideoBatchPage::removeChecked);
    connect(m_deleteButton, &QPushButton::released, this, &VideoBatchPage::deleteCheckedFiles);
    connect(m_clearButton, &QPushButton::released, this, &VideoBatchPage::clearList);
    connect(m_compareButton, &QPushButton::released, this, &VideoBatchPage::startCompare);

    QHBoxLayout* controls = new QHBoxLayout;
    controls->addWidget(m_addFolderButton);
    controls->addWidget(m_addVideosButton);
    controls->addWidget(m_selectAllButton);
    controls->addWidget(m_invertButton);
    controls->addSpacing(8);
    controls->addWidget(m_removeButton);
    controls->addWidget(m_deleteButton);
    controls->addWidget(m_clearButton);
    controls->addStretch(1);
    controls->addWidget(m_compareButton);
    controls->addSpacing(12);
    m_policyLabel = new QLabel(this);
    controls->addWidget(m_policyLabel);
    m_policyButton = new QPushButton(this);
    m_policyButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_policyButton, &QPushButton::released, this,
            &VideoBatchPage::editKeepPolicy);
    controls->addWidget(m_policyButton);
    root->addLayout(controls);

    QSettings settings;
    m_keepRules = decodeKeepRules(settings.value(QStringLiteral("batchVideo/keepRules"))
                                      .toStringList(),
                                  false);
    m_keepAll = settings.value(QStringLiteral("batchVideo/keepAll"), false).toBool();
    updatePolicyButton();

    QHBoxLayout* searchRow = new QHBoxLayout;
    m_searchLabel = new QLabel(this);
    searchRow->addWidget(m_searchLabel);
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(280);
    m_search->hide();
    searchRow->addWidget(m_search);
    searchRow->addSpacing(18);
    // Where the preview frames are taken from. A spin box plus an explicit
    // re-grab button rather than a live re-grab: grabbing a frame means seeking
    // and decoding, so re-running it on every keystroke would be wasteful.
    m_captureLabel = new QLabel(this);
    searchRow->addWidget(m_captureLabel);
    m_capturePosition = new QSpinBox(this);
    m_capturePosition->setRange(0, 99);
    m_capturePosition->setValue(kDefaultCapturePercent);
    m_capturePosition->setMaximumWidth(84);
    searchRow->addWidget(m_capturePosition);
    m_regrabButton = new QPushButton(this);
    m_regrabButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_regrabButton, &QPushButton::released, this, &VideoBatchPage::regrabThumbnails);
    searchRow->addWidget(m_regrabButton);
    searchRow->addSpacing(18);
    searchRow->addWidget(m_progress, 1);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    root->addLayout(searchRow);
    connect(m_search, &QLineEdit::textChanged, this, &VideoBatchPage::searchChanged);

    m_tree->setColumnCount(ColumnCount);
    m_tree->setRootIsDecorated(true);
    m_tree->setItemsExpandable(true);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setIndentation(6);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setUniformRowHeights(false);
    m_tree->setItemDelegate(m_delegate);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setMinimumSectionSize(24);
    m_tree->header()->resizeSection(CheckColumn, 52);
    m_tree->header()->resizeSection(PreviewColumn, 160);
    m_tree->header()->resizeSection(NameColumn, 380);
    m_tree->header()->resizeSection(ResolutionColumn, 100);
    m_tree->header()->resizeSection(DurationColumn, 80);
    m_tree->header()->resizeSection(SizeColumn, 100);
    m_delegate->setPreviewColumnWidth(m_tree->header()->sectionSize(PreviewColumn));

    connect(m_tree->header(), &QHeaderView::sectionResized, this,
            [this](int index, int, int newSize) {
                if (index != PreviewColumn) {
                    return;
                }
                m_delegate->setPreviewColumnWidth(newSize);
                // The rows were laid out at the old preview size, and a resize
                // alone does not re-measure them, so ask for a fresh layout: that
                // is what makes the picture and the row height follow the edge.
                m_tree->doItemsLayout();
            });

    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &VideoBatchPage::showTreeContextMenu);
    connect(m_tree, &QTreeWidget::itemChanged, this, &VideoBatchPage::onItemChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &VideoBatchPage::onItemDoubleClicked);

    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->installEventFilter(this);
    m_tree->viewport()->installEventFilter(this);
    root->addWidget(m_tree, 1);
    root->addWidget(m_log);

    m_status->setStyleSheet(QStringLiteral("color:#64748b;"));
    root->addWidget(m_status);

    QShortcut* findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this]() {
        m_search->show();
        m_search->setFocus();
        m_search->selectAll();
    });

    connect(this, &VideoBatchPage::thumbnailReady, this, &VideoBatchPage::applyThumbnail,
            Qt::QueuedConnection);

    retranslate();
    setBusy(false);
    updateStatus();
}

void VideoBatchPage::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void VideoBatchPage::retranslate()
{
    m_backButton->setText(tr("Back to mode selection"));
    m_titleLabel->setText(tr("Batch video comparison"));
    m_addFolderButton->setText(tr("Choose folder"));
    m_addVideosButton->setText(tr("Choose videos"));
    m_selectAllButton->setText(tr("Select all"));
    m_invertButton->setText(tr("Invert"));
    m_removeButton->setText(tr("Remove checked"));
    m_deleteButton->setText(tr("Move to Recycle Bin"));
    m_clearButton->setText(tr("Clear list"));
    m_compareButton->setText(tr("Start comparison"));
    m_policyLabel->setText(tr("Keep policy"));
    m_policyButton->setText(tr("Edit priorities..."));
    updatePolicyButton();
    m_searchLabel->setText(tr("Locate"));
    m_search->setPlaceholderText(tr("Ctrl+F to search name or path"));
    m_captureLabel->setText(tr("Frame position"));
    m_capturePosition->setSuffix(tr(" %"));
    m_capturePosition->setToolTip(
        tr("How far into each clip the thumbnail is taken, as a percentage of its "
           "length. The default is 50%, the middle.\n"
           "0% is the first frame. Changing the value applies on \"Re-grab frames\"."));
    m_regrabButton->setText(tr("Re-grab frames"));
    m_regrabButton->setToolTip(tr("Grab every thumbnail again at the current frame "
                                  "position"));

    const QStringList headers = {tr("Selected"), tr("Frame"), tr("Name"), tr("Resolution"),
                                 tr("Duration"), tr("Size"), tr("Modified")};
    if (m_tree->headerItem() == nullptr) {
        m_tree->setHeaderLabels(headers);
    } else {
        for (int i = 0; i < headers.size() && i < m_tree->columnCount(); ++i) {
            m_tree->headerItem()->setText(i, headers.at(i));
        }
    }
    m_tree->header()->setToolTip(
        tr("Check the clips to keep; a comparison checks them according to the keep "
           "policy.\n"
           "Dragging the Frame column wider scales the thumbnails and the row heights "
           "with it.\n"
           "Right-click a clip or a group for its menu."));

    // Group headings and the status line come from the current state, so they are
    // regenerated rather than stored.
    refreshGroupLabels();
    updateStatus();
    if (m_lastClipCount > 0) {
        m_progress->setFormat(tr("Comparison finished - %1 clips - %2")
                                  .arg(m_lastClipCount)
                                  .arg(formatutils::durationLabel(m_lastTotalMs)));
    } else if (!m_busy && m_progress->value() == m_progress->minimum()) {
        m_progress->setFormat(tr("Waiting to compare"));
    }
}

void VideoBatchPage::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Choose a video folder"));
    if (folder.isEmpty()) {
        return;
    }
    QStringList paths;
    QDirIterator iterator(folder, videoWildcards(), QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        paths.append(iterator.next());
    }
    addPaths(paths);
}

void VideoBatchPage::chooseVideos()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Choose videos"), QString(),
        tr("Videos (*.mp4 *.mov *.mkv *.avi *.webm *.m4v);;All files (*)"));
    addPaths(paths);
}

int VideoBatchPage::capturePositionPercent() const
{
    return m_capturePosition != nullptr ? m_capturePosition->value()
                                        : kDefaultCapturePercent;
}

void VideoBatchPage::requestThumbnails()
{
    // Grabs run on the global pool; each result arrives on its own signal rather
    // than blocking, so a folder of clips fills in progressively.
    const int percent = capturePositionPercent();
    for (const RowData& row : m_records) {
        if (!row.thumbnail.isNull() || m_pendingThumbnails.contains(row.path)) {
            continue;
        }
        m_pendingThumbnails.insert(row.path);
        QThreadPool::globalInstance()->start(new ThumbnailTask(row.path, percent, this));
    }
}

void VideoBatchPage::regrabThumbnails()
{
    if (m_records.isEmpty()) {
        QMessageBox::information(this, tr("Re-grab frames"), tr("Add some videos first."));
        return;
    }
    // Drop what is stored so requestThumbnails() picks every clip up again. A
    // grab that is still in flight for a clip will land and be kept, which is
    // harmless: it is a frame from the same position.
    //
    // The measured size and duration are kept: they describe the clip, not the
    // position the frame was taken from.
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        it.value().thumbnail = QImage();
    }
    for (QTreeWidgetItem* item : allRowItems()) {
        item->setData(PreviewColumn, kThumbnailImageRole, QVariant());
    }
    m_tree->doItemsLayout();
    requestThumbnails();
    m_progress->setFormat(
        tr("Grabbing thumbnails again at %1% of each clip").arg(capturePositionPercent()));
}

void VideoBatchPage::applyThumbnail(const QString& path, const QImage& image, int width,
                                    int height, double duration)
{
    m_pendingThumbnails.remove(path);

    const int recordId = recordIdForPath(path);
    if (recordId > 0) {
        RowData& row = m_records[recordId];
        // The probe opened the clip, so the real resolution and duration are known
        // now. Filling them here is what stops those two columns sitting on a dash
        // until a comparison has run.
        if (!row.measured && width > 0 && height > 0) {
            row.measured = true;
            row.width = width;
            row.height = height;
            row.duration = duration;
        }
        if (!image.isNull()) {
            row.thumbnail = image;
        }
    }
    if (image.isNull() && width <= 0) {
        // Nothing could be read; a rebuild would only fail the same way.
        return;
    }

    // Refresh every row showing this clip, then let the tree work out the new row
    // heights. Rebuilding the rows is simpler and safer than patching them in
    // place, and it is what makes the resolution and duration text appear too.
    const RowData* record = recordId > 0 ? &m_records[recordId] : nullptr;
    for (QTreeWidgetItem* item : allRowItems()) {
        const RowData* row = recordForItem(item);
        if (row == nullptr || record == nullptr
            || QString::compare(row->path, path, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!image.isNull()) {
            item->setData(PreviewColumn, kThumbnailImageRole, image);
        }
        if (record->measured) {
            item->setText(ResolutionColumn,
                          QStringLiteral("%1x%2").arg(record->width).arg(record->height));
            item->setText(DurationColumn, formatDuration(record->duration));
        }
    }
    // The delegate sizes rows from the stored image, so the layout has to be redone
    // for the row to grow. Asking the view to re-evaluate the hint alone is not
    // enough for rows already laid out.
    m_tree->doItemsLayout();
}

void VideoBatchPage::updateCompareButton()
{
    // The button follows the ticks, not the list length: the ticks are what a run
    // would compare, so after a comparison they mark one keeper per group and
    // "开始比对" is offered only when there is really something to compare.
    // Unchecking everything therefore disables it rather than silently comparing
    // the whole list again.
    const int checked = checkedItems().size();
    const bool ready = !m_busy && checked >= 2;
    m_compareButton->setEnabled(ready);
    m_compareButton->setToolTip(
        ready ? tr("Compare the %1 checked clips").arg(checked)
              : tr("Check at least 2 clips to start a comparison"));
    // The hint only replaces a waiting prompt, never a result or a running stage.
    if (!m_busy && checked < 2 && m_lastClipCount == 0
        && m_progress->value() == m_progress->minimum()) {
        m_progress->setFormat(tr("%1 checked, at least 2 needed").arg(checked));
    }
}

void VideoBatchPage::addPaths(const QStringList& paths)
{
    int added = 0;
    for (const QString& path : paths) {
        if (!isSupportedVideoFile(path)) {
            continue;
        }
        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty()) {
            continue;
        }
        bool known = false;
        for (const RowData& row : m_records) {
            if (QString::compare(row.path, canonical, Qt::CaseInsensitive) == 0) {
                known = true;
                break;
            }
        }
        if (known) {
            continue;
        }

        RowData row;
        row.id = m_nextRecordId++;
        row.path = canonical;
        row.name = info.fileName();
        row.fileSize = info.size();
        row.modified = info.lastModified();
        row.modifiedText = row.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        m_records.insert(row.id, row);
        ++added;
    }

    if (added > 0) {
        // Show the new clips immediately. Waiting for a comparison to display
        // them leaves the list looking as if the add did nothing.
        rebuildPendingTree();
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
        m_progress->setFormat(tr("Added %1 clips, waiting to compare").arg(added));
    }
    updateStatus();
}

QTreeWidgetItem* VideoBatchPage::createRowItem(int recordId) const
{
    const auto iterator = m_records.constFind(recordId);
    if (iterator == m_records.constEnd()) {
        return nullptr;
    }
    const RowData& row = iterator.value();

    QTreeWidgetItem* item = new QTreeWidgetItem;
    item->setData(CheckColumn, kRecordIdRole, recordId);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(CheckColumn, Qt::Checked);
    // The preview lives on the record, so every rebuilt row gets its picture back
    // instead of only the rows that happened to be on screen when it was grabbed.
    if (!row.thumbnail.isNull()) {
        item->setData(PreviewColumn, kThumbnailImageRole, row.thumbnail);
    }
    item->setText(NameColumn, row.name);
    item->setText(ResolutionColumn, row.measured
                                        ? QStringLiteral("%1x%2").arg(row.width).arg(row.height)
                                        : QStringLiteral("—"));
    item->setText(DurationColumn,
                  row.measured ? formatDuration(row.duration) : QStringLiteral("—"));
    item->setText(SizeColumn, humanSize(row.fileSize));
    item->setText(ModifiedColumn, row.modifiedText);
    item->setToolTip(NameColumn, row.path);
    item->setToolTip(ResolutionColumn, row.path);
    return item;
}

QTreeWidgetItem* VideoBatchPage::ensurePendingGroup()
{
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* group = m_tree->topLevelItem(i);
        if (group->data(CheckColumn, kRecordIdRole).toInt() == kPendingMarker) {
            return group;
        }
    }

    QTreeWidgetItem* group = new QTreeWidgetItem(m_tree);
    group->setFirstColumnSpanned(true);
    group->setFlags(Qt::ItemIsEnabled);
    group->setData(CheckColumn, kRecordIdRole, kPendingMarker);
    group->setData(CheckColumn, kGroupBaseRole, tr("Waiting to compare"));
    group->setText(CheckColumn, tr("Waiting to compare"));
    group->setExpanded(true);
    group->setBackground(CheckColumn, QBrush(QColor(232, 237, 243)));
    group->setForeground(CheckColumn, QBrush(QColor(51, 65, 85)));
    QFont font = group->font(CheckColumn);
    font.setBold(true);
    group->setFont(CheckColumn, font);
    return group;
}

void VideoBatchPage::rebuildPendingTree()
{
    // Result groups stay put; only the clips that are not shown yet are (re)built.
    // That way adding a clip to an already compared list extends the list instead
    // of wiping the results.
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        if (m_tree->topLevelItem(i)->data(CheckColumn, kRecordIdRole).toInt()
            == kPendingMarker) {
            delete m_tree->takeTopLevelItem(i);
        }
    }

    // Rows are added newest last, so walk the records in insertion order.
    QVector<int> ids;
    ids.reserve(m_records.size());
    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
        ids.append(it.key());
    }
    std::sort(ids.begin(), ids.end());

    QVector<int> missing;
    for (const int id : ids) {
        if (!m_listedPaths.contains(m_records.value(id).path)) {
            missing.append(id);
        }
    }
    if (missing.isEmpty()) {
        updateStatus();
        return;
    }

    // `m_listedPaths` is empty until a comparison has run, so this is the first
    // view of the list and the clips are ticked ready to compare.
    const bool firstView = m_listedPaths.isEmpty();
    QTreeWidgetItem* group = ensurePendingGroup();
    group->setData(CheckColumn, kGroupBaseRole,
                   firstView ? tr("Waiting to compare") : tr("Newly added"));
    QTreeWidgetItem* first = nullptr;
    for (const int id : missing) {
        QTreeWidgetItem* item = createRowItem(id);
        if (item == nullptr) {
            continue;
        }
        group->addChild(item);
        item->setCheckState(CheckColumn, firstView ? Qt::Checked : Qt::Unchecked);
        if (first == nullptr) {
            first = item;
        }
    }
    refreshGroupLabels();
    applyPolicyToChecks();
    if (first != nullptr) {
        m_tree->scrollToItem(first);
    }
    // Start grabbing preview frames for the clips just added.
    requestThumbnails();
    updateStatus();
}

void VideoBatchPage::rebuildTree(const VideoBatchOutcome& outcome)
{
    m_tree->clear();
    m_listedPaths.clear();

    // Copy the measured values back into the records first, so a later rebuild
    // of the list shows the same numbers the result rows showed.
    for (const VideoBatchGroupInfo& group : outcome.groups) {
        for (const VideoBatchRow& row : group.rows) {
            for (auto it = m_records.begin(); it != m_records.end(); ++it) {
                if (QString::compare(it.value().path, row.path, Qt::CaseInsensitive) == 0) {
                    it.value().measured = true;
                    it.value().width = row.width;
                    it.value().height = row.height;
                    it.value().duration = row.duration;
                    break;
                }
            }
        }
    }

    const auto addGroup = [this](const QString& label, bool duplicate,
                                 const QVector<VideoBatchRow>& rows) {
        QTreeWidgetItem* groupItem = new QTreeWidgetItem(m_tree);
        groupItem->setFirstColumnSpanned(true);
        groupItem->setFlags(Qt::ItemIsEnabled);
        groupItem->setData(CheckColumn, kGroupBaseRole, label);
        groupItem->setData(CheckColumn, kDuplicateGroupRole, duplicate);
        groupItem->setText(CheckColumn, label);
        groupItem->setExpanded(true);
        groupItem->setBackground(CheckColumn, QBrush(QColor(232, 237, 243)));
        groupItem->setForeground(CheckColumn, QBrush(QColor(51, 65, 85)));
        QFont font = groupItem->font(CheckColumn);
        font.setBold(true);
        groupItem->setFont(CheckColumn, font);

        for (const VideoBatchRow& row : rows) {
            const int recordId = recordIdForPath(row.path);
            if (recordId < 0) {
                continue;
            }
            if (QTreeWidgetItem* child = createRowItem(recordId)) {
                groupItem->addChild(child);
            }
        }
    };

    // One group for every clip that matched nothing; a long folder would
    // otherwise show dozens of one-clip groups. Montage-only clips keep their
    // separate heading because they did overlap, just not on one timeline.
    QVector<VideoBatchRow> unmatched;
    for (const VideoBatchGroupInfo& group : outcome.groups) {
        if (!group.duplicate && !group.montage) {
            unmatched += group.rows;
            continue;
        }
        addGroup(group.label, group.duplicate, group.rows);
    }
    if (!unmatched.isEmpty()) {
        std::sort(unmatched.begin(), unmatched.end(),
                  [](const VideoBatchRow& left, const VideoBatchRow& right) {
                      return static_cast<long long>(left.width) * left.height
                             > static_cast<long long>(right.width) * right.height;
                  });
        addGroup(tr("No duplicates - %1 clips").arg(unmatched.size()), false, unmatched);
    }

    // Clips that took no part in this run (unchecked, or added afterwards) are
    // still in the list, so they are shown as waiting rather than dropped: the
    // user asked for them to be there.
    QVector<int> notCompared;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        for (int j = 0; j < m_tree->topLevelItem(i)->childCount(); ++j) {
            m_listedPaths.append(
                recordForItem(m_tree->topLevelItem(i)->child(j))->path);
        }
    }
    QVector<int> ids;
    ids.reserve(m_records.size());
    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
        ids.append(it.key());
    }
    std::sort(ids.begin(), ids.end());
    for (const int id : ids) {
        if (!m_listedPaths.contains(m_records.value(id).path)) {
            notCompared.append(id);
        }
    }
    if (!notCompared.isEmpty()) {
        QTreeWidgetItem* group = ensurePendingGroup();
        group->setData(CheckColumn, kGroupBaseRole, tr("Waiting to compare"));
        QTreeWidgetItem* first = nullptr;
        for (const int id : notCompared) {
            if (QTreeWidgetItem* item = createRowItem(id)) {
                group->addChild(item);
                item->setCheckState(CheckColumn, Qt::Unchecked);
                if (first == nullptr) {
                    first = item;
                }
            }
        }
        if (first != nullptr) {
            m_tree->scrollToItem(first);
        }
    }

    refreshGroupLabels();
    applyPolicyToChecks();
}

void VideoBatchPage::refreshGroupLabels()
{
    // Only the pending group is renamed here: its heading is "waiting" or "newly
    // added" plus a live count. Result groups carry a label built from the verdict
    // and the similarity, which the count says nothing about, so they are set once
    // in rebuildTree and left alone.
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* group = m_tree->topLevelItem(i);
        if (group->data(CheckColumn, kRecordIdRole).toInt() != kPendingMarker) {
            continue;
        }
        const QString base = group->data(CheckColumn, kGroupBaseRole).toString();
        if (base.isEmpty()) {
            continue;
        }
        group->setText(CheckColumn, tr("%1 - %2 clips").arg(base).arg(group->childCount()));
    }
}

void VideoBatchPage::applyPolicyToChecks()
{
    // In the compared view, tick what the policy would keep and untick the rest:
    // the ticks are then a preview of what "移出勾选项" would leave behind.
    // Clips that have not been compared cannot be ranked, so their ticks are left
    // exactly as the user set them.
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* group = m_tree->topLevelItem(i);
        if (group->data(CheckColumn, kRecordIdRole).toInt() == kPendingMarker) {
            continue;
        }
        // Only duplicate groups have a choice to make. Unmatched and montage
        // groups keep every row checked; otherwise one of them would be unchecked.
        if (!group->data(CheckColumn, kDuplicateGroupRole).toBool()) {
            continue;
        }
        if (group->childCount() == 0) {
            continue;
        }
        if (m_keepAll) {
            for (int j = 0; j < group->childCount(); ++j) {
                group->child(j)->setCheckState(CheckColumn, Qt::Checked);
            }
            continue;
        }

        const auto better = [this](const RowData& left, const RowData& right) {
            for (const KeepRule& rule : m_keepRules) {
                const qint64 leftValue = ruleValue(left, rule.criterion);
                const qint64 rightValue = ruleValue(right, rule.criterion);
                if (leftValue == rightValue) {
                    continue;
                }
                return rule.direction == KeepDirection::Higher ? leftValue > rightValue
                                                               : leftValue < rightValue;
            }
            return false;
        };

        int best = -1;
        for (int j = 0; j < group->childCount(); ++j) {
            const RowData* row = recordForItem(group->child(j));
            if (row == nullptr) {
                continue;
            }
            if (best < 0 || better(*row, *recordForItem(group->child(best)))) {
                best = j;
            }
        }
        for (int j = 0; j < group->childCount(); ++j) {
            group->child(j)->setCheckState(CheckColumn,
                                           j == best ? Qt::Checked : Qt::Unchecked);
        }
    }
    updateStatus();
}

qint64 VideoBatchPage::ruleValue(const RowData& row, KeepCriterion criterion) const
{
    switch (criterion) {
        case KeepCriterion::Resolution:
            return static_cast<qint64>(row.width) * row.height;
        case KeepCriterion::FileSize:
            return row.fileSize;
        case KeepCriterion::Modified:
            return row.modified.toMSecsSinceEpoch();
        case KeepCriterion::BitDepth:
            return 0;
    }
    return 0;
}

void VideoBatchPage::editKeepPolicy()
{
    KeepPolicyDialog dialog(m_keepRules, false, m_keepAll, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_keepRules = dialog.rules();
    m_keepAll = dialog.keepAll();

    QSettings settings;
    settings.setValue(QStringLiteral("batchVideo/keepRules"), encodeKeepRules(m_keepRules));
    settings.setValue(QStringLiteral("batchVideo/keepAll"), m_keepAll);

    updatePolicyButton();
    applyPolicyToChecks();
    updateStatus();
}

void VideoBatchPage::updatePolicyButton()
{
    m_policyButton->setToolTip(keepPrioritySummary(m_keepRules, m_keepAll));
}

QVector<QTreeWidgetItem*> VideoBatchPage::allRowItems() const
{
    QVector<QTreeWidgetItem*> items;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* group = m_tree->topLevelItem(i);
        for (int j = 0; j < group->childCount(); ++j) {
            items.append(group->child(j));
        }
    }
    return items;
}

QVector<QTreeWidgetItem*> VideoBatchPage::checkedItems() const
{
    QVector<QTreeWidgetItem*> items;
    for (QTreeWidgetItem* item : allRowItems()) {
        if (item->checkState(CheckColumn) == Qt::Checked) {
            items.append(item);
        }
    }
    return items;
}

QVector<QTreeWidgetItem*> VideoBatchPage::selectedRowItems() const
{
    QVector<QTreeWidgetItem*> items;
    for (QTreeWidgetItem* item : m_tree->selectedItems()) {
        if (recordForItem(item) != nullptr) {
            items.append(item);
        }
    }
    return items;
}

QVector<QTreeWidgetItem*> VideoBatchPage::contextTargetItems(QTreeWidgetItem* clicked) const
{
    QVector<QTreeWidgetItem*> items = selectedRowItems();
    if (!items.isEmpty()) {
        return items;
    }
    items = checkedItems();
    if (!items.isEmpty()) {
        return items;
    }
    if (clicked != nullptr && recordForItem(clicked) != nullptr) {
        items.append(clicked);
    }
    return items;
}

const VideoBatchPage::RowData* VideoBatchPage::recordForItem(const QTreeWidgetItem* item) const
{
    if (item == nullptr) {
        return nullptr;
    }
    const int id = item->data(CheckColumn, kRecordIdRole).toInt();
    const auto iterator = m_records.constFind(id);
    return iterator == m_records.constEnd() ? nullptr : &iterator.value();
}

VideoBatchPage::RowData* VideoBatchPage::recordForItem(QTreeWidgetItem* item)
{
    return const_cast<RowData*>(static_cast<const VideoBatchPage*>(this)->recordForItem(item));
}

int VideoBatchPage::recordIdForPath(const QString& path) const
{
    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
        if (QString::compare(it.value().path, path, Qt::CaseInsensitive) == 0) {
            return it.key();
        }
    }
    return -1;
}

void VideoBatchPage::removeItems(const QVector<QTreeWidgetItem*>& items)
{
    for (QTreeWidgetItem* item : items) {
        const int id = item->data(CheckColumn, kRecordIdRole).toInt();
        if (id > 0) {
            m_records.remove(id);
        }
        delete item;
    }
    // A group with no clips left has nothing to show.
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* group = m_tree->topLevelItem(i);
        if (group->childCount() == 0) {
            delete m_tree->takeTopLevelItem(i);
        }
    }
    if (m_records.isEmpty()) {
        m_listedPaths.clear();
        m_lastClipCount = 0;
        m_lastDuplicateGroups = 0;
        m_lastFailedCount = 0;
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
        m_progress->setFormat(tr("Waiting to compare"));
    }
    // Removing the clip that was ticked leaves a group with nothing ticked, which
    // no longer previews the keep policy; re-apply it.
    refreshGroupLabels();
    applyPolicyToChecks();
}

void VideoBatchPage::setItemsChecked(const QVector<QTreeWidgetItem*>& items, bool checked)
{
    for (QTreeWidgetItem* item : items) {
        item->setCheckState(CheckColumn, checked ? Qt::Checked : Qt::Unchecked);
    }
    updateStatus();
}

void VideoBatchPage::deleteFiles(const QVector<QTreeWidgetItem*>& items)
{
    if (items.isEmpty()) {
        QMessageBox::information(
            this, tr("Move to Recycle Bin"),
            tr("Choose or check the videos to move to the Recycle Bin first."));
        return;
    }
    if (QMessageBox::question(
            this, tr("Move to Recycle Bin"),
            tr("Move %1 video files to the Recycle Bin?\n\nThe files can be "
               "restored from there.")
                .arg(items.size()))
        != QMessageBox::Yes) {
        return;
    }

    QVector<QTreeWidgetItem*> moved;
    QStringList errors;
    for (QTreeWidgetItem* item : items) {
        const RowData* row = recordForItem(item);
        if (row == nullptr) {
            continue;
        }
        if (QFile::moveToTrash(row->path)) {
            moved.append(item);
        } else {
            errors.append(row->path);
        }
    }
    removeItems(moved);
    if (!errors.isEmpty()) {
        QMessageBox::warning(
            this, tr("Move to Recycle Bin"),
            tr("These %1 files could not be moved to the Recycle Bin:\n%2")
                .arg(errors.size())
                .arg(errors.join(QLatin1Char('\n'))));
    }
}

void VideoBatchPage::clearList()
{
    if (m_busy) {
        return;
    }
    m_records.clear();
    m_listedPaths.clear();
    m_tree->clear();
    m_lastClipCount = 0;
    m_lastDuplicateGroups = 0;
    m_lastFailedCount = 0;
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("Waiting to compare"));
    updateStatus();
}

void VideoBatchPage::invertChecked()
{
    for (QTreeWidgetItem* item : allRowItems()) {
        item->setCheckState(CheckColumn,
                            item->checkState(CheckColumn) == Qt::Checked ? Qt::Unchecked
                                                                         : Qt::Checked);
    }
    updateStatus();
}

void VideoBatchPage::selectAllChecked()
{
    setItemsChecked(allRowItems(), true);
}

void VideoBatchPage::removeChecked()
{
    removeItems(checkedItems());
}

void VideoBatchPage::deleteCheckedFiles()
{
    deleteFiles(checkedItems());
}

void VideoBatchPage::setBusy(bool busy)
{
    m_busy = busy;
    m_clearButton->setEnabled(!busy);
    m_policyButton->setEnabled(!busy);
    m_selectAllButton->setEnabled(!busy);
    m_invertButton->setEnabled(!busy);
    m_removeButton->setEnabled(!busy);
    m_deleteButton->setEnabled(!busy);
    m_search->setEnabled(!busy);
    // The compare button's enabled state depends on the ticks, so it is decided
    // in one place rather than here.
    updateCompareButton();
}

int VideoBatchPage::unlistedPathCount() const
{
    int count = 0;
    for (const RowData& row : m_records) {
        if (!m_listedPaths.contains(row.path)) {
            ++count;
        }
    }
    return count;
}

void VideoBatchPage::startCompare()
{
    if (m_busy || m_records.size() < 2) {
        return;
    }
    if (m_thread && m_thread->isRunning()) {
        return;
    }

    // Compare the checked clips, so the checkboxes really do select what is
    // compared rather than only what would be kept.
    const QVector<QTreeWidgetItem*> checked = checkedItems();
    const QVector<QTreeWidgetItem*> source = checked.size() >= 2 ? checked : allRowItems();
    QStringList paths;
    for (QTreeWidgetItem* item : source) {
        const RowData* row = recordForItem(item);
        if (row != nullptr) {
            paths.append(row->path);
        }
    }
    if (paths.size() < 2) {
        QMessageBox::information(this, tr("Start comparison"),
                                 tr("At least two videos are needed to compare."));
        return;
    }

    setBusy(true);
    // The pending list stays on screen while the worker runs; it is replaced by
    // the grouped results when they arrive.
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("Preparing..."));
    m_log->clear();
    m_log->info(tr("Keep policy: %1")
                    .arg(keepPrioritySummary(m_keepRules, m_keepAll)));

    // No parent: moveToThread refuses an object that already has one, and the
    // QThread::finished -> deleteLater connection owns its lifetime instead.
    m_worker = new VideoBatchWorker(paths, m_keepRules, m_keepAll);
    m_thread = new QThread(this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &VideoBatchWorker::run);
    connect(m_worker, &VideoBatchWorker::progress, this, &VideoBatchPage::onWorkerProgress,
            Qt::QueuedConnection);
    connect(m_worker, &VideoBatchWorker::logMessage, this, &VideoBatchPage::onWorkerLog,
            Qt::QueuedConnection);
    connect(m_worker, &VideoBatchWorker::finished, this, &VideoBatchPage::onCompareFinished,
            Qt::QueuedConnection);
    connect(m_worker, &VideoBatchWorker::failed, this, &VideoBatchPage::onCompareFailed,
            Qt::QueuedConnection);
    connect(m_worker, &VideoBatchWorker::finished, m_thread, &QThread::quit);
    connect(m_worker, &VideoBatchWorker::failed, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, this, &VideoBatchPage::onThreadFinished,
            Qt::QueuedConnection);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

void VideoBatchPage::onWorkerProgress(int done, int total, const QString& stage)
{
    if (total <= 0) {
        return;
    }
    if (m_progress->maximum() != total) {
        m_progress->setRange(0, total);
    }
    m_progress->setValue(done);
    m_progress->setFormat(QStringLiteral("%1 %p%").arg(stage));
}

void VideoBatchPage::onWorkerLog(int level, const QString& message)
{
    m_log->append(static_cast<ComparisonLogLevel>(level), message);
}

void VideoBatchPage::onCompareFinished(VideoBatchOutcome outcome)
{
    rebuildTree(outcome);
    m_lastClipCount = outcome.clipCount;
    m_lastFailedCount = outcome.failedCount;
    m_lastDuplicateGroups = 0;
    for (const VideoBatchGroupInfo& group : outcome.groups) {
        if (group.duplicate) {
            ++m_lastDuplicateGroups;
        }
    }
    m_progress->setRange(0, 1);
    m_progress->setValue(1);

    // Report the split because extraction is normally the bulk of the wait.
    m_lastTotalMs = outcome.extractMs + outcome.compareMs;
    m_progress->setFormat(tr("Comparison finished - %1 clips - %2 - %3 duplicate groups - "
                             "%4 pairs compared")
                              .arg(outcome.clipCount)
                              .arg(formatutils::durationLabel(m_lastTotalMs))
                              .arg(m_lastDuplicateGroups)
                              .arg(outcome.pairsCompared));
    if (outcome.failedCount > 0) {
        QMessageBox::warning(
            this, tr("Some videos could not be read"),
            tr("%1 videos could not be read and were skipped:\n%2")
                .arg(outcome.failedCount)
                .arg(outcome.failures.join(QLatin1Char('\n'))));
    }
    updateStatus();
}

void VideoBatchPage::onCompareFailed(const QString& message)
{
    m_progress->setFormat(tr("Comparison failed"));
    QMessageBox::warning(this, tr("Comparison failed"), message);
    updateStatus();
}

void VideoBatchPage::onThreadFinished()
{
    setBusy(false);
}

void VideoBatchPage::onItemChanged(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(item);
    Q_UNUSED(column);
    updateStatus();
}

void VideoBatchPage::onItemDoubleClicked(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);
    const RowData* row = recordForItem(item);
    if (row != nullptr) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(row->path));
    }
}

void VideoBatchPage::showProperties(const QVector<QTreeWidgetItem*>& items)
{
    if (items.isEmpty()) {
        return;
    }
    const RowData* row = recordForItem(items.first());
    if (row == nullptr) {
        return;
    }
    const QString details =
        tr("Name: %1\nResolution: %2\nDuration: %3\nSize: %4\nModified: %5\nLocation: %6")
            .arg(row->name)
            .arg(row->measured ? QStringLiteral("%1x%2").arg(row->width).arg(row->height)
                               : tr("not read yet"))
            .arg(row->measured ? formatDuration(row->duration) : tr("not read yet"))
            .arg(humanSize(row->fileSize))
            .arg(row->modifiedText)
            .arg(row->path);
    QMessageBox box(this);
    box.setWindowTitle(tr("Video properties"));
    box.setIcon(QMessageBox::NoIcon);
    box.setText(details);
    box.exec();
}

void VideoBatchPage::copyPaths(const QVector<QTreeWidgetItem*>& items)
{
    QStringList paths;
    QList<QUrl> urls;
    for (QTreeWidgetItem* item : items) {
        const RowData* row = recordForItem(item);
        if (row != nullptr) {
            paths.append(row->path);
            urls.append(QUrl::fromLocalFile(row->path));
        }
    }
    if (!paths.isEmpty()) {
        QMimeData* mime = new QMimeData;
        mime->setUrls(urls);
        mime->setText(paths.join(QLatin1Char('\n')));
        QApplication::clipboard()->setMimeData(mime);
    }
}

void VideoBatchPage::openContainingFolder(const QVector<QTreeWidgetItem*>& items)
{
    if (items.isEmpty()) {
        return;
    }
    const RowData* row = recordForItem(items.first());
    if (row != nullptr) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(row->path).absolutePath()));
    }
}

void VideoBatchPage::showGroupContextMenu(QTreeWidgetItem* group, const QPoint& position)
{
    QVector<QTreeWidgetItem*> members;
    for (int i = 0; i < group->childCount(); ++i) {
        members.append(group->child(i));
    }
    const bool hasMembers = !members.isEmpty();

    QMenu menu(this);
    QAction* expandAction =
        menu.addAction(group->isExpanded() ? tr("Collapse this group") : tr("Expand this group"));
    menu.addSeparator();
    QAction* checkAllAction = menu.addAction(tr("Check every clip in the group"));
    QAction* uncheckAllAction = menu.addAction(tr("Uncheck every clip in the group"));
    menu.addSeparator();
    QAction* openFirstAction = menu.addAction(tr("Open the first clip in the group"));
    QAction* folderAction = menu.addAction(tr("Open containing folder"));
    menu.addSeparator();
    QAction* removeAction = menu.addAction(tr("Remove this group"));
    QAction* removeCheckedAction = menu.addAction(tr("Remove the checked clips in the group"));
    QAction* deleteCheckedAction =
        menu.addAction(tr("Move the checked files in the group to the Recycle Bin"));

    for (QAction* action : {checkAllAction, uncheckAllAction, openFirstAction, folderAction,
                            removeAction, removeCheckedAction, deleteCheckedAction}) {
        action->setEnabled(hasMembers);
    }

    QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(position));
    if (chosen == expandAction) {
        group->setExpanded(!group->isExpanded());
    } else if (chosen == checkAllAction) {
        setItemsChecked(members, true);
    } else if (chosen == uncheckAllAction) {
        setItemsChecked(members, false);
    } else if (chosen == openFirstAction) {
        if (const RowData* row = recordForItem(members.first())) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(row->path));
        }
    } else if (chosen == folderAction) {
        openContainingFolder(members);
    } else if (chosen == removeAction) {
        removeItems(members);
    } else if (chosen == removeCheckedAction) {
        QVector<QTreeWidgetItem*> checked;
        for (QTreeWidgetItem* item : members) {
            if (item->checkState(CheckColumn) == Qt::Checked) {
                checked.append(item);
            }
        }
        removeItems(checked);
    } else if (chosen == deleteCheckedAction) {
        QVector<QTreeWidgetItem*> checked;
        for (QTreeWidgetItem* item : members) {
            if (item->checkState(CheckColumn) == Qt::Checked) {
                checked.append(item);
            }
        }
        deleteFiles(checked);
    }
}

void VideoBatchPage::showTreeContextMenu(const QPoint& position)
{
    QTreeWidgetItem* clicked = m_tree->itemAt(position);
    // A group row (one with children) gets its own menu.
    if (clicked != nullptr && clicked->parent() == nullptr) {
        showGroupContextMenu(clicked, position);
        return;
    }
    if (clicked != nullptr && recordForItem(clicked) != nullptr && !clicked->isSelected()) {
        m_tree->clearSelection();
        clicked->setSelected(true);
        m_tree->setCurrentItem(clicked);
    }

    const QVector<QTreeWidgetItem*> target = contextTargetItems(clicked);
    if (target.isEmpty()) {
        return;
    }

    QMenu menu(this);
    QAction* toggleAction = menu.addAction(tr("Check / uncheck"));
    QAction* invertAction = menu.addAction(tr("Invert"));
    menu.addSeparator();
    QAction* openAction = menu.addAction(tr("Open video"));
    QAction* copyAction = menu.addAction(tr("Copy path"));
    QAction* propertyAction = menu.addAction(tr("Properties"));
    QAction* folderAction = menu.addAction(tr("Open containing folder"));
    menu.addSeparator();
    QAction* removeAction = menu.addAction(tr("Remove from list"));
    QAction* deleteAction = menu.addAction(tr("Move to Recycle Bin"));
    menu.addSeparator();
    QAction* compareAction = menu.addAction(tr("Compare the checked videos"));

    QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(position));
    if (chosen == toggleAction) {
        bool anyChecked = false;
        for (QTreeWidgetItem* item : target) {
            anyChecked = anyChecked || item->checkState(CheckColumn) == Qt::Checked;
        }
        setItemsChecked(target, !anyChecked);
    } else if (chosen == invertAction) {
        invertChecked();
    } else if (chosen == openAction) {
        if (const RowData* row = recordForItem(target.first())) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(row->path));
        }
    } else if (chosen == copyAction) {
        copyPaths(target);
    } else if (chosen == propertyAction) {
        showProperties(target);
    } else if (chosen == folderAction) {
        openContainingFolder(target);
    } else if (chosen == removeAction) {
        removeItems(target);
    } else if (chosen == deleteAction) {
        deleteFiles(target);
    } else if (chosen == compareAction) {
        startCompare();
    }
}

void VideoBatchPage::searchChanged(const QString& text)
{
    m_tree->clearSelection();
    const QString query = text.trimmed();
    if (query.isEmpty()) {
        return;
    }

    for (QTreeWidgetItem* item : allRowItems()) {
        const RowData* row = recordForItem(item);
        if (row == nullptr) {
            continue;
        }
        if (row->name.contains(query, Qt::CaseInsensitive)
            || row->path.contains(query, Qt::CaseInsensitive)) {
            item->setSelected(true);
            m_tree->setCurrentItem(item);
            if (item->parent() != nullptr) {
                item->parent()->setExpanded(true);
            }
            m_tree->scrollToItem(item);
            break;
        }
    }
}

void VideoBatchPage::updateStatus()
{
    // Called from every path that can change the ticks, the list or the busy
    // state, which makes it the one place that keeps the compare button honest.
    updateCompareButton();

    QString text = tr("%1 videos, %2 checked")
                       .arg(m_records.size())
                       .arg(checkedItems().size());
    if (m_lastClipCount > 0) {
        text += tr(", %1 compared in %2 duplicate groups")
                    .arg(m_lastClipCount)
                    .arg(m_lastDuplicateGroups);
    }
    // Clips added after a comparison are listed but not compared yet; saying so
    // avoids the impression that the results already cover them.
    const int pending = unlistedPathCount();
    if (pending > 0) {
        text += tr(", %1 waiting to be compared").arg(pending);
    }
    if (m_lastFailedCount > 0) {
        text += tr(", %1 could not be read").arg(m_lastFailedCount);
    }
    text += tr(", keep policy: %1")
                .arg(keepPrioritySummary(m_keepRules, m_keepAll));
    m_status->setText(text);
}

bool VideoBatchPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_tree && watched != m_tree->viewport()) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* dragEvent = static_cast<QDragEnterEvent*>(event);
        if (dragEvent->mimeData()->hasUrls()) {
            dragEvent->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        QStringList paths;
        for (const QUrl& url : dropEvent->mimeData()->urls()) {
            if (!url.isLocalFile()) {
                continue;
            }
            const QString path = url.toLocalFile();
            if (QFileInfo(path).isDir()) {
                QDirIterator iterator(path, videoWildcards(), QDir::Files,
                                      QDirIterator::Subdirectories);
                while (iterator.hasNext()) {
                    paths.append(iterator.next());
                }
            } else if (isSupportedVideoFile(path)) {
                paths.append(path);
            }
        }
        if (!paths.isEmpty()) {
            addPaths(paths);
            dropEvent->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            const QVector<QTreeWidgetItem*> selected = selectedRowItems();
            if (!selected.isEmpty()) {
                onItemDoubleClicked(selected.first(), 0);
            }
            return true;
        }
        if (keyEvent->key() == Qt::Key_Delete) {
            const QVector<QTreeWidgetItem*> selected = selectedRowItems();
            removeItems(selected.isEmpty() ? checkedItems() : selected);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VideoBatchPage::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void VideoBatchPage::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isSupportedVideoFile(url.toLocalFile())) {
            paths.append(url.toLocalFile());
        }
    }
    if (!paths.isEmpty()) {
        addPaths(paths);
        event->acceptProposedAction();
    }
}
