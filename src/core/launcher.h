/* Running tools: Windows programs in the game's Proton prefix -- the same way
 * your vortex.sh does, with the variables Proton wants from Steam -- and Linux
 * programs, such as a native LOOT, directly. */
#ifndef FANMO_LAUNCHER_H
#define FANMO_LAUNCHER_H

#include "instance.h"

#include <QProcess>
#include <QString>

#include <optional>

namespace Launcher {

/* Sets up a QProcess ready to start. On a bad setup returns false with
 * *error saying which part is missing. */
bool prepare(const Instance &instance, const Tool &tool, QProcess *process, QString *error);

/* Tool::exe made absolute. */
QString resolveExe(const Instance &instance, const Tool &tool);

/* Whether a program needs Proton: .exe, .bat, .cmd, .com and .msi do,
 * anything else runs as it is. */
bool isWindowsProgram(const QString &exe);

/* A Linux build of LOOT: a program called LOOT that isn't a Windows one. */
bool isNativeLoot(const Tool &tool);

/* Where a native LOOT keeps its data: the tool's --loot-data-path if it has
 * one, otherwise LOOT's own default, $XDG_DATA_HOME/LOOT. */
QString nativeLootDataPath(const Tool &tool);

/* A tool by name, ignoring case. No name means the game: a tool running
 * skse64_loader.exe, else SkyrimSE.exe, whose program is there (SKSE is
 * only once it's deployed), else either one run bare from the game folder. */
std::optional<Tool> findTool(const Instance &instance, const QString &name);

/* Where a tool's output goes: logs/<name>.log in the instance, rewritten each
 * run. The folder is made if it isn't there. */
QString logFile(const Instance &instance, const Tool &tool);

/* From a failed run's output, the last few lines likeliest to say why: a
 * missing DLL, say. */
QStringList failureHints(const QString &logFile);

/* The biggest icon a Windows program carries, as a one-image .ico file for
 * QImage to read. Empty when it has none or isn't a program at all. */
QByteArray exeIcon(const QString &exe);

/* A .desktop file's Exec= value running command, quoted and escaped the way
 * the Desktop Entry spec wants. */
QString desktopExec(const QStringList &command);

/* Splits an argument string, honouring double quotes. */
QStringList splitArguments(const QString &text);

} /* namespace Launcher */

#endif
