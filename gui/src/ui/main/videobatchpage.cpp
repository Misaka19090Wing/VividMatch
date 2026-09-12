#include "videobatchpage.h"

#include "formatutils.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QComboBox>
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
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

namespace {

constexpr int kRecordIdRole = Qt::UserRole + 1;
// Marks the group holding clips that are not compared yet, so a rebuild can
// replace it without touching the result groups.
constexpr int kPendingMarker = -1;
constexpr int kGroupBaseRole = Qt::UserRole + 2;

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

// Heading for one result group. Built here rather than in the page so the worker
// can label a group before it is handed over.
QString groupLabel(const VideoBatchGroupInfo& group)
{
    if (group.duplicate) {
        QString label =
            QCoreApplication::translate("videobatchpage", "重复组 · 相似度 %1% · %2 个")
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
        emit failed(QString::fromUtf8(error.what()));
    }
}

// --- page ------------------------------------------------------------------

VideoBatchPage::VideoBatchPage(QWidget* parent)
    : QWidget(parent)
    , m_tree(new QTreeWidget(this))
    , m_policy(new QComboBox(this))
    , m_search(new QLineEdit(this))
    , m_progress(new QProgressBar(this))
    , m_compareButton(nullptr)
    , m_clearButton(nullptr)
    , m_invertButton(nullptr)
    , m_selectAllButton(nullptr)
    , m_removeButton(nullptr)
    , m_deleteButton(nullptr)
    , m_status(new QLabel(this))
    , m_nextRecordId(1)
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

    // Controls mirror the batch image page: add, then act on the checked rows,
    // with the primary action on the right next to the keep policy.
    QPushButton* addFolderButton = new QPushButton(tr("选择文件夹"), this);
    addFolderButton->setObjectName(QStringLiteral("secondaryButton"));
    QPushButton* addVideosButton = new QPushButton(tr("选择视频"), this);
    addVideosButton->setObjectName(QStringLiteral("secondaryButton"));
    m_selectAllButton = new QPushButton(tr("全选"), this);
    m_selectAllButton->setObjectName(QStringLiteral("secondaryButton"));
    m_invertButton = new QPushButton(tr("反选"), this);
    m_invertButton->setObjectName(QStringLiteral("secondaryButton"));
    m_removeButton = new QPushButton(tr("移出勾选项"), this);
    m_removeButton->setObjectName(QStringLiteral("secondaryButton"));
    m_deleteButton = new QPushButton(tr("删除勾选文件"), this);
    m_deleteButton->setObjectName(QStringLiteral("secondaryButton"));
    m_clearButton = new QPushButton(tr("清空列表"), this);
    m_clearButton->setObjectName(QStringLiteral("secondaryButton"));
    m_compareButton = new QPushButton(tr("开始比对"), this);
    m_compareButton->setObjectName(QStringLiteral("primaryButton"));

    connect(addFolderButton, &QPushButton::released, this, &VideoBatchPage::chooseFolder);
    connect(addVideosButton, &QPushButton::released, this, &VideoBatchPage::chooseVideos);
    connect(m_selectAllButton, &QPushButton::released, this, &VideoBatchPage::selectAllChecked);
    connect(m_invertButton, &QPushButton::released, this, &VideoBatchPage::invertChecked);
    connect(m_removeButton, &QPushButton::released, this, &VideoBatchPage::removeChecked);
    connect(m_deleteButton, &QPushButton::released, this, &VideoBatchPage::deleteCheckedFiles);
    connect(m_clearButton, &QPushButton::released, this, &VideoBatchPage::clearList);
    connect(m_compareButton, &QPushButton::released, this, &VideoBatchPage::startCompare);

