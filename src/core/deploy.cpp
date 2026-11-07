#include "deploy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QThreadPool>
#include <QtConcurrent>

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace Deploy {

static const QLatin1String overwriteId("<overwrite>");

std::vector<Layer> layersFor(const Instance &instance, const Profile &profile)
{
    std::vector<Layer> layers;
    layers.reserve(profile.mods.size() + 1);
    for (const ProfileMod &m : profile.mods) {
        if (!m.enabled)
            continue;
        const auto it = instance.mods().constFind(m.folder);
        layers.push_back({m.folder, instance.modsPath() + QLatin1Char('/') + m.folder,
                          it != instance.mods().constEnd() && it->rootDeploy});
    }
    layers.push_back({QString(overwriteId), instance.overwritePath(), false});
    return layers;
}

static QString makeKey(bool root, const QString &relPath)
{
    return (root ? QStringLiteral("r/") : QStringLiteral("d/")) + Util::foldKey(relPath);
}

/* Mods carry things that aren't game data: FOMOD leftovers aren't filtered
 * (Vortex deploys them too), only our own and Vortex's markers are. */
static bool ignoredFile(const QString &relPath)
{
    return relPath == QLatin1String("__folder_managed_by_vortex");
}

FileIndex buildIndex(std::vector<Layer> layers)
{
    FileIndex index;
    index.layers = std::move(layers);
    const int n = int(index.layers.size());

    /* Reading folders is the slow part, and it's all I/O. */
    std::vector<int> ids(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        ids[size_t(i)] = i;
    QThreadPool pool;
    pool.setMaxThreadCount(8);
    const QList<QStringList> lists = QtConcurrent::blockingMapped<QList<QStringList>>(
        &pool, ids, [&](int i) { return Util::walkFiles(index.layers[size_t(i)].path); });

    qsizetype total = 0;
    for (const QStringList &l : lists)
        total += l.size();
    index.providers.reserve(total);
    index.summary.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        const bool root = index.layers[size_t(i)].rootDeploy;
        for (const QString &rel : lists[i]) {
            if (ignoredFile(rel))
                continue;
            index.providers[makeKey(root, rel)].push_back({i, rel});
            ++index.summary[size_t(i)].total;
        }
    }

    std::vector<QSet<int>> beatenBy(static_cast<size_t>(n)), beats(static_cast<size_t>(n));
    for (auto it = index.providers.cbegin(); it != index.providers.cend(); ++it) {
        const auto &list = it.value();
        if (list.size() < 2)
            continue;
        const int winner = list.back().layer;
        for (size_t p = 0; p + 1 < list.size(); ++p) {
            const int loser = list[p].layer;
            ++index.summary[size_t(loser)].losing;
            beatenBy[size_t(loser)].insert(winner);
            beats[size_t(winner)].insert(loser);
        }
        ++index.summary[size_t(winner)].winning;
    }
    for (int i = 0; i < n; ++i) {
        for (int other : beatenBy[size_t(i)])
            index.summary[size_t(i)].overriddenBy << index.layers[size_t(other)].id;
        for (int other : beats[size_t(i)])
            index.summary[size_t(i)].overrides << index.layers[size_t(other)].id;
        index.summary[size_t(i)].overriddenBy.sort();
        index.summary[size_t(i)].overrides.sort();
    }
    return index;
}

std::vector<FileConflict> conflictsFor(const FileIndex &index, int layer)
{
    std::vector<FileConflict> out;
    for (auto it = index.providers.cbegin(); it != index.providers.cend(); ++it) {
        const auto &list = it.value();
        if (list.size() < 2)
            continue;
        const FileIndex::Provider *mine = nullptr;
        for (const auto &p : list)
            if (p.layer == layer)
                mine = &p;
        if (!mine)
            continue;
        FileConflict c;
        c.relPath = mine->relPath;
        c.winner = list.back().layer == layer;
        for (const auto &p : list)
            if (p.layer != layer)
                c.others << index.layers[size_t(p.layer)].id;
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const FileConflict &a, const FileConflict &b) {
        return a.relPath.compare(b.relPath, Qt::CaseInsensitive) < 0;
    });
    return out;
}

