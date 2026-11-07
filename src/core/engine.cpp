#include "engine.h"

#include "archive.h"
#include "archiveindex.h"
#include "installer.h"
#include "nexus.h"
#include "plugins.h"
#include "userlist.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QSemaphore>
#include <QSet>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent>

namespace Engine {

static void log(const Callbacks &c, const QString &line)
{
    if (c.log)
        c.log(line);
}

static bool cancelled(const Callbacks &c)
{
    return c.cancel && c.cancel->load();
}

static QString choicesKey(const Collection::Mod &mod)
{
    QJsonObject o;
    if (mod.choices)
        o.insert(QStringLiteral("choices"), Fomod::presetToJson(*mod.choices));
    QJsonArray patches;
    for (const auto &[path, crc] : mod.patches)
        patches.append(path + QLatin1Char('=') + crc);
    o.insert(QStringLiteral("patches"), patches);
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Md5).toHex());
}

bool unpackBundle(const Instance &instance, const QString &bundle7z, const QString &name,
                  Collection::Manifest *out, QString *error)
{
    const QString dest = instance.collectionsPath() + QLatin1Char('/') + Util::safeFolderName(name);
    QDir(dest).removeRecursively();
    if (!Archive::extract(bundle7z, dest, error))
        return false;
    return Collection::load(dest + QStringLiteral("/collection.json"), out, error);
}

bool fetchCollection(const Instance &instance, const QString &apiKey, const QString &slug,
                     int revision, Collection::Manifest *out, const Callbacks &callbacks,
                     QString *error)
{
    Nexus::Client client(apiKey);
    if (revision <= 0) {
        revision = client.latestRevision(slug, error);
        if (revision <= 0)
            return false;
    }
    const auto info = client.collectionRevision(slug, revision, error);
    if (!info)
        return false;
    log(callbacks, QStringLiteral("%1 revision %2: %3 mods").arg(info->name).arg(info->revision).arg(info->modCount));
    const QStringList links = client.collectionLinks(info->downloadLink, error);
    if (links.isEmpty())
        return false;
    const QString file = instance.collectionsPath() + QStringLiteral("/%1-%2.7z").arg(slug).arg(info->revision);
    if (!QFileInfo::exists(file) && !client.download(links.first(), file, {}, callbacks.cancel, error))
        return false;
    return unpackBundle(instance, file, QStringLiteral("%1-%2").arg(slug).arg(info->revision), out, error);
}

/* The folder a mod's patches sit under in the bundle: Vortex names it after
 * the mod. */
static QString patchFolder(const Collection::Manifest &manifest, const Collection::Mod &mod)
{
    return manifest.rootDir + QStringLiteral("/patches/") + mod.name;
}

static bool installOne(Instance &instance, const Collection::Manifest &manifest,
                       const Collection::Mod &mod, const QString &archive, const QString &folder,
                       const Fomod::Environment &env, const QString &label, ModInfo *info,
                       QStringList *warnings, QString *error)
{
    const QString work = instance.tempPath() + QLatin1Char('/') + mod.source.md5;
    const QString extracted = work + QStringLiteral("/x");
    const QString built = work + QStringLiteral("/mod");
    QDir(work).removeRecursively();
    const auto cleanup = qScopeGuard([&] { QDir(work).removeRecursively(); });

    if (!Archive::extract(archive, extracted, error))
        return false;
    Installer::Request request;
    request.extractedDir = extracted;
    request.preset = mod.choices;
    request.env = env;
    request.rootDeploy = mod.type == QLatin1String("dinput");
    Installer::Plan plan = Installer::plan(request);
    if (!plan.ok()) {
        *error = plan.error;
        return false;
    }
    for (const QString &w : std::as_const(plan.warnings))
        *warnings << mod.name + QStringLiteral(": ") + w;
    if (!Installer::apply(extracted, plan.copies, built, error))
        return false;
    if (!mod.patches.isEmpty()
        && !Installer::applyPatches(built, patchFolder(manifest, mod), mod.patches, warnings, error))
        return false;

    /* Only a finished mod appears in mods/: the rename is the commit. */
    const QString target = instance.modsPath() + QLatin1Char('/') + folder;
    QDir(target).removeRecursively();
    if (!QFile::rename(built, target)) {
        *error = QStringLiteral("cannot move %1 into mods/").arg(folder);
        return false;
    }
    info->folder = folder;
    info->name = mod.name;
    info->version = mod.version;
    info->category = mod.category;
    info->author = mod.author;
    info->nexusModId = mod.source.modId;
    info->nexusFileId = mod.source.fileId;
    info->md5 = mod.source.md5;
    info->archive = QFileInfo(archive).fileName();
    info->collection = label;
    info->rootDeploy = mod.type == QLatin1String("dinput");
    info->choicesKey = choicesKey(mod);
    if (mod.choices)
        info->choices = QJsonObject{{QStringLiteral("type"), QStringLiteral("fomod")},
                                    {QStringLiteral("options"), Fomod::presetToJson(*mod.choices)}};
    info->installed = QDateTime::currentSecsSinceEpoch();
    return true;
}

