#include "launcher.h"

#include "steam.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

namespace Launcher {

QString resolveExe(const Instance &instance, const Tool &tool)
{
    if (QFileInfo(tool.exe).isAbsolute())
        return tool.exe;
    return instance.gamePath() + QLatin1Char('/') + tool.exe;
}

bool isWindowsProgram(const QString &exe)
{
    static const QStringList windows{QStringLiteral("exe"), QStringLiteral("bat"), QStringLiteral("cmd"),
                                     QStringLiteral("com"), QStringLiteral("msi")};
    return windows.contains(QFileInfo(exe).suffix().toLower());
}

bool isNativeLoot(const Tool &tool)
{
    return !isWindowsProgram(tool.exe)
        && QFileInfo(tool.exe).fileName().compare(QLatin1String("LOOT"), Qt::CaseInsensitive) == 0;
}

QString nativeLootDataPath(const Tool &tool)
{
    const QStringList args = splitArguments(tool.arguments);
    const QString flag = QStringLiteral("--loot-data-path");
    for (qsizetype i = 0; i < args.size(); ++i) {
        if (args[i].startsWith(flag + QLatin1Char('=')))
            return args[i].mid(flag.size() + 1);
        if (args[i] == flag && i + 1 < args.size())
            return args[i + 1];
    }
    /* LOOT's getLocalAppDataPath() on Linux. */
    QString base = qEnvironmentVariable("XDG_DATA_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/LOOT");
}

QStringList splitArguments(const QString &text)
{
    QStringList out;
    QString current;
    bool quoted = false, started = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            started = true;
        } else if (c.isSpace() && !quoted) {
            if (started)
                out << current;
            current.clear();
            started = false;
        } else {
            current += c;
            started = true;
        }
    }
    if (started)
        out << current;
    return out;
}

bool prepare(const Instance &instance, const Tool &tool, QProcess *process, QString *error)
{
    const QString exe = resolveExe(instance, tool);
    if (!QFileInfo::exists(exe)) {
        *error = QStringLiteral("%1 not found at %2").arg(tool.name, exe);
        return false;
    }
    const QString workingDir = tool.workingDir.isEmpty() ? QFileInfo(exe).absolutePath() : tool.workingDir;
    if (!isWindowsProgram(exe)) {
        if (!QFileInfo(exe).isExecutable()) {
            *error = QStringLiteral("%1 isn't executable: chmod +x %2").arg(tool.name, exe);
            return false;
        }
        /* A Linux program finds the game and the prefix itself, or from its
         * arguments: Steam's variables would only mislead it. */
        process->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
        process->setProgram(exe);
        process->setArguments(splitArguments(tool.arguments));
        process->setWorkingDirectory(workingDir);
        return true;
    }
    if (!QFileInfo::exists(instance.protonPath())) {
        *error = QStringLiteral("Proton not found at %1; set it in Settings").arg(instance.protonPath());
        return false;
    }
    if (instance.compatDataPath().isEmpty()) {
        *error = QStringLiteral("no Proton prefix yet: run the game from Steam once");
        return false;
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("STEAM_COMPAT_DATA_PATH"), instance.compatDataPath());
    env.insert(QStringLiteral("STEAM_COMPAT_CLIENT_INSTALL_PATH"), Steam::steamRoot());
    const QString appId = QString::number(Steam::skyrimAppId);
    env.insert(QStringLiteral("STEAM_COMPAT_APP_ID"), appId);
    env.insert(QStringLiteral("SteamAppId"), appId);
    env.insert(QStringLiteral("SteamGameId"), appId);
    /* What Steam passes too. With both, Proton maps the library to S:, where
     * the game registers itself ("S:\\common\\Skyrim Special Edition"): a
     * tool started without them, LOOT for one, can't find the game. */
    env.insert(QStringLiteral("STEAM_COMPAT_INSTALL_PATH"), instance.gamePath());
    const QDir common = QFileInfo(instance.gamePath()).dir();
    if (common.dirName().compare(QLatin1String("common"), Qt::CaseInsensitive) == 0)
        env.insert(QStringLiteral("STEAM_COMPAT_LIBRARY_PATHS"), QFileInfo(common.absolutePath()).absolutePath());
    process->setProcessEnvironment(env);
    process->setProgram(instance.protonPath());
    process->setArguments(QStringList{QStringLiteral("run"), exe} + splitArguments(tool.arguments));
    process->setWorkingDirectory(workingDir);
    return true;
}

} /* namespace Launcher */
