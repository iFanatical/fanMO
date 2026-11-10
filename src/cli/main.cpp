/* fanmo-cli: the core without the window. Also the place the install engine
 * gets checked against reference data on a real system. */
#include "core/archive.h"
#include "core/archiveindex.h"
#include "core/collection.h"
#include "core/deploy.h"
#include "core/engine.h"
#include "core/installer.h"
#include "core/instance.h"
#include "core/nexus.h"
#include "core/patch.h"
#include "core/plugins.h"
#include "core/steam.h"
#include "core/util.h"
#include "core/version.h"
#include "core/wabbajack.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtConcurrent>

#include <cstdio>
#include <iostream>

static void say(const QString &s)
{
    std::cout << s.toStdString() << std::endl;
}

static QString option(QStringList &args, const QString &name, const QString &fallback = {})
{
    const qsizetype i = args.indexOf(name);
    if (i < 0 || i + 1 >= args.size())
        return fallback;
    const QString value = args.at(i + 1);
    args.remove(i, 2);
    return value;
}

static QStringList ignoredVortexFiles()
{
    return {QStringLiteral("__folder_managed_by_vortex")};
}

/* Installs every mod of a collection into scratch/mods, comparing each with
 * what Vortex made of the same archive. Touches nothing but scratch. */
static int verifyCollection(QStringList args)
{
    const QString manifestPath = option(args, QStringLiteral("--collection"));
    const QString archives = option(args, QStringLiteral("--archives"));
    const QString vortex = option(args, QStringLiteral("--vortex-staging"));
    const QString scratch = option(args, QStringLiteral("--scratch"));
    const QString baseData = option(args, QStringLiteral("--base-data"));
    const QString only = option(args, QStringLiteral("--only"));
    const int jobs = option(args, QStringLiteral("--jobs"), QStringLiteral("8")).toInt();
    const bool extractOnlyFomods = args.contains(QStringLiteral("--fomods-only"));

    Collection::Manifest manifest;
    QString error;
    if (!Collection::load(manifestPath, &manifest, &error)) {
        say(QStringLiteral("cannot load collection: ") + error);
        return 1;
    }
    QDir().mkpath(scratch);

    QList<ArchiveIndex::Wanted> wanted;
    for (const Collection::Mod &m : manifest.mods)
        wanted.append({m.source.md5, m.source.size});
    say(QStringLiteral("scanning archives for %1 mods...").arg(manifest.mods.size()));
    const auto found = ArchiveIndex::scan(archives.split(QLatin1Char(',')), wanted,
                                          scratch + QStringLiteral("/md5-cache.json"),
                                          [](qint64 d, qint64 t, const QString &) {
                                              if (d % 200 == 0 || d == t)
                                                  say(QStringLiteral("  hashed %1/%2").arg(d).arg(t));
                                          });
    say(QStringLiteral("archives found: %1 of %2").arg(found.size()).arg(manifest.mods.size()));

    QStringList baseFiles;
    if (!baseData.isEmpty())
        baseFiles = Util::walkFiles(baseData);
    const Fomod::Environment env = Collection::environment(manifest, baseFiles);

    struct Outcome {
        QString name;
        QString archive;
        bool fomod = false;
        QString error;
        QStringList missing, extra, sizeDiff, warnings;
        int files = 0;
        bool vortexFound = false;
    };
    QList<Outcome> outcomes;
    QMutex lock;
    int done = 0;

    QList<int> indices;
    for (int i = 0; i < int(manifest.mods.size()); ++i) {
        const Collection::Mod &m = manifest.mods[size_t(i)];
        if (extractOnlyFomods && !m.choices)
            continue;
        if (!only.isEmpty() && !QRegularExpression(only, QRegularExpression::CaseInsensitiveOption).match(m.name).hasMatch())
            continue;
        indices << i;
    }

    QThreadPool pool;
    pool.setMaxThreadCount(jobs);
    const int total = indices.size();
    QtConcurrent::blockingMap(&pool, indices, [&](int index) {
        const Collection::Mod &mod = manifest.mods[size_t(index)];
        Outcome o;
        o.name = mod.name;
        const QString archive = found.value(mod.source.md5);
        if (archive.isEmpty()) {
            o.error = QStringLiteral("archive not found");
        } else {
            o.archive = archive;
            const QString tmp = scratch + QStringLiteral("/tmp/%1").arg(index);
            QDir(tmp).removeRecursively();
            QString err;
            if (!Archive::extract(archive, tmp, &err)) {
                o.error = QStringLiteral("extract: ") + err;
            } else {
                Installer::Request request;
                request.extractedDir = tmp;
                request.preset = mod.choices;
                request.env = env;
                request.rootDeploy = mod.type == QLatin1String("dinput");
                const Installer::Plan plan = Installer::plan(request);
                o.fomod = plan.isFomod;
                o.warnings = plan.warnings;
                const QString base = QFileInfo(archive).completeBaseName();
                const QString folder = Util::safeFolderName(base);
                const QString target = scratch + QStringLiteral("/mods/") + folder;
                QDir(target).removeRecursively();
                if (!plan.ok()) {
                    o.error = plan.error;
                } else if (!Installer::apply(tmp, plan.copies, target, &err)) {
                    o.error = err;
                } else {
                    const QStringList mine = Util::walkFiles(target);
                    o.files = mine.size();
                    const QString theirs = vortex + QLatin1Char('/') + base;
                    if (QFileInfo(theirs).isDir()) {
                        o.vortexFound = true;
                        QHash<QString, QString> theirFiles;
                        const QStringList ignore = ignoredVortexFiles();
                        for (const QString &f : Util::walkFiles(theirs))
                            if (!ignore.contains(f) && !f.startsWith(QLatin1String("vortex.")))
                                theirFiles.insert(Util::foldKey(f), f);
                        for (const QString &f : mine) {
                            const QString key = Util::foldKey(f);
                            const auto it = theirFiles.constFind(key);
                            if (it == theirFiles.constEnd()) {
                                o.extra << f;
                                continue;
                            }
                            if (QFileInfo(target + QLatin1Char('/') + f).size()
                                != QFileInfo(theirs + QLatin1Char('/') + *it).size())
                                o.sizeDiff << f;
                            theirFiles.erase(it);
                        }
                        o.missing = theirFiles.values();
                    }
                }
            }
            QDir(tmp).removeRecursively();
        }
        QMutexLocker guard(&lock);
        outcomes.append(o);
        if (++done % 100 == 0)
            say(QStringLiteral("  %1/%2").arg(done).arg(total));
    });

    int clean = 0, differ = 0, failed = 0, noRef = 0, fomods = 0, fomodClean = 0;
    qint64 totalFiles = 0;
    QJsonArray report;
    for (const Outcome &o : std::as_const(outcomes)) {
        totalFiles += o.files;
        if (o.fomod)
            ++fomods;
        if (!o.error.isEmpty()) {
            ++failed;
            say(QStringLiteral("FAIL  %1: %2").arg(o.name, o.error));
        } else if (!o.vortexFound) {
            ++noRef;
        } else if (o.missing.isEmpty() && o.extra.isEmpty() && o.sizeDiff.isEmpty()) {
            ++clean;
            if (o.fomod)
                ++fomodClean;
        } else {
            ++differ;
            report.append(QJsonObject{
                {QStringLiteral("name"), o.name},
                {QStringLiteral("archive"), QFileInfo(o.archive).fileName()},
                {QStringLiteral("fomod"), o.fomod},
                {QStringLiteral("files"), o.files},
                {QStringLiteral("missing"), QJsonArray::fromStringList(o.missing.mid(0, 40))},
                {QStringLiteral("extra"), QJsonArray::fromStringList(o.extra.mid(0, 40))},
                {QStringLiteral("missingCount"), int(o.missing.size())},
                {QStringLiteral("extraCount"), int(o.extra.size())},
                {QStringLiteral("sizeDiff"), QJsonArray::fromStringList(o.sizeDiff.mid(0, 40))},
                {QStringLiteral("sizeDiffCount"), int(o.sizeDiff.size())},
                {QStringLiteral("warnings"), QJsonArray::fromStringList(o.warnings.mid(0, 10))},
            });
        }
    }
    Util::writeJson(scratch + QStringLiteral("/report.json"), QJsonDocument(report));
    say(QStringLiteral("\nmods %1  identical %2  different %3  failed %4  no Vortex folder %5")
            .arg(outcomes.size()).arg(clean).arg(differ).arg(failed).arg(noRef));
    say(QStringLiteral("fomods %1  identical %2   files installed %3").arg(fomods).arg(fomodClean).arg(totalFiles));
    say(QStringLiteral("differences: %1/report.json").arg(scratch));
    return failed + differ ? 2 : 0;
}