/* Downloads what's missing. Failures land in `result`; only a cancel stops
 * the rest. */
static void downloadMissing(Instance &instance, const Collection::Manifest &manifest,
                            const std::vector<int> &indices, QHash<QString, QString> *found,
                            const CollectionOptions &options, const Callbacks &callbacks,
                            CollectionResult *result)
{
    QList<int> wanted;
    for (int i : indices) {
        const Collection::Mod &mod = manifest.mods[size_t(i)];
        if (found->contains(mod.source.md5))
            continue;
        const QString type = mod.source.type;
        if (type == QLatin1String("nexus") && !options.apiKey.isEmpty()) {
            wanted << i;
        } else if (type == QLatin1String("direct") && !mod.source.url.isEmpty()) {
            wanted << i;
        } else {
            QString url = mod.source.url;
            if (url.isEmpty() && mod.source.modId > 0)
                url = QStringLiteral("https://www.nexusmods.com/%1/mods/%2?tab=files&file_id=%3")
                          .arg(mod.domain).arg(mod.source.modId).arg(mod.source.fileId);
            const QString reason = !mod.source.instructions.isEmpty() ? mod.source.instructions
                                 : type == QLatin1String("nexus")
                                       ? QStringLiteral("no Nexus API key set")
                                       : QStringLiteral("download it yourself");
            result->manual.push_back({mod.name, url, reason, mod.source.modId, mod.source.fileId,
                                      mod.source.md5, mod.source.size});
        }
    }
    if (wanted.isEmpty())
        return;
    log(callbacks, QStringLiteral("downloading %1 archives").arg(wanted.size()));

    QMutex lock;
    qint64 done = 0;
    const qint64 total = wanted.size();
    std::atomic_bool premiumStop{false};
    QThreadPool pool;
    pool.setMaxThreadCount(options.downloadJobs);
    QtConcurrent::blockingMap(&pool, wanted, [&](int i) {
        const Collection::Mod &mod = manifest.mods[size_t(i)];
        if (cancelled(callbacks))
            return;
        const auto manual = [&](const QString &reason) {
            QMutexLocker guard(&lock);
            result->manual.push_back({mod.name, QStringLiteral("https://www.nexusmods.com/%1/mods/%2?tab=files&file_id=%3")
                                                    .arg(mod.domain).arg(mod.source.modId).arg(mod.source.fileId),
                                      reason, mod.source.modId, mod.source.fileId, mod.source.md5, mod.source.size});
        };
        if (premiumStop.load()) {
            manual(QStringLiteral("this account needs Premium for automatic downloads"));
            return;
        }
        Nexus::Client client(options.apiKey);
        QString error;
        QStringList links;
        if (mod.source.type == QLatin1String("direct")) {
            links << mod.source.url;
        } else {
            bool premium = false;
            links = client.downloadLinks(mod.domain, mod.source.modId, mod.source.fileId, {}, {},
                                         &premium, &error);
            if (premium) {
                premiumStop = true;
                manual(QStringLiteral("this account needs Premium for automatic downloads"));
                return;
            }
        }
        /* The name the CDN serves it under is the archive's real name, and a
         * mod's folder is named from that. */
        QString dest;
        bool ok = false;
        for (const QString &link : std::as_const(links)) {
            error.clear();
            QString name = QUrl(link).fileName(QUrl::FullyDecoded);
            if (name.isEmpty())
                name = mod.name + QLatin1Char('-') + mod.source.md5.left(8);
            dest = instance.downloadsPath() + QLatin1Char('/') + Util::safeFolderName(name);
            /* Two mods can ship files with one name. */
            if (QFileInfo::exists(dest))
                dest = instance.downloadsPath() + QLatin1Char('/') + mod.source.md5.left(8)
                    + QLatin1Char('-') + Util::safeFolderName(name);
            for (int attempt = 0; attempt < 3 && !ok && !cancelled(callbacks); ++attempt)
                ok = client.download(link, dest, {}, callbacks.cancel, &error);
            if (ok)
                break;
        }
        QString finalPath;
        if (ok) {
            /* A wrong file must not install quietly as the right one. */
            if (QFileInfo(dest).size() != mod.source.size
                || QString::fromLatin1(Util::md5File(dest, callbacks.cancel)) != mod.source.md5) {
                QFile::remove(dest);
                ok = false;
                error = QStringLiteral("checksum mismatch");
            } else {
                finalPath = dest;
            }
        }
        QMutexLocker guard(&lock);
        if (ok) {
            found->insert(mod.source.md5, finalPath);
            ++result->downloaded;
        } else if (!cancelled(callbacks)) {
            result->failed << QStringLiteral("%1: %2").arg(mod.name, error.isEmpty() ? QStringLiteral("no download link") : error);
        }
        ++done;
        if (callbacks.progress)
            callbacks.progress(done, total, mod.name);
    });
}

