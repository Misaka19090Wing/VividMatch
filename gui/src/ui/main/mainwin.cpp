#include "mainwin.h"

#include "batchcomparepage.h"
#include "imagecomparepage.h"
#include "videobatchpage.h"
#include "videocomparepage.h"

#include <QAction>
#include <QActionGroup>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

MainWin::MainWin(QWidget* parent)
    : QMainWindow(parent)
    , m_languages(new LanguageManager(this))
    , m_stack(new QStackedWidget(this))
    , m_homePage(nullptr)
    , m_imagePage(new ImageComparePage(this))
    , m_batchPage(new BatchComparePage(this))
    , m_videoPage(new VideoComparePage(this))
    , m_videoBatchPage(new VideoBatchPage(this))
    , m_titleLabel(nullptr)
    , m_subtitleLabel(nullptr)
    , m_footerLabel(nullptr)
    , m_fileMenu(nullptr)
    , m_languageMenu(nullptr)
    , m_languageSystemAction(nullptr)
    , m_languageEnglishAction(nullptr)
    , m_languageChineseAction(nullptr)
    , m_homeAction(nullptr)
    , m_exitAction(nullptr)
    , m_restartNoticeShowing(false)
{
    resize(980, 680);
    setMinimumSize(860, 600);

    // Built here rather than in the initialiser list: createHomePage() records the
    // mode buttons in m_modeButtons, and members are constructed in declaration
    // order, so calling it before that vector exists silently loses them (the
    // buttons then have no text at all).
    m_homePage = createHomePage();

    setupMenus();

    m_stack->addWidget(m_homePage);
    m_stack->addWidget(m_imagePage);
    m_stack->addWidget(m_batchPage);
    m_stack->addWidget(m_videoPage);
    m_stack->addWidget(m_videoBatchPage);
    setCentralWidget(m_stack);
    showHomePage();
    connect(m_imagePage, &ImageComparePage::backRequested,
            this, &MainWin::showHomePage, Qt::UniqueConnection);
    connect(m_batchPage, &BatchComparePage::backRequested,
            this, &MainWin::showHomePage, Qt::UniqueConnection);
    connect(m_videoPage, &VideoComparePage::backRequested,
            this, &MainWin::showHomePage, Qt::UniqueConnection);
    connect(m_videoBatchPage, &VideoBatchPage::backRequested,
            this, &MainWin::showHomePage, Qt::UniqueConnection);

    // Picking a language installs the translation; Qt then sends a
    // LanguageChange event, and changeEvent() rewrites the visible text.
    // A language switch has two halves: Qt's own LanguageChange event (handled in
    // changeEvent) rewrites the text of every widget that implements it, and this
    // signal handles the window's own strings plus the restart hint.
    connect(m_languages, &LanguageManager::languageChanged, this,
            &MainWin::onLanguageChanged);

    setStyleSheet(QStringLiteral(R"(
        QMainWindow { background: #f3f5f7; }
        QLabel#appTitle { font-size: 30px; font-weight: 700; color: #1f2937; }
        QLabel#appSubtitle { font-size: 14px; color: #6b7280; }
        QLabel#modeTitle { font-size: 17px; font-weight: 600; color: #1f2937; }
        QLabel#modeHint { color: #6b7280; }
        QPushButton#modeButton {
            background: #ffffff; border: 1px solid #d7dce2; border-radius: 8px;
            padding: 12px 18px; min-height: 74px;
        }
        QPushButton#modeButton:hover { background: #eef4ff; border-color: #7aa2f7; }
        QPushButton#modeButton:disabled { color: #9ca3af; background: #eceff3; }
        QPushButton#primaryButton {
            background: #2563eb; color: #ffffff; border-radius: 6px;
            padding: 9px 22px; font-weight: 600;
        }
        QPushButton#primaryButton:hover { background: #1d4ed8; }
        QPushButton#primaryButton:disabled { background: #a5b4fc; }
        QPushButton#secondaryButton {
            background: #ffffff; color: #334155; border: 1px solid #cbd5e1;
            border-radius: 6px; padding: 8px 16px;
        }
        QPushButton#secondaryButton:hover { background: #eef2f7; }
    )"));

    retranslateUi();
}

MainWin::~MainWin() = default;

void MainWin::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QMainWindow::changeEvent(event);
}

