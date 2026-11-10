/* The long-running operations, in a form both front ends call from a worker
 * thread: fetch and install a collection, install one archive, activate a
 * profile. Nothing here touches the game folder except activateProfile. */
#ifndef FANMO_ENGINE_H
#define FANMO_ENGINE_H

#include "collection.h"
#include "deploy.h"
#include "instance.h"
#include "util.h"

#include <QStringList>

#include <atomic>
#include <functional>

namespace Engine {

struct Callbacks {
    Util::Progress progress;
    Util::Logger log;
    const std::atomic_bool *cancel = nullptr;
};

struct CollectionOptions {
    QString profile;           /* created if it doesn't exist */
    bool includeOptional = true;
    int jobs = 6;
    QString apiKey;            /* empty: no network, archives must be on disk */
    int downloadJobs = 4;
};

struct ManualFile {
    QString name;
    QString url;
    QString reason;
    qint64 modId = 0;
    qint64 fileId = 0;
    QString md5;
    qint64 size = 0;
};

struct CollectionResult {
    int installed = 0;
    int reused = 0;        /* already installed with the same archive and choices */
    int downloaded = 0;
    std::vector<ManualFile> manual;  /* needs a browser or a person */
    QStringList failed;
    QStringList warnings;
    bool complete() const { return manual.empty() && failed.isEmpty(); }
};

/* Unpacks a collection archive into instance/collections and loads it. */
bool unpackBundle(const Instance &instance, const QString &bundle7z, const QString &name,
                  Collection::Manifest *out, QString *error);

/* Downloads a revision's bundle from Nexus and unpacks it. */
bool fetchCollection(const Instance &instance, const QString &apiKey, const QString &slug,
                     int revision, Collection::Manifest *out, const Callbacks &callbacks,
                     QString *error);

/* Installs a collection into a profile. Safe to run again: mods already
 * installed from the same archive with the same choices are kept, which is
 * also what makes moving to a new revision an incremental update. */
bool installCollection(Instance &instance, const Collection::Manifest &manifest,
                       const QString &label, const CollectionOptions &options,
                       const Callbacks &callbacks, CollectionResult *result, QString *error);

/* An archive unpacked and looked at, between deciding to install it and
 * answering its installer. discard() or installPrepared() cleans it up. */
struct Prepared {
    QString archive;
    QString workDir;
    QString extractedDir;
    bool isFomod = false;
    Fomod::Script script;
    QString fomodRoot;
};

bool prepareArchive(const Instance &instance, const QString &archive, Prepared *out, QString *error);
void discard(const Prepared &prepared);

/* What's known about an archive before it's installed: its Nexus mod, name
 * and version, from the meta file a download leaves beside it or, failing
 * that, from the way Nexus names its files. */
ModInfo metaForArchive(const QString &archive);

/* Mods installed on their own before fanMO kept that, with no Nexus id: gives
 * them one the same way, along with a proper name and version where they
 * still have the archive's. Returns the folders it changed. */
QStringList resolveNexusIds(Instance &instance);

/* Fills in what a mod's archive name couldn't say, author and category,
 * from Nexus. Only empty fields change. */
bool describeFromNexus(Instance &instance, const QStringList &folders, const QString &apiKey,
                       const Callbacks &callbacks, QString *error);

/* preset: nullopt uses the installer's defaults. */
bool installPrepared(Instance &instance, const Prepared &prepared, const ModInfo &meta,
                     const std::optional<Fomod::Preset> &preset, const Fomod::Environment &env,
                     QString *folder, QStringList *warnings, QString *error);

/* The environment a hand-installed FOMOD sees: the profile's plugins. */
Fomod::Environment environmentFor(const Instance &instance, const Profile &profile);

/* Makes a profile the live one: deploys it, writes plugins, swaps INIs. */
bool activateProfile(Instance &instance, const QString &name, const Callbacks &callbacks,
                     Deploy::Report *report, QString *error);

/* Merges INI tweak files into a target INI, keeping everything else as is. */
bool mergeIni(const QString &target, const QStringList &tweakFiles, QString *error);

} /* namespace Engine */

#endif