/* Copies the bundle's INI tweaks beside the profile. Optional ones are left
 * for the person to choose, as Vortex does. */
static QStringList copyIniTweaks(const Instance &instance, const Collection::Manifest &manifest,
                                 const QString &profile)
{
    QStringList out;
    const QDir source(manifest.rootDir + QStringLiteral("/INI Tweaks"));
    const QString dest = instance.profileDir(profile) + QStringLiteral("/ini-tweaks");
    QDir().mkpath(dest);
    for (const QFileInfo &fi : source.entryInfoList(QDir::Files)) {
        if (fi.fileName().startsWith(QLatin1String("(optional)."), Qt::CaseInsensitive))
            continue;
        QFile::remove(dest + QLatin1Char('/') + fi.fileName());
        if (QFile::copy(fi.filePath(), dest + QLatin1Char('/') + fi.fileName()))
            out << fi.fileName();
    }
    return out;
}

/* A collection's root mods can bring programs it expects you to run, like
 * Skyrim Runtime Swapper. They land beside the game, so offer them. */
static void addRootTools(Instance &instance, const QList<QString> &folders)
{
    for (const QString &folder : folders) {
        const auto mod = instance.mods().constFind(folder);
        if (mod == instance.mods().constEnd() || !mod->rootDeploy)
            continue;
        const QDir dir(instance.modsPath() + QLatin1Char('/') + folder);
        for (const QString &exe : dir.entryList({QStringLiteral("*.exe")}, QDir::Files)) {
            /* The game's script extender is the one tool everyone already has. */
            const bool known = std::any_of(instance.tools().begin(), instance.tools().end(),
                [&](const Tool &t) { return t.exe.compare(exe, Qt::CaseInsensitive) == 0; });
            if (known)
                continue;
            Tool tool;
            tool.name = QFileInfo(exe).completeBaseName();
            tool.exe = exe;
            instance.tools().push_back(std::move(tool));
        }
    }
}

