#ifndef BATCHCOMPAREPAGE_H
#define BATCHCOMPAREPAGE_H

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "batchcompareworker.h"

class QComboBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QThread;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class BatchComparePage : public QWidget
{
    Q_OBJECT

public:
    explicit BatchComparePage(QWidget* parent = nullptr);

signals:
    void backRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

public:
    enum Column {
        CheckColumn = 0,
        ThumbnailColumn,
        NameColumn,
        ResolutionColumn,
        TypeColumn,
        SizeColumn,
        ModifiedColumn,
        PathColumn,
        DepthColumn,
        ColumnCount,
    };

private:
    enum class SelectionPolicy {
        HighestResolution = 0,
        LowestResolution,
        LargestFile,
        SmallestFile,
        NewestModified,
        OldestModified,
        HighestBitDepth,
        LowestBitDepth,
        KeepAll,
    };

    class BatchItemDelegate;

    // Entries of the group (top-level row) context menu.
    enum class GroupAction {
        ToggleExpand = 0,
        ExpandAll,
        CollapseAll,
        CheckAll,
        UncheckAll,
        InvertChecked,
        KeepOnlyBest,
        OpenAll,
        OpenFolder,
        RemoveGroup,
        RemoveChecked,
        DeleteChecked,
        Compare,
    };

    struct SortRule {
        int column = NameColumn;
        Qt::SortOrder order = Qt::AscendingOrder;
    };

    struct RowData {
        int id = -1;
        QString path;
        QString name;
        QString type;
        QString resolution;
        QString sizeText;
        QString modifiedText;
        QString bitDepth;
        qint64 fileSize = 0;
        int width = 0;
        int height = 0;
        int depth = 0;
        QDateTime modified;
        QImage thumbnail;
    };

private slots:
    void chooseFolder();
    void chooseImages();
    void invertChecked();
    void removeChecked();
    void deleteCheckedFiles();
    void startCompare();
    void compareFinished(QVector<BatchCluster> clusters, qint64 elapsedMs);
    void compareFailed(const QString& message);
    void onProgressChanged(int done, int total);
    void onCompareThreadFinished();
    void onSectionResized(int column, int oldSize, int newSize);
    void showTreeContextMenu(const QPoint& position);
    void showGroupContextMenu(QTreeWidgetItem* group, const QPoint& position);
    void showHeaderContextMenu(const QPoint& position);
    void handleHeaderClicked(int column);
    void openSelected();
    void openItem(QTreeWidgetItem* item, int column);
    void searchChanged(const QString& text);

private:
#ifdef VIVIDMATCH_TEST_HOOKS
    // Only compiled when a test harness asks for it; the file dialog is native
    // and cannot be driven from an automated run.
    friend class BatchComparePageTestHook;
#endif

    void addImagePaths(const QStringList& paths);
    bool addImagePath(const QString& path, int& recordId);
    void updateThumbnails();
    void updateMinimumColumnWidth();
    void enforceCheckColumnMinimum();
    QTreeWidgetItem* createChildItem(int recordId, bool checked);
    QTreeWidgetItem* ensureAddGroup();
    QTreeWidgetItem* createGroup(const QString& baseLabel);
    void addRecordToDisplay(int recordId);
    void rebuildGroupedTree(QVector<BatchCluster> clusters);
    void refreshGroupLabels();
    QVector<QTreeWidgetItem*> allChildItems() const;
    bool isGroupItem(const QTreeWidgetItem* item) const;
    QVector<QTreeWidgetItem*> groupChildren(const QTreeWidgetItem* group) const;
    void buildGroupMenu(QMenu& menu, const QTreeWidgetItem* group) const;
    void applyGroupAction(QTreeWidgetItem* group, int actionId);
    void setAllGroupsExpanded(bool expanded);
    void keepOnlyBest(QTreeWidgetItem* group);
    QVector<QTreeWidgetItem*> checkedItems() const;
    QVector<QTreeWidgetItem*> selectedChildItems() const;
    QVector<QTreeWidgetItem*> contextTargetItems(QTreeWidgetItem* clicked) const;
    void removeItems(const QVector<QTreeWidgetItem*>& items);
    void setItemsChecked(const QVector<QTreeWidgetItem*>& items, bool checked);
    void deleteFiles(const QVector<QTreeWidgetItem*>& items);
    int chooseWinnerId(const QVector<int>& recordIds) const;
    qint64 policyValue(const RowData& row) const;
    SelectionPolicy currentPolicy() const;
    const RowData* recordForItem(const QTreeWidgetItem* item) const;
    RowData* recordForItem(QTreeWidgetItem* item);
    bool matchesSearch(const RowData& row, const QString& text) const;
    void updateStatus();
    void updateHeaderLabels();
    void updateSortForColumn(int column, bool multiColumn);
    void applySort();
    void sortGroupChildren(QTreeWidgetItem* group);
    int compareRecords(const RowData& left, const RowData& right) const;
    int compareValues(qint64 left, qint64 right, Qt::SortOrder order) const;
    int compareStrings(const QString& left, const QString& right, Qt::SortOrder order) const;
    void showProperties(const QVector<QTreeWidgetItem*>& items);
    void copyPaths(const QVector<QTreeWidgetItem*>& items);
    void openContainingFolder(const QVector<QTreeWidgetItem*>& items);

private:
    QTreeWidget* m_tree;
    QProgressBar* m_progress;
    QComboBox* m_policy;
    QLineEdit* m_search;
    QPushButton* m_invertButton;
    QPushButton* m_removeButton;
    QPushButton* m_deleteButton;
    QPushButton* m_compareButton;
    QLabel* m_status;

    QHash<int, RowData> m_records;
    QVector<int> m_compareRecordIds;
    QVector<SortRule> m_sortRules;
    QStringList m_baseHeaders;
    QPointer<QThread> m_thread;
    BatchCompareWorker* m_worker;
    BatchItemDelegate* m_delegate;
    QTimer* m_thumbnailRefresh;
    int m_thumbnailColumnWidth;
    int m_nextRecordId;
    bool m_hasCompared;
    bool m_comparing;
};

#endif // BATCHCOMPAREPAGE_H
