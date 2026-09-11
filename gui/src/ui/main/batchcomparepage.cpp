#include "batchcomparepage.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
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

namespace {

constexpr int kRecordIdRole = Qt::UserRole + 1;
constexpr int kGroupBaseRole = Qt::UserRole + 2;

QStringList imageWildcards()
{
    return {
        QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
        QStringLiteral("*.bmp"), QStringLiteral("*.webp"),
    };
}

QStringList imageExtensions()
{
    return {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("webp"),
    };
}

bool isSupportedImageFile(const QString& path)
{
    return imageExtensions().contains(QFileInfo(path).suffix().toLower());
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

QString fileTypeName(const QString& suffix)
{
    const QString upper = suffix.toUpper();
    if (upper == QStringLiteral("JPG") || upper == QStringLiteral("JPEG")) {
        return QStringLiteral("JPEG");
    }
    return upper.isEmpty() ? QStringLiteral("未知") : upper;
}

} // namespace

BatchComparePage::BatchComparePage(QWidget* parent)
    : QWidget(parent)
    , m_tree(new QTreeWidget(this))
    , m_progress(new QProgressBar(this))
    , m_policy(new QComboBox(this))
    , m_search(new QLineEdit(this))
    , m_invertButton(nullptr)
    , m_removeButton(nullptr)
    , m_deleteButton(nullptr)
    , m_compareButton(nullptr)
    , m_status(new QLabel(this))
    , m_worker(nullptr)
    , m_nextRecordId(1)
    , m_hasCompared(false)
    , m_comparing(false)
{
    qRegisterMetaType<QVector<BatchCluster>>("QVector<BatchCluster>");

    setAcceptDrops(true);
    m_baseHeaders = {
        tr("选中"), tr("缩略图"), tr("名称"), tr("分辨率"), tr("类型"),
        tr("大小"), tr("修改日期"), tr("文件位置"), tr("位深度"),
    };

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 16, 24, 16);
    root->setSpacing(10);

    QHBoxLayout* header = new QHBoxLayout;
    QPushButton* backButton = new QPushButton(tr("返回功能选择"), this);
    backButton->setObjectName(QStringLiteral("secondaryButton"));
    QLabel* title = new QLabel(tr("批量图片比对"), this);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;color:#1f2937;"));
    header->addWidget(backButton);
    header->addSpacing(10);
    header->addWidget(title);
    header->addStretch(1);
    root->addLayout(header);
    connect(backButton, &QPushButton::released, this, &BatchComparePage::backRequested);

    QPushButton* addFolderButton = new QPushButton(tr("选择文件夹"), this);
    addFolderButton->setObjectName(QStringLiteral("secondaryButton"));
    QPushButton* addImagesButton = new QPushButton(tr("选择图片"), this);
    addImagesButton->setObjectName(QStringLiteral("secondaryButton"));
    m_invertButton = new QPushButton(tr("反选"), this);
    m_invertButton->setObjectName(QStringLiteral("secondaryButton"));
    m_removeButton = new QPushButton(tr("移出勾选项"), this);
    m_removeButton->setObjectName(QStringLiteral("secondaryButton"));
    m_deleteButton = new QPushButton(tr("删除勾选文件"), this);
    m_deleteButton->setObjectName(QStringLiteral("secondaryButton"));
    m_compareButton = new QPushButton(tr("开始比对"), this);
    m_compareButton->setObjectName(QStringLiteral("primaryButton"));

    connect(addFolderButton, &QPushButton::released, this, &BatchComparePage::chooseFolder);
    connect(addImagesButton, &QPushButton::released, this, &BatchComparePage::chooseImages);
    connect(m_invertButton, &QPushButton::released, this, &BatchComparePage::invertChecked);
    connect(m_removeButton, &QPushButton::released, this, &BatchComparePage::removeChecked);
    connect(m_deleteButton, &QPushButton::released, this, &BatchComparePage::deleteCheckedFiles);
    connect(m_compareButton, &QPushButton::released, this, &BatchComparePage::startCompare);