bool installCollection(Instance &instance, const Collection::Manifest &manifest,
                       const QString &label, const CollectionOptions &options,
                       const Callbacks &callbacks, CollectionResult *result, QString *error)
{
    if (manifest.hasPatches() && !QDir(manifest.rootDir + QStringLiteral("/patches")).exists())
        result->warnings << QStringLiteral("this manifest has binary patches but no patches/ folder; "
                                           "those files will stay unpatched");

    std::vector<int> included;
    for (int i = 0; i < int(manifest.mods.size()); ++i)
        if (options.includeOptional || !manifest.mods[size_t(i)].optional)
            included.push_back(i);

    /* Find everything we already have. */
    QStringList folders = {instance.downloadsPath()};
    folders += instance.archiveFolders();
    QList<ArchiveIndex::Wanted> wanted;
    for (int i : included)
        wanted.append({manifest.mods[size_t(i)].source.md5, manifest.mods[size_t(i)].source.size});
    log(callbacks, QStringLiteral("looking for %1 archives on disk").arg(wanted.size()));
    QHash<QString, QString> found = ArchiveIndex::scan(folders, wanted,
                                                       instance.statePath() + QStringLiteral("/md5-cache.json"),
                                                       callbacks.progress, callbacks.cancel);
    log(callbacks, QStringLiteral("%1 of %2 archives already on disk").arg(found.size()).arg(wanted.size()));

    /* What's installed already, by archive. */
    QHash<QString, const ModInfo *> installedByMd5;
    for (const ModInfo &m : std::as_const(instance.mods()))
        if (!m.md5.isEmpty())
            installedByMd5.insert(m.md5 + QLatin1Char('/') + m.choicesKey, &m);

    std::vector<int> needDownload;
    for (int i : included) {
        const Collection::Mod &mod = manifest.mods[size_t(i)];
        if (!installedByMd5.contains(mod.source.md5 + QLatin1Char('/') + choicesKey(mod)))
            needDownload.push_back(i);
    }
    downloadMissing(instance, manifest, needDownload, &found, options, callbacks, result);
    if (cancelled(callbacks)) {
        *error = QStringLiteral("cancelled");
        return false;
    }

    const Fomod::Environment env = Collection::environment(manifest, Util::walkFiles(instance.dataPath()));

    /* Phases run in order; a later phase may depend on an earlier one. */
    QList<int> phases;
    for (int i : included)
        if (!phases.contains(manifest.mods[size_t(i)].phase))
            phases << manifest.mods[size_t(i)].phase;
    std::sort(phases.begin(), phases.end());

    QHash<int, QString> folderOf;
    QSet<QString> taken;
    QMutex lock;
    qint64 done = 0;
    const qint64 total = qint64(included.size());
    for (int phase : std::as_const(phases)) {
        QList<int> work;
        for (int i : included) {
            const Collection::Mod &mod = manifest.mods[size_t(i)];
            if (mod.phase != phase)
                continue;
            const auto existing = installedByMd5.constFind(mod.source.md5 + QLatin1Char('/') + choicesKey(mod));
            if (existing != installedByMd5.constEnd()) {
                folderOf.insert(i, (*existing)->folder);
                ++result->reused;
                ++done;
                continue;
            }
            const QString archive = found.value(mod.source.md5);
            if (archive.isEmpty())
                continue;  /* already in manual or failed */
            work << i;
            /* Reserve the folder here, on one thread. */
            const QString wantedName = QFileInfo(archive).completeBaseName();
            QString folder = instance.uniqueFolder(wantedName);
            for (int n = 2; taken.contains(folder); ++n)
                folder = instance.uniqueFolder(QStringLiteral("%1 (%2)").arg(wantedName).arg(n));
            taken.insert(folder);
            folderOf.insert(i, folder);
        }

        QThreadPool pool;
        pool.setMaxThreadCount(options.jobs);
        QtConcurrent::blockingMap(&pool, work, [&](int i) {
            if (cancelled(callbacks))
                return;
            const Collection::Mod &mod = manifest.mods[size_t(i)];
            ModInfo info;
            QStringList warnings;
            QString err;
            const bool ok = installOne(instance, manifest, mod, found.value(mod.source.md5),
                                       folderOf.value(i), env, label, &info, &warnings, &err);
            QMutexLocker guard(&lock);
            result->warnings += warnings;
            if (ok) {
                instance.mods().insert(info.folder, info);
                ++result->installed;
            } else {
                result->failed << QStringLiteral("%1: %2").arg(mod.name, err);
                folderOf.remove(i);
            }
            ++done;
            if (callbacks.progress)
                callbacks.progress(done, total, mod.name);
        });
        if (cancelled(callbacks))
            break;
        /* Each phase gets saved: a crash loses a phase's tail, not the run. */
        instance.save(nullptr);
    }
    addRootTools(instance, folderOf.values());
    if (!instance.save(error))
        return false;
    if (cancelled(callbacks)) {
        *error = QStringLiteral("cancelled; run it again to continue where it stopped");
        return false;
    }

    /* Order the profile: the collection's mods by its rules, then anything
     * the person added themselves, kept above it so their overrides hold. */
    Profile profile;
    QString ignored;
    if (!instance.loadProfile(options.profile, &profile, &ignored))
        profile.name = options.profile;

    Collection::Sorted sorted = Collection::sortMods(manifest, included);
    for (const QString &c : std::as_const(sorted.cycles))
        result->warnings << QStringLiteral("rule cycle dropped: %1").arg(c);
    QSet<QString> collectionFolders(folderOf.cbegin(), folderOf.cend());
    std::vector<ProfileMod> mods;
    QHash<QString, bool> wasEnabled;
    for (const ProfileMod &m : profile.mods)
        wasEnabled.insert(m.folder, m.enabled);
    for (int i : sorted.order)
        if (folderOf.contains(i)) {
            const QString folder = folderOf.value(i);
            mods.push_back({folder, wasEnabled.value(folder, true)});
        }
    for (const ProfileMod &m : profile.mods) {
        const auto it = instance.mods().constFind(m.folder);
        const bool oldRevision = it != instance.mods().constEnd() && !it->collection.isEmpty();
        if (!collectionFolders.contains(m.folder) && !oldRevision)
            mods.push_back(m);
    }
    profile.mods = std::move(mods);

    profile.collection = label;
    profile.iniTweaks = copyIniTweaks(instance, manifest, options.profile);

    /* Keep a sorted order across an update: only new plugins lack a place. */
    const std::vector<ProfilePlugin> previous = profile.plugins;
    profile.plugins.clear();
    for (const Collection::Plugin &p : manifest.plugins)
        profile.plugins.push_back({p.name, p.enabled});
    const Deploy::FileIndex index = Deploy::buildIndex(Deploy::layersFor(instance, profile));
    const auto present = Plugins::available(instance, index);
    std::vector<Plugins::OrderRule> rules;
    for (const Collection::PluginRule &r : manifest.pluginRules)
        rules.push_back({r.name, r.after});
    if (!previous.empty()) {
        QStringList reference;
        for (const ProfilePlugin &p : previous)
            reference << p.name;
        Plugins::reconcile(instance, &profile, present);
        Plugins::adoptOrder(&profile, reference);
    }
    Plugins::sortSafely(instance, &profile, present, rules);
    profile.needsSorting = true;

    /* The curator's rules, for LOOT. JSON is valid YAML, and LOOT's format is
     * what Vortex stored them in. */
    QJsonObject userlist = manifest.pluginRulesJson;
    if (!userlist.contains(QLatin1String("globals")))
        userlist.insert(QStringLiteral("globals"), QJsonArray());
    if (!Util::writeJson(instance.profileDir(options.profile) + QStringLiteral("/userlist.yaml"),
                         QJsonDocument(userlist), error))
        return false;
    return instance.saveProfile(profile, error);
}

