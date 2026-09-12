#ifndef VIDEOBATCHPAGE_H
#define VIDEOBATCHPAGE_H

#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "video_batch.hpp"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void chooseFolder();
    void chooseVideos();
    void clearList();
    void startCompare();
    void onWorkerProgress(int done, int total, const QString& stage);
    void onCompareFinished(VideoBatchOutcome outcome);
    void onCompareFailed(const QString& message);
    void onThreadFinished();

private:
#ifdef VIVIDMATCH_TEST_HOOKS
    // Only compiled for a test harness: the file dialog is native, and SendKeys
    // cannot type CJK paths, so an automated run cannot select clips otherwise.
    friend class VideoBatchPageTestHook;
#endif

    void addPaths(const QStringList& paths);
    void rebuildTree(const VideoBatchOutcome& outcome);
    void updateStatus();
    void setBusy(bool busy);
    KeepPolicy currentPolicy() const;

    QTreeWidget* m_tree;
    QComboBox* m_policy;
    QProgressBar* m_progress;
    QPushButton* m_compareButton;
    QPushButton* m_clearButton;
    QLabel* m_status;

    QStringList m_paths;
    int m_lastClipCount;
    int m_lastDuplicateGroups;
    int m_lastFailedCount;
    QPointer<QThread> m_thread;
    VideoBatchWorker* m_worker;
    bool m_busy;
};

#endif  // VIDEOBATCHPAGE_H
