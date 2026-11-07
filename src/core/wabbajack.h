/* Installing .wabbajack lists.
 *
 * A list is a compiled MO2 instance: every output file is a directive saying
 * which archive it came from, and how. fanMO runs the directives that produce
 * game content -- mods/, the profile, Game Root -- and leaves out the copy of
 * MO2 itself, then imports the result as an ordinary profile.
 *
 * Supported directives: FromArchive, PatchedFromArchive, InlineFile,
 * RemappedInlineFile. Lists that build BSAs or recompress textures use more,
 * and are refused up front by inspect(), before anything is written. */
#ifndef FANMO_WABBAJACK_H
#define FANMO_WABBAJACK_H

#include "engine.h"
#include "instance.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace Wabbajack {

struct Archive {
    QString hash;
    QString name;
    qint64 size = 0;
    QString kind;   /* Nexus, Http, GameFile, ... */
    qint64 modId = 0, fileId = 0;
    QString url;
    QString gameFile;
};

struct Summary {
    QString name;
    QString author;
    QString version;
    QString game;
    QString website;
    int archiveCount = 0;
    qint64 archiveBytes = 0;
    int directiveCount = 0;
    QHash<QString, int> directiveTypes;
    QStringList unsupported;  /* one line each; empty means it can install */
};

/* Reads a list's header without installing anything. */
bool inspect(const QString &wabbajackFile, Summary *out, QString *error);

struct Options {
    QString profile;
    int jobs = 6;
    QString apiKey;
    /* Install what's possible rather than stopping at a missing archive. The
     * result is an incomplete list, so this is for testing. */
    bool allowMissing = false;
    /* Hash every file written against the list. Patched files always are. */
    bool verifyHashes = false;
};

struct Result {
    int filesWritten = 0;
    int filesVerified = 0;
    int modsImported = 0;
    QStringList hashMismatches;
    QStringList missingArchives;
    QStringList warnings;
};

/* Takes what MO2 kept about a mod out of its folder: meta.ini fills in what's
 * known, and leaves, since in Data it would be a stray file. Returns false for
 * a separator, which is a list heading and not a mod. */
bool readMo2Mod(const QString &folder, ModInfo *mod);

bool install(Instance &instance, const QString &wabbajackFile, const Options &options,
             const Engine::Callbacks &callbacks, Result *result, QString *error);

} /* namespace Wabbajack */

#endif