    QHBoxLayout* controls = new QHBoxLayout;
    controls->addWidget(addFolderButton);
    controls->addWidget(addVideosButton);
    controls->addWidget(m_selectAllButton);
    controls->addWidget(m_invertButton);
    controls->addSpacing(8);
    controls->addWidget(m_removeButton);
    controls->addWidget(m_deleteButton);
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
    m_policy->setCurrentIndex(0);
    controls->addWidget(m_policy);
    root->addLayout(controls);
    // Changing the policy re-picks which clip of each group stays checked.
    connect(m_policy, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyPolicyToChecks(); });

    QHBoxLayout* searchRow = new QHBoxLayout;
    searchRow->addWidget(new QLabel(tr("定位"), this));
    m_search->setPlaceholderText(tr("Ctrl+F 搜索名称或路径"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(280);
    m_search->hide();
    searchRow->addWidget(m_search);
    searchRow->addSpacing(18);
    searchRow->addWidget(m_progress, 1);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("等待比对"));
    root->addLayout(searchRow);
    connect(m_search, &QLineEdit::textChanged, this, &VideoBatchPage::searchChanged);

    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("选中"), tr("名称"), tr("分辨率"), tr("时长"), tr("大小"),
                             tr("修改日期")});
    m_tree->setRootIsDecorated(true);
    m_tree->setItemsExpandable(true);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setIndentation(6);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->resizeSection(CheckColumn, 52);
    m_tree->header()->resizeSection(NameColumn, 420);
    m_tree->header()->resizeSection(ResolutionColumn, 100);
    m_tree->header()->resizeSection(DurationColumn, 80);
    m_tree->header()->resizeSection(SizeColumn, 100);
    m_tree->header()->setToolTip(
        tr("勾选要保留的视频；比对后按保留策略自动勾选。\n"
           "右键可对单个视频或整个分组操作。"));

    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &VideoBatchPage::showTreeContextMenu);
    connect(m_tree, &QTreeWidget::itemChanged, this, &VideoBatchPage::onItemChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &VideoBatchPage::onItemDoubleClicked);

    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->installEventFilter(this);
    m_tree->viewport()->installEventFilter(this);
    root->addWidget(m_tree, 1);

    m_status->setStyleSheet(QStringLiteral("color:#64748b;"));
    root->addWidget(m_status);

    QShortcut* findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this]() {
        m_search->show();
        m_search->setFocus();
        m_search->selectAll();
    });

    setBusy(false);
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
        m_progress->setFormat(tr("已加入 %1 个视频，等待比对").arg(added));
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
    item->setText(NameColumn, row.name);
    // A dash, not a value: reading the clip is what the comparison run does.
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
    group->setData(CheckColumn, kGroupBaseRole, tr("待比对"));
    group->setText(CheckColumn, tr("待比对"));
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
                   firstView ? tr("待比对") : tr("新加入"));
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

    for (const VideoBatchGroupInfo& group : outcome.groups) {
        QTreeWidgetItem* groupItem = new QTreeWidgetItem(m_tree);
        groupItem->setFirstColumnSpanned(true);
        groupItem->setFlags(Qt::ItemIsEnabled);
        groupItem->setData(CheckColumn, kGroupBaseRole, group.label);
        groupItem->setText(CheckColumn, group.label);
        groupItem->setExpanded(true);
        groupItem->setBackground(CheckColumn, QBrush(QColor(232, 237, 243)));
        groupItem->setForeground(CheckColumn, QBrush(QColor(51, 65, 85)));
        QFont font = groupItem->font(CheckColumn);
        font.setBold(true);
        groupItem->setFont(CheckColumn, font);

        for (const VideoBatchRow& row : group.rows) {
            const int recordId = recordIdForPath(row.path);
            if (recordId < 0) {
                continue;
            }
            if (QTreeWidgetItem* child = createRowItem(recordId)) {
                groupItem->addChild(child);
            }
        }
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
        group->setData(CheckColumn, kGroupBaseRole, tr("待比对"));
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
    // Only the pending group is renamed here: its heading is "待比对" or "新加入"
    // plus a live count. Result groups carry a label built from the verdict and
    // the similarity, which the count says nothing about, so they are set once
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
        group->setText(CheckColumn, tr("%1 · %2 个").arg(base).arg(group->childCount()));
    }
}

