#ifndef IMAGECOMPAREPAGE_H
#define IMAGECOMPAREPAGE_H

#include <QImage>
#include <QObject>
#include <QString>
#include <QWidget>

class QEvent;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QResizeEvent;

class ImageComparePage : public QWidget
{
    Q_OBJECT

public:
    explicit ImageComparePage(QWidget* parent = nullptr);

signals:
    void backRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // Qt sends LanguageChange when a translator is installed or removed, which is
    // how the page re-reads its strings without being rebuilt.
    void changeEvent(QEvent* event) override;

private slots:
    void chooseFirstImage();
    void chooseSecondImage();
    void compareSelectedImages();

private:
#ifdef VIVIDMATCH_TEST_HOOKS
    // Only compiled when a test harness asks for it; the file dialog is native
    // and cannot be driven from an automated run.
    friend class ImageComparePageTestHook;
#endif

    void chooseImage(int slot);
    void setImagePath(int slot, const QString& path);
    void refreshPreview(int slot);
    void refreshPreviews();
    void showError(const QString& message);
    // elapsedMs is the measured comparison time, shown alongside the verdict.
    void setResult(double score, double threshold, int firstWidth, int firstHeight,
                   int secondWidth, int secondHeight, qint64 elapsedMs);
    void retranslate();
    // Rewrites the result panel in the current language. The verdict text embeds
    // the numbers, so it is recomputed rather than re-translated.
    void refreshResultText();

private:
    QString m_firstPath;
    QString m_secondPath;
    QImage m_firstImage;
    QImage m_secondImage;

    QLabel* m_firstPreview;
    QLabel* m_secondPreview;
    QLabel* m_firstPathLabel;
    QLabel* m_secondPathLabel;
    QLabel* m_resultVerdict;
    QLabel* m_resultScore;
    QLabel* m_resultSource;
    QDoubleSpinBox* m_threshold;
    QPushButton* m_compareButton;

    // Kept so a language switch can rewrite them.
    QPushButton* m_backButton;
    QLabel* m_titleLabel;
    QLabel* m_firstSideLabel;
    QLabel* m_secondSideLabel;
    QPushButton* m_firstButton;
    QPushButton* m_secondButton;
    QLabel* m_thresholdLabel;

    // Last comparison result, re-rendered on a language switch.
    bool m_hasResult;
    double m_score;
    double m_resultThreshold;
    int m_firstWidth;
    int m_firstHeight;
    int m_secondWidth;
    int m_secondHeight;
    qint64 m_elapsedMs;
};

#endif // IMAGECOMPAREPAGE_H
