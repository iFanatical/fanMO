/* Turns an unpacked archive into a mod folder: works out where its data
 * starts, runs its FOMOD if it has one, applies binary patches. */
#ifndef FANMO_INSTALLER_H
#define FANMO_INSTALLER_H

#include "fomod.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace Installer {

struct Plan {
    std::vector<Fomod::Copy> copies;
    bool isFomod = false;
    QString fomodPrefix;     /* folder holding fomod/ inside the archive */
    QStringList warnings;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

/* The folder holding fomod/ModuleConfig.xml, or nullopt. C# script installers
 * count as absent: they are .NET programs and never run here. */
std::optional<QString> findFomodRoot(const QStringList &files);

/* For an archive with no installer: Vortex's rule for where a mod's data
 * begins, driven by the same stop patterns. */
QString pathPrefix(const QStringList &files);

/* The folder holding a root mod's programs, shallowest first. */
QString rootPrefix(const QStringList &files);

struct Request {
    QString extractedDir;
    std::optional<Fomod::Preset> preset; /* nullopt: defaults, or the wizard's choices */
    Fomod::Environment env;
    /* Deploys to the game folder: its Data/ is a real folder there, and its
     * root is wherever its programs are. */
    bool rootDeploy = false;
};

Plan plan(const Request &request);

/* Lays the plan out in target. Sources are hard-linked from the extraction
 * folder, which must be on the same filesystem. Paths that differ only by
 * case merge into the first spelling seen; of two such files the later copy
 * wins, lower priorities going first. */
bool apply(const QString &extractedDir, const std::vector<Fomod::Copy> &copies,
           const QString &target, QString *error);

/* Patches in place, by the source CRC Vortex recorded. patches maps a path
 * in the mod to that CRC; diffs live at patchDir/<path>.diff. A CRC that
 * doesn't match is left alone and reported, as Vortex does. */
bool applyPatches(const QString &modDir, const QString &patchDir,
                  const QList<std::pair<QString, QString>> &patches,
                  QStringList *warnings, QString *error);

} /* namespace Installer */

#endif