    QHBoxLayout* controls = new QHBoxLayout;
    controls->addWidget(addFolderButton);
    controls->addWidget(addImagesButton);
    controls->addWidget(m_invertButton);
    controls->addSpacing(8);
    controls->addWidget(m_removeButton);
    controls->addWidget(m_deleteButton);
    controls->addStretch(1);
    controls->addWidget(new QLabel(tr("勾选策略"), this));
    m_policy->addItem(tr("分辨率最高"), static_cast<int>(SelectionPolicy::HighestResolution));
    m_policy->addItem(tr("分辨率最低"), static_cast<int>(SelectionPolicy::LowestResolution));
    m_policy->addItem(tr("文件大小最大"), static_cast<int>(SelectionPolicy::LargestFile));
    m_policy->addItem(tr("文件大小最小"), static_cast<int>(SelectionPolicy::SmallestFile));
    m_policy->addItem(tr("修改日期最近"), static_cast<int>(SelectionPolicy::NewestModified));
    m_policy->addItem(tr("修改日期最远"), static_cast<int>(SelectionPolicy::OldestModified));
    m_policy->addItem(tr("位深度最高"), static_cast<int>(SelectionPolicy::HighestBitDepth));
    m_policy->addItem(tr("位深度最低"), static_cast<int>(SelectionPolicy::LowestBitDepth));
    m_policy->addItem(tr("不取消勾选"), static_cast<int>(SelectionPolicy::KeepAll));
    m_policy->setCurrentIndex(0);
    controls->addWidget(m_policy);
    root->addLayout(controls);

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
    connect(m_search, &QLineEdit::textChanged, this, &BatchComparePage::searchChanged);

    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels(m_baseHeaders);
    m_tree->setRootIsDecorated(true);
    m_tree->setItemsExpandable(true);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setIndentation(18);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setIconSize(QSize(72, 54));
    m_tree->setUniformRowHeights(false);
    m_tree->header()->setSectionsClickable(true);
    m_tree->header()->setSectionsMovable(true);
    m_tree->header()->setSortIndicatorShown(false);
    m_tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setToolTip(tr(
        "单击：升序 / 降序 / 取消排序\n"
        "Ctrl+单击：添加次级排序\n"
        "右键表头：显示或隐藏列，也可拖动表头调整顺序"));
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->resizeSection(CheckColumn, 42);
    m_tree->header()->resizeSection(ThumbnailColumn, 82);
    m_tree->header()->resizeSection(NameColumn, 190);
    m_tree->header()->resizeSection(ResolutionColumn, 92);
    m_tree->header()->resizeSection(TypeColumn, 68);
    m_tree->header()->resizeSection(SizeColumn, 96);
    m_tree->header()->resizeSection(ModifiedColumn, 142);
    m_tree->header()->resizeSection(PathColumn, 310);
    m_tree->header()->resizeSection(DepthColumn, 72);

    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &BatchComparePage::showTreeContextMenu);
    connect(m_tree->header(), &QHeaderView::customContextMenuRequested,
            this, &BatchComparePage::showHeaderContextMenu);
    connect(m_tree->header(), &QHeaderView::sectionClicked,
            this, &BatchComparePage::handleHeaderClicked);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int) {
        updateStatus();
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &BatchComparePage::openItem);

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

    updateStatus();
}

void BatchComparePage::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("选择图片文件夹"));
    if (folder.isEmpty()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList paths;
    QDirIterator iterator(folder, imageWildcards(), QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        paths.append(iterator.next());
    }
    addImagePaths(paths);
    QApplication::restoreOverrideCursor();
}

void BatchComparePage::chooseImages()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("选择图片"), QString(),
        tr("图片 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*)"));
    addImagePaths(paths);
}

void BatchComparePage::invertChecked()
{
    const QVector<QTreeWidgetItem*> items = allChildItems();
    for (QTreeWidgetItem* item : items) {
        item->setCheckState(CheckColumn,
                            item->checkState(CheckColumn) == Qt::Checked ? Qt::Unchecked
                                                                         : Qt::Checked);
    }
    updateStatus();
}

void BatchComparePage::removeChecked()
{
    removeItems(checkedItems());
}

void BatchComparePage::deleteCheckedFiles()
{
    deleteFiles(checkedItems());
}

