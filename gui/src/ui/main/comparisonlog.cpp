#include "comparisonlog.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QColor levelColour(ComparisonLogLevel level)
{
    switch (level) {
        case ComparisonLogLevel::Info:
            return QColor(QStringLiteral("#475569"));
        case ComparisonLogLevel::Success:
            return QColor(QStringLiteral("#15803d"));
        case ComparisonLogLevel::Warning:
            return QColor(QStringLiteral("#b45309"));
        case ComparisonLogLevel::Error:
            return QColor(QStringLiteral("#b91c1c"));
        case ComparisonLogLevel::Detail:
            return QColor(QStringLiteral("#64748b"));
    }
    return QColor(QStringLiteral("#334155"));
}

QString levelTag(ComparisonLogLevel level)
{
    switch (level) {
        case ComparisonLogLevel::Info:
            return QStringLiteral("INFO");
        case ComparisonLogLevel::Success:
            return QStringLiteral("OK");
        case ComparisonLogLevel::Warning:
            return QStringLiteral("WARN");
        case ComparisonLogLevel::Error:
            return QStringLiteral("ERROR");
        case ComparisonLogLevel::Detail:
            return QStringLiteral("DETAIL");
    }
    return QStringLiteral("INFO");
}

QString formatLine(const QDateTime& time, ComparisonLogLevel level, const QString& message)
{
    return QStringLiteral("[%1] %2 %3")
        .arg(time.toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(levelTag(level), -6)
        .arg(message);
}

} // namespace

ComparisonLogWidget::ComparisonLogWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("comparisonLogCard"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "QWidget#comparisonLogCard { background:#ffffff; border:1px solid #d7dce2;"
        "border-radius:6px; }"
        "QPlainTextEdit { background:#fbfcfd; border:1px solid #e2e8f0;"
        "border-radius:4px; }"));

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 6, 10, 8);
    root->setSpacing(4);

    // One toolbar row: a second row of controls would make the panel tall
    // enough to squeeze the table it is attached to on a 680 px window.
    QHBoxLayout* header = new QHBoxLayout;
    header->setSpacing(8);
    m_title = new QLabel(this);
    m_title->setStyleSheet(QStringLiteral("font-weight:600;color:#334155;"));
    m_count = new QLabel(this);
    m_count->setStyleSheet(QStringLiteral("color:#94a3b8;"));
    m_details = new QCheckBox(this);
    m_details->setChecked(true);
    connect(m_details, &QCheckBox::toggled, this, &ComparisonLogWidget::setDetailVisible);
    m_autoScroll = new QCheckBox(this);
    m_autoScroll->setChecked(true);
    m_copy = new QPushButton(this);
    m_copy->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_copy, &QPushButton::released, this, &ComparisonLogWidget::copyToClipboard);
    m_save = new QPushButton(this);
    m_save->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_save, &QPushButton::released, this, &ComparisonLogWidget::saveToFile);
    m_clear = new QPushButton(this);
    m_clear->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_clear, &QPushButton::released, this, &ComparisonLogWidget::clear);
    m_collapse = new QToolButton(this);
    m_collapse->setAutoRaise(true);
    m_collapse->setArrowType(Qt::DownArrow);
    m_collapse->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(m_collapse, &QToolButton::clicked, this,
            [this]() { setCollapsed(!m_collapsed); });

    header->addWidget(m_title);
    header->addWidget(m_count);
    header->addStretch(1);
    header->addWidget(m_details);
    header->addWidget(m_autoScroll);
    header->addWidget(m_copy);
    header->addWidget(m_save);
    header->addWidget(m_clear);
    header->addWidget(m_collapse);
    root->addLayout(header);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setUndoRedoEnabled(false);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    // Keep the widget's own block count in step with the entry cap, so a very
    // long run cannot leave already-dropped lines visible in the view.
    m_view->setMaximumBlockCount(kMaxEntries + 2);
    m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // Small enough to shrink with a 600 px window; the panel still gets the
    // remaining vertical space on the normal 680 px one.
    m_view->setMinimumHeight(20);
    root->addWidget(m_view, 1);

    setMinimumHeight(0);
    retranslate();
    updateCountLabel();
}

void ComparisonLogWidget::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void ComparisonLogWidget::retranslate()
{
    m_title->setText(tr("Comparison log"));
    m_details->setText(tr("Details"));
    m_autoScroll->setText(tr("Auto-scroll"));
    m_copy->setText(tr("Copy"));
    m_save->setText(tr("Save..."));
    m_clear->setText(tr("Clear"));
    m_collapse->setText(m_collapsed ? tr("Show") : tr("Hide"));
    m_collapse->setToolTip(m_collapsed ? tr("Expand the log panel")
                                       : tr("Collapse the log panel"));
    updateCountLabel();
}

void ComparisonLogWidget::append(ComparisonLogLevel level, const QString& message)
{
    appendEntry(level, message);
}

void ComparisonLogWidget::info(const QString& message)
{
    appendEntry(ComparisonLogLevel::Info, message);
}

