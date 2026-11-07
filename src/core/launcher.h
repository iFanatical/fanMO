/* Running Windows programs in the game's Proton prefix -- the same way your
 * vortex.sh does, with the variables Proton wants from Steam. */
#ifndef FANMO_LAUNCHER_H
#define FANMO_LAUNCHER_H

#include "instance.h"

#include <QProcess>
#include <QString>

namespace Launcher {

/* Sets up a QProcess ready to start. On a bad setup returns false with
 * *error saying which part is missing. */
bool prepare(const Instance &instance, const Tool &tool, QProcess *process, QString *error);

/* Tool::exe made absolute. */
QString resolveExe(const Instance &instance, const Tool &tool);

/* Splits an argument string, honouring double quotes. */
QStringList splitArguments(const QString &text);

} /* namespace Launcher */

#endif