bool prepareArchive(const Instance &instance, const QString &archive, Prepared *out, QString *error)
{
    Prepared p;
    p.archive = archive;
    p.workDir = instance.tempPath() + QStringLiteral("/manual-%1").arg(QDateTime::currentMSecsSinceEpoch());
    p.extractedDir = p.workDir + QStringLiteral("/x");
    if (!Archive::extract(archive, p.extractedDir, error)) {
        discard(p);
        return false;
    }
    const QStringList files = Util::walkFiles(p.extractedDir);
    if (const auto root = Installer::findFomodRoot(files)) {
        const QString wanted = Util::foldKey(*root + QStringLiteral("fomod/ModuleConfig.xml"));
        for (const QString &f : files) {
            if (Util::foldKey(f) != wanted)
                continue;
            QFile xml(p.extractedDir + QLatin1Char('/') + f);
            if (!xml.open(QIODevice::ReadOnly) || !Fomod::parse(xml.readAll(), &p.script, error)) {
                discard(p);
                return false;
            }
        }
        p.isFomod = true;
        p.fomodRoot = *root;
    }
    *out = std::move(p);
    return true;
}

void discard(const Prepared &prepared)
{
    QDir(prepared.workDir).removeRecursively();
}

bool installPrepared(Instance &instance, const Prepared &prepared, const ModInfo &meta,
                     const std::optional<Fomod::Preset> &preset, const Fomod::Environment &env,
                     QString *folderOut, QStringList *warnings, QString *error)
{
    const auto cleanup = qScopeGuard([&] { discard(prepared); });
    const QString built = prepared.workDir + QStringLiteral("/mod");
    Installer::Request request;
    request.extractedDir = prepared.extractedDir;
    request.preset = preset;
    request.env = env;
    Installer::Plan plan = Installer::plan(request);
    if (!plan.ok()) {
        *error = plan.error;
        return false;
    }
    if (warnings)
        *warnings += plan.warnings;
    if (!Installer::apply(prepared.extractedDir, plan.copies, built, error))
        return false;
    const QString folder = instance.uniqueFolder(meta.name.isEmpty() ? QFileInfo(prepared.archive).completeBaseName()
                                                                     : meta.name);
    if (!QFile::rename(built, instance.modsPath() + QLatin1Char('/') + folder)) {
        *error = QStringLiteral("cannot move %1 into mods/").arg(folder);
        return false;
    }
    ModInfo info = meta;
    info.folder = folder;
    if (info.name.isEmpty())
        info.name = QFileInfo(prepared.archive).completeBaseName();
    info.archive = QFileInfo(prepared.archive).fileName();
    info.installed = QDateTime::currentSecsSinceEpoch();
    if (preset)
        info.choices = QJsonObject{{QStringLiteral("type"), QStringLiteral("fomod")},
                                   {QStringLiteral("options"), Fomod::presetToJson(*preset)}};
    instance.mods().insert(folder, info);
    if (folderOut)
        *folderOut = folder;
    return instance.save(error);
}

