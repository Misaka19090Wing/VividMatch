#include "videobatchpage.h"

#include "formatutils.h"

#include <QApplication>
#include <QBrush>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

namespace {

constexpr int kPathRole = Qt::UserRole + 1;
// Marks the group holding clips that have not been compared yet, so a rebuild
// can replace it without touching the result groups.
constexpr int kPendingMarker = -1;

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

QString groupLabel(const VideoBatchGroupInfo& group)
{
    if (group.duplicate) {
        QString label = QCoreApplication::translate("videobatchpage", "重复组 · 相似度 %1% · %2 个")
                            .arg(group.similarity * 100.0, 0, 'f', 1)
                            .arg(group.rows.size());
        if (group.audioDiffers) {
            label += QCoreApplication::translate("videobatchpage", " · 含音频不同的版本");
        }
        return label;
    }
    if (group.montage) {
        return QCoreApplication::translate("videobatchpage", "疑似混剪 · 仅部分重合");
    }
    return QCoreApplication::translate("videobatchpage", "无重复");
}

}  // namespace

// --- ranking ---------------------------------------------------------------

int rankVideoGroup(KeepPolicy policy, const std::vector<int>& members,
                   const std::vector<vividmatch::VideoBatchItem>& items)
{
    if (policy == KeepPolicy::KeepAll || members.empty()) {
        return -1;
    }

    // Half the policies keep the largest value and half the smallest.
    const bool keepLargest = policy == KeepPolicy::HighestResolution
                             || policy == KeepPolicy::LargestFile
                             || policy == KeepPolicy::NewestModified;

    int best = members.front();
    double bestValue = 0.0;
    bool first = true;
    for (const int member : members) {
        const vividmatch::VideoBatchItem& item = items[static_cast<std::size_t>(member)];
        double value = 0.0;
        switch (policy) {
            case KeepPolicy::HighestResolution:
            case KeepPolicy::LowestResolution:
                value = static_cast<double>(item.fingerprint.width) * item.fingerprint.height;
                break;
            case KeepPolicy::LargestFile:
            case KeepPolicy::SmallestFile: {
                std::error_code code;
                value = static_cast<double>(
                    std::filesystem::file_size(std::filesystem::u8path(item.path), code));
                if (code) {
                    value = 0.0;
                }
                break;
            }
            case KeepPolicy::NewestModified:
            case KeepPolicy::OldestModified: {
                const QFileInfo file(QString::fromStdString(item.path));
                value = static_cast<double>(file.lastModified().toMSecsSinceEpoch());
                break;
            }
            case KeepPolicy::KeepAll:
                return -1;
        }
        if (first || (keepLargest ? value > bestValue : value < bestValue)) {
            best = member;
            bestValue = value;
            first = false;
        }
    }
    return best;
}

// --- worker ----------------------------------------------------------------

VideoBatchWorker::VideoBatchWorker(QStringList paths, KeepPolicy policy)
    : m_paths(std::move(paths))
    , m_policy(policy)
{
}

void VideoBatchWorker::run()
{
    try {
        std::vector<std::string> paths;
        paths.reserve(static_cast<std::size_t>(m_paths.size()));
        for (const QString& path : m_paths) {
            paths.push_back(path.toStdString());
        }

        // The policy is carried by value, so ranking a group here touches no
        // page state from this thread.
        const KeepPolicy policy = m_policy;
        const vividmatch::VideoRanking ranking =
            [policy](const std::vector<int>& members,
                     const std::vector<vividmatch::VideoBatchItem>& items) {
                return rankVideoGroup(policy, members, items);
            };

        const vividmatch::VideoBatchResult result = vividmatch::compareVideoBatch(
            paths, vividmatch::kDefaultFrameThreshold, vividmatch::kDefaultSignatureThreshold,
            vividmatch::kDefaultBatchWorkers, ranking,
            [this](int done, int total, const std::string& stage) {
                const QString label = stage == "extracting" ? tr("提取指纹")
                                      : stage == "audio"    ? tr("比对音频")
                                                            : tr("比对中");
                emit progress(done, total, label);
            });

        VideoBatchOutcome outcome;
        outcome.clipCount = static_cast<int>(result.items.size());
        outcome.failedCount = result.failed;
        outcome.extractMs = static_cast<qint64>(result.extractMs);
        outcome.compareMs = static_cast<qint64>(result.compareMs);
        outcome.pairsCompared = result.pairsCompared;
        const int n = outcome.clipCount;
        outcome.pairsPossible = n > 1 ? n * (n - 1) / 2 : 0;

        for (const vividmatch::VideoBatchItem& item : result.items) {
            if (!item.ok) {
                outcome.failures << QStringLiteral("%1（%2）")
                                        .arg(QString::fromStdString(item.path))
                                        .arg(QString::fromStdString(item.error));
            }
        }

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
                row.durationText = formatDuration(item.fingerprint.duration);
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
        emit failed(QString::fromUtf8(error.what()));
    }
}