void BatchComparePage::startCompare()
{
    if (m_comparing) {
        return;
    }
    if (m_thread && m_thread->isRunning()) {
        return;
    }

    const QVector<QTreeWidgetItem*> checked = checkedItems();
    if (checked.size() < 2) {
        QMessageBox::information(
            this, tr("开始比对"), tr("至少需要勾选两张图片才能比对。"));
        return;
    }

    m_compareRecordIds.clear();
    QVector<QString> paths;
    for (QTreeWidgetItem* item : checked) {
        const int id = item->data(CheckColumn, kRecordIdRole).toInt();
        if (m_records.contains(id)) {
            m_compareRecordIds.append(id);
            paths.append(m_records.value(id).path);
        }
    }
    if (paths.size() < 2) {
        return;
    }

    m_comparing = true;
    m_compareButton->setEnabled(false);
    m_invertButton->setEnabled(false);
    m_removeButton->setEnabled(false);
    m_deleteButton->setEnabled(false);
    m_search->setEnabled(false);

    m_worker = new BatchCompareWorker(paths, 0.78);
    m_thread = new QThread(this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &BatchCompareWorker::run);
    connect(m_worker, &BatchCompareWorker::progressChanged,
            this, &BatchComparePage::onProgressChanged, Qt::QueuedConnection);
    connect(m_worker, &BatchCompareWorker::finished,
            this, &BatchComparePage::compareFinished, Qt::QueuedConnection);
    connect(m_worker, &BatchCompareWorker::failed,
            this, &BatchComparePage::compareFailed, Qt::QueuedConnection);
    connect(m_thread, &QThread::finished,
            this, &BatchComparePage::onCompareThreadFinished, Qt::QueuedConnection);
    connect(m_worker, &BatchCompareWorker::finished, m_thread, &QThread::quit);
    connect(m_worker, &BatchCompareWorker::failed, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);

    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(tr("比对中..."));
    m_thread->start();
}

void BatchComparePage::compareFinished(QVector<BatchCluster> clusters)
{
    rebuildGroupedTree(std::move(clusters));
    m_compareRecordIds.clear();
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    m_progress->setFormat(tr("比对完成"));
}

void BatchComparePage::compareFailed(const QString& message)
{
    m_progress->setFormat(tr("比对失败"));
    QMessageBox::warning(this, tr("比对失败"), message);
}

void BatchComparePage::onProgressChanged(int done, int total)
{
    if (total <= 0) {
        return;
    }
    m_progress->setRange(0, total);
    m_progress->setValue(done);
    m_progress->setFormat(QStringLiteral("%p%"));
}

void BatchComparePage::onCompareThreadFinished()
{
    m_comparing = false;
    m_invertButton->setEnabled(true);
    m_removeButton->setEnabled(true);
    m_deleteButton->setEnabled(true);
    m_compareButton->setEnabled(true);
    m_search->setEnabled(true);
    updateStatus();
}

void BatchComparePage::addImagePaths(const QStringList& paths)
{
    int added = 0;
    for (const QString& path : paths) {
        int recordId = -1;
        if (isSupportedImageFile(path) && addImagePath(path, recordId)) {
            addRecordToDisplay(recordId);
            ++added;
        }
    }

    if (added > 0) {
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
        m_progress->setFormat(tr("已加入 %1 张，等待比对").arg(added));
        applySort();
        refreshGroupLabels();
    }
    updateStatus();
}