Fomod::Environment environmentFor(const Instance &instance, const Profile &profile)
{
    auto active = std::make_shared<QSet<QString>>();
    auto present = std::make_shared<QSet<QString>>();
    for (const ProfilePlugin &p : profile.plugins) {
        present->insert(Util::foldKey(p.name));
        if (p.enabled)
            active->insert(Util::foldKey(p.name));
    }
    for (const QString &f : QDir(instance.dataPath()).entryList(QDir::Files))
        present->insert(Util::foldKey(f));
    Fomod::Environment env;
    env.fileState = [active, present](const QString &file) {
        const QString key = Util::foldKey(Util::posixPath(file));
        if (active->contains(key))
            return Fomod::FileState::Active;
        return present->contains(key) ? Fomod::FileState::Inactive : Fomod::FileState::Missing;
    };
    return env;
}

bool mergeIni(const QString &target, const QStringList &tweakFiles, QString *error)
{
    QFile in(target);
    QStringList lines;
    if (in.open(QIODevice::ReadOnly)) {
        lines = QString::fromUtf8(in.readAll()).split(QRegularExpression(QStringLiteral("\r?\n")));
        in.close();
        const QString backup = target + QStringLiteral(".fanmo-backup");
        if (!QFileInfo::exists(backup))
            QFile::copy(target, backup);
    }
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    const auto sectionOf = [](const QString &line) {
        const QString t = line.trimmed();
        return t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']')) ? t.mid(1, t.size() - 2) : QString();
    };
    for (const QString &tweak : tweakFiles) {
        QFile f(tweak);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QString section;
        const QStringList tweakLines = QString::fromUtf8(f.readAll())
                                           .split(QRegularExpression(QStringLiteral("\r?\n")));
        for (const QString &raw : tweakLines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#')))
                continue;
            const QString header = sectionOf(line);
            if (!header.isEmpty()) {
                section = header;
                continue;
            }
            const qsizetype eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0 || section.isEmpty())
                continue;
            const QString key = line.left(eq).trimmed();
            const QString value = key + QLatin1Char('=') + line.mid(eq + 1).trimmed();

            qsizetype sectionAt = -1, keyAt = -1, sectionEnd = lines.size();
            for (qsizetype i = 0; i < lines.size(); ++i) {
                const QString h = sectionOf(lines[i]);
                if (!h.isEmpty()) {
                    if (sectionAt >= 0) {
                        sectionEnd = i;
                        break;
                    }
                    if (h.compare(section, Qt::CaseInsensitive) == 0)
                        sectionAt = i;
                    continue;
                }
                if (sectionAt >= 0) {
                    const qsizetype e = lines[i].indexOf(QLatin1Char('='));
                    if (e > 0 && lines[i].left(e).trimmed().compare(key, Qt::CaseInsensitive) == 0)
                        keyAt = i;
                }
            }
            if (keyAt >= 0) {
                lines[keyAt] = value;
            } else if (sectionAt >= 0) {
                qsizetype at = sectionEnd;
                while (at > sectionAt + 1 && lines[at - 1].trimmed().isEmpty())
                    --at;
                lines.insert(at, value);
            } else {
                lines << QString() << QLatin1Char('[') + section + QLatin1Char(']') << value;
            }
        }
    }
    return Util::writeFile(target, (lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8(), error);
}