namespace {

struct Entry {
    QString rel;      /* path under the target, spelled as deployed */
    QString layer;
    QString source;   /* absolute */
    bool root = false;
    /* The inode we linked. A deployed file with another one was replaced by a
     * program; comparing with the source instead would blame a reinstalled
     * mod, whose source changes inode while the deployed link stays put. */
    qint64 inode = 0;
};

struct Manifest {
    QHash<QString, Entry> entries;
    QHash<QString, QString> backups;     /* key -> file in state/backup */
    QSet<QString> createdDirs;           /* "d/Foo/Bar", spelled as created */
};

QString manifestPath(const Instance &i) { return i.statePath() + QStringLiteral("/deployment.json"); }
QString journalPath(const Instance &i) { return i.statePath() + QStringLiteral("/deployment.pending.json"); }
QString baselinePath(const Instance &i) { return i.statePath() + QStringLiteral("/baseline.json"); }
QString backupRoot(const Instance &i) { return i.statePath() + QStringLiteral("/backup"); }

Manifest readManifest(const QString &path)
{
    Manifest m;
    const QJsonObject o = Util::readJson(path).object();
    const QJsonArray files = o.value(QLatin1String("files")).toArray();
    m.entries.reserve(files.size());
    for (const auto &v : files) {
        const QJsonArray a = v.toArray();
        if (a.size() < 4)
            continue;
        Entry e{a[0].toString(), a[1].toString(), a[2].toString(), a[3].toBool(), a[4].toInteger()};
        m.entries.insert(makeKey(e.root, e.rel), std::move(e));
    }
    const QJsonObject backups = o.value(QLatin1String("backups")).toObject();
    for (auto it = backups.begin(); it != backups.end(); ++it)
        m.backups.insert(it.key(), it.value().toString());
    for (const auto &v : o.value(QLatin1String("createdDirs")).toArray())
        m.createdDirs.insert(v.toString());
    return m;
}

bool writeManifest(const QString &path, const Manifest &m, QString *error)
{
    QJsonArray files;
    for (const Entry &e : m.entries)
        files.append(QJsonArray{e.rel, e.layer, e.source, e.root, e.inode});
    QJsonObject backups;
    for (auto it = m.backups.cbegin(); it != m.backups.cend(); ++it)
        backups.insert(it.key(), it.value());
    QStringList dirs(m.createdDirs.begin(), m.createdDirs.end());
    dirs.sort();
    const QJsonObject o{{QStringLiteral("version"), 1},
                        {QStringLiteral("files"), files},
                        {QStringLiteral("backups"), backups},
                        {QStringLiteral("createdDirs"), QJsonArray::fromStringList(dirs)}};
    return Util::writeFile(path, QJsonDocument(o).toJson(QJsonDocument::Compact), error);
}

bool statInode(const QByteArray &path, ino_t *ino, bool *exists = nullptr)
{
    struct stat st;
    const bool ok = lstat(path.constData(), &st) == 0;
    if (exists)
        *exists = ok;
    if (ok)
        *ino = st.st_ino;
    return ok;
}

/* What's really on disk under a target, by folded path. Deploying merges into
 * these spellings: Data/Scripts exists, so a mod's data/scripts lands in it. */
struct Tree {
    QString root;
    QHash<QString, QString> dirs;   /* folded -> as spelled */
    QHash<QString, QString> files;

    /* skip: a top-level folder to leave out; the game folder holds Data. */
    void load(const QString &path, const QString &skip = {})
    {
        root = path;
        dirs.clear();
        files.clear();
        walk(QFile::encodeName(path), QString(), Util::foldKey(skip));
    }

    void walk(const QByteArray &dir, const QString &prefix, const QString &skip = {})
    {
        QDir d(QFile::decodeName(dir));
        const auto entries = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &fi : entries) {
            const QString rel = prefix + fi.fileName();
            if (fi.isDir() && !fi.isSymLink()) {
                if (!skip.isEmpty() && Util::foldKey(rel) == skip) {
                    dirs.insert(skip, rel);
                    continue;
                }
                dirs.insert(Util::foldKey(rel), rel);
                walk(QFile::encodeName(fi.filePath()), rel + QLatin1Char('/'));
            } else {
                files.insert(Util::foldKey(rel), rel);
            }
        }
    }