bool BatchComparePage::addImagePath(const QString& path, int& recordId)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        return false;
    }
    for (const RowData& row : m_records) {
        if (QString::compare(row.path, canonical, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }

    const QImage image(path);
    if (image.isNull()) {
        return false;
    }

    RowData row;
    row.id = m_nextRecordId++;
    row.path = canonical;
    row.name = info.fileName();
    row.type = fileTypeName(info.suffix());
    row.resolution = QStringLiteral("%1x%2").arg(image.width()).arg(image.height());
    row.fileSize = info.size();
    row.sizeText = humanSize(row.fileSize);
    row.modified = info.lastModified();
    row.modifiedText = row.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    row.bitDepth = QStringLiteral("%1 位").arg(image.depth());
    row.width = image.width();
    row.height = image.height();
    row.depth = image.depth();
    row.thumbnail = QPixmap::fromImage(
        image.scaled(72, 54, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    recordId = row.id;
    m_records.insert(recordId, row);
    return true;
}

QTreeWidgetItem* BatchComparePage::createChildItem(int recordId, bool checked)
{
    const auto iterator = m_records.constFind(recordId);
    if (iterator == m_records.constEnd()) {
        return nullptr;
    }
    const RowData& row = iterator.value();

    QTreeWidgetItem* item = new QTreeWidgetItem;
    item->setData(CheckColumn, kRecordIdRole, recordId);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(CheckColumn, checked ? Qt::Checked : Qt::Unchecked);
    if (!row.thumbnail.isNull()) {
        item->setIcon(ThumbnailColumn, QIcon(row.thumbnail));
    }
    item->setText(NameColumn, row.name);
    item->setText(ResolutionColumn, row.resolution);
    item->setText(TypeColumn, row.type);
    item->setText(SizeColumn, row.sizeText);
    item->setText(ModifiedColumn, row.modifiedText);
    item->setText(PathColumn, row.path);
    item->setText(DepthColumn, row.bitDepth);
    item->setSizeHint(ThumbnailColumn, QSize(72, 58));
    item->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
    item->setTextAlignment(DepthColumn, Qt::AlignRight | Qt::AlignVCenter);
    item->setToolTip(PathColumn, row.path);
    return item;
}

QTreeWidgetItem* BatchComparePage::createGroup(const QString& baseLabel)
{
    QTreeWidgetItem* group = new QTreeWidgetItem(m_tree);
    group->setData(CheckColumn, kGroupBaseRole, baseLabel);
    group->setFirstColumnSpanned(true);
    group->setFlags(Qt::ItemIsEnabled);
    group->setText(CheckColumn, baseLabel);
    group->setExpanded(true);
    group->setBackground(CheckColumn, QBrush(QColor(232, 237, 243)));
    group->setForeground(CheckColumn, QBrush(QColor(51, 65, 85)));
    QFont font = group->font(CheckColumn);
    font.setBold(true);
    group->setFont(CheckColumn, font);
    return group;
}

QTreeWidgetItem* BatchComparePage::ensureAddGroup()
{
    const QString wanted = m_hasCompared ? tr("新加入") : tr("待比对");
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* group = m_tree->topLevelItem(i);
        if (group->data(CheckColumn, kGroupBaseRole).toString() == wanted) {
            return group;
        }
    }
    return createGroup(wanted);
}

void BatchComparePage::addRecordToDisplay(int recordId)
{
    QTreeWidgetItem* group = ensureAddGroup();
    group->addChild(createChildItem(recordId, true));
    sortGroupChildren(group);
}

void BatchComparePage::rebuildGroupedTree(QVector<BatchCluster> clusters)
{
    m_tree->clear();
    QVector<int> singles;

    for (const BatchCluster& cluster : clusters) {
        QVector<int> ids;
        for (const int localRow : cluster.rows) {
            if (localRow >= 0 && localRow < m_compareRecordIds.size()) {
                ids.append(m_compareRecordIds.at(localRow));
            }
        }
        if (ids.isEmpty()) {
            continue;
        }
        if (!cluster.duplicate) {
            singles += ids;
            continue;
        }

        const SelectionPolicy policy = currentPolicy();
        const int winner = policy == SelectionPolicy::KeepAll ? -1 : chooseWinnerId(ids);
        QTreeWidgetItem* group = createGroup(
            tr("相似组 · 相似度 %1%").arg(cluster.similarity * 100.0, 0, 'f', 1));
        for (const int id : ids) {
            group->addChild(createChildItem(id, policy == SelectionPolicy::KeepAll || id == winner));
        }
        sortGroupChildren(group);
    }

    if (!singles.isEmpty()) {
        QTreeWidgetItem* group = createGroup(tr("无相同图片"));
        for (const int id : singles) {
            group->addChild(createChildItem(id, true));
        }
        sortGroupChildren(group);
    }

    m_hasCompared = true;
    refreshGroupLabels();
    updateHeaderLabels();
    applySort();
    updateStatus();
}

void BatchComparePage::refreshGroupLabels()
{
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* group = m_tree->topLevelItem(i);
        const int count = group->childCount();
        if (count == 0) {
            delete m_tree->takeTopLevelItem(i);
            continue;
        }
        const QString base = group->data(CheckColumn, kGroupBaseRole).toString();
        group->setText(CheckColumn, tr("%1 · %2 张").arg(base).arg(count));
    }
}

QVector<QTreeWidgetItem*> BatchComparePage::allChildItems() const
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

QVector<QTreeWidgetItem*> BatchComparePage::checkedItems() const
{
    QVector<QTreeWidgetItem*> items;
    for (QTreeWidgetItem* item : allChildItems()) {
        if (item->checkState(CheckColumn) == Qt::Checked) {
            items.append(item);
        }
    }
    return items;
}

QVector<QTreeWidgetItem*> BatchComparePage::selectedChildItems() const
{
    QVector<QTreeWidgetItem*> items;
    for (QTreeWidgetItem* item : m_tree->selectedItems()) {
        if (recordForItem(item)) {
            items.append(item);
        }
    }
    return items;
}

QVector<QTreeWidgetItem*> BatchComparePage::contextTargetItems(
    QTreeWidgetItem* clicked) const
{
    QVector<QTreeWidgetItem*> items = selectedChildItems();
    if (!items.isEmpty()) {
        return items;
    }
    items = checkedItems();
    if (!items.isEmpty()) {
        return items;
    }
    if (clicked && recordForItem(clicked)) {
        items.append(clicked);
    }
    return items;
}

void BatchComparePage::removeItems(const QVector<QTreeWidgetItem*>& items)
{
    for (QTreeWidgetItem* item : items) {
        const int id = item->data(CheckColumn, kRecordIdRole).toInt();
        if (id > 0) {
            m_records.remove(id);
        }
        delete item;
    }
    refreshGroupLabels();
    applySort();
    updateStatus();
}

void BatchComparePage::setItemsChecked(const QVector<QTreeWidgetItem*>& items, bool checked)
{
    for (QTreeWidgetItem* item : items) {
        item->setCheckState(CheckColumn, checked ? Qt::Checked : Qt::Unchecked);
    }
    updateStatus();
}

void BatchComparePage::deleteFiles(const QVector<QTreeWidgetItem*>& items)
{
    if (items.isEmpty()) {
        QMessageBox::information(this, tr("删除文件"), tr("请先选择或勾选要删除的图片。"));
        return;
    }

    if (QMessageBox::question(
            this, tr("删除文件"),
            tr("将从磁盘永久删除 %1 个图片文件，是否继续？").arg(items.size()))
        != QMessageBox::Yes) {
        return;
    }

    QVector<QTreeWidgetItem*> deleted;
    QStringList errors;
    for (QTreeWidgetItem* item : items) {
        const RowData* row = recordForItem(item);
        if (row && QFile::remove(row->path)) {
            deleted.append(item);
        } else if (row) {
            errors.append(row->path);
        }
    }
    removeItems(deleted);
    if (!errors.isEmpty()) {
        QMessageBox::warning(
            this, tr("删除文件"),
            tr("以下 %1 个文件删除失败：\n%2").arg(errors.size()).arg(errors.join('\n')));
    }
}

int BatchComparePage::chooseWinnerId(const QVector<int>& recordIds) const
{
    if (recordIds.isEmpty()) {
        return -1;
    }
    const SelectionPolicy policy = currentPolicy();
    if (policy == SelectionPolicy::KeepAll) {
        return -1;
    }

    const bool highest =
        policy == SelectionPolicy::HighestResolution
        || policy == SelectionPolicy::LargestFile
        || policy == SelectionPolicy::NewestModified
        || policy == SelectionPolicy::HighestBitDepth;

    int winner = recordIds.first();
    qint64 winnerValue = policyValue(m_records.value(winner));
    for (const int id : recordIds) {
        const qint64 value = policyValue(m_records.value(id));
        if ((highest && value > winnerValue) || (!highest && value < winnerValue)) {
            winner = id;
            winnerValue = value;
        }
    }

    // Exact copies may differ only by name. In that case the most recently
    // modified file wins, regardless of the selected primary policy.
    const RowData& winningRow = m_records.value(winner);
    for (const int id : recordIds) {
        const RowData& row = m_records.value(id);
        if (id != winner && policyValue(row) == winnerValue
            && row.modified > winningRow.modified) {
            winner = id;
        }
    }
    return winner;
}

qint64 BatchComparePage::policyValue(const RowData& row) const
{
    switch (currentPolicy()) {
        case SelectionPolicy::LowestResolution:
        case SelectionPolicy::HighestResolution:
            return static_cast<qint64>(row.width) * row.height;
        case SelectionPolicy::SmallestFile:
        case SelectionPolicy::LargestFile:
            return row.fileSize;
        case SelectionPolicy::OldestModified:
        case SelectionPolicy::NewestModified:
            return row.modified.toMSecsSinceEpoch();
        case SelectionPolicy::LowestBitDepth:
        case SelectionPolicy::HighestBitDepth:
            return row.depth;
        case SelectionPolicy::KeepAll:
            return 0;
    }
    return 0;
}

BatchComparePage::SelectionPolicy BatchComparePage::currentPolicy() const
{
    return static_cast<SelectionPolicy>(m_policy->currentData().toInt());
}

const BatchComparePage::RowData* BatchComparePage::recordForItem(
    const QTreeWidgetItem* item) const
{
    if (!item) {
        return nullptr;
    }
    const int id = item->data(CheckColumn, kRecordIdRole).toInt();
    const auto iterator = m_records.constFind(id);
    return iterator == m_records.constEnd() ? nullptr : &iterator.value();
}

BatchComparePage::RowData* BatchComparePage::recordForItem(QTreeWidgetItem* item)
{
    return const_cast<RowData*>(
        static_cast<const BatchComparePage*>(this)->recordForItem(item));
}

bool BatchComparePage::matchesSearch(const RowData& row, const QString& text) const
{
    return row.name.contains(text, Qt::CaseInsensitive)
           || row.path.contains(text, Qt::CaseInsensitive);
}

void BatchComparePage::showTreeContextMenu(const QPoint& position)
{
    QTreeWidgetItem* clicked = m_tree->itemAt(position);
    if (clicked && recordForItem(clicked) && !clicked->isSelected()) {
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
    QAction* removeAction = menu.addAction(tr("移出列表"));
    QAction* deleteAction = menu.addAction(tr("删除文件"));
    menu.addSeparator();
    QAction* copyAction = menu.addAction(tr("复制"));
    QAction* propertyAction = menu.addAction(tr("属性查看"));
    QAction* folderAction = menu.addAction(tr("打开文件所在文件夹"));
    menu.addSeparator();
    QAction* compareAction = menu.addAction(tr("开始比对勾选图片"));

    QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(position));
    if (chosen == toggleAction) {
        bool anyChecked = false;
        for (QTreeWidgetItem* item : target) {
            anyChecked = anyChecked || item->checkState(CheckColumn) == Qt::Checked;
        }
        setItemsChecked(target, !anyChecked);
    } else if (chosen == invertAction) {
        invertChecked();
    } else if (chosen == removeAction) {
        removeItems(target);
    } else if (chosen == deleteAction) {
        deleteFiles(target);
    } else if (chosen == copyAction) {
        copyPaths(target);
    } else if (chosen == propertyAction) {
        showProperties(target);
    } else if (chosen == folderAction) {
        openContainingFolder(target);
    } else if (chosen == compareAction) {
        startCompare();
    }
}

void BatchComparePage::showHeaderContextMenu(const QPoint& position)
{
    QMenu menu(this);
    for (int column = ThumbnailColumn; column < ColumnCount; ++column) {
        QAction* action = menu.addAction(m_baseHeaders.at(column));
        action->setCheckable(true);
        action->setChecked(!m_tree->header()->isSectionHidden(column));
        connect(action, &QAction::triggered, this,
                [this, column, action]() {
                    const bool visible = action->isChecked();
                    m_tree->header()->setSectionHidden(column, !visible);
                    if (!visible) {
                        m_sortRules.erase(
                            std::remove_if(m_sortRules.begin(), m_sortRules.end(),
                                           [column](const SortRule& rule) {
                                               return rule.column == column;
                                           }),
                            m_sortRules.end());
                    }
                    updateHeaderLabels();
                    applySort();
                });
    }
    menu.exec(m_tree->header()->mapToGlobal(position));
}

void BatchComparePage::handleHeaderClicked(int column)
{
    if (column < NameColumn || column >= ColumnCount) {
        return;
    }
    const bool multiColumn = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
    int index = -1;
    for (int i = 0; i < m_sortRules.size(); ++i) {
        if (m_sortRules.at(i).column == column) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        if (!multiColumn) {
            m_sortRules.clear();
        }
        m_sortRules.append({column, Qt::AscendingOrder});
    } else if (m_sortRules.at(index).order == Qt::AscendingOrder) {
        if (multiColumn) {
            m_sortRules[index].order = Qt::DescendingOrder;
        } else {
            m_sortRules.clear();
            m_sortRules.append({column, Qt::DescendingOrder});
        }
    } else if (multiColumn) {
        m_sortRules.removeAt(index);
    } else {
        m_sortRules.clear();
    }

    updateHeaderLabels();
    applySort();
}

void BatchComparePage::openSelected()
{
    QVector<QTreeWidgetItem*> items = selectedChildItems();
    if (items.isEmpty()) {
        items = checkedItems();
    }
    if (!items.isEmpty()) {
        const RowData* row = recordForItem(items.first());
        if (row) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(row->path));
        }
    }
}