// --- page ------------------------------------------------------------------

VideoBatchPage::VideoBatchPage(QWidget* parent)
    : QWidget(parent)
    , m_tree(new QTreeWidget(this))
    , m_policy(new QComboBox(this))
    , m_progress(new QProgressBar(this))
    , m_compareButton(nullptr)
    , m_clearButton(nullptr)
    , m_status(new QLabel(this))
    , m_lastClipCount(0)
    , m_lastDuplicateGroups(0)
    , m_lastFailedCount(0)
    , m_worker(nullptr)
    , m_busy(false)
{
    qRegisterMetaType<VideoBatchOutcome>("VideoBatchOutcome");

    setAcceptDrops(true);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 16, 24, 16);
    root->setSpacing(10);

    QHBoxLayout* header = new QHBoxLayout;
    QPushButton* backButton = new QPushButton(tr("返回功能选择"), this);
    backButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(backButton, &QPushButton::released, this, &VideoBatchPage::backRequested);
    QLabel* title = new QLabel(tr("批量视频比对"), this);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;color:#1f2937;"));
    header->addWidget(backButton);
    header->addSpacing(10);
    header->addWidget(title);
    header->addStretch(1);
    root->addLayout(header);

    QHBoxLayout* controls = new QHBoxLayout;
    QPushButton* addFolderButton = new QPushButton(tr("添加文件夹"), this);
    addFolderButton->setObjectName(QStringLiteral("secondaryButton"));
    QPushButton* addVideosButton = new QPushButton(tr("添加视频"), this);
    addVideosButton->setObjectName(QStringLiteral("secondaryButton"));
    m_clearButton = new QPushButton(tr("清空列表"), this);
    m_clearButton->setObjectName(QStringLiteral("secondaryButton"));
    m_compareButton = new QPushButton(tr("开始比对"), this);
    m_compareButton->setObjectName(QStringLiteral("primaryButton"));
    connect(addFolderButton, &QPushButton::released, this, &VideoBatchPage::chooseFolder);
    connect(addVideosButton, &QPushButton::released, this, &VideoBatchPage::chooseVideos);
    connect(m_clearButton, &QPushButton::released, this, &VideoBatchPage::clearList);
    connect(m_compareButton, &QPushButton::released, this, &VideoBatchPage::startCompare);

    controls->addWidget(addFolderButton);
    controls->addWidget(addVideosButton);
    controls->addWidget(m_clearButton);
    controls->addStretch(1);
    controls->addWidget(m_compareButton);
    controls->addSpacing(12);
    controls->addWidget(new QLabel(tr("保留策略"), this));
    m_policy->addItem(tr("分辨率最高"), static_cast<int>(KeepPolicy::HighestResolution));
    m_policy->addItem(tr("分辨率最低"), static_cast<int>(KeepPolicy::LowestResolution));
    m_policy->addItem(tr("文件最大"), static_cast<int>(KeepPolicy::LargestFile));
    m_policy->addItem(tr("文件最小"), static_cast<int>(KeepPolicy::SmallestFile));
    m_policy->addItem(tr("修改日期最近"), static_cast<int>(KeepPolicy::NewestModified));
    m_policy->addItem(tr("修改日期最远"), static_cast<int>(KeepPolicy::OldestModified));
    m_policy->addItem(tr("全部保留"), static_cast<int>(KeepPolicy::KeepAll));
    controls->addWidget(m_policy);
    root->addLayout(controls);

    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("名称"), tr("分辨率"), tr("时长"), tr("大小"),
                             tr("修改日期")});
    m_tree->setRootIsDecorated(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->resizeSection(0, 420);
    m_tree->header()->resizeSection(1, 110);
    m_tree->header()->resizeSection(2, 90);
    m_tree->header()->resizeSection(3, 110);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QTreeWidgetItem* item = m_tree->itemAt(position);
        if (item == nullptr || item->parent() == nullptr) {
            return;
        }
        QMenu menu(this);
        QAction* openAction = menu.addAction(tr("打开视频"));
        QAction* folderAction = menu.addAction(tr("打开所在文件夹"));
        QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(position));
        const QString path = item->data(0, kPathRole).toString();
        if (path.isEmpty()) {
            return;
        }
        if (chosen == openAction) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        } else if (chosen == folderAction) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        }
    });
    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->installEventFilter(this);
    m_tree->viewport()->installEventFilter(this);
    root->addWidget(m_tree, 1);

    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("等待比对"));
    root->addWidget(m_progress);

    m_status->setStyleSheet(QStringLiteral("color:#64748b;"));
    root->addWidget(m_status);

    updateStatus();
}

