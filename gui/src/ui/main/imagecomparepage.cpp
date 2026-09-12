#include "imagecomparepage.h"

#include "formatutils.h"

#include <opencv2/imgcodecs.hpp>

#include "visual_fingerprint.hpp"

#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <vector>

namespace {

constexpr int kPreviewWidth = 340;
constexpr int kPreviewHeight = 230;

QString previewText(const QString& path)
{
    return path.isEmpty() ? QStringLiteral("未选择图片") : path;
}

bool decodeImage(const QString& path, cv::Mat& output)
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

QString elidedPath(const QLabel* label, const QString& path)
{
    return label->fontMetrics().elidedText(path, Qt::ElideMiddle, 360);
}

bool isSupportedImageFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QStringList supported = {
        QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"),  QStringLiteral("webp"),
    };
    return supported.contains(suffix);
}

QString firstDroppedImage(const QDropEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (!mime || !mime->hasUrls()) {
        return QString();
    }
    const QList<QUrl> urls = mime->urls();
    for (const QUrl& url : urls) {
        if (url.isLocalFile() && isSupportedImageFile(url.toLocalFile())) {
            return url.toLocalFile();
        }
    }
    return QString();
}

} // namespace

ImageComparePage::ImageComparePage(QWidget* parent)
    : QWidget(parent)
    , m_firstPreview(nullptr)
    , m_secondPreview(nullptr)
    , m_firstPathLabel(nullptr)
    , m_secondPathLabel(nullptr)
    , m_resultVerdict(nullptr)
    , m_resultScore(nullptr)
    , m_resultSource(nullptr)
    , m_threshold(nullptr)
    , m_compareButton(nullptr)
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 18, 28, 18);
    root->setSpacing(14);

    QHBoxLayout* header = new QHBoxLayout;
    QPushButton* backButton = new QPushButton(tr("返回功能选择"), this);
    backButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(backButton, &QPushButton::released, this, &ImageComparePage::backRequested);
    QLabel* title = new QLabel(tr("图片比对模式"), this);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;color:#1f2937;"));
    header->addWidget(backButton);
    header->addSpacing(10);
    header->addWidget(title);
    header->addStretch(1);
    root->addLayout(header);

    auto makePreview = [this]() -> QLabel* {
        QLabel* label = new QLabel(this);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumSize(kPreviewWidth, kPreviewHeight);
        label->setMaximumSize(kPreviewWidth * 2, kPreviewHeight * 2);
        label->setStyleSheet(QStringLiteral(
            "background:#ffffff;border:1px solid #d7dce2;border-radius:6px;color:#9ca3af;"));
        return label;
    };

    m_firstPreview = makePreview();
    m_secondPreview = makePreview();
    m_firstPreview->setText(tr("未选择图片一"));
    m_secondPreview->setText(tr("未选择图片二"));
    m_firstPreview->setAcceptDrops(true);
    m_secondPreview->setAcceptDrops(true);
    m_firstPreview->installEventFilter(this);
    m_secondPreview->installEventFilter(this);
    setAcceptDrops(true);
    installEventFilter(this);

    auto makePathLabel = [this]() -> QLabel* {
        QLabel* label = new QLabel(this);
        label->setMinimumWidth(340);
        label->setMaximumWidth(440);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("color:#6b7280;"));
        return label;
    };

    m_firstPathLabel = makePathLabel();
    m_secondPathLabel = makePathLabel();

    auto makeColumn = [this](const QString& sideTitle, QLabel* preview, QLabel* pathLabel,
                             QPushButton* chooseButton) {
        QWidget* column = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(column);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        QLabel* side = new QLabel(sideTitle, column);
        side->setStyleSheet(QStringLiteral("font-weight:600;color:#334155;"));
        layout->addWidget(side);
        layout->addWidget(preview, 0, Qt::AlignHCenter);
        layout->addWidget(pathLabel, 0, Qt::AlignHCenter);
        layout->addWidget(chooseButton, 0, Qt::AlignHCenter);
        return column;
    };

    QPushButton* firstButton = new QPushButton(tr("选择图片一"), this);
    firstButton->setObjectName(QStringLiteral("secondaryButton"));
    QPushButton* secondButton = new QPushButton(tr("选择图片二"), this);
    secondButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(firstButton, &QPushButton::released, this, &ImageComparePage::chooseFirstImage);
    connect(secondButton, &QPushButton::released, this, &ImageComparePage::chooseSecondImage);

    QHBoxLayout* imageRow = new QHBoxLayout;
    imageRow->setSpacing(22);
    imageRow->addStretch(1);
    imageRow->addWidget(makeColumn(tr("图片一"), m_firstPreview, m_firstPathLabel, firstButton));
    imageRow->addWidget(makeColumn(tr("图片二"), m_secondPreview, m_secondPathLabel, secondButton));
    imageRow->addStretch(1);
    root->addLayout(imageRow);

    QHBoxLayout* compareRow = new QHBoxLayout;
    compareRow->addStretch(1);
    compareRow->addWidget(new QLabel(tr("匹配阈值"), this));
    m_threshold = new QDoubleSpinBox(this);
    m_threshold->setRange(0.50, 0.99);
    m_threshold->setDecimals(2);
    m_threshold->setSingleStep(0.01);
    m_threshold->setValue(0.78);
    compareRow->addWidget(m_threshold);
    compareRow->addSpacing(18);
    m_compareButton = new QPushButton(tr("开始比对"), this);
    m_compareButton->setObjectName(QStringLiteral("primaryButton"));
    m_compareButton->setEnabled(false);
    connect(m_compareButton, &QPushButton::released, this,
            &ImageComparePage::compareSelectedImages);
    compareRow->addWidget(m_compareButton);
    compareRow->addStretch(1);
    root->addLayout(compareRow);

    QFrame* resultPanel = new QFrame(this);
    resultPanel->setFrameShape(QFrame::StyledPanel);
    resultPanel->setStyleSheet(QStringLiteral(
        "QFrame { background:#ffffff; border:1px solid #d7dce2; border-radius:8px; }"
        "QLabel { color:#334155; }"
        "QLabel#verdict { font-size:18px; font-weight:700; }"
        "QLabel#score { font-size:26px; font-weight:700; color:#2563eb; }"));
    QHBoxLayout* resultLayout = new QHBoxLayout(resultPanel);
    resultLayout->setContentsMargins(18, 14, 18, 14);
    resultLayout->setSpacing(24);

    QVBoxLayout* verdictLayout = new QVBoxLayout;
    m_resultVerdict = new QLabel(tr("等待比对"), resultPanel);
    m_resultVerdict->setObjectName(QStringLiteral("verdict"));
    m_resultSource = new QLabel(QString(), resultPanel);
    verdictLayout->addWidget(m_resultVerdict);
    verdictLayout->addWidget(m_resultSource);
    resultLayout->addLayout(verdictLayout);
    resultLayout->addStretch(1);
    m_resultScore = new QLabel(QStringLiteral("--"), resultPanel);
    m_resultScore->setObjectName(QStringLiteral("score"));
    m_resultScore->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    resultLayout->addWidget(m_resultScore);
    root->addWidget(resultPanel);
    root->addStretch(1);
}