void BatchComparePage::openItem(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);
    const RowData* row = recordForItem(item);
    if (row) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(row->path));
    }
}

void BatchComparePage::searchChanged(const QString& text)
{
    m_tree->clearSelection();
    const QString query = text.trimmed();
    if (query.isEmpty()) {
        return;
    }

    for (QTreeWidgetItem* item : allChildItems()) {
        const RowData* row = recordForItem(item);
        if (row && matchesSearch(*row, query)) {
            item->setSelected(true);
            m_tree->setCurrentItem(item);
            item->parent()->setExpanded(true);
            m_tree->scrollToItem(item);
            break;
        }
    }
}

void BatchComparePage::updateStatus()
{
    QStringList sortLabels;
    for (const SortRule& rule : m_sortRules) {
        sortLabels.append(QStringLiteral("%1 %2")
                              .arg(m_baseHeaders.value(rule.column))
                              .arg(rule.order == Qt::AscendingOrder ? QStringLiteral("↑")
                                                                    : QStringLiteral("↓")));
    }
    const QString sortText = sortLabels.isEmpty() ? tr("无") : sortLabels.join(QStringLiteral(" > "));
    m_status->setText(tr("共 %1 张图片，已勾选 %2 张，排序：%3，勾选策略：%4")
                          .arg(m_records.size())
                          .arg(checkedItems().size())
                          .arg(sortText)
                          .arg(m_policy->currentText()));
}

