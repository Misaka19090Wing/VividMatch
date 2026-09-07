#ifndef MAINWIN_H
#define MAINWIN_H

#include <QMainWindow>

class QStackedWidget;

class ImageComparePage;

class MainWin : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWin(QWidget* parent = nullptr);
    ~MainWin() override;

private:
    void setupMenus();
    QWidget* createHomePage();
    void showHomePage();
    void showImageComparePage();

private:
    QStackedWidget* m_stack;
    QWidget* m_homePage;
    ImageComparePage* m_imagePage;
};

#endif // MAINWIN_H
