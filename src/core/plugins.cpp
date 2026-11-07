#include "plugins.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace Plugins {

static quint32 u32(const char *p)
{
    return quint8(p[0]) | quint8(p[1]) << 8 | quint8(p[2]) << 16 | quint32(quint8(p[3])) << 24;
}

static quint16 u16(const char *p)
{
    return quint16(quint8(p[0]) | quint8(p[1]) << 8);
}

Header readHeader(const QString &path)
{
    Header h;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return h;
    const QByteArray head = f.read(24);
    if (head.size() < 24 || !head.startsWith("TES4"))
        return h;
    const quint32 size = u32(head.constData() + 4);
    const quint32 flags = u32(head.constData() + 8);
    h.master = flags & 0x1;
    h.light = flags & 0x200;
    if (size > 16 * 1024 * 1024)
        return h;
    const QByteArray body = f.read(size);
    if (quint32(body.size()) < size)
        return h;
    h.valid = true;
    qsizetype pos = 0;
    while (pos + 6 <= body.size()) {
        const QByteArray type = body.mid(pos, 4);
        const quint16 len = u16(body.constData() + pos + 4);
        pos += 6;
        if (pos + len > body.size())
            break;
        /* Plugin text is a Windows code page; Latin-1 can't fail on it. */
        const QString text = QString::fromLatin1(body.constData() + pos,
                                                 qstrnlen(body.constData() + pos, len));
        if (type == "MAST")
            h.masters << text;
        else if (type == "CNAM")
            h.author = text;
        else if (type == "SNAM")
            h.description = text;
        pos += len;
    }
    return h;
}

QStringList earlyLoaders(const Instance &instance)
{
    QStringList out = baseMasters();
    QFile ccc(instance.gamePath() + QStringLiteral("/Skyrim.ccc"));
    if (ccc.open(QIODevice::ReadOnly))
        while (!ccc.atEnd()) {
            const QString line = QString::fromUtf8(ccc.readLine()).trimmed();
            if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')))
                out << line;
        }
    return out;
}

bool isPluginName(const QString &name)
{
    return name.endsWith(QLatin1String(".esp"), Qt::CaseInsensitive)
        || name.endsWith(QLatin1String(".esm"), Qt::CaseInsensitive)
        || name.endsWith(QLatin1String(".esl"), Qt::CaseInsensitive);
}

QStringList baseMasters()
{
    return {QStringLiteral("Skyrim.esm"), QStringLiteral("Update.esm"),
            QStringLiteral("Dawnguard.esm"), QStringLiteral("HearthFires.esm"),
            QStringLiteral("Dragonborn.esm")};
}

std::vector<Available> available(const Instance &instance, const Deploy::FileIndex &index)
{
    std::vector<Available> out;
    QSet<QString> seen;
    for (auto it = index.providers.cbegin(); it != index.providers.cend(); ++it) {
        const auto &top = it.value().back();
        /* Plugins only load from the top of Data. */
        if (!it.key().startsWith(QLatin1String("d/")) || top.relPath.contains(QLatin1Char('/'))
            || !isPluginName(top.relPath))
            continue;
        const Deploy::Layer &layer = index.layers[size_t(top.layer)];
        out.push_back({top.relPath, layer.path + QLatin1Char('/') + top.relPath, layer.id});
        seen.insert(top.relPath.toLower());
    }
    for (const QFileInfo &fi : QDir(instance.dataPath()).entryInfoList(QDir::Files)) {
        if (isPluginName(fi.fileName()) && !seen.contains(fi.fileName().toLower())) {
            out.push_back({fi.fileName(), fi.filePath(), QString()});
            seen.insert(fi.fileName().toLower());
        }
    }
    return out;
}