void VideoBatchPage::applyPolicyToChecks()
{
    // In the compared view, tick what the policy would keep and untick the rest:
    // the ticks are then a preview of what "移出勾选项" would leave behind.
    // Clips that have not been compared cannot be ranked, so their ticks are left
    // exactly as the user set them.
    const KeepPolicy policy = currentPolicy();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* group = m_tree->topLevelItem(i);
        if (group->data(CheckColumn, kRecordIdRole).toInt() == kPendingMarker) {
            continue;
        }
        if (group->childCount() == 0) {
            continue;
        }
        if (policy == KeepPolicy::KeepAll) {
            for (int j = 0; j < group->childCount(); ++j) {
                group->child(j)->setCheckState(CheckColumn, Qt::Checked);
            }
            continue;
        }

        // Score each row the way the keep policy ranks clips.
        int best = 0;
        double bestValue = 0.0;
        const bool keepLargest = policy == KeepPolicy::HighestResolution
                                 || policy == KeepPolicy::LargestFile
                                 || policy == KeepPolicy::NewestModified;
        for (int j = 0; j < group->childCount(); ++j) {
            const RowData* row = recordForItem(group->child(j));
            if (row == nullptr) {
                continue;
            }
            double value = 0.0;
            switch (policy) {
                case KeepPolicy::HighestResolution:
                case KeepPolicy::LowestResolution:
                    value = static_cast<double>(row->width) * row->height;
                    break;
                case KeepPolicy::LargestFile:
                case KeepPolicy::SmallestFile:
                    value = static_cast<double>(row->fileSize);
                    break;
                case KeepPolicy::NewestModified:
                case KeepPolicy::OldestModified:
                    value = static_cast<double>(row->modified.toMSecsSinceEpoch());
                    break;
                case KeepPolicy::KeepAll:
                    break;
            }
            if (j == 0 || (keepLargest ? value > bestValue : value < bestValue)) {
                best = j;
                bestValue = value;
            }
        }
        for (int j = 0; j < group->childCount(); ++j) {
            group->child(j)->setCheckState(CheckColumn,
                                           j == best ? Qt::Checked : Qt::Unchecked);
        }
    }
    updateStatus();
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
        m_progress->setFormat(tr("等待比对"));
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
        QMessageBox::information(this, tr("删除文件"), tr("请先选择或勾选要删除的视频。"));
        return;
    }
    if (QMessageBox::question(
            this, tr("删除文件"),
            tr("将从磁盘永久删除 %1 个视频文件，是否继续？").arg(items.size()))
        != QMessageBox::Yes) {
        return;
    }

    QVector<QTreeWidgetItem*> deleted;
    QStringList errors;
    for (QTreeWidgetItem* item : items) {
        const RowData* row = recordForItem(item);
        if (row == nullptr) {
            continue;
        }
        if (QFile::remove(row->path)) {
            deleted.append(item);
        } else {
            errors.append(row->path);
        }
    }
    removeItems(deleted);
    if (!errors.isEmpty()) {
        QMessageBox::warning(this, tr("删除文件"),
                             tr("以下 %1 个文件删除失败：\n%2")
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
    m_progress->setFormat(tr("等待比对"));
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
    m_compareButton->setEnabled(!busy && m_records.size() >= 2);
    m_clearButton->setEnabled(!busy);
    m_policy->setEnabled(!busy);
    m_selectAllButton->setEnabled(!busy);
    m_invertButton->setEnabled(!busy);
    m_removeButton->setEnabled(!busy);
    m_deleteButton->setEnabled(!busy);
    m_search->setEnabled(!busy);
}

KeepPolicy VideoBatchPage::currentPolicy() const
{
    return static_cast<KeepPolicy>(m_policy->currentData().toInt());
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
        QMessageBox::information(this, tr("开始比对"), tr("至少需要两个视频才能比对。"));
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
    m_worker = new VideoBatchWorker(paths, currentPolicy());
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
    m_progress->setFormat(tr("比对完成 · %1 个视频 · %2 · 重复组 %3 个 · 比对 %4 对")
                              .arg(outcome.clipCount)
                              .arg(formatutils::durationLabel(totalMs))
                              .arg(m_lastDuplicateGroups)
                              .arg(outcome.pairsCompared));
    if (outcome.failedCount > 0) {
        QMessageBox::warning(
            this, tr("部分视频无法读取"),
            tr("有 %1 个视频无法读取，已跳过：\n%2")
                .arg(outcome.failedCount)
                .arg(outcome.failures.join(QLatin1Char('\n'))));
    }
    updateStatus();
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
        tr("名称：%1\n分辨率：%2\n时长：%3\n大小：%4\n修改日期：%5\n文件位置：%6")
            .arg(row->name)
            .arg(row->measured ? QStringLiteral("%1x%2").arg(row->width).arg(row->height)
                               : tr("未读取"))
            .arg(row->measured ? formatDuration(row->duration) : tr("未读取"))
            .arg(humanSize(row->fileSize))
            .arg(row->modifiedText)
            .arg(row->path);
    QMessageBox box(this);
    box.setWindowTitle(tr("视频属性"));
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
    QAction* expandAction = menu.addAction(group->isExpanded() ? tr("折叠分组") : tr("展开分组"));
    menu.addSeparator();
    QAction* checkAllAction = menu.addAction(tr("全选分组内视频"));
    QAction* uncheckAllAction = menu.addAction(tr("取消全选分组内视频"));
    menu.addSeparator();
    QAction* openFirstAction = menu.addAction(tr("打开分组内第一个视频"));
    QAction* folderAction = menu.addAction(tr("打开所在文件夹"));
    menu.addSeparator();
    QAction* removeAction = menu.addAction(tr("移出该分组"));
    QAction* removeCheckedAction = menu.addAction(tr("移出分组内勾选项"));
    QAction* deleteCheckedAction = menu.addAction(tr("删除分组内勾选文件"));

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
    QAction* toggleAction = menu.addAction(tr("勾选 / 取消勾选"));
    QAction* invertAction = menu.addAction(tr("反选"));
    menu.addSeparator();
    QAction* openAction = menu.addAction(tr("打开视频"));
    QAction* copyAction = menu.addAction(tr("复制路径"));
    QAction* propertyAction = menu.addAction(tr("属性查看"));
    QAction* folderAction = menu.addAction(tr("打开文件所在文件夹"));
    menu.addSeparator();
    QAction* removeAction = menu.addAction(tr("移出列表"));
    QAction* deleteAction = menu.addAction(tr("删除文件"));
    menu.addSeparator();
    QAction* compareAction = menu.addAction(tr("开始比对勾选视频"));

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
    QString text = tr("共 %1 个视频，已勾选 %2 个").arg(m_records.size()).arg(checkedItems().size());
    if (m_lastClipCount > 0) {
        text += tr("，已比对 %1 个，重复组 %2 个")
                    .arg(m_lastClipCount)
                    .arg(m_lastDuplicateGroups);
    }
    // Clips added after a comparison are listed but not compared yet; saying so
    // avoids the impression that the results already cover them.
    const int pending = unlistedPathCount();
    if (pending > 0) {
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
