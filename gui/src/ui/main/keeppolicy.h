#ifndef KEEPPOLICY_H
#define KEEPPOLICY_H

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>

// One criterion in the keep-policy priority list. A rule says which measurement
// to compare and which direction wins it.
enum class KeepCriterion {
    Resolution,
    FileSize,
    Modified,
    BitDepth,  // images only; normaliseKeepRules drops it for video
};

enum class KeepDirection {
    Higher,  // highest resolution / largest file / newest modified / highest depth
    Lower,
};

struct KeepRule {
    KeepCriterion criterion = KeepCriterion::Resolution;
    KeepDirection direction = KeepDirection::Higher;
};

// The four labels here are also used as the dialog's list text, so they live in
// one translation context rather than being repeated per page.
inline QString keepCriterionName(KeepCriterion criterion)
{
    switch (criterion) {
        case KeepCriterion::Resolution:
            return QCoreApplication::translate("KeepPolicyDialog", "Resolution");
        case KeepCriterion::FileSize:
            return QCoreApplication::translate("KeepPolicyDialog", "File size");
        case KeepCriterion::Modified:
            return QCoreApplication::translate("KeepPolicyDialog", "Modified date");
        case KeepCriterion::BitDepth:
            return QCoreApplication::translate("KeepPolicyDialog", "Bit depth");
    }
    return QString();
}

inline QString keepDirectionName(const KeepRule& rule)
{
    switch (rule.criterion) {
        case KeepCriterion::Resolution:
        case KeepCriterion::BitDepth:
            return rule.direction == KeepDirection::Higher
                       ? QCoreApplication::translate("KeepPolicyDialog", "Highest")
                       : QCoreApplication::translate("KeepPolicyDialog", "Lowest");
        case KeepCriterion::FileSize:
            return rule.direction == KeepDirection::Higher
                       ? QCoreApplication::translate("KeepPolicyDialog", "Largest")
                       : QCoreApplication::translate("KeepPolicyDialog", "Smallest");
        case KeepCriterion::Modified:
            return rule.direction == KeepDirection::Higher
                       ? QCoreApplication::translate("KeepPolicyDialog", "Newest")
                       : QCoreApplication::translate("KeepPolicyDialog", "Oldest");
    }
    return QString();
}

inline QString keepRuleLabel(const KeepRule& rule)
{
    return QCoreApplication::translate("KeepPolicyDialog", "%1 - %2")
        .arg(keepCriterionName(rule.criterion), keepDirectionName(rule));
}

// Default priority: resolution, then file size, then modified date, then bit
// depth (images only). The direction matches the old primary policy.
inline QVector<KeepRule> defaultKeepRules(bool includeBitDepth)
{
    QVector<KeepRule> rules;
    rules.append({KeepCriterion::Resolution, KeepDirection::Higher});
    rules.append({KeepCriterion::FileSize, KeepDirection::Higher});
    rules.append({KeepCriterion::Modified, KeepDirection::Higher});
    if (includeBitDepth) {
        rules.append({KeepCriterion::BitDepth, KeepDirection::Higher});
    }
    return rules;
}

// Keeps one rule per criterion, in the given order, and appends any criterion
// the input omitted. Settings written by an older build therefore still load.
inline QVector<KeepRule> normaliseKeepRules(const QVector<KeepRule>& input,
                                            bool includeBitDepth)
{
    QVector<KeepRule> result;
    const auto add = [&result, includeBitDepth](const KeepRule& rule) {
        if (rule.criterion == KeepCriterion::BitDepth && !includeBitDepth) {
            return;
        }
        for (const KeepRule& existing : result) {
            if (existing.criterion == rule.criterion) {
                return;
            }
        }
        result.append(rule);
    };
    for (const KeepRule& rule : input) {
        add(rule);
    }
    for (const KeepRule& rule : defaultKeepRules(includeBitDepth)) {
        add(rule);
    }
    return result;
}

inline QString keepPrioritySummary(const QVector<KeepRule>& rules, bool keepAll)
{
    if (keepAll) {
        return QCoreApplication::translate("KeepPolicyDialog", "Keep every file checked");
    }
    QStringList parts;
    for (const KeepRule& rule : rules) {
        parts.append(keepRuleLabel(rule));
    }
    return parts.join(QStringLiteral(" > "));
}

// Compact form for QSettings: "<criterion>:<1=higher|0=lower>".
inline QStringList encodeKeepRules(const QVector<KeepRule>& rules)
{
    QStringList encoded;
    for (const KeepRule& rule : rules) {
        encoded.append(QStringLiteral("%1:%2")
                           .arg(static_cast<int>(rule.criterion))
                           .arg(rule.direction == KeepDirection::Higher ? 1 : 0));
    }
    return encoded;
}

inline QVector<KeepRule> decodeKeepRules(const QStringList& encoded, bool includeBitDepth)
{
    QVector<KeepRule> parsed;
    for (const QString& value : encoded) {
        const QStringList parts = value.split(QLatin1Char(':'));
        if (parts.size() != 2) {
            continue;
        }
        bool criterionOk = false;
        bool directionOk = false;
        const int criterion = parts.at(0).toInt(&criterionOk);
        const int direction = parts.at(1).toInt(&directionOk);
        if (!criterionOk || !directionOk || direction < 0 || direction > 1) {
            continue;
        }
        if (criterion < static_cast<int>(KeepCriterion::Resolution)
            || criterion > static_cast<int>(KeepCriterion::BitDepth)) {
            continue;
        }
        parsed.append({static_cast<KeepCriterion>(criterion),
                       direction == 1 ? KeepDirection::Higher : KeepDirection::Lower});
    }
    return normaliseKeepRules(parsed, includeBitDepth);
}

#endif  // KEEPPOLICY_H
