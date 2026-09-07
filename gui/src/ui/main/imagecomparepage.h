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

private slots:
    void chooseFirstImage();
    void chooseSecondImage();
    void compareSelectedImages();

private:
    void chooseImage(int slot);
    void setImagePath(int slot, const QString& path);
    void refreshPreview(int slot);
    void refreshPreviews();
    void showError(const QString& message);
    void setResult(double score, double threshold, int firstWidth, int firstHeight,
                   int secondWidth, int secondHeight);

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
};

#endif // IMAGECOMPAREPAGE_H