void ComparisonLogWidget::success(const QString& message)
{
    appendEntry(ComparisonLogLevel::Success, message);
}

void ComparisonLogWidget::warning(const QString& message)
{
    appendEntry(ComparisonLogLevel::Warning, message);
}

void ComparisonLogWidget::error(const QString& message)
{
    appendEntry(ComparisonLogLevel::Error, message);
}

void ComparisonLogWidget::detail(const QString& message)
{
    appendEntry(ComparisonLogLevel::Detail, message);
}

void ComparisonLogWidget::appendEntry(ComparisonLogLevel level, const QString& message)
{
    Entry entry;
    entry.time = QDateTime::currentDateTime();
    entry.level = level;
    entry.message = message;
    m_entries.append(entry);

    if (m_entries.size() > kMaxEntries) {
        const int drop = static_cast<int>(m_entries.size()) - kMaxEntries;
        m_entries.remove(0, drop);
        if (!m_truncated) {
            // The first drop changes what the panel has to show above the new
            // line, so rebuild rather than appending onto a stale beginning.
            m_truncated = true;
            rebuild();
            return;
        }
    }

    if (m_detailVisible || level != ComparisonLogLevel::Detail) {
        renderEntry(entry);
    }
    updateCountLabel();
    if (m_autoScroll->isChecked()) {
        scrollToEnd();
    }
}

void ComparisonLogWidget::renderEntry(const Entry& entry)
{
    if (!m_detailVisible && entry.level == ComparisonLogLevel::Detail) {
        return;
    }

    QTextCursor cursor(m_view->document());
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat timeFormat;
    timeFormat.setForeground(QColor(QStringLiteral("#94a3b8")));
    cursor.insertText(QStringLiteral("[%1] ").arg(
                          entry.time.toString(QStringLiteral("HH:mm:ss.zzz"))),
                      timeFormat);

    QTextCharFormat levelFormat;
    levelFormat.setForeground(levelColour(entry.level));
    levelFormat.setFontWeight(QFont::DemiBold);
    cursor.insertText(levelTag(entry.level).leftJustified(6) + QLatin1Char(' '),
                      levelFormat);

    QTextCharFormat messageFormat;
    messageFormat.setForeground(levelColour(entry.level));
    cursor.insertText(entry.message + QLatin1Char('\n'), messageFormat);
}

void ComparisonLogWidget::rebuild()
{
    m_view->clear();
    if (m_truncated) {
        Entry notice;
        notice.time = QDateTime::currentDateTime();
        notice.level = ComparisonLogLevel::Warning;
        notice.message =
            tr("Older log lines were dropped to keep the log responsive.");
        renderEntry(notice);
    }
    for (const Entry& entry : m_entries) {
        renderEntry(entry);
    }
    updateCountLabel();
    if (m_autoScroll->isChecked()) {
        scrollToEnd();
    }
}

void ComparisonLogWidget::clear()
{
    m_entries.clear();
    m_truncated = false;
    m_view->clear();
    updateCountLabel();
}

void ComparisonLogWidget::updateCountLabel()
{
    m_count->setText(tr("%1 lines").arg(static_cast<int>(m_entries.size())));
}

void ComparisonLogWidget::scrollToEnd()
{
    QScrollBar* bar = m_view->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void ComparisonLogWidget::setDetailVisible(bool visible)
{
    if (m_detailVisible == visible) {
        return;
    }
    m_detailVisible = visible;
    rebuild();
}

bool ComparisonLogWidget::isDetailVisible() const
{
    return m_detailVisible;
}

bool ComparisonLogWidget::isCollapsed() const
{
    return m_collapsed;
}

void ComparisonLogWidget::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed) {
        return;
    }
    m_collapsed = collapsed;
    // Hiding the view is what gives the vertical space back to the table above;
    // the toolbar stays so the log can still be copied, saved or filtered.
    m_view->setVisible(!collapsed);
    m_collapse->setArrowType(collapsed ? Qt::RightArrow : Qt::DownArrow);
    retranslate();
}

QString ComparisonLogWidget::plainText() const
{
    QStringList lines;
    if (m_truncated) {
        lines << QStringLiteral("[--:--:--.---] WARN   ")
                     + tr("Older log lines were dropped to keep the log responsive.");
    }
    for (const Entry& entry : m_entries) {
        lines << formatLine(entry.time, entry.level, entry.message);
    }
    return lines.join(QLatin1Char('\n'));
}

void ComparisonLogWidget::copyToClipboard()
{
    QGuiApplication::clipboard()->setText(plainText());
}

void ComparisonLogWidget::saveToFile()
{
    const QString suggested =
        QDir(QDir::homePath()).filePath(QStringLiteral("vividmatch-comparison-log.txt"));
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save comparison log"), suggested,
        tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Could not save the log"),
                             tr("The log file could not be written:\n%1")
                                 .arg(file.errorString()));
        return;
    }
    file.write(plainText().toUtf8());
    file.write("\n");
    file.close();
}