    /* Real spelling of relDir, creating what's missing. */
    QString ensureDir(const QString &relDir, QSet<QString> *created, const QString &createdPrefix,
                      QString *error)
    {
        QString built;
        for (const QString &part : relDir.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            const QString candidate = built.isEmpty() ? part : built + QLatin1Char('/') + part;
            const QString key = Util::foldKey(candidate);
            auto it = dirs.constFind(key);
            if (it == dirs.constEnd()) {
                const QByteArray full = QFile::encodeName(root + QLatin1Char('/') + candidate);
                if (mkdir(full.constData(), 0755) != 0 && errno != EEXIST) {
                    *error = QStringLiteral("cannot create %1: %2").arg(candidate, QString::fromLocal8Bit(strerror(errno)));
                    return {};
                }
                it = dirs.insert(key, candidate);
                created->insert(createdPrefix + candidate);
            }
            built = *it;
        }
        return built;
    }
};

/* Moves a file out of the game folder into a capture folder. */
bool moveToDir(const QString &from, const QString &destDir, const QString &relPath)
{
    const QString to = destDir + QLatin1Char('/') + relPath;
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile::remove(to);
    return QFile::rename(from, to);
}

void removeEmptyDirs(const QString &target, const QString &prefix, QSet<QString> &created)
{
    QStringList dirs;
    for (const QString &d : created)
        if (d.startsWith(prefix))
            dirs << d;
    /* Deepest first, so parents are empty when their turn comes. */
    std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) {
        return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });
    for (const QString &d : std::as_const(dirs))
        if (rmdir(QFile::encodeName(target + QLatin1Char('/') + d.mid(prefix.size())).constData()) == 0)
            created.remove(d);
}

/* Snapshot of Data before the first deployment: everything in it is the
 * game's, so it isn't captured later as tool output. */
void ensureBaseline(const Instance &instance)
{
    if (QFileInfo::exists(baselinePath(instance)))
        return;
    QStringList files = Util::walkFiles(instance.dataPath());
    for (QString &f : files)
        f = Util::foldKey(f);
    Util::writeJson(baselinePath(instance), QJsonDocument(QJsonArray::fromStringList(files)));
}

} /* namespace */

QString blocker(const Instance &instance)
{
    if (!QFileInfo(instance.gamePath() + QStringLiteral("/SkyrimSE.exe")).exists())
        return QStringLiteral("the game isn't at %1").arg(instance.gamePath());
    if (!Util::sameFilesystem(instance.modsPath(), instance.dataPath()))
        return QStringLiteral("mods/ and the game are on different filesystems");
    for (const QString &path : {instance.dataPath() + QStringLiteral("/vortex.deployment.json"),
                                instance.gamePath() + QStringLiteral("/vortex.deployment.dinput.json")}) {
        if (!QFileInfo::exists(path))
            continue;
        const QJsonArray files = Util::readJson(path).object().value(QLatin1String("files")).toArray();
        if (!files.isEmpty())
            return QStringLiteral("Vortex has %1 files deployed here (%2). Purge them in Vortex, or "
                                  "point this instance at another install.")
                .arg(files.size()).arg(QFileInfo(path).fileName());
    }
    return {};
}

