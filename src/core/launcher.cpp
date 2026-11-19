#include "launcher.h"

#include "steam.h"
#include "util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <cstring>
#include <vector>
#include <QProcessEnvironment>

namespace Launcher {

QString resolveExe(const Instance &instance, const Tool &tool)
{
    if (QFileInfo(tool.exe).isAbsolute())
        return tool.exe;
    return instance.gamePath() + QLatin1Char('/') + tool.exe;
}

std::optional<Tool> findTool(const Instance &instance, const QString &name)
{
    if (!name.isEmpty()) {
        for (const Tool &t : instance.tools())
            if (t.name.compare(name, Qt::CaseInsensitive) == 0)
                return t;
        return std::nullopt;
    }
    for (const char *exe : {"skse64_loader.exe", "SkyrimSE.exe"}) {
        for (const Tool &t : instance.tools())
            if (QFileInfo(t.exe).fileName().compare(QLatin1String(exe), Qt::CaseInsensitive) == 0
                && QFileInfo::exists(resolveExe(instance, t)))
                return t;
    }
    for (const char *exe : {"skse64_loader.exe", "SkyrimSE.exe"}) {
        if (QFileInfo::exists(instance.gamePath() + QLatin1Char('/') + QLatin1String(exe))) {
            Tool t;
            t.name = QLatin1String(exe).startsWith(QLatin1String("skse")) ? QStringLiteral("SKSE") : QStringLiteral("Skyrim");
            t.exe = QLatin1String(exe);
            return t;
        }
    }
    return std::nullopt;
}

QString logFile(const Instance &instance, const Tool &tool)
{
    const QString dir = instance.root() + QStringLiteral("/logs");
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + Util::safeFolderName(tool.name) + QStringLiteral(".log");
}

QStringList failureHints(const QString &logFile)
{
    QFile f(logFile);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QStringList lines;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.contains(QLatin1String("err:")) || line.contains(QLatin1String("not found"), Qt::CaseInsensitive))
            lines << line.left(200);
    }
    return lines.mid(qMax(0, int(lines.size()) - 4));
}

namespace {

/* Just enough of a PE file to walk its resources, every read bounds-checked:
 * a program is anybody's file. */
class PeResources {
public:
    explicit PeResources(QByteArrayView data) : m_d(data) { parse(); }
    bool ok() const { return m_root >= 0; }

    /* The first entry of a directory with this id, or any entry for id < 0:
     * returns the offset of its subdirectory, or of its data entry. */
    qint64 find(qint64 dir, qint64 id, bool wantDir) const
    {
        const quint16 named = u16(dir + 12), ids = u16(dir + 14);
        for (int i = 0; i < named + ids && i < 4096; ++i) {
            const qint64 e = dir + 16 + 8 * qint64(i);
            const quint32 name = u32(e), target = u32(e + 4);
            if (m_bad)
                return -1;
            const bool isDir = target & 0x80000000u;
            if (isDir != wantDir || (id >= 0 && ((name & 0x80000000u) || name != id)))
                continue;
            return m_root + (target & 0x7fffffffu);
        }
        return -1;
    }
    /* The bytes behind a resource: type, then name, then the first language. */
    QByteArrayView resource(quint32 type, qint64 name) const
    {
        qint64 dir = find(m_root, type, true);
        if (dir >= 0)
            dir = find(dir, name, true);
        const qint64 entry = dir >= 0 ? find(dir, -1, false) : -1;
        if (entry < 0)
            return {};
        const qint64 at = offset(u32(entry)), size = u32(entry + 4);
        if (m_bad || at < 0 || size <= 0 || at + size > m_d.size())
            return {};
        return m_d.sliced(at, size);
    }
    quint16 u16(qint64 at) const
    {
        if (at < 0 || at + 2 > m_d.size()) { m_bad = true; return 0; }
        return qFromLittleEndian<quint16>(m_d.data() + at);
    }
    quint32 u32(qint64 at) const
    {
        if (at < 0 || at + 4 > m_d.size()) { m_bad = true; return 0; }
        return qFromLittleEndian<quint32>(m_d.data() + at);
    }

private:
    struct Section { quint32 va, size, raw, rawSize; };
    void parse()
    {
        if (m_d.size() < 64 || m_d[0] != 'M' || m_d[1] != 'Z')
            return;
        const qint64 pe = u32(0x3c);
        if (u32(pe) != 0x00004550u)  /* "PE\0\0" */
            return;
        const quint16 sections = u16(pe + 6), optionalSize = u16(pe + 20);
        const qint64 opt = pe + 24;
        const quint16 magic = u16(opt);
        const qint64 dirs = opt + (magic == 0x20b ? 112 : 96);
        if (u32(opt + (magic == 0x20b ? 108 : 92)) < 3)  /* no resource directory */
            return;
        const quint32 rva = u32(dirs + 16);
        for (int i = 0; i < sections; ++i) {
            const qint64 s = opt + optionalSize + 40 * qint64(i);
            m_sections.push_back({u32(s + 12), u32(s + 8), u32(s + 20), u32(s + 16)});
        }
        const qint64 root = offset(rva);
        if (!m_bad && rva && root >= 0)
            m_root = root;
    }
    qint64 offset(quint32 rva) const
    {
        for (const Section &s : m_sections)
            if (rva >= s.va && rva - s.va < s.rawSize)
                return qint64(s.raw) + (rva - s.va);
        return -1;
    }

