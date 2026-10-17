#ifndef COMPARISONLOG_H
#define COMPARISONLOG_H

#include <QDateTime>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

// Severity of one log line. Detail is the verbose level (per-file and per-pair
// numbers) and can be hidden without losing the lines: they stay stored, so the
// copy and save actions always contain the full log.
enum class ComparisonLogLevel {
    Info,
    Success,
    Warning,
    Error,
    Detail,
};

// The log panel shared by all four comparison modes.
//
// Keeping one widget rather than four hand-built text areas is what makes the
// panel behave the same everywhere: same timestamp format, same colours, same
// copy / save / clear actions, and the same "detailed lines" filter. A page only
// calls append() and the widget owns presentation.
class ComparisonLogWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ComparisonLogWidget(QWidget* parent = nullptr);

    // Appends one line. The timestamp is taken here, so the order in the panel
    // is the order the page received the messages.
    void append(ComparisonLogLevel level, const QString& message);
    void info(const QString& message);
    void success(const QString& message);
    void warning(const QString& message);
    void error(const QString& message);
    void detail(const QString& message);

    void clear();
    bool isCollapsed() const;
    void setCollapsed(bool collapsed);
    bool isDetailVisible() const;

protected:
    void changeEvent(QEvent* event) override;

private:
    struct Entry {
        QDateTime time;
        ComparisonLogLevel level = ComparisonLogLevel::Info;
        QString message;
    };

    void appendEntry(ComparisonLogLevel level, const QString& message);
    QString plainText() const;
    void renderEntry(const Entry& entry);
    void rebuild();
    void updateCountLabel();
    void retranslate();
    void copyToClipboard();
    void saveToFile();
    void setDetailVisible(bool visible);
    void scrollToEnd();

    QLabel* m_title = nullptr;
    QLabel* m_count = nullptr;
    QToolButton* m_collapse = nullptr;
    QCheckBox* m_details = nullptr;
    QCheckBox* m_autoScroll = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_clear = nullptr;
    QPlainTextEdit* m_view = nullptr;

    QVector<Entry> m_entries;
    bool m_collapsed = false;
    bool m_detailVisible = true;
    // Set once the oldest entries have been dropped, so a rebuild can say so
    // instead of silently presenting a log that is missing its beginning.
    bool m_truncated = false;

    // shortcut: the log keeps 20000 entries in memory, raise it if a run must be
    // reviewed line by line past that point.
    static constexpr int kMaxEntries = 20000;
};

#endif // COMPARISONLOG_H
