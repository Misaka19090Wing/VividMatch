#ifndef FORMATUTILS_H
#define FORMATUTILS_H

#include <QCoreApplication>
#include <QString>

namespace formatutils {

// Renders a measured duration without any label, e.g. "140 ms" or "11.27 s".
//
// Milliseconds carry no useful digits once a comparison took seconds, so the
// unit switches with the magnitude and the number stays readable at both ends
// of the range. Composing callers use this form so the label is not repeated;
// standalone callers use durationLabel() below.
inline QString durationValue(qint64 milliseconds)
{
    if (milliseconds < 0) {
        milliseconds = 0;
    }
    if (milliseconds < 1000) {
        return QCoreApplication::translate("formatutils", "%1 ms").arg(milliseconds);
    }
    if (milliseconds < 60000) {
        const double seconds = static_cast<double>(milliseconds) / 1000.0;
        return QCoreApplication::translate("formatutils", "%1 s")
            .arg(QString::number(seconds, 'f', 2));
    }
    const qint64 totalSeconds = milliseconds / 1000;
    return QCoreApplication::translate("formatutils", "%1 min %2 s")
        .arg(totalSeconds / 60)
        .arg(totalSeconds % 60);
}

// Same value with the "Time taken" label, for panels that show a single timing.
inline QString durationLabel(qint64 milliseconds)
{
    return QCoreApplication::translate("formatutils", "Time %1")
        .arg(durationValue(milliseconds));
}

}  // namespace formatutils

#endif // FORMATUTILS_H