void BatchComparePage::updateHeaderLabels()
{
    for (int column = 0; column < ColumnCount; ++column) {
        QString label = m_baseHeaders.value(column);
        for (int i = 0; i < m_sortRules.size(); ++i) {
            if (m_sortRules.at(i).column == column) {
                label += QStringLiteral(" [%1%2]")
                             .arg(i + 1)
                             .arg(m_sortRules.at(i).order == Qt::AscendingOrder
                                      ? QStringLiteral("↑")
                                      : QStringLiteral("↓"));
                break;
            }
        }
        m_tree->headerItem()->setText(column, label);
    }

    if (m_sortRules.isEmpty()) {
        m_tree->header()->setSortIndicatorShown(false);
    } else {
        m_tree->header()->setSortIndicatorShown(true);
        m_tree->header()->setSortIndicator(m_sortRules.first().column,
                                           m_sortRules.first().order);
    }
}

void BatchComparePage::applySort()
{
    if (m_sortRules.isEmpty()) {
        return;
    }
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        sortGroupChildren(m_tree->topLevelItem(i));
    }
}

void BatchComparePage::sortGroupChildren(QTreeWidgetItem* group)
{
    if (!group || m_sortRules.isEmpty()) {
        return;
    }
    QList<QTreeWidgetItem*> children = group->takeChildren();
    std::stable_sort(children.begin(), children.end(),
                     [this](const QTreeWidgetItem* left, const QTreeWidgetItem* right) {
                         const RowData* leftData = recordForItem(left);
                         const RowData* rightData = recordForItem(right);
                         if (!leftData || !rightData) {
                             return false;
                         }
                         return compareRecords(*leftData, *rightData) < 0;
                     });
    group->addChildren(children);
}