static Engine::Callbacks consoleCallbacks()
{
    Engine::Callbacks c;
    c.log = [](const QString &line) { say(line); };
    c.progress = [](qint64 done, qint64 total, const QString &what) {
        if (total > 0 && (done == total || done % 250 == 0))
            say(QStringLiteral("  %1/%2 %3").arg(done).arg(total).arg(what));
    };
    return c;
}

static bool openInstance(QStringList &args, Instance *instance)
{
    const QString path = option(args, QStringLiteral("--instance"));
    QString error;
    if (path.isEmpty() || !instance->load(path, &error)) {
        say(QStringLiteral("need --instance DIR: %1").arg(error));
        return false;
    }
    return true;
}

static int createInstance(QStringList args)
{
    const QString root = option(args, QStringLiteral("--instance"));
    QString game = option(args, QStringLiteral("--game"));
    QString compat = option(args, QStringLiteral("--compat"));
    QString proton = option(args, QStringLiteral("--proton"), Steam::defaultProton());
    if (game.isEmpty()) {
        const auto installs = Steam::findSkyrim();
        for (const Steam::Install &i : installs)
            say(QStringLiteral("found: %1  (prefix: %2)").arg(i.gamePath, i.compatDataPath));
        say(QStringLiteral("pass --game DIR to choose one"));
        return 1;
    }
    QString error;
    if (!Instance::create(root, game, compat, proton, &error)) {
        say(error);
        return 1;
    }
    say(QStringLiteral("created %1").arg(root));
    return 0;
}