int reconcile(const Instance &instance, Profile *profile, const std::vector<Available> &present)
{
    QHash<QString, QString> names;
    for (const Available &a : present)
        names.insert(a.name.toLower(), a.name);
    int changed = 0;
    std::vector<ProfilePlugin> kept;
    QSet<QString> have;
    for (ProfilePlugin &p : profile->plugins) {
        const auto it = names.constFind(p.name.toLower());
        if (it == names.constEnd() || have.contains(p.name.toLower())) {
            ++changed;
            continue;
        }
        have.insert(p.name.toLower());
        kept.push_back({*it, p.enabled});
    }
    QStringList added;
    for (const Available &a : present)
        if (!have.contains(a.name.toLower()))
            added << a.name;
    added.sort(Qt::CaseInsensitive);
    for (const QString &name : std::as_const(added)) {
        kept.push_back({name, true});
        ++changed;
    }
    /* Show early loaders where the game puts them, and on, as the game has them. */
    std::vector<ProfilePlugin> ordered;
    QSet<QString> early;
    for (const QString &name : earlyLoaders(instance)) {
        early.insert(name.toLower());
        for (const ProfilePlugin &p : kept)
            if (p.name.compare(name, Qt::CaseInsensitive) == 0)
                ordered.push_back({p.name, true});
    }
    for (const ProfilePlugin &p : kept)
        if (!early.contains(p.name.toLower()))
            ordered.push_back(p);
    profile->plugins = std::move(ordered);
    return changed;
}

void sortSafely(const Instance &instance, Profile *profile, const std::vector<Available> &present,
                const std::vector<OrderRule> &rules)
{
    reconcile(instance, profile, present);
    const int n = int(profile->plugins.size());
    QHash<QString, int> index;
    for (int i = 0; i < n; ++i)
        index.insert(profile->plugins[size_t(i)].name.toLower(), i);

    QHash<QString, const Available *> byName;
    for (const Available &a : present)
        byName.insert(a.name.toLower(), &a);

    std::vector<std::vector<int>> before(static_cast<size_t>(n));
    std::vector<char> isMaster(size_t(n), 0);
    for (int i = 0; i < n; ++i) {
        const Available *a = byName.value(profile->plugins[size_t(i)].name.toLower());
        if (!a)
            continue;
        const Header h = readHeader(a->path);
        isMaster[size_t(i)] = h.master;
        for (const QString &m : h.masters)
            if (const auto it = index.constFind(m.toLower()); it != index.constEnd())
                before[size_t(i)].push_back(*it);
    }
    for (const OrderRule &rule : rules) {
        const auto self = index.constFind(rule.plugin.toLower());
        if (self == index.constEnd())
            continue;
        for (const QString &a : rule.after)
            if (const auto it = index.constFind(a.toLower()); it != index.constEnd())
                before[size_t(*self)].push_back(*it);
    }

    const QStringList early = earlyLoaders(instance);
    QSet<int> earlySet;
    for (const QString &name : early)
        if (const auto it = index.constFind(name.toLower()); it != index.constEnd())
            earlySet.insert(*it);

    /* Depth-first in list order: only what a constraint moves, moves. A rule
     * that would close a loop loses to whatever got there first. */
    std::vector<char> state(size_t(n), 0);
    std::vector<ProfilePlugin> out;
    out.reserve(size_t(n));
    const auto emitFrom = [&](int start) {
        struct Frame { int node; size_t next; };
        if (state[size_t(start)] != 0)
            return;
        std::vector<Frame> stack = {{start, 0}};
        state[size_t(start)] = 1;
        while (!stack.empty()) {
            Frame &top = stack.back();
            const auto &deps = before[size_t(top.node)];
            if (top.next < deps.size()) {
                const int dep = deps[top.next++];
                if (state[size_t(dep)] == 0) {
                    state[size_t(dep)] = 1;
                    stack.push_back({dep, 0});
                }
            } else {
                state[size_t(top.node)] = 2;
                out.push_back(profile->plugins[size_t(top.node)]);
                stack.pop_back();
            }
        }
    };
    for (const QString &name : early)
        if (const auto it = index.constFind(name.toLower()); it != index.constEnd())
            emitFrom(*it);
    for (int i = 0; i < n; ++i)
        if (isMaster[size_t(i)])
            emitFrom(i);
    for (int i = 0; i < n; ++i)
        emitFrom(i);
    profile->plugins = std::move(out);
}

