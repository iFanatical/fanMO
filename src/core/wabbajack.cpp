#include "wabbajack.h"

#include "archive.h"
#include "archiveindex.h"
#include "nexus.h"
#include "patch.h"
#include "plugins.h"
#include "util.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QThreadPool>
#include <QtConcurrent>

#include <unistd.h>

namespace Wabbajack {

namespace {

struct Directive {
    QString type;
    QString to;                 /* posix */
    QString hash;
    qint64 size = 0;
    QStringList archivePath;    /* [archive hash, path inside...] */
    QString sourceDataId;
    QString patchId;
};

struct ModList {
    Summary summary;
    QList<Archive> archives;
    std::vector<Directive> directives;
};

/* Pulls one member out of the .wabbajack (itself a zip). */
bool extractMembers(const QString &file, const QStringList &members, const QString &dest, QString *error)
{
    QDir().mkpath(dest);
    QProcess p;
    QStringList args = {QStringLiteral("e"), QStringLiteral("-y"), QStringLiteral("-bd"),
                        QStringLiteral("-bso0"), QStringLiteral("-bsp0"), QStringLiteral("-o") + dest,
                        QStringLiteral("--"), file};
    args += members;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(::Archive::sevenZip(), args);
    if (!p.waitForFinished(10 * 60 * 1000) || p.exitCode() != 0) {
        *error = QStringLiteral("cannot read %1").arg(QFileInfo(file).fileName());
        return false;
    }
    return true;
}

QString shortType(const QString &type)
{
    return type.section(QLatin1Char(','), 0, 0).remove(QStringLiteral("Downloader"));
}

bool readList(const QString &file, const QString &workDir, ModList *out, QString *error)
{
    if (!extractMembers(file, {QStringLiteral("modlist")}, workDir, error))
        return false;
    const QJsonDocument doc = Util::readJson(workDir + QStringLiteral("/modlist"), error);
    if (!doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    Summary &s = out->summary;
    s.name = root.value(QLatin1String("Name")).toString();
    s.author = root.value(QLatin1String("Author")).toString();
    s.version = root.value(QLatin1String("Version")).toString();
    s.game = root.value(QLatin1String("GameType")).toString();
    s.website = root.value(QLatin1String("Website")).toString();

    const QJsonArray archives = root.value(QLatin1String("Archives")).toArray();
    s.archiveCount = int(archives.size());
    for (const auto &v : archives) {
        const QJsonObject o = v.toObject();
        const QJsonObject state = o.value(QLatin1String("State")).toObject();
        Archive a;
        a.hash = o.value(QLatin1String("Hash")).toString();
        a.name = o.value(QLatin1String("Name")).toString();
        a.size = o.value(QLatin1String("Size")).toInteger();
        a.kind = shortType(state.value(QLatin1String("$type")).toString());
        a.modId = state.value(QLatin1String("ModID")).toInteger();
        a.fileId = state.value(QLatin1String("FileID")).toInteger();
        a.url = state.value(QLatin1String("Url")).toString();
        a.gameFile = state.value(QLatin1String("GameFile")).toString();
        s.archiveBytes += a.size;
        out->archives.append(std::move(a));
    }

    QStringList nested;
    const QJsonArray directives = root.value(QLatin1String("Directives")).toArray();
    s.directiveCount = int(directives.size());
    out->directives.reserve(size_t(directives.size()));
    for (const auto &v : directives) {
        const QJsonObject o = v.toObject();
        Directive d;
        d.type = o.value(QLatin1String("$type")).toString().section(QLatin1Char(','), 0, 0);
        d.to = Util::posixPath(o.value(QLatin1String("To")).toString());
        d.hash = o.value(QLatin1String("Hash")).toString();
        d.size = o.value(QLatin1String("Size")).toInteger();
        d.sourceDataId = o.value(QLatin1String("SourceDataID")).toString();
        d.patchId = o.value(QLatin1String("PatchID")).toString();
        for (const auto &p : o.value(QLatin1String("ArchiveHashPath")).toArray())
            d.archivePath << p.toString();
        ++s.directiveTypes[d.type];
        if (d.archivePath.size() > 2)
            nested << d.to;
        out->directives.push_back(std::move(d));
    }

    static const QStringList known = {QStringLiteral("FromArchive"), QStringLiteral("PatchedFromArchive"),
                                      QStringLiteral("InlineFile"), QStringLiteral("RemappedInlineFile")};
    for (auto it = s.directiveTypes.cbegin(); it != s.directiveTypes.cend(); ++it)
        if (!known.contains(it.key()))
            s.unsupported << QStringLiteral("%1 × %2").arg(it.key()).arg(it.value());
    if (!nested.isEmpty())
        s.unsupported << QStringLiteral("files inside BSAs or nested archives × %1").arg(nested.size());
    if (s.game != QLatin1String("SkyrimSpecialEdition"))
        s.unsupported << QStringLiteral("game %1").arg(s.game);
    return true;
}

/* MO2's top-level folders that are game content. Everything else in a list
 * is MO2 itself. */
bool wantedOutput(const QString &to)
{
    const QString head = to.section(QLatin1Char('/'), 0, 0).toLower();
    return head == QLatin1String("mods") || head == QLatin1String("profiles")
        || head == QLatin1String("game root");
}

QString remapTokens(QString text, const Instance &instance, const QString &output)
{
    /* The Windows view of a Linux path, inside Proton: Z: maps to /. */
    const auto windowsPath = [](const QString &path, const QString &sep) {
        QString w = QStringLiteral("Z:") + path;
        w.replace(QLatin1Char('/'), sep);
        return w;
    };
    const QStringList kinds = {QStringLiteral("GAME_PATH"), QStringLiteral("MO2_PATH"),
                               QStringLiteral("DOWNLOAD_PATH")};
    const QStringList paths = {instance.gamePath(), output, instance.downloadsPath()};
    const QList<std::pair<const char *, QString>> forms = {
        {"MAGIC_BACK", QStringLiteral("\\")},
        {"MAGIC_DOUBLE_BACK", QStringLiteral("\\\\")},
        {"MAGIC_FORWARD", QStringLiteral("/")},
    };
    for (qsizetype i = 0; i < kinds.size(); ++i)
        for (const auto &[suffix, sep] : forms)
            text.replace(QStringLiteral("{--||%1_%2||--}").arg(kinds[i], QLatin1String(suffix)),
                         windowsPath(paths[i], sep));
    return text;
}

/* Output path -> final file path, resolved case-insensitively so "mods/A/x"
 * and "mods/a/X" land together. */
class Writer {
public:
    explicit Writer(QString root) : m_root(std::move(root)) {}

    QString prepare(const QString &rel)
    {
        const qsizetype slash = rel.lastIndexOf(QLatin1Char('/'));
        QString built;
        {
            QMutexLocker guard(&m_lock);
            if (slash > 0)
                for (const QString &part : rel.left(slash).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                    const QString candidate = built.isEmpty() ? part : built + QLatin1Char('/') + part;
                    const QString key = Util::foldKey(candidate);
                    auto it = m_dirs.constFind(key);
                    if (it == m_dirs.constEnd()) {
                        it = m_dirs.insert(key, candidate);
                        QDir().mkpath(m_root + QLatin1Char('/') + candidate);
                    }
                    built = *it;
                }
        }
        const QString name = rel.mid(slash + 1);
        return built.isEmpty() ? m_root + QLatin1Char('/') + name
                               : m_root + QLatin1Char('/') + built + QLatin1Char('/') + name;
    }

private:
    QString m_root;
    QMutex m_lock;
    QHash<QString, QString> m_dirs;
};

/* Reads MO2's modlist.txt: top line is the highest priority, and it isn't
 * read backwards anywhere but here. */
void readMo2Profile(const QString &dir, Profile *profile)
{
    QFile list(dir + QStringLiteral("/modlist.txt"));
    std::vector<ProfileMod> mods;
    if (list.open(QIODevice::ReadOnly)) {
        while (!list.atEnd()) {
            const QString line = QString::fromUtf8(list.readLine()).trimmed();
            if (line.size() < 2 || line.startsWith(QLatin1Char('#')))
                continue;
            if (line.endsWith(QLatin1String("_separator")))
                continue;
            if (line[0] == QLatin1Char('+') || line[0] == QLatin1Char('-'))
                mods.insert(mods.begin(), {line.mid(1), line[0] == QLatin1Char('+')});
        }
    }
    profile->mods = std::move(mods);

    QFile plugins(dir + QStringLiteral("/plugins.txt"));
    QFile order(dir + QStringLiteral("/loadorder.txt"));
    QSet<QString> active;
    QStringList listed;
    if (plugins.open(QIODevice::ReadOnly)) {
        while (!plugins.atEnd()) {
            QString line = QString::fromUtf8(plugins.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            if (line.startsWith(QLatin1Char('*'))) {
                line.remove(0, 1);
                active.insert(line.toLower());
            }
            listed << line;
        }
    }
    if (order.open(QIODevice::ReadOnly)) {
        QStringList ordered;
        while (!order.atEnd()) {
            const QString line = QString::fromUtf8(order.readLine()).trimmed();
            if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')))
                ordered << line;
        }
        if (!ordered.isEmpty())
            listed = ordered;
    }
    profile->plugins.clear();
    for (const QString &name : std::as_const(listed))
        profile->plugins.push_back({name, active.contains(name.toLower())});
}

} /* namespace */

bool readMo2Mod(const QString &folder, ModInfo *mod)
{
    if (QFileInfo(folder).fileName().endsWith(QLatin1String("_separator")))
        return false;
    const QString path = folder + QStringLiteral("/meta.ini");
    QFile meta(path);
    if (!meta.open(QIODevice::ReadOnly))
        return true;
    while (!meta.atEnd()) {
        const QString line = QString::fromUtf8(meta.readLine()).trimmed();
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq), value = line.mid(eq + 1);
        if (key == QLatin1String("modid"))
            mod->nexusModId = value.toLongLong();
        else if (key == QLatin1String("version"))
            mod->version = value;
    }
    meta.close();
    QFile::remove(path);
    return true;
}

bool inspect(const QString &file, Summary *out, QString *error)
{
    const QString work = QDir::tempPath() + QStringLiteral("/fanmo-wj-%1").arg(QDateTime::currentMSecsSinceEpoch());
    const auto cleanup = qScopeGuard([&] { QDir(work).removeRecursively(); });
    ModList list;
    if (!readList(file, work, &list, error))
        return false;
    *out = list.summary;
    return true;
}

bool install(Instance &instance, const QString &file, const Options &options,
             const Engine::Callbacks &callbacks, Result *result, QString *error)
{
    const auto say = [&](const QString &line) {
        if (callbacks.log)
            callbacks.log(line);
    };
    const QString work = instance.tempPath() + QStringLiteral("/wabbajack-%1").arg(QDateTime::currentMSecsSinceEpoch());
    const auto cleanup = qScopeGuard([&] { QDir(work).removeRecursively(); });

    ModList list;
    if (!readList(file, work, &list, error))
        return false;
    if (!list.summary.unsupported.isEmpty()) {
        *error = QStringLiteral("fanMO can't install this list yet: ") + list.summary.unsupported.join(QStringLiteral(", "));
        return false;
    }

    std::vector<const Directive *> todo;
    QSet<QString> neededArchives;
    for (const Directive &d : list.directives) {
        if (!wantedOutput(d.to))
            continue;
        todo.push_back(&d);
        if (!d.archivePath.isEmpty())
            neededArchives.insert(d.archivePath.first());
    }
    say(QStringLiteral("%1: %2 files from %3 archives").arg(list.summary.name).arg(todo.size()).arg(neededArchives.size()));

    /* Locate the archives, by size then xxHash. */
    QList<ArchiveIndex::Wanted> wanted;
    QHash<QString, const Archive *> byHash;
    for (const Archive &a : std::as_const(list.archives)) {
        byHash.insert(a.hash, &a);
        if (neededArchives.contains(a.hash))
            wanted.append({a.hash, a.size});
    }
    QStringList folders = {instance.downloadsPath()};
    folders += instance.archiveFolders();
    folders << instance.gamePath();
    QHash<QString, QString> found = ArchiveIndex::scan(folders, wanted,
                                                       instance.statePath() + QStringLiteral("/xxh-cache.json"),
                                                       callbacks.progress, callbacks.cancel,
                                                       ArchiveIndex::Hash::Wabbajack);
    say(QStringLiteral("%1 of %2 archives already on disk").arg(found.size()).arg(neededArchives.size()));

    /* Download the rest. */
    if (!options.apiKey.isEmpty()) {
        QList<const Archive *> fetch;
        for (const QString &hash : std::as_const(neededArchives))
            if (!found.contains(hash)) {
                const Archive *a = byHash.value(hash);
                if (a && (a->kind == QLatin1String("Nexus") || a->kind == QLatin1String("Http")))
                    fetch << a;
            }
        QMutex lock;
        QThreadPool pool;
        pool.setMaxThreadCount(4);
        qint64 done = 0;
        QtConcurrent::blockingMap(&pool, fetch, [&](const Archive *a) {
            if (callbacks.cancel && callbacks.cancel->load())
                return;
            Nexus::Client client(options.apiKey);
            QString err;
            QStringList links;
            if (a->kind == QLatin1String("Http"))
                links << a->url;
            else
                links = client.downloadLinks(QStringLiteral("skyrimspecialedition"), a->modId, a->fileId,
                                             {}, {}, nullptr, &err);
            const QString dest = instance.downloadsPath() + QLatin1Char('/') + Util::safeFolderName(a->name);
            bool ok = false;
            for (const QString &link : std::as_const(links))
                if ((ok = client.download(link, dest, {}, callbacks.cancel, &err)))
                    break;
            if (ok && Util::wabbajackHash(Util::xxh64File(dest)) != a->hash) {
                QFile::remove(dest);
                ok = false;
            }
            QMutexLocker guard(&lock);
            if (ok)
                found.insert(a->hash, dest);
            if (callbacks.progress)
                callbacks.progress(++done, fetch.size(), a->name);
        });
    }

    for (const QString &hash : std::as_const(neededArchives))
        if (!found.contains(hash)) {
            const Archive *a = byHash.value(hash);
            result->missingArchives << (a ? a->name : hash);
        }
    if (!result->missingArchives.isEmpty() && !options.allowMissing) {
        *error = QStringLiteral("%1 archives are missing").arg(result->missingArchives.size());
        return false;
    }
    if (options.allowMissing)
        std::erase_if(todo, [&](const Directive *d) {
            return !d->archivePath.isEmpty() && !found.contains(d->archivePath.first());
        });

    /* Bundled data: the inline files and patches, in one pass. */
    QStringList blobs;
    for (const Directive *d : todo) {
        if (!d->sourceDataId.isEmpty())
            blobs << d->sourceDataId;
        if (!d->patchId.isEmpty())
            blobs << d->patchId;
    }
    const QString blobDir = work + QStringLiteral("/blobs");
    for (qsizetype i = 0; i < blobs.size(); i += 400)
        if (!extractMembers(file, blobs.mid(i, 400), blobDir, error))
            return false;

    const QString output = instance.root() + QStringLiteral("/wabbajack/") + Util::safeFolderName(list.summary.name);
    Writer writer(output);

    /* Group by archive so each is unpacked once. */
    QHash<QString, std::vector<const Directive *>> byArchive;
    std::vector<const Directive *> inlineFiles;
    for (const Directive *d : todo) {
        if (d->archivePath.isEmpty())
            inlineFiles.push_back(d);
        else
            byArchive[d->archivePath.first()].push_back(d);
    }

    for (const Directive *d : inlineFiles) {
        QFile blob(blobDir + QLatin1Char('/') + d->sourceDataId);
        if (!blob.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("list is missing %1").arg(d->sourceDataId);
            return false;
        }
        QByteArray data = blob.readAll();
        if (d->type == QLatin1String("RemappedInlineFile"))
            data = remapTokens(QString::fromUtf8(data), instance, output).toUtf8();
        if (!Util::writeFile(writer.prepare(d->to), data, error))
            return false;
        ++result->filesWritten;
    }

    QMutex lock;
    qint64 done = 0;
    const qint64 total = byArchive.size();
    QStringList firstError;
    const QStringList hashes = byArchive.keys();
    QThreadPool pool;
    pool.setMaxThreadCount(options.jobs);
    QtConcurrent::blockingMap(&pool, hashes, [&](const QString &hash) {
        if (callbacks.cancel && callbacks.cancel->load())
            return;
        const auto &directives = byArchive[hash];
        const QString archivePath = found.value(hash);
        const Archive *info = byHash.value(hash);
        const bool loose = std::all_of(directives.begin(), directives.end(),
                                       [](const Directive *d) { return d->archivePath.size() == 1; });
        const QString tmp = work + QStringLiteral("/x/") + QString::number(qHash(hash));
        QString err;
        bool ok = true;
        if (!loose)
            ok = ::Archive::extract(archivePath, tmp, &err);

        QHash<QString, QString> members;
        if (ok && !loose)
            for (const QString &f : Util::walkFiles(tmp))
                members.insert(Util::foldKey(f), f);

        int written = 0, verified = 0;
        QStringList mismatches;
        for (const Directive *d : directives) {
            if (!ok)
                break;
            QString source = archivePath;
            if (d->archivePath.size() > 1) {
                const auto m = members.constFind(Util::foldKey(Util::posixPath(d->archivePath[1])));
                if (m == members.constEnd()) {
                    err = QStringLiteral("%1 lacks %2").arg(info ? info->name : hash, d->archivePath[1]);
                    ok = false;
                    break;
                }
                source = tmp + QLatin1Char('/') + *m;
            }
            const QString target = writer.prepare(d->to);
            if (d->type == QLatin1String("PatchedFromArchive")) {
                QFile src(source), patch(blobDir + QLatin1Char('/') + d->patchId);
                if (!src.open(QIODevice::ReadOnly) || !patch.open(QIODevice::ReadOnly)) {
                    err = QStringLiteral("cannot read patch for %1").arg(d->to);
                    ok = false;
                    break;
                }
                const QByteArray patched = Patch::octodiff(src.readAll(), patch.readAll(), &err);
                if (!err.isEmpty() || Util::wabbajackHash(Util::xxh64Bytes(patched)) != d->hash) {
                    if (err.isEmpty())
                        err = QStringLiteral("patched file failed its checksum: %1").arg(d->to);
                    ok = false;
                    break;
                }
                ok = Util::writeFile(target, patched, &err);
            } else {
                QFile::remove(target);
                /* Copied, not moved: one archive member can feed many outputs. */
                ok = link(QFile::encodeName(source).constData(), QFile::encodeName(target).constData()) == 0
                    || QFile::copy(source, target);
                if (!ok)
                    err = QStringLiteral("cannot write %1").arg(d->to);
                else if (QFileInfo(target).size() != d->size) {
                    err = QStringLiteral("size mismatch on %1").arg(d->to);
                    ok = false;
                }
            }
            if (ok) {
                ++written;
                if (options.verifyHashes) {
                    ++verified;
                    if (Util::wabbajackHash(Util::xxh64File(target)) != d->hash)
                        mismatches << d->to;
                }
            }
        }
        QDir(tmp).removeRecursively();
        QMutexLocker guard(&lock);
        result->filesWritten += written;
        result->filesVerified += verified;
        result->hashMismatches += mismatches;
        if (!ok)
            firstError << err;
        if (callbacks.progress)
            callbacks.progress(++done, total, info ? info->name : hash);
    });
    if (!firstError.isEmpty()) {
        result->warnings = firstError;
        *error = firstError.first();
        return false;
    }
    if (callbacks.cancel && callbacks.cancel->load()) {
        *error = QStringLiteral("cancelled");
        return false;
    }

    /* Import: its mods move into mods/, its profile becomes a profile. */
    say(QStringLiteral("importing into profile %1").arg(options.profile));
    const QString label = list.summary.name + QStringLiteral(" ") + list.summary.version;
    QHash<QString, QString> renamed;
    for (const QFileInfo &fi : QDir(output + QStringLiteral("/mods")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        ModInfo mod;
        if (!readMo2Mod(fi.filePath(), &mod))
            continue;
        const QString folder = instance.uniqueFolder(fi.fileName());
        if (!QFile::rename(fi.filePath(), instance.modsPath() + QLatin1Char('/') + folder)) {
            *error = QStringLiteral("cannot move %1 into mods/").arg(fi.fileName());
            return false;
        }
        mod.folder = folder;
        mod.name = fi.fileName();
        mod.collection = label;
        mod.installed = QDateTime::currentSecsSinceEpoch();
        instance.mods().insert(folder, mod);
        renamed.insert(fi.fileName(), folder);
        ++result->modsImported;
    }

    Profile profile;
    profile.name = options.profile;
    profile.collection = label;
    const QFileInfoList profiles = QDir(output + QStringLiteral("/profiles")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (!profiles.isEmpty()) {
        /* Lists ship several; the one named after the list is the one to use. */
        QFileInfo chosen = profiles.first();
        for (const QFileInfo &fi : profiles)
            if (list.summary.name.contains(fi.fileName(), Qt::CaseInsensitive))
                chosen = fi;
        readMo2Profile(chosen.filePath(), &profile);
    }
    std::vector<ProfileMod> mods;
    for (const ProfileMod &m : profile.mods) {
        const auto it = renamed.constFind(m.folder);
        if (it != renamed.constEnd())
            mods.push_back({*it, m.enabled});
    }
    profile.mods = std::move(mods);

    /* Game Root: anything not simply the game's own file becomes a root mod.
     * Its Data folder is a mod of its own, in the ordinary place. */
    const QString gameRoot = output + QStringLiteral("/Game Root");
    if (QFileInfo(gameRoot).isDir()) {
        QSet<QString> gameFiles;
        for (const Directive *d : todo)
            if (!d->archivePath.isEmpty()) {
                const Archive *a = byHash.value(d->archivePath.first());
                if (a && a->kind == QLatin1String("GameFileSource"))
                    gameFiles.insert(Util::foldKey(d->to));
            }
        const QString rootFolder = instance.uniqueFolder(list.summary.name + QStringLiteral(" - Game Root"));
        const QString dataFolder = instance.uniqueFolder(list.summary.name + QStringLiteral(" - Game Data"));
        int rootCount = 0, dataCount = 0;
        for (const QString &f : Util::walkFiles(gameRoot)) {
            if (gameFiles.contains(Util::foldKey(QStringLiteral("Game Root/") + f)))
                continue;
            const bool isData = f.startsWith(QLatin1String("data/"), Qt::CaseInsensitive);
            const QString to = instance.modsPath() + QLatin1Char('/')
                + (isData ? dataFolder + QLatin1Char('/') + f.mid(5) : rootFolder + QLatin1Char('/') + f);
            QDir().mkpath(QFileInfo(to).absolutePath());
            if (QFile::rename(gameRoot + QLatin1Char('/') + f, to))
                ++(isData ? dataCount : rootCount);
        }
        for (const auto &[folder, count, isRoot] : {std::tuple{rootFolder, rootCount, true},
                                                    std::tuple{dataFolder, dataCount, false}}) {
            if (count == 0)
                continue;
            ModInfo mod;
            mod.folder = folder;
            mod.name = folder;
            mod.collection = label;
            mod.rootDeploy = isRoot;
            mod.installed = QDateTime::currentSecsSinceEpoch();
            instance.mods().insert(folder, mod);
            profile.mods.push_back({folder, true});
        }
    }
    QDir(output).removeRecursively();

    const Deploy::FileIndex index = Deploy::buildIndex(Deploy::layersFor(instance, profile));
    Plugins::reconcile(instance, &profile, Plugins::available(instance, index));
    return instance.saveProfile(profile, error) && instance.save(error);
}

} /* namespace Wabbajack */