static int installCollection(QStringList args)
{
    Instance instance;
    if (!openInstance(args, &instance))
        return 1;
    const QString source = option(args, QStringLiteral("--collection"));
    const QString archives = option(args, QStringLiteral("--archives"));
    Engine::CollectionOptions options;
    options.profile = option(args, QStringLiteral("--profile"), QStringLiteral("Collection"));
    options.jobs = option(args, QStringLiteral("--jobs"), QStringLiteral("6")).toInt();
    options.includeOptional = !args.contains(QStringLiteral("--no-optional"));
    options.apiKey = Nexus::loadKey();
    if (!archives.isEmpty())
        instance.setArchiveFolders(archives.split(QLatin1Char(',')));

    const auto callbacks = consoleCallbacks();
    Collection::Manifest manifest;
    QString error;
    const Nexus::NxmLink link = Nexus::parseCollectionUrl(source);
    bool ok;
    if (link.kind == Nexus::NxmLink::Collection)
        ok = Engine::fetchCollection(instance, options.apiKey, link.collectionSlug, link.revision, &manifest, callbacks, &error);
    else if (source.endsWith(QLatin1String(".json")))
        ok = Collection::load(source, &manifest, &error);
    else
        ok = Engine::unpackBundle(instance, source, QFileInfo(source).completeBaseName(), &manifest, &error);
    if (!ok) {
        say(error);
        return 1;
    }
    Engine::CollectionResult result;
    ok = Engine::installCollection(instance, manifest, manifest.name, options, callbacks, &result, &error);
    for (const QString &w : std::as_const(result.warnings))
        say(QStringLiteral("warning: ") + w);
    for (const QString &f : std::as_const(result.failed))
        say(QStringLiteral("failed: ") + f);
    for (const Engine::ManualFile &m : result.manual)
        say(QStringLiteral("needs you: %1 (%2) %3").arg(m.name, m.reason, m.url));
    say(QStringLiteral("%1 installed, %2 kept, %3 downloaded, %4 need you, %5 failed")
            .arg(result.installed).arg(result.reused).arg(result.downloaded)
            .arg(result.manual.size()).arg(result.failed.size()));
    if (!ok)
        say(error);
    return ok && result.complete() ? 0 : 2;
}