QVector<MainWin::ModeEntry> MainWin::modeEntries() const
{
    // English is the source language; the Chinese comes from the .qm.
    return {
        {tr("Image comparison"),
         tr("Image comparison mode\nPick two images to see if they show the same scene"),
         tr("VividMatch - Image comparison"), QStringLiteral("Alt+1"),
         &MainWin::showImageComparePage},
        {tr("Batch image comparison"),
         tr("Batch image comparison mode\nAdd many images and group the identical ones"),
         tr("VividMatch - Batch image comparison"), QStringLiteral("Alt+2"),
         &MainWin::showBatchComparePage},
        {tr("Video comparison"),
         tr("Video comparison mode\nCompare the picture and the sound of two clips"),
         tr("VividMatch - Video comparison"), QStringLiteral("Alt+3"),
         &MainWin::showVideoComparePage},
        {tr("Batch video comparison"),
         tr("Batch video comparison mode\nAdd many clips and group the identical ones"),
         tr("VividMatch - Batch video comparison"), QStringLiteral("Alt+4"),
         &MainWin::showVideoBatchPage},
    };
}

void MainWin::setupMenus()
{
    m_fileMenu = menuBar()->addMenu(QString());

    // One list drives the File menu and the home page buttons, so adding a mode
    // cannot leave the two out of step. The two-line label used on the buttons is
    // flattened for the menu, where an entry is a single line.
    const QVector<ModeEntry> modes = modeEntries();
    for (const ModeEntry& mode : modes) {
        QAction* action = m_fileMenu->addAction(QString());
        // Alt+digit rather than Ctrl+digit: on Windows Ctrl+1..4 are commonly
        // taken by other applications, and the menu shows Alt combinations.
        action->setShortcut(QKeySequence(mode.shortcut));
        connect(action, &QAction::triggered, this, mode.show);
        m_modeActions.append(action);
    }

    m_fileMenu->addSeparator();
    m_homeAction = m_fileMenu->addAction(QString());
    m_homeAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(m_homeAction, &QAction::triggered, this, &MainWin::showHomePage);

    // Language submenu. "Follow the system" is the default and is what most users
    // want; the two explicit entries override it and are remembered.
    m_languageMenu = m_fileMenu->addMenu(QString());
    auto* languageGroup = new QActionGroup(this);
    languageGroup->setExclusive(true);

    const auto addLanguage = [&](LanguageManager::Language language) {
        QAction* action = m_languageMenu->addAction(QString());
        action->setCheckable(true);
        action->setActionGroup(languageGroup);
        action->setData(static_cast<int>(language));
        connect(action, &QAction::triggered, this, [this, language]() {
            m_languages->setLanguage(language);
        });
        return action;
    };
    m_languageSystemAction = addLanguage(LanguageManager::Language::System);
    m_languageMenu->addSeparator();
    m_languageEnglishAction = addLanguage(LanguageManager::Language::English);
    m_languageChineseAction = addLanguage(LanguageManager::Language::Chinese);

    m_fileMenu->addSeparator();
    m_exitAction = m_fileMenu->addAction(QString());
    m_exitAction->setShortcut(QKeySequence::Quit);
    connect(m_exitAction, &QAction::triggered, this, &QWidget::close);
}

QWidget* MainWin::createHomePage()
{
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(48, 36, 48, 32);
    root->setSpacing(8);

    m_titleLabel = new QLabel(page);
    m_titleLabel->setObjectName(QStringLiteral("appTitle"));
    m_subtitleLabel = new QLabel(page);
    m_subtitleLabel->setObjectName(QStringLiteral("appSubtitle"));
    root->addWidget(m_titleLabel);
    root->addWidget(m_subtitleLabel);
    root->addSpacing(34);

    QWidget* optionArea = new QWidget(page);
    QVBoxLayout* options = new QVBoxLayout(optionArea);
    options->setContentsMargins(0, 0, 0, 0);
    options->setSpacing(14);

    // Built from the same list as the File menu, so the two cannot drift apart.
    for (const ModeEntry& mode : modeEntries()) {
        QPushButton* button = new QPushButton(optionArea);
        button->setObjectName(QStringLiteral("modeButton"));
        button->setMinimumWidth(420);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::released, this, mode.show);
        options->addWidget(button, 0, Qt::AlignHCenter);
        m_modeButtons.append(button);
    }
    options->addStretch(1);

    QHBoxLayout* centered = new QHBoxLayout;
    centered->addStretch(1);
    centered->addWidget(optionArea);
    centered->addStretch(1);
    root->addLayout(centered);
    root->addStretch(1);

    m_footerLabel = new QLabel(page);
    m_footerLabel->setAlignment(Qt::AlignRight);
    m_footerLabel->setStyleSheet(QStringLiteral("color:#9ca3af;"));
    root->addWidget(m_footerLabel);
    return page;
}

void MainWin::onLanguageChanged()
{
    retranslateUi();
    // Not every string can follow a translator swap: text already sitting in a
    // status line or a finished result panel is only rewritten where the code
    // explicitly does so. Saying a restart applies it everywhere is more honest
    // than pretending the switch is always complete.
    // Deferred so the dialog is built once this signal has finished dispatching,
    // and therefore in the language just chosen rather than the previous one.
    QTimer::singleShot(0, this, [this]() { showRestartNotice(); });
}