void ImageComparePage::chooseFirstImage()
{
    chooseImage(0);
}

void ImageComparePage::chooseSecondImage()
{
    chooseImage(1);
}

void ImageComparePage::chooseImage(int slot)
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("选择图片"), QString(),
        tr("图片 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }
    setImagePath(slot, path);
}

void ImageComparePage::setImagePath(int slot, const QString& path)
{
    QImage image(path);
    if (image.isNull()) {
        showError(tr("无法读取图片：%1").arg(path));
        return;
    }

    if (slot == 0) {
        m_firstPath = path;
        m_firstImage = image;
        m_firstPathLabel->setText(elidedPath(m_firstPathLabel, path));
        m_firstPathLabel->setToolTip(path);
    } else {
        m_secondPath = path;
        m_secondImage = image;
        m_secondPathLabel->setText(elidedPath(m_secondPathLabel, path));
        m_secondPathLabel->setToolTip(path);
    }
    refreshPreview(slot);
    m_compareButton->setEnabled(!m_firstPath.isEmpty() && !m_secondPath.isEmpty());
    m_resultVerdict->setText(tr("等待比对"));
    m_resultScore->setText(QStringLiteral("--"));
    m_resultSource->clear();
}

void ImageComparePage::refreshPreview(int slot)
{
    QLabel* preview = slot == 0 ? m_firstPreview : m_secondPreview;
    const QImage& image = slot == 0 ? m_firstImage : m_secondImage;
    if (image.isNull() || preview->width() <= 0 || preview->height() <= 0) {
        return;
    }

    const int usableWidth = std::max(40, preview->width() - 8);
    const int usableHeight = std::max(40, preview->height() - 8);
    const QPixmap scaled = QPixmap::fromImage(image)
                               .scaled(usableWidth, usableHeight, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
    preview->setPixmap(scaled);
    preview->setText(QString());
}

void ImageComparePage::refreshPreviews()
{
    refreshPreview(0);
    refreshPreview(1);
}

void ImageComparePage::compareSelectedImages()
{
    if (m_firstPath.isEmpty() || m_secondPath.isEmpty()) {
        return;
    }

    // Times the whole operation, decoding included: what the user waits for is
    // "click to result", not just the hashing.
    QElapsedTimer timer;
    timer.start();

    cv::Mat first;
    cv::Mat second;
    if (!decodeImage(m_firstPath, first) || !decodeImage(m_secondPath, second)) {
        showError(tr("图片解码失败，请确认两张图片仍可正常读取。"));
        return;
    }

    try {
        const vividmatch::Fingerprint left = vividmatch::makeFingerprint(first);
        const vividmatch::Fingerprint right = vividmatch::makeFingerprint(second);
        const double threshold = m_threshold->value();
        const double score = vividmatch::compareFingerprints(left, right);
        setResult(score, threshold, left.width, left.height, right.width, right.height,
                  timer.elapsed());
    } catch (const std::exception& error) {
        showError(QString::fromUtf8(error.what()));
    }
}

void ImageComparePage::setResult(double score, double threshold, int firstWidth,
                                 int firstHeight, int secondWidth, int secondHeight,
                                 qint64 elapsedMs)
{
    const bool matched = score >= threshold;
    m_resultVerdict->setText(matched ? tr("判定：相同图片") : tr("判定：不同图片"));
    m_resultVerdict->setStyleSheet(matched
                                       ? QStringLiteral("color:#15803d;font-size:18px;font-weight:700;")
                                       : QStringLiteral("color:#b91c1c;font-size:18px;font-weight:700;"));
    m_resultScore->setText(QStringLiteral("%1%").arg(score * 100.0, 0, 'f', 2));
    m_resultSource->setText(
        tr("图片一 %1x%2   vs   图片二 %3x%4   阈值 %5   %6")
            .arg(firstWidth)
            .arg(firstHeight)
            .arg(secondWidth)
            .arg(secondHeight)
            .arg(threshold, 0, 'f', 2)
            .arg(formatutils::durationLabel(elapsedMs)));
}

void ImageComparePage::showError(const QString& message)
{
    QMessageBox::warning(this, tr("比对失败"), message);
}

bool ImageComparePage::eventFilter(QObject* watched, QEvent* event)
{
    const bool overFirst = watched == m_firstPreview;
    const bool overSecond = watched == m_secondPreview;
    const bool overPage = watched == this;

    if (!overFirst && !overSecond && !overPage) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* dragEvent = static_cast<QDragEnterEvent*>(event);
        if (!firstDroppedImage(dragEvent).isEmpty()) {
            dragEvent->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        const QString path = firstDroppedImage(dropEvent);
        if (!path.isEmpty()) {
            int slot = 0;
            if (overSecond) {
                slot = 1;
            } else if (overPage) {
                const QPoint position = dropEvent->position().toPoint();
                slot = position.x() < width() / 2 ? 0 : 1;
            }
            setImagePath(slot, path);
            dropEvent->acceptProposedAction();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ImageComparePage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    refreshPreviews();
}