static int status(QStringList args)
{
    Instance instance;
    if (!openInstance(args, &instance))
        return 1;
    const QString name = option(args, QStringLiteral("--profile"), instance.activeProfile());
    Profile profile;
    QString error;
    if (!instance.loadProfile(name, &profile, &error)) {
        say(error);
        return 1;
    }
    QElapsedTimer timer;
    timer.start();
    const Deploy::FileIndex index = Deploy::buildIndex(Deploy::layersFor(instance, profile));
    say(QStringLiteral("profile %1: %2 mods, %3 plugins, %4 files (scanned in %5 ms)")
            .arg(name).arg(profile.mods.size()).arg(profile.plugins.size()).arg(index.providers.size()).arg(timer.elapsed()));
    const QString blocked = Deploy::blocker(instance);
    say(blocked.isEmpty() ? QStringLiteral("ready to deploy") : QStringLiteral("blocked: ") + blocked);
    return 0;
}

static int deployCommand(QStringList args)
{
    Instance instance;
    if (!openInstance(args, &instance))
        return 1;
    const QString name = option(args, QStringLiteral("--profile"), instance.activeProfile());
    Deploy::Report report;
    QString error;
    QElapsedTimer timer;
    timer.start();
    if (!Engine::activateProfile(instance, name, consoleCallbacks(), &report, &error)) {
        say(error);
        return 1;
    }
    say(QStringLiteral("%1 linked, %2 removed, %3 unchanged, %4 backed up, %5 restored, %6 captured, %7 ms")
            .arg(report.linked).arg(report.removed).arg(report.unchanged).arg(report.backedUp)
            .arg(report.restored).arg(report.captured).arg(timer.elapsed()));
    for (const QString &warning : std::as_const(report.warnings))
        say(QStringLiteral("warning: %1").arg(warning));
    return 0;
}

static int purgeCommand(QStringList args)
{
    Instance instance;
    if (!openInstance(args, &instance))
        return 1;
    Deploy::Report report;
    QString error;
    Deploy::Options options;
    options.progress = consoleCallbacks().progress;
    if (!Deploy::purge(instance, options, &report, &error)) {
        say(error);
        return 1;
    }
    say(QStringLiteral("%1 removed, %2 restored").arg(report.removed).arg(report.restored));
    return 0;
}

static int importOrder(QStringList args)
{
    Instance instance;
    if (!openInstance(args, &instance))
        return 1;
    const QString name = option(args, QStringLiteral("--profile"), instance.activeProfile());
    const QString file = option(args, QStringLiteral("--file"));
    Profile profile;
    QString error;
    if (!instance.loadProfile(name, &profile, &error)) {
        say(error);
        return 1;
    }
    const QStringList reference = Plugins::readOrderFile(file, &error);
    const int known = Plugins::adoptOrder(&profile, reference);
    profile.needsSorting = false;
    say(QStringLiteral("%1 of %2 plugins placed").arg(known).arg(profile.plugins.size()));
    return instance.saveProfile(profile, &error) ? 0 : 1;
}