void VideoBatchPage::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("选择视频文件夹"));
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
        this, tr("选择视频"), QString(),
        tr("视频 (*.mp4 *.mov *.mkv *.avi *.webm *.m4v);;所有文件 (*)"));
    addPaths(paths);
}

void VideoBatchPage::addPaths(const QStringList& paths)
{
    int added = 0;
    for (const QString& path : paths) {
        if (!isSupportedVideoFile(path)) {
            continue;
        }
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (canonical.isEmpty() || m_paths.contains(canonical)) {
            continue;
        }
        m_paths.append(canonical);
        ++added;
    }
    if (added > 0) {
        // Show the new clips immediately. Waiting for a comparison to display
        // them leaves the list looking as if the add did nothing.
        rebuildPendingTree();
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
        m_progress->setFormat(tr("已加入 %1 个视频，等待比对").arg(added));
    }
    updateStatus();
}

void VideoBatchPage::rebuildPendingTree()
{
    // Result groups stay put; only the clips that are not shown yet are (re)built.
    // That way adding a clip to an already compared list extends the list instead
    // of wiping the results.
    const bool hadResults = !m_listedPaths.isEmpty();
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        if (m_tree->topLevelItem(i)->data(0, kPathRole).toInt() == kPendingMarker) {
            delete m_tree->takeTopLevelItem(i);
        }
    }

    QStringList missing;
    for (const QString& path : m_paths) {
        if (!m_listedPaths.contains(path)) {
            missing.append(path);
        }
    }
    if (missing.isEmpty()) {
        updateStatus();
        return;
    }

    QTreeWidgetItem* groupItem = new QTreeWidgetItem(m_tree);
    groupItem->setFirstColumnSpanned(true);
    groupItem->setFlags(Qt::ItemIsEnabled);
    groupItem->setData(0, kPathRole, kPendingMarker);
    groupItem->setText(0, hadResults ? tr("新加入 · %1 个").arg(missing.size())
                                     : tr("待比对 · %1 个").arg(missing.size()));
    groupItem->setExpanded(true);
    groupItem->setBackground(0, QBrush(QColor(232, 237, 243)));
    groupItem->setForeground(0, QBrush(QColor(51, 65, 85)));
    QFont font = groupItem->font(0);
    font.setBold(true);
    groupItem->setFont(0, font);

    for (const QString& path : missing) {
        const QFileInfo file(path);
        QTreeWidgetItem* child = new QTreeWidgetItem(groupItem);
        child->setData(0, kPathRole, path);
        child->setText(0, file.fileName());
        // A dash, not a value: reading the clip is what the comparison run does.
        child->setText(1, QStringLiteral("—"));
        child->setText(2, QStringLiteral("—"));
        child->setText(3, humanSize(file.size()));
        child->setText(4, file.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        child->setToolTip(0, path);
    }
    updateStatus();
}

int VideoBatchPage::unlistedPathCount() const
{
    int count = 0;
    for (const QString& path : m_paths) {
        if (!m_listedPaths.contains(path)) {
            ++count;
        }
    }
    return count;
}

