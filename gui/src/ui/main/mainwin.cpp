#include "mainwin.h"

#include "batchcomparepage.h"
#include "imagecomparepage.h"
#include "videobatchpage.h"
#include "videocomparepage.h"

#include <QAction>
#include <QFont>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

MainWin::MainWin(QWidget* parent)
    : QMainWindow(parent)
    , m_stack(new QStackedWidget(this))
    , m_homePage(createHomePage())
    , m_imagePage(new ImageComparePage(this))
    , m_batchPage(new BatchComparePage(this))
    , m_videoPage(new VideoComparePage(this))
    , m_videoBatchPage(new VideoBatchPage(this))
{
    setWindowTitle(tr("VividMatch - 跨分辨率图片识别"));
    resize(980, 680);
    setMinimumSize(860, 600);

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
}

MainWin::~MainWin() = default;

QVector<MainWin::ModeEntry> MainWin::modeEntries() const
{
    return {
        {tr("图片比对"), tr("图片比对模式\n选择两张图片，判断是否为同一画面"),
         QKeySequence(QStringLiteral("Ctrl+1")), &MainWin::showImageComparePage},
        {tr("批量图片比对"), tr("批量图片比对模式\n批量添加图片并自动分组相同图片"),
         QKeySequence(QStringLiteral("Ctrl+2")), &MainWin::showBatchComparePage},
        {tr("视频比对"), tr("视频比对模式\n比对两段视频的画面与声音"),
         QKeySequence(QStringLiteral("Ctrl+3")), &MainWin::showVideoComparePage},
        {tr("批量视频比对"), tr("批量视频比对模式\n批量添加视频并自动分组相同视频"),
         QKeySequence(QStringLiteral("Ctrl+4")), &MainWin::showVideoBatchPage},
    };
}

void MainWin::setupMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("文件(&F)"));

    // One list drives the 文件 menu and the home page buttons, so adding a mode
    // cannot leave the two out of step. The two-line label used on the buttons is
    // flattened here because a menu entry is a single line.
    const QVector<ModeEntry> modes = modeEntries();
    for (const ModeEntry& mode : modes) {
        QAction* action = fileMenu->addAction(mode.menuLabel);
        action->setShortcut(mode.shortcut);
        connect(action, &QAction::triggered, this, mode.show);
    }

    fileMenu->addSeparator();
    // Enabled so the Escape shortcut keeps working from here; on the home page it
    // is simply a no-op.
    QAction* homeAction = fileMenu->addAction(tr("返回功能选择"));
    homeAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(homeAction, &QAction::triggered, this, &MainWin::showHomePage);

    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(tr("退出"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
}

QWidget* MainWin::createHomePage()
{
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(48, 36, 48, 32);
    root->setSpacing(8);

    QLabel* title = new QLabel(tr("VividMatch"), page);
    title->setObjectName(QStringLiteral("appTitle"));
    QLabel* subtitle = new QLabel(tr("不同分辨率下的图片 / 视频是否相同"), page);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    root->addWidget(title);
    root->addWidget(subtitle);
    root->addSpacing(34);

    QWidget* optionArea = new QWidget(page);
    QVBoxLayout* options = new QVBoxLayout(optionArea);
    options->setContentsMargins(0, 0, 0, 0);
    options->setSpacing(14);

    // Built from the same list as the 文件 menu, so the two cannot drift apart.
    for (const ModeEntry& mode : modeEntries()) {
        QPushButton* button = new QPushButton(mode.buttonLabel, optionArea);
        button->setObjectName(QStringLiteral("modeButton"));
        button->setMinimumWidth(420);
        button->setCursor(Qt::PointingHandCursor);
        if (!mode.shortcut.isEmpty()) {
            button->setToolTip(tr("快捷键 %1").arg(mode.shortcut.toString(QKeySequence::NativeText)));
        }
        connect(button, &QPushButton::released, this, mode.show);
        options->addWidget(button, 0, Qt::AlignHCenter);
    }
    options->addStretch(1);

    QHBoxLayout* centered = new QHBoxLayout;
    centered->addStretch(1);
    centered->addWidget(optionArea);
    centered->addStretch(1);
    root->addLayout(centered);
    root->addStretch(1);

    QLabel* footer = new QLabel(tr("VividMatch 0.1.0"), page);
    footer->setAlignment(Qt::AlignRight);
    footer->setStyleSheet(QStringLiteral("color:#9ca3af;"));
    root->addWidget(footer);
    return page;
}

void MainWin::showHomePage()
{
    m_stack->setCurrentWidget(m_homePage);
    setWindowTitle(tr("VividMatch - 功能选择"));
}

void MainWin::showImageComparePage()
{
    m_stack->setCurrentWidget(m_imagePage);
    setWindowTitle(tr("VividMatch - 图片比对"));
}

void MainWin::showBatchComparePage()
{
    m_stack->setCurrentWidget(m_batchPage);
    setWindowTitle(tr("VividMatch - 批量图片比对"));
}

void MainWin::showVideoComparePage()
{
    m_stack->setCurrentWidget(m_videoPage);
    setWindowTitle(tr("VividMatch - 视频比对"));
}

void MainWin::showVideoBatchPage()
{
    m_stack->setCurrentWidget(m_videoBatchPage);
    setWindowTitle(tr("VividMatch - 批量视频比对"));
}