/* Which mod wins each file, against what Vortex actually deployed. */
static int verifyDeploy(QStringList args)
{
    Instance instance;
    if (!openInstance(args, &instance))
        return 1;
    const QString name = option(args, QStringLiteral("--profile"), instance.activeProfile());
    const QString manifestPath = option(args, QStringLiteral("--vortex-manifest"));
    const QString reportPath = option(args, QStringLiteral("--report"));
    Profile profile;
    QString error;
    if (!instance.loadProfile(name, &profile, &error)) {
        say(error);
        return 1;
    }
    const Deploy::FileIndex index = Deploy::buildIndex(Deploy::layersFor(instance, profile));
    const QJsonObject vortex = Util::readJson(manifestPath, &error).object();
    const QJsonArray files = vortex.value(QLatin1String("files")).toArray();
    QHash<QString, QString> theirs;
    theirs.reserve(files.size());
    for (const auto &v : files) {
        const QJsonObject o = v.toObject();
        theirs.insert(QStringLiteral("d/") + Util::foldKey(Util::posixPath(o.value(QLatin1String("relPath")).toString())),
                      o.value(QLatin1String("source")).toString());
    }
    qint64 same = 0, different = 0, onlyMine = 0, onlyTheirs = 0;
    QHash<QString, int> pairs;
    QHash<QString, QStringList> examples;
    for (auto it = index.providers.cbegin(); it != index.providers.cend(); ++it) {
        if (!it.key().startsWith(QLatin1String("d/")))
            continue;
        const QString mine = index.layers[size_t(it.value().back().layer)].id;
        const auto other = theirs.constFind(it.key());
        if (other == theirs.constEnd()) {
            ++onlyMine;
            ++pairs[QStringLiteral("only fanMO: ") + mine];
            continue;
        }
        if (*other == mine) {
            ++same;
        } else {
            ++different;
            const QString pair = QStringLiteral("%1  <->  %2").arg(mine, *other);
            ++pairs[pair];
            if (examples[pair].size() < 3)
                examples[pair] << it.value().back().relPath;
        }
        theirs.erase(other);
    }
    QHash<QString, int> leftover;
    for (const QString &source : std::as_const(theirs)) {
        ++onlyTheirs;
        ++leftover[source];
    }
    say(QStringLiteral("files: same winner %1, different winner %2, only fanMO %3, only Vortex %4")
            .arg(same).arg(different).arg(onlyMine).arg(onlyTheirs));
    QJsonArray out;
    QList<std::pair<int, QString>> sorted;
    for (auto it = pairs.cbegin(); it != pairs.cend(); ++it)
        sorted.append({it.value(), it.key()});
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &[count, pair] : sorted)
        out.append(QJsonObject{{QStringLiteral("count"), count}, {QStringLiteral("pair"), pair},
                               {QStringLiteral("examples"), QJsonArray::fromStringList(examples.value(pair))}});
    QJsonArray missing;
    for (auto it = leftover.cbegin(); it != leftover.cend(); ++it)
        missing.append(QJsonObject{{QStringLiteral("count"), it.value()}, {QStringLiteral("mod"), it.key()}});
    if (!reportPath.isEmpty())
        Util::writeJson(reportPath, QJsonDocument(QJsonObject{{QStringLiteral("pairs"), out}, {QStringLiteral("onlyVortex"), missing}}));
    for (qsizetype i = 0; i < qMin<qsizetype>(sorted.size(), 25); ++i)
        say(QStringLiteral("  %1  %2").arg(sorted[i].first).arg(sorted[i].second));
    return different ? 2 : 0;
}

static int wabbajackInfo(QStringList args)
{
    const QString file = option(args, QStringLiteral("--list"));
    Wabbajack::Summary s;
    QString error;
    if (!Wabbajack::inspect(file, &s, &error)) {
        say(error);
        return 1;
    }
    say(QStringLiteral("%1 %2 by %3").arg(s.name, s.version, s.author));
    say(QStringLiteral("%1 archives, %2; %3 directives").arg(s.archiveCount).arg(Util::humanSize(s.archiveBytes)).arg(s.directiveCount));
    for (auto it = s.directiveTypes.cbegin(); it != s.directiveTypes.cend(); ++it)
        say(QStringLiteral("  %1 %2").arg(it.value()).arg(it.key()));
    say(s.unsupported.isEmpty() ? QStringLiteral("fanMO can install this list")
                                : QStringLiteral("unsupported: ") + s.unsupported.join(QStringLiteral(", ")));
    return s.unsupported.isEmpty() ? 0 : 2;
}