void VideoBatchPage::clearList()
{
    if (m_busy) {
        return;
    }
    m_paths.clear();
    m_listedPaths.clear();
    m_tree->clear();
    m_lastClipCount = 0;
    m_lastDuplicateGroups = 0;
    m_lastFailedCount = 0;
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("等待比对"));
    updateStatus();
}

void VideoBatchPage::setBusy(bool busy)
{
    m_busy = busy;
    m_compareButton->setEnabled(!busy && m_paths.size() >= 2);
    m_clearButton->setEnabled(!busy);
    m_policy->setEnabled(!busy);
}

KeepPolicy VideoBatchPage::currentPolicy() const
{
    return static_cast<KeepPolicy>(m_policy->currentData().toInt());
}

void VideoBatchPage::startCompare()
{
    if (m_busy || m_paths.size() < 2) {
        return;
    }
    if (m_thread && m_thread->isRunning()) {
        return;
    }

    setBusy(true);
    // The pending list stays on screen while the worker runs; it is replaced by
    // the grouped results when they arrive.
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("准备中..."));

    // No parent: moveToThread refuses an object that already has one, and the
    // QThread::finished -> deleteLater connection owns its lifetime instead.
    m_worker = new VideoBatchWorker(m_paths, currentPolicy());
    m_thread = new QThread(this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &VideoBatchWorker::run);
    connect(m_worker, &VideoBatchWorker::progress, this, &VideoBatchPage::onWorkerProgress,
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
    const qint64 totalMs = outcome.extractMs + outcome.compareMs;
    m_progress->setFormat(tr("比对完成 · %1 个视频 · %2")
                              .arg(outcome.clipCount)
                              .arg(formatutils::durationLabel(totalMs)));
    updateStatus();
}

void VideoBatchPage::rebuildTree(const VideoBatchOutcome& outcome)
{
    m_tree->clear();
    // Every clip is now represented by its result row.
    m_listedPaths = m_paths;
    for (const VideoBatchGroupInfo& group : outcome.groups) {
        QTreeWidgetItem* groupItem = new QTreeWidgetItem(m_tree);
        groupItem->setFirstColumnSpanned(true);
        groupItem->setFlags(Qt::ItemIsEnabled);
        groupItem->setText(0, group.label);
        groupItem->setExpanded(true);
        groupItem->setBackground(0, QBrush(QColor(232, 237, 243)));
        groupItem->setForeground(0, QBrush(QColor(51, 65, 85)));
        QFont font = groupItem->font(0);
        font.setBold(true);
        groupItem->setFont(0, font);

        for (const VideoBatchRow& row : group.rows) {
            QTreeWidgetItem* child = new QTreeWidgetItem(groupItem);
            child->setData(0, kPathRole, row.path);
            // The kept clip is marked so the group reads at a glance; when the
            // policy keeps everything, every row is marked.
            child->setText(0, row.preferred ? tr("★ %1").arg(row.name) : row.name);
            child->setText(1, row.resolution);
            child->setText(2, row.durationText);
            child->setText(3, row.sizeText);
            child->setText(4, row.modifiedText);
            child->setToolTip(0, row.path);
        }
    }
}

void VideoBatchPage::onCompareFailed(const QString& message)
{
    m_progress->setFormat(tr("比对失败"));
    QMessageBox::warning(this, tr("比对失败"), message);
    updateStatus();
}

void VideoBatchPage::onThreadFinished()
{
    setBusy(false);
}

void VideoBatchPage::updateStatus()
{
    QString text = tr("共 %1 个视频").arg(m_paths.size());
    if (m_lastClipCount > 0) {
        text += tr("，已比对 %1 个，重复组 %2 个")
                    .arg(m_lastClipCount)
                    .arg(m_lastDuplicateGroups);
    }
    // Clips added after a comparison are listed but not compared yet; saying so
    // avoids the impression that the results already cover them.
    const int pending = unlistedPathCount();
    if (pending > 0 && m_lastClipCount > 0) {
        text += tr("，%1 个待比对").arg(pending);
    }
    if (m_lastFailedCount > 0) {
        text += tr("，%1 个无法读取").arg(m_lastFailedCount);
    }
    text += tr("，保留策略：%1").arg(m_policy->currentText());
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
