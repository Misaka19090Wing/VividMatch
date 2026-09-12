#ifndef VIDEOBATCHPAGE_H
#define VIDEOBATCHPAGE_H

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "video_batch.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStyledItemDelegate;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;

// How the winner of a duplicate group is chosen. Declared outside the page so
// the worker can carry it by value: reading it off the page from the worker
// thread would be a race against the combo box.
enum class KeepPolicy {
    HighestResolution = 0,
    LowestResolution,
    LargestFile,
    SmallestFile,
    NewestModified,
    OldestModified,
    KeepAll,
};

// Ranks a group under `policy`, returning the index to keep or -1 for "keep
// everything". Free of page state so it is safe to call from a worker thread.
int rankVideoGroup(KeepPolicy policy, const std::vector<int>& members,
                   const std::vector<vividmatch::VideoBatchItem>& items);

// One row of the batch result, flattened for the list. `row` is the index into
// the batch's item vector, which is how a row maps back to its clip.
struct VideoBatchRow {
    int row = -1;
    QString path;
    QString name;
    QString resolution;
    QString durationText;
    QString sizeText;
    QString modifiedText;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    qint64 fileSize = 0;
    QDateTime modified;
    bool preferred = false;
};

struct VideoBatchGroupInfo {
    QString label;
    bool duplicate = false;
    bool montage = false;
    bool audioDiffers = false;
    double similarity = 0.0;
    QVector<VideoBatchRow> rows;
};

struct VideoBatchOutcome {
    QVector<VideoBatchGroupInfo> groups;
    int clipCount = 0;
    int failedCount = 0;
    QStringList failures;
    qint64 extractMs = 0;
    qint64 compareMs = 0;
    int pairsCompared = 0;
    int pairsPossible = 0;
};

Q_DECLARE_METATYPE(VideoBatchOutcome)

// Fingerprints and groups a folder of clips off the UI thread. Each clip is
// decoded once; the comparing stage is a small fraction of the time.
class VideoBatchWorker : public QObject
{
    Q_OBJECT

public:
    // `paths` is a snapshot of the list and `policy` is taken by value: the
    // worker runs on its own thread and must not read page state.
    VideoBatchWorker(QStringList paths, KeepPolicy policy);

public slots:
    void run();

signals:
    void progress(int done, int total, const QString& stage);
    void finished(VideoBatchOutcome outcome);
    void failed(const QString& message);

private:
    QStringList m_paths;
    KeepPolicy m_policy;
};

class VideoBatchPage : public QWidget
{
    Q_OBJECT

public:
    explicit VideoBatchPage(QWidget* parent = nullptr);

signals:
    void backRequested();
    // Emitted when a background frame grab finishes; an empty image means the
    // clip could not be read as a picture.
    void thumbnailReady(const QString& path, const QImage& image);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void chooseFolder();
    void chooseVideos();
    void clearList();
    void invertChecked();
    void removeChecked();
    void deleteCheckedFiles();
    void selectAllChecked();
    void startCompare();
    void onWorkerProgress(int done, int total, const QString& stage);
    void onCompareFinished(VideoBatchOutcome outcome);
    void onCompareFailed(const QString& message);
    void onThreadFinished();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void showTreeContextMenu(const QPoint& position);
    void showGroupContextMenu(QTreeWidgetItem* group, const QPoint& position);
    void searchChanged(const QString& text);

private:
#ifdef VIVIDMATCH_TEST_HOOKS
    // Only compiled for a test harness: the file dialog is native, and SendKeys
    // cannot type CJK paths, so an automated run cannot select clips otherwise.
    friend class VideoBatchPageTestHook;
#endif

    // Columns of the list.
    enum Column {
        CheckColumn = 0,
        PreviewColumn,
        NameColumn,
        ResolutionColumn,
        DurationColumn,
        SizeColumn,
        ModifiedColumn,
        ColumnCount,
    };