static int wabbajackInstall(QStringList args)
{
    Instance instance;
    if (!openInstance(args, &instance))
        return 1;
    const QString file = option(args, QStringLiteral("--list"));
    const QString archives = option(args, QStringLiteral("--archives"));
    if (!archives.isEmpty())
        instance.setArchiveFolders(archives.split(QLatin1Char(',')));
    Wabbajack::Options options;
    options.profile = option(args, QStringLiteral("--profile"), QStringLiteral("Wabbajack"));
    options.apiKey = Nexus::loadKey();
    options.jobs = option(args, QStringLiteral("--jobs"), QStringLiteral("6")).toInt();
    options.allowMissing = args.contains(QStringLiteral("--allow-missing"));
    options.verifyHashes = args.contains(QStringLiteral("--verify"));
    Wabbajack::Result result;
    QString error;
    const bool ok = Wabbajack::install(instance, file, options, consoleCallbacks(), &result, &error);
    for (const QString &m : std::as_const(result.missingArchives))
        say(QStringLiteral("missing: ") + m);
    for (qsizetype i = 0; i < qMin<qsizetype>(result.hashMismatches.size(), 40); ++i)
        say(QStringLiteral("hash mismatch: ") + result.hashMismatches[i]);
    say(QStringLiteral("%1 files, %2 mods, %3 verified, %4 mismatched")
            .arg(result.filesWritten).arg(result.modsImported).arg(result.filesVerified).arg(result.hashMismatches.size()));
    if (!ok)
        say(error);
    return ok ? 0 : 2;
}

static int hashCommand(const QStringList &args)
{
    for (const QString &file : args)
        say(QStringLiteral("%1  %2  %3").arg(QString::fromLatin1(Util::md5File(file)),
                                             Util::wabbajackHash(Util::xxh64File(file)), file));
    return 0;
}

/* The one command that changes your desktop's nxm:// handler. */
static int registerNxm()
{
    QProcess p;
    p.start(QStringLiteral("xdg-mime"), {QStringLiteral("default"), QStringLiteral("fanmo.desktop"),
                                         QStringLiteral("x-scheme-handler/nxm")});
    p.waitForFinished();
    say(p.exitCode() == 0 ? QStringLiteral("fanmo now handles nxm:// links")
                          : QStringLiteral("xdg-mime failed"));
    return p.exitCode();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("fanMO"));
    QCoreApplication::setApplicationVersion(QStringLiteral(FANMO_VERSION));
    QStringList args = app.arguments().mid(1);
    if (args == QStringList{QStringLiteral("--version")}) {
        say(QStringLiteral("fanmo-cli " FANMO_VERSION));
        return 0;
    }
    if (args.isEmpty()) {
        say(QStringLiteral(
            "fanmo-cli " FANMO_VERSION "\n"
            "usage: fanmo-cli COMMAND ...\n"
            "  find-game\n"
            "  create-instance --instance DIR --game DIR --compat DIR [--proton FILE]\n"
            "  install-collection --instance DIR --collection FILE|URL [--profile NAME] [--archives DIR,DIR] [--no-optional]\n"
            "  status --instance DIR [--profile NAME]\n"
            "  deploy --instance DIR [--profile NAME]\n"
            "  purge --instance DIR\n"
            "  import-order --instance DIR --file LOADORDER.TXT [--profile NAME]\n"
            "  wabbajack-info --list FILE\n"
            "  wabbajack-install --instance DIR --list FILE [--profile NAME] [--archives DIR,DIR] [--verify] [--allow-missing]\n"
            "  register-nxm\n"
            "  verify-collection --collection FILE --archives DIR[,DIR] --vortex-staging DIR --scratch DIR\n"
            "  verify-deploy --instance DIR --vortex-manifest FILE [--report FILE]"));
        return 1;
    }
    const QString command = args.takeFirst();
    if (command == QLatin1String("verify-collection"))
        return verifyCollection(args);
    if (command == QLatin1String("find-game") || command == QLatin1String("create-instance"))
        return createInstance(args);
    if (command == QLatin1String("install-collection"))
        return installCollection(args);
    if (command == QLatin1String("status"))
        return status(args);
    if (command == QLatin1String("deploy"))
        return deployCommand(args);
    if (command == QLatin1String("purge"))
        return purgeCommand(args);
    if (command == QLatin1String("import-order"))
        return importOrder(args);
    if (command == QLatin1String("verify-deploy"))
        return verifyDeploy(args);
    if (command == QLatin1String("wabbajack-info"))
        return wabbajackInfo(args);
    if (command == QLatin1String("wabbajack-install"))
        return wabbajackInstall(args);
    if (command == QLatin1String("register-nxm"))
        return registerNxm();
    if (command == QLatin1String("hash"))
        return hashCommand(args);
    say(QStringLiteral("unknown command: ") + command);
    return 1;
}