/* The file in My Games a tweak named "... [SkyrimPrefs].ini" goes to. */
static QString tweakTarget(const QString &fileName)
{
    static const QRegularExpression re(QStringLiteral("\\[([^\\]]+)\\]\\.ini$"), QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(fileName);
    return m.hasMatch() ? m.captured(1) + QStringLiteral(".ini") : QStringLiteral("Skyrim.ini");
}

/* Swaps the profile's INIs into My Games, and points saves at its folder. */
static bool applyIniState(Instance &instance, const QString &previous, const Profile &next, QString *error)
{
    const QString myGames = instance.myGamesPath();
    QDir().mkpath(myGames);
    static const QStringList inis = {QStringLiteral("Skyrim.ini"), QStringLiteral("SkyrimPrefs.ini"),
                                     QStringLiteral("SkyrimCustom.ini")};
    Profile prior;
    QString ignored;
    if (previous != next.name && instance.loadProfile(previous, &prior, &ignored) && prior.localInis)
        for (const QString &ini : inis) {
            const QString stored = instance.profileDir(previous) + QStringLiteral("/ini/") + ini;
            QDir().mkpath(QFileInfo(stored).absolutePath());
            QFile::remove(stored);
            QFile::copy(myGames + QLatin1Char('/') + ini, stored);
        }
    if (next.localInis) {
        const QString dir = instance.profileDir(next.name) + QStringLiteral("/ini/");
        for (const QString &ini : inis) {
            if (QFileInfo::exists(dir + ini)) {
                QFile::remove(myGames + QLatin1Char('/') + ini);
                QFile::copy(dir + ini, myGames + QLatin1Char('/') + ini);
            }
        }
    }
    for (const QString &tweak : next.iniTweaks) {
        const QString file = instance.profileDir(next.name) + QStringLiteral("/ini-tweaks/") + tweak;
        if (!mergeIni(myGames + QLatin1Char('/') + tweakTarget(tweak), {file}, error))
            return false;
    }

    /* A one-line INI of our own holding one value, merged like a tweak. */
    const QString savePath = next.localSaves ? QStringLiteral("Saves\\%1\\").arg(Util::safeFolderName(next.name))
                                             : QStringLiteral("Saves\\");
    const QString tempTweak = instance.tempPath() + QStringLiteral("/save-path.ini");
    if (!Util::writeFile(tempTweak, QStringLiteral("[General]\r\nsLocalSavePath=%1\r\n").arg(savePath).toUtf8(), error))
        return false;
    const bool ok = mergeIni(myGames + QStringLiteral("/Skyrim.ini"), {tempTweak}, error);
    QFile::remove(tempTweak);
    return ok;
}

bool activateProfile(Instance &instance, const QString &name, const Callbacks &callbacks,
                     Deploy::Report *report, QString *error)
{
    Profile profile;
    if (!instance.loadProfile(name, &profile, error))
        return false;
    const QString blocked = Deploy::blocker(instance);
    if (!blocked.isEmpty()) {
        *error = blocked;
        return false;
    }
    const Deploy::FileIndex index = Deploy::buildIndex(Deploy::layersFor(instance, profile));
    Deploy::Options options;
    options.progress = callbacks.progress;
    options.log = callbacks.log;
    options.cancel = callbacks.cancel;
    if (!Deploy::deploy(instance, index, options, report, error))
        return false;
    Plugins::reconcile(instance, &profile, Plugins::available(instance, index));
    if (!instance.saveProfile(profile, error) || !Plugins::write(instance, profile, error))
        return false;

    /* The game is deployed by now, so a userlist that can't be merged is a
     * warning: the rules are missing from LOOT, nothing else is wrong. */
    QStringList notes;
    QString rulesError;
    if (!Userlist::sync(instance, name, &notes, &rulesError))
        report->warnings << QStringLiteral("LOOT's userlist left as it was, so the collection's rules aren't in it: %1")
                                .arg(rulesError);
    report->warnings += notes;

    const QString previous = instance.activeProfile();
    if (!applyIniState(instance, previous, profile, error))
        return false;
    instance.setActiveProfile(name);
    return instance.save(error);
}

} /* namespace Engine */