int BatchComparePage::compareRecords(const RowData& left, const RowData& right) const
{
    for (const SortRule& rule : m_sortRules) {
        int result = 0;
        switch (rule.column) {
            case NameColumn:
                result = compareStrings(left.name, right.name, rule.order);
                break;
            case ResolutionColumn:
                result = compareValues(static_cast<qint64>(left.width) * left.height,
                                       static_cast<qint64>(right.width) * right.height,
                                       rule.order);
                if (result == 0) {
                    result = compareValues(left.width, right.width, rule.order);
                }
                break;
            case TypeColumn:
                result = compareStrings(left.type, right.type, rule.order);
                break;
            case SizeColumn:
                result = compareValues(left.fileSize, right.fileSize, rule.order);
                break;
            case ModifiedColumn:
                result = compareValues(left.modified.toMSecsSinceEpoch(),
                                       right.modified.toMSecsSinceEpoch(), rule.order);
                break;
            case PathColumn:
                result = compareStrings(left.path, right.path, rule.order);
                break;
            case DepthColumn:
                result = compareValues(left.depth, right.depth, rule.order);
                break;
            default:
                break;
        }
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

int BatchComparePage::compareValues(qint64 left, qint64 right, Qt::SortOrder order) const
{
    if (left == right) {
        return 0;
    }
    return order == Qt::AscendingOrder ? (left < right ? -1 : 1) : (left > right ? -1 : 1);
}

int BatchComparePage::compareStrings(const QString& left, const QString& right,
                                     Qt::SortOrder order) const
{
    const int result = QString::compare(left, right, Qt::CaseInsensitive);
    if (result == 0) {
        return 0;
    }
    return order == Qt::AscendingOrder ? result : -result;
}

void BatchComparePage::copyPaths(const QVector<QTreeWidgetItem*>& items)
{
    QStringList paths;
    QList<QUrl> urls;
    for (QTreeWidgetItem* item : items) {
        const RowData* row = recordForItem(item);
        if (row) {
            paths.append(row->path);
            urls.append(QUrl::fromLocalFile(row->path));
        }
    }
    if (!urls.isEmpty()) {
        QMimeData* mime = new QMimeData;
        mime->setUrls(urls);
        mime->setText(paths.join('\n'));
        QApplication::clipboard()->setMimeData(mime);
    }
}

void BatchComparePage::showProperties(const QVector<QTreeWidgetItem*>& items)
{
    if (items.isEmpty()) {
        return;
    }
    const RowData* row = recordForItem(items.first());
    if (!row) {
        return;
    }
    const QString details =
        tr("名称：%1\n分辨率：%2\n类型：%3\n大小：%4\n修改日期：%5\n文件位置：%6\n位深度：%7")
            .arg(row->name)
            .arg(row->resolution)
            .arg(row->type)
            .arg(row->sizeText)
            .arg(row->modifiedText)
            .arg(row->path)
            .arg(row->bitDepth);
    QMessageBox box(this);
    box.setWindowTitle(tr("图片属性"));
    box.setIcon(QMessageBox::NoIcon);
    box.setText(details);
    box.exec();
}

void BatchComparePage::openContainingFolder(const QVector<QTreeWidgetItem*>& items)
{
    if (items.isEmpty()) {
        return;
    }
    const RowData* row = recordForItem(items.first());
    if (row) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(row->path).absolutePath()));
    }
}

