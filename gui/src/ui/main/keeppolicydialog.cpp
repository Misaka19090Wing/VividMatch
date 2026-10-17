#include "keeppolicydialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

KeepPolicyDialog::KeepPolicyDialog(const QVector<KeepRule>& rules, bool includeBitDepth,
                                   bool keepAll, QWidget* parent)
    : QDialog(parent)
    , m_rules(normaliseKeepRules(rules, includeBitDepth))
{
    setWindowTitle(tr("Keep policy priorities"));

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setSpacing(10);

    QLabel* hint = new QLabel(this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#475569;"));
    hint->setText(tr("Files are compared by the first rule; if they tie, the next rule "
                     "is used."));
    root->addWidget(hint);

    QHBoxLayout* listRow = new QHBoxLayout;
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setMinimumHeight(150);
    listRow->addWidget(m_list, 1);

    QVBoxLayout* moveColumn = new QVBoxLayout;
    m_up = new QPushButton(tr("Move Up"), this);
    m_up->setObjectName(QStringLiteral("secondaryButton"));
    m_down = new QPushButton(tr("Move Down"), this);
    m_down->setObjectName(QStringLiteral("secondaryButton"));
    moveColumn->addWidget(m_up);
    moveColumn->addWidget(m_down);
    moveColumn->addStretch(1);
    listRow->addLayout(moveColumn);
    root->addLayout(listRow);

    QHBoxLayout* directionRow = new QHBoxLayout;
    QLabel* directionLabel = new QLabel(tr("Direction"), this);
    m_direction = new QComboBox(this);
    directionRow->addWidget(directionLabel);
    directionRow->addWidget(m_direction, 1);
    root->addLayout(directionRow);

    m_keepAll = new QCheckBox(tr("Keep every file checked"), this);
    m_keepAll->setChecked(keepAll);
    root->addWidget(m_keepAll);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                     this);
    m_buttons->button(QDialogButtonBox::Save)->setText(tr("Save"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(m_buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) {
        updateDirectionCombo();
        updateEnabledState();
    });
    connect(m_up, &QPushButton::clicked, this, [this]() { moveSelected(-1); });
    connect(m_down, &QPushButton::clicked, this, [this]() { moveSelected(1); });
    connect(m_direction, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyDirection(); });
    connect(m_keepAll, &QCheckBox::toggled, this, [this](bool) { updateEnabledState(); });

    buildList(0);
    setMinimumSize(520, 380);
}

bool KeepPolicyDialog::keepAll() const
{
    return m_keepAll->isChecked();
}

int KeepPolicyDialog::currentRow() const
{
    return m_list->currentRow();
}

void KeepPolicyDialog::buildList(int selectRow)
{
    m_list->clear();
    for (int i = 0; i < static_cast<int>(m_rules.size()); ++i) {
        m_list->addItem(tr("%1. %2").arg(i + 1).arg(keepRuleLabel(m_rules.at(i))));
    }
    if (!m_rules.isEmpty()) {
        m_list->setCurrentRow(
            std::max(0, std::min(selectRow, static_cast<int>(m_rules.size()) - 1)));
    }
    updateDirectionCombo();
    updateEnabledState();
}

void KeepPolicyDialog::refreshRow(int row)
{
    QListWidgetItem* item = m_list->item(row);
    if (item == nullptr) {
        return;
    }
    item->setText(tr("%1. %2").arg(row + 1).arg(keepRuleLabel(m_rules.at(row))));
}

void KeepPolicyDialog::updateDirectionCombo()
{
    const int row = currentRow();
    if (row < 0 || row >= m_rules.size()) {
        m_direction->clear();
        return;
    }

    m_updatingDirection = true;
    m_direction->clear();
    const KeepRule higher{m_rules.at(row).criterion, KeepDirection::Higher};
    const KeepRule lower{m_rules.at(row).criterion, KeepDirection::Lower};
    m_direction->addItem(keepDirectionName(higher), 1);
    m_direction->addItem(keepDirectionName(lower), 0);
    m_direction->setCurrentIndex(m_rules.at(row).direction == KeepDirection::Higher ? 0
                                                                                    : 1);
    m_updatingDirection = false;
}

void KeepPolicyDialog::applyDirection()
{
    if (m_updatingDirection) {
        return;
    }
    const int row = currentRow();
    if (row < 0 || row >= m_rules.size()) {
        return;
    }
    m_rules[row].direction =
        m_direction->currentData().toInt() == 1 ? KeepDirection::Higher
                                                : KeepDirection::Lower;
    refreshRow(row);
}

void KeepPolicyDialog::moveSelected(int delta)
{
    const int row = currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_rules.size()) {
        return;
    }
    std::swap(m_rules[row], m_rules[target]);
    buildList(target);
}

void KeepPolicyDialog::updateEnabledState()
{
    const bool enabled = !m_keepAll->isChecked();
    const int row = currentRow();
    m_list->setEnabled(enabled);
    m_direction->setEnabled(enabled && row >= 0);
    m_up->setEnabled(enabled && row > 0);
    m_down->setEnabled(enabled && row >= 0 && row + 1 < m_list->count());
}