/* Applies `next` over `current`, whichever of them is the empty manifest. */
static bool applyDiff(Instance &instance, Manifest current, const QHash<QString, Entry> &next,
                      const Options &options, Report *report, QString *error)
{
    const QString dataRoot = instance.dataPath();
    const QString gameRoot = instance.gamePath();
    const auto targetOf = [&](const Entry &e) {
        return (e.root ? gameRoot : dataRoot) + QLatin1Char('/') + e.rel;
    };

    /* The journal is the union of old and new: whatever state a crash leaves,
     * everything we might have touched stays known to the next run. */
    Manifest journal = current;
    for (auto it = next.cbegin(); it != next.cend(); ++it)
        journal.entries.insert(it.key(), it.value());
    if (!writeManifest(journalPath(instance), journal, error))
        return false;

    /* Step one: take out what's going, keeping anything a program rewrote. */
    qint64 done = 0;
    const qint64 total = current.entries.size() + next.size();
    QStringList toCapture;
    for (auto it = current.entries.cbegin(); it != current.entries.cend(); ++it) {
        if (options.cancel && options.cancel->load())
            break;
        const Entry &old = it.value();
        const auto wanted = next.constFind(it.key());
        const QByteArray target = QFile::encodeName(targetOf(old));
        ino_t deployedIno = 0, sourceIno = 0;
        bool exists = false;
        statInode(target, &deployedIno, &exists);
        const bool sourceOk = statInode(QFile::encodeName(old.source), &sourceIno);
        const bool intact = exists && sourceOk && deployedIno == sourceIno;
        /* Entries from a journal have no inode yet: all we can ask of those
         * is whether they still match their source. */
        const bool rewritten = exists && (old.inode ? qint64(deployedIno) != old.inode
                                                    : sourceOk && !intact);

        if (wanted != next.constEnd() && wanted->source == old.source && intact) {
            ++report->unchanged;
        } else if (exists) {
            if (rewritten) {
                /* Rewritten, not linked: this content exists nowhere else. */
                if (moveToDir(targetOf(old), instance.overwritePath(), old.rel)) {
                    ++report->captured;
                    report->warnings << QStringLiteral("kept a rewritten file in overwrite: %1").arg(old.rel);
                } else {
                    *error = QStringLiteral("cannot move rewritten file %1").arg(old.rel);
                    return false;
                }
            } else if (unlink(target.constData()) == 0) {
                ++report->removed;
            } else {
                *error = QStringLiteral("cannot remove %1: %2").arg(old.rel, QString::fromLocal8Bit(strerror(errno)));
                return false;
            }
        }
        if (options.progress && ++done % 2000 == 0)
            options.progress(done, total, old.rel);
    }
    if (options.cancel && options.cancel->load()) {
        *error = QStringLiteral("cancelled; run deploy again to finish");
        return false;
    }

    /* Step two: restore what's no longer displaced. Before linking, because a
     * new mod may then displace it again in the ordinary way. */
    for (auto it = current.backups.begin(); it != current.backups.end();) {
        if (next.contains(it.key())) {
            ++it;
            continue;
        }
        const bool root = it.key().startsWith(QLatin1String("r/"));
        const QString rel = it.value().section(QLatin1Char('/'), 1);
        const QString to = (root ? gameRoot : dataRoot) + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (!QFile::rename(backupRoot(instance) + QLatin1Char('/') + it.value(), to)) {
            report->warnings << QStringLiteral("could not restore %1").arg(rel);
            ++it;
            continue;
        }
        ++report->restored;
        it = current.backups.erase(it);
    }

    /* Step three: link what's new, in the spellings already on disk. */
    Tree data, game;
    data.load(dataRoot);
    game.load(gameRoot, QStringLiteral("Data"));

    QHash<QString, Entry> finalEntries;
    finalEntries.reserve(next.size());
    for (auto it = next.cbegin(); it != next.cend(); ++it) {
        if (options.cancel && options.cancel->load()) {
            *error = QStringLiteral("cancelled; run deploy again to finish");
            return false;
        }
        Entry e = it.value();
        Tree &tree = e.root ? game : data;
        const QString prefix = e.root ? QStringLiteral("r/") : QStringLiteral("d/");
        const qsizetype slash = e.rel.lastIndexOf(QLatin1Char('/'));
        const QString dir = slash < 0 ? QString()
                                      : tree.ensureDir(e.rel.left(slash), &current.createdDirs, prefix, error);
        if (!error->isEmpty())
            return false;
        const QString name = e.rel.mid(slash + 1);
        const QString key = Util::foldKey(dir.isEmpty() ? name : dir + QLatin1Char('/') + name);
        /* Keep the spelling of a file already there (the baseline's, usually). */
        const auto existing = tree.files.constFind(key);
        const QString spelled = existing != tree.files.constEnd()
                                    ? *existing
                                    : (dir.isEmpty() ? name : dir + QLatin1Char('/') + name);
        e.rel = spelled;
        const QString target = tree.root + QLatin1Char('/') + spelled;
        const QByteArray targetBytes = QFile::encodeName(target);

        ino_t have = 0, want = 0;
        bool exists = false;
        statInode(targetBytes, &have, &exists);
        if (!statInode(QFile::encodeName(e.source), &want)) {
            *error = QStringLiteral("source vanished: %1").arg(e.source);
            return false;
        }
        e.inode = qint64(want);
        if (exists && have == want) {
            finalEntries.insert(it.key(), std::move(e));
            if (options.progress && ++done % 2000 == 0)
                options.progress(done, total, spelled);
            continue;
        }
        if (exists) {
            /* Not ours and in the way: the game's own file, or a stray. */
            const QString stored = (e.root ? QStringLiteral("root/") : QStringLiteral("data/")) + spelled;
            const QString backup = backupRoot(instance) + QLatin1Char('/') + stored;
            QDir().mkpath(QFileInfo(backup).absolutePath());
            if (!QFile::rename(target, backup)) {
                *error = QStringLiteral("cannot back up %1").arg(spelled);
                return false;
            }
            current.backups.insert(it.key(), stored);
            ++report->backedUp;
        }
        if (link(QFile::encodeName(e.source).constData(), targetBytes.constData()) != 0) {
            *error = QStringLiteral("cannot link %1: %2").arg(spelled, QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
        tree.files.insert(key, spelled);
        ++report->linked;
        finalEntries.insert(it.key(), std::move(e));
        if (options.progress && ++done % 2000 == 0)
            options.progress(done, total, spelled);
    }

    removeEmptyDirs(dataRoot, QStringLiteral("d/"), current.createdDirs);
    removeEmptyDirs(gameRoot, QStringLiteral("r/"), current.createdDirs);

    Manifest result;
    result.entries = std::move(finalEntries);
    result.backups = current.backups;
    result.createdDirs = current.createdDirs;
    if (!writeManifest(manifestPath(instance), result, error))
        return false;
    QFile::remove(journalPath(instance));
    if (options.progress)
        options.progress(total, total, QStringLiteral("done"));
    return true;
}

static Manifest currentManifest(const Instance &instance)
{
    /* A journal means the last run stopped partway: its union is the truth. */
    if (QFileInfo::exists(journalPath(instance)))
        return readManifest(journalPath(instance));
    return readManifest(manifestPath(instance));
}

bool deploy(Instance &instance, const FileIndex &index, const Options &options, Report *report,
            QString *error)
{
    const QString blocked = blocker(instance);
    if (!blocked.isEmpty()) {
        *error = blocked;
        return false;
    }
    ensureBaseline(instance);

    QHash<QString, Entry> next;
    next.reserve(index.providers.size());
    for (auto it = index.providers.cbegin(); it != index.providers.cend(); ++it) {
        const FileIndex::Provider &top = it.value().back();
        const Layer &layer = index.layers[size_t(top.layer)];
        next.insert(it.key(), Entry{top.relPath, layer.id,
                                    layer.path + QLatin1Char('/') + top.relPath, layer.rootDeploy});
    }
    return applyDiff(instance, currentManifest(instance), next, options, report, error);
}

bool purge(Instance &instance, const Options &options, Report *report, QString *error)
{
    return applyDiff(instance, currentManifest(instance), {}, options, report, error);
}

QStringList unmanagedFiles(const Instance &instance)
{
    QSet<QString> known;
    const QJsonArray baseline = Util::readJson(baselinePath(instance)).array();
    known.reserve(baseline.size());
    for (const auto &v : baseline)
        known.insert(v.toString());
    const Manifest m = currentManifest(instance);
    for (const Entry &e : m.entries)
        if (!e.root)
            known.insert(Util::foldKey(e.rel));
    QStringList out;
    for (const QString &f : Util::walkFiles(instance.dataPath()))
        if (!known.contains(Util::foldKey(f)))
            out << f;
    return out;
}

int captureInto(const Instance &instance, const QStringList &files, const QString &destDir,
                QString *error)
{
    int moved = 0;
    for (const QString &f : files) {
        if (moveToDir(instance.dataPath() + QLatin1Char('/') + f, destDir, f)) {
            ++moved;
        } else if (error) {
            *error = QStringLiteral("cannot capture %1").arg(f);
            return moved;
        }
    }
    return moved;
}

} /* namespace Deploy */