void MainWin::retranslateUi()
{
    // Called from the constructor (before every member exists) and on every
    // language change, hence the guards.
    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(tr("VividMatch"));
    }
    if (m_subtitleLabel != nullptr) {
        m_subtitleLabel->setText(tr("Are these pictures / videos the same at different resolutions?"));
    }
    if (m_footerLabel != nullptr) {
        m_footerLabel->setText(tr("VividMatch %1").arg(QStringLiteral("0.1.0")));
    }

    if (m_fileMenu != nullptr) {
        m_fileMenu->setTitle(tr("&File"));
    }

    const QVector<ModeEntry> modes = modeEntries();
    for (int i = 0; i < m_modeActions.size() && i < modes.size(); ++i) {
        m_modeActions.at(i)->setText(modes.at(i).menuLabel);
    }
    for (int i = 0; i < m_modeButtons.size() && i < modes.size(); ++i) {
        const ModeEntry& mode = modes.at(i);
        // The button carries both lines; split them out of the single translated
        // string so the menu and the button share one translation.
        QStringList parts = mode.buttonLabel.split(QLatin1Char('\n'));
        const QString hint = parts.size() > 1 ? parts.at(1) : QString();
        m_modeButtons.at(i)->setText(mode.buttonLabel);
        m_modeButtons.at(i)->setToolTip(
            hint.isEmpty() ? QString() : tr("%1\nShortcut %2").arg(hint, mode.shortcut));
    }

    if (m_homeAction != nullptr) {
        m_homeAction->setText(tr("Back to mode selection"));
    }
    if (m_languageMenu != nullptr) {
        m_languageMenu->setTitle(tr("&Language"));
    }
    if (m_languageSystemAction != nullptr) {
        m_languageSystemAction->setText(tr("Follow the system (%1)")
                                            .arg(m_languages->effective()
                                                         == LanguageManager::Language::Chinese
                                                     ? tr("Chinese")
                                                     : tr("English")));
        m_languageSystemAction->setChecked(m_languages->preference()
                                           == LanguageManager::Language::System);
    }
    if (m_languageEnglishAction != nullptr) {
        m_languageEnglishAction->setText(tr("English"));
        m_languageEnglishAction->setChecked(m_languages->preference()
                                           == LanguageManager::Language::English);
    }
    if (m_languageChineseAction != nullptr) {
        m_languageChineseAction->setText(tr("Simplified Chinese"));
        m_languageChineseAction->setChecked(m_languages->preference()
                                           == LanguageManager::Language::Chinese);
    }
    if (m_exitAction != nullptr) {
        m_exitAction->setText(tr("E&xit"));
    }

    // Re-apply the title for whichever page is showing, now in the new language.
    if (m_stack->currentWidget() == m_homePage) {
        showHomePage();
        return;
    }
    for (int i = 0; i < m_modeActions.size() && i < modes.size(); ++i) {
        if (m_stack->currentWidget() == pageForMode(i)) {
            setWindowTitle(modes.at(i).windowTitle);
            return;
        }
    }
}

QWidget* MainWin::pageForMode(int index) const
{
    switch (index) {
        case 0:
            return m_imagePage;
        case 1:
            return m_batchPage;
        case 2:
            return m_videoPage;
        case 3:
            return m_videoBatchPage;
        default:
            break;
    }
    return nullptr;
}

void MainWin::showRestartNotice()
{
    if (m_restartNoticeShowing) {
        return;
    }
    m_restartNoticeShowing = true;

    // Only the restart is promised. Asking the user to restart is what actually
    // guarantees a consistent interface; a notice claiming the text has already
    // changed would be wrong wherever it has not.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(tr("Language saved"));
    box.setText(tr("The language setting has been saved."));
    box.setInformativeText(tr("Restart the application for it to take effect everywhere."));
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();

    m_restartNoticeShowing = false;
}

void MainWin::showHomePage()
{
    m_stack->setCurrentWidget(m_homePage);
    setWindowTitle(tr("VividMatch - Mode selection"));
}

void MainWin::showImageComparePage()
{
    m_stack->setCurrentWidget(m_imagePage);
    setWindowTitle(modeEntries().at(0).windowTitle);
}

void MainWin::showBatchComparePage()
{
    m_stack->setCurrentWidget(m_batchPage);
    setWindowTitle(modeEntries().at(1).windowTitle);
}

void MainWin::showVideoComparePage()
{
    m_stack->setCurrentWidget(m_videoPage);
    setWindowTitle(modeEntries().at(2).windowTitle);
}

void MainWin::showVideoBatchPage()
{
    m_stack->setCurrentWidget(m_videoBatchPage);
    setWindowTitle(modeEntries().at(3).windowTitle);
}