    QByteArrayView m_d;
    std::vector<Section> m_sections;
    qint64 m_root = -1;
    mutable bool m_bad = false;
};

} /* namespace */

QByteArray exeIcon(const QString &exe)
{
    QFile file(exe);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const uchar *map = file.map(0, file.size());
    if (!map)
        return {};
    const PeResources pe(QByteArrayView(map, file.size()));
    if (!pe.ok())
        return {};
    enum : quint32 { RtIcon = 3, RtGroupIcon = 14 };
    /* The first icon group is the program's own; its directory lists sizes. */
    const QByteArrayView group = pe.resource(RtGroupIcon, -1);
    if (group.size() < 6)
        return {};
    const int count = qFromLittleEndian<quint16>(group.data() + 4);
    int best = -1, bestSize = 0, bestBits = 0;
    for (int i = 0; i < count && 6 + 14 * (i + 1) <= group.size(); ++i) {
        const char *e = group.data() + 6 + 14 * i;
        const int size = quint8(e[0]) ? quint8(e[0]) : 256;
        const int bits = qFromLittleEndian<quint16>(e + 6);
        if (size > bestSize || (size == bestSize && bits > bestBits)) {
            best = i;
            bestSize = size;
            bestBits = bits;
        }
    }
    if (best < 0)
        return {};
    const char *e = group.data() + 6 + 14 * best;
    const QByteArrayView image = pe.resource(RtIcon, qFromLittleEndian<quint16>(e + 12));
    if (image.isEmpty())
        return {};
    /* An .ico file is the same directory, pointing at offsets, not ids. */
    QByteArray ico(22, '\0');
    qToLittleEndian<quint16>(1, ico.data() + 2);
    qToLittleEndian<quint16>(1, ico.data() + 4);
    memcpy(ico.data() + 6, e, 8);
    qToLittleEndian<quint32>(quint32(image.size()), ico.data() + 14);
    qToLittleEndian<quint32>(22, ico.data() + 18);
    ico += image.toByteArray();
    return ico;
}

QString desktopExec(const QStringList &command)
{
    static const QString reserved = QStringLiteral(" \t\n\"'\\><~|&;$*?#()`");
    QStringList parts;
    for (const QString &arg : command) {
        bool quote = arg.isEmpty();
        for (const QChar c : arg)
            quote = quote || reserved.contains(c);
        if (!quote) {
            parts << arg;
            continue;
        }
        QString q;
        for (const QChar c : arg) {
            if (c == u'"' || c == u'`' || c == u'$' || c == u'\\')
                q += u'\\';
            q += c;
        }
        parts << QLatin1Char('"') + q + QLatin1Char('"');
    }
    /* Then the rules for any string value: a backslash doubles again, and a
     * percent sign is a field code unless doubled. */
    QString line = parts.join(QLatin1Char(' '));
    line.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    line.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return line;
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