void BatchComparePage::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (mime && mime->hasUrls()) {
        for (const QUrl& url : mime->urls()) {
            if (!url.isLocalFile()) {
                continue;
            }
            const QString path = url.toLocalFile();
            if (QFileInfo(path).isDir() || isSupportedImageFile(path)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void BatchComparePage::dropEvent(QDropEvent* event)
{
    QStringList paths;
    const QMimeData* mime = event->mimeData();
    if (mime && mime->hasUrls()) {
        for (const QUrl& url : mime->urls()) {
            if (!url.isLocalFile()) {
                continue;
            }
            const QString path = url.toLocalFile();
            if (QFileInfo(path).isDir()) {
                QDirIterator iterator(path, imageWildcards(), QDir::Files,
                                      QDirIterator::Subdirectories);
                while (iterator.hasNext()) {
                    paths.append(iterator.next());
                }
            } else if (isSupportedImageFile(path)) {
                paths.append(path);
            }
        }
    }
    if (!paths.isEmpty()) {
        addImagePaths(paths);
        event->acceptProposedAction();
    }
}

bool BatchComparePage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_tree && watched != m_tree->viewport()) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::DragEnter) {
        auto* dragEvent = static_cast<QDragEnterEvent*>(event);
        dragEnterEvent(dragEvent);
        return dragEvent->isAccepted();
    }
    if (event->type() == QEvent::DragMove) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::Drop) {
        dropEvent(static_cast<QDropEvent*>(event));
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->matches(QKeySequence::Find)) {
            m_search->show();
            m_search->setFocus();
            m_search->selectAll();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            openSelected();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Delete) {
            const QVector<QTreeWidgetItem*> selected = selectedChildItems();
            removeItems(selected.isEmpty() ? checkedItems() : selected);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
