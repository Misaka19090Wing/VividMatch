#ifndef KEEPPOLICYDIALOG_H
#define KEEPPOLICYDIALOG_H

#include <QDialog>

#include "keeppolicy.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QListWidget;
class QPushButton;

// Edits the keep-policy priority list. The dialog only edits; the page owns
// persistence and applying the result.
class KeepPolicyDialog : public QDialog
{
    Q_OBJECT

public:
    KeepPolicyDialog(const QVector<KeepRule>& rules, bool includeBitDepth, bool keepAll,
                     QWidget* parent = nullptr);

    QVector<KeepRule> rules() const { return m_rules; }
    bool keepAll() const;

private:
    int currentRow() const;
    void buildList(int selectRow);
    void refreshRow(int row);
    void updateDirectionCombo();
    void applyDirection();
    void moveSelected(int delta);
    void updateEnabledState();

    QVector<KeepRule> m_rules;
    QListWidget* m_list = nullptr;
    QComboBox* m_direction = nullptr;
    QPushButton* m_up = nullptr;
    QPushButton* m_down = nullptr;
    QCheckBox* m_keepAll = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_updatingDirection = false;
};

#endif  // KEEPPOLICYDIALOG_H