    // Keeps the checkbox hugging the left of its column and scales the preview
    // frame to whatever width that column currently has, so dragging the column
    // edge really resizes the picture and the row grows to fit it.
    class VideoBatchItemDelegate;

    // A clip in the list. Rows are created as soon as a clip is added, so the
    // list is never empty while a comparison is pending.
    struct RowData {
        int id = -1;
        QString path;
        QString name;
        qint64 fileSize = 0;
        QDateTime modified;
        QString modifiedText;
        // Preview frame, grabbed in the background after the clip is added.
        QImage thumbnail;
        // Filled in by the comparison; a dash is shown until then because these
        // two need the clip to be read.
        bool measured = false;
        int width = 0;
        int height = 0;
        double duration = 0.0;
    };

    void addPaths(const QStringList& paths);
    // Starts background frame grabs for clips that do not have a preview yet.
    void requestThumbnails();
    void applyThumbnail(const QString& path, const QImage& image);
    // Drops every stored preview and grabs them again, used after the capture
    // position changes so the column shows frames from the new position.
    void regrabThumbnails();
    // Where in the clip the preview is taken from, as a percentage of its length.
    int capturePositionPercent() const;
    void updateCompareButton();
    // Rebuilds the result groups from an outcome.
    void rebuildTree(const VideoBatchOutcome& outcome);
    // Rebuilds only the clips that are not shown yet, leaving result groups in
    // place, so adding clips extends the list instead of wiping results.
    void rebuildPendingTree();
    QTreeWidgetItem* createRowItem(int recordId) const;
    QTreeWidgetItem* ensurePendingGroup();
    void refreshGroupLabels();
    int unlistedPathCount() const;
    void applyPolicyToChecks();
    QVector<QTreeWidgetItem*> allRowItems() const;
    QVector<QTreeWidgetItem*> checkedItems() const;
    QVector<QTreeWidgetItem*> selectedRowItems() const;
    QVector<QTreeWidgetItem*> contextTargetItems(QTreeWidgetItem* clicked) const;
    void removeItems(const QVector<QTreeWidgetItem*>& items);
    void setItemsChecked(const QVector<QTreeWidgetItem*>& items, bool checked);
    void deleteFiles(const QVector<QTreeWidgetItem*>& items);
    void showProperties(const QVector<QTreeWidgetItem*>& items);
    void copyPaths(const QVector<QTreeWidgetItem*>& items);
    void openContainingFolder(const QVector<QTreeWidgetItem*>& items);
    const RowData* recordForItem(const QTreeWidgetItem* item) const;
    RowData* recordForItem(QTreeWidgetItem* item);
    // Record id whose path matches, or -1. Used to keep rows stable across
    // rebuilds, since the comparison reports clips by path.
    int recordIdForPath(const QString& path) const;
    void updateStatus();
    void setBusy(bool busy);
    KeepPolicy currentPolicy() const;

    QTreeWidget* m_tree;
    QComboBox* m_policy;
    QLineEdit* m_search;
    QSpinBox* m_capturePosition;
    QProgressBar* m_progress;
    QPushButton* m_compareButton;
    QPushButton* m_clearButton;
    QPushButton* m_invertButton;
    QPushButton* m_selectAllButton;
    QPushButton* m_removeButton;
    QPushButton* m_deleteButton;
    QLabel* m_status;

    VideoBatchItemDelegate* m_delegate;
    // Clips whose preview has been requested but not delivered yet, so a rebuild
    // never queues the same grab twice.
    QSet<QString> m_pendingThumbnails;

    QHash<int, RowData> m_records;
    QStringList m_listedPaths;  // clips already shown in the tree
    int m_nextRecordId;
    int m_lastClipCount;
    int m_lastDuplicateGroups;
    int m_lastFailedCount;
    QPointer<QThread> m_thread;
    VideoBatchWorker* m_worker;
    bool m_busy;
};

#endif  // VIDEOBATCHPAGE_H