int adoptOrder(Profile *profile, const QStringList &reference)
{
    QHash<QString, int> rank;
    for (qsizetype i = 0; i < reference.size(); ++i)
        rank.insert(reference[i].toLower(), int(i));
    /* The slots known plugins hold now, refilled in the reference's order. */
    std::vector<ProfilePlugin> known;
    for (const ProfilePlugin &p : profile->plugins)
        if (rank.contains(p.name.toLower()))
            known.push_back(p);
    std::stable_sort(known.begin(), known.end(), [&](const ProfilePlugin &a, const ProfilePlugin &b) {
        return rank.value(a.name.toLower()) < rank.value(b.name.toLower());
    });
    size_t next = 0;
    for (ProfilePlugin &p : profile->plugins)
        if (rank.contains(p.name.toLower()))
            p = known[next++];
    return int(known.size());
}

QStringList readOrderFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return {};
    }
    QStringList out;
    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QChar(0xfeff)))
            line.remove(0, 1);
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('*')))
            line.remove(0, 1);
        out << line;
    }
    return out;
}

QString lootUserlistPath(const Instance &instance)
{
    return instance.compatDataPath()
        + QStringLiteral("/pfx/drive_c/users/steamuser/AppData/Local/LOOT/games/Skyrim Special Edition/userlist.yaml");
}

/* The file's real name in the prefix: yours has a capital P, others don't. */
static QString findFile(const QString &dir, const QString &name)
{
    for (const QString &entry : QDir(dir).entryList(QDir::Files))
        if (entry.compare(name, Qt::CaseInsensitive) == 0)
            return dir + QLatin1Char('/') + entry;
    return dir + QLatin1Char('/') + name;
}

bool write(const Instance &instance, const Profile &profile, QString *error)
{
    const QString dir = instance.appDataPath();
    QDir().mkpath(dir);
    QByteArray plugins("# Written by fanMO. Edits here are read back after running a tool.\r\n");
    QByteArray order;
    QSet<QString> early;
    for (const QString &name : earlyLoaders(instance))
        early.insert(name.toLower());
    for (const ProfilePlugin &p : profile.plugins) {
        const QByteArray name = p.name.toUtf8();
        order += name + "\r\n";
        /* The game adds these itself; listing them is how orders go wrong. */
        if (!early.contains(p.name.toLower()))
            plugins += (p.enabled ? "*" : "") + name + "\r\n";
    }
    for (const auto &[name, data] : {std::pair{QStringLiteral("plugins.txt"), plugins},
                                     std::pair{QStringLiteral("loadorder.txt"), order}}) {
        const QString path = findFile(dir, name);
        const QString backup = path + QStringLiteral(".fanmo-backup");
        if (QFileInfo::exists(path) && !QFileInfo::exists(backup))
            QFile::copy(path, backup);
        if (!Util::writeFile(path, data, error))
            return false;
    }
    return true;
}

bool read(const Instance &instance, Profile *profile, QString *error)
{
    QFile f(findFile(instance.appDataPath(), QStringLiteral("plugins.txt")));
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return false;
    }
    std::vector<ProfilePlugin> listed;
    QSet<QString> seen;
    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const bool enabled = line.startsWith(QLatin1Char('*'));
        if (enabled)
            line.remove(0, 1);
        if (!seen.contains(line.toLower())) {
            seen.insert(line.toLower());
            listed.push_back({line, enabled});
        }
    }
    /* Plugins.txt leaves out early loaders, always on, and some tools leave
     * out whatever's off: those stay off, at the end. */
    std::vector<ProfilePlugin> order;
    for (const QString &name : earlyLoaders(instance))
        for (const ProfilePlugin &p : profile->plugins)
            if (p.name.compare(name, Qt::CaseInsensitive) == 0 && !seen.contains(p.name.toLower())) {
                order.push_back({p.name, true});
                seen.insert(p.name.toLower());
            }
    order.insert(order.end(), listed.begin(), listed.end());
    for (const ProfilePlugin &p : profile->plugins)
        if (!seen.contains(p.name.toLower()))
            order.push_back({p.name, false});
    profile->plugins = std::move(order);
    profile->needsSorting = false;
    return true;
}

} /* namespace Plugins */
