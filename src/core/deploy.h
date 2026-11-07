/* Hard-link deployment, and the conflict picture it follows from.
 *
 * Deploying is a diff against the last deployment's manifest, so a reorder
 * touches a handful of links rather than 200,000. Three promises the code
 * below keeps:
 *   - nothing not in a manifest is deleted: a file in the way moves to
 *     state/backup and returns when the mod that displaced it goes;
 *   - a deployed file that isn't the same inode as its source any more was
 *     replaced by a program, and is captured instead of removed;
 *   - an interrupted run leaves a journal, and the next run finishes from it. */
#ifndef FANMO_DEPLOY_H
#define FANMO_DEPLOY_H

#include "instance.h"
#include "util.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <vector>

namespace Deploy {

/* One mod's turn in priority order. */
struct Layer {
    QString id;            /* mods/ folder, or "<overwrite>" */
    QString path;          /* absolute */
    bool rootDeploy = false;
};

/* The layers an enabled profile adds up to, lowest first, overwrite last. */
std::vector<Layer> layersFor(const Instance &instance, const Profile &profile);

/* Who provides what. Keys are "<d|r>/<folded path>". */
struct FileIndex {
    struct Provider {
        int layer;
        QString relPath;
    };
    std::vector<Layer> layers;
    QHash<QString, std::vector<Provider>> providers; /* lowest priority first */

    struct ModConflicts {
        int winning = 0;   /* files where this mod beats another */
        int losing = 0;    /* files where another beats it */
        int total = 0;
        QStringList overriddenBy; /* layer ids */
        QStringList overrides;
    };
    std::vector<ModConflicts> summary;
};

FileIndex buildIndex(std::vector<Layer> layers);

/* Files of layer i with their state: what it wins, what it loses. */
struct FileConflict {
    QString relPath;
    QStringList others;   /* other providers, by id */
    bool winner = false;
};
std::vector<FileConflict> conflictsFor(const FileIndex &index, int layer);

struct Options {
    Util::Progress progress;
    Util::Logger log;
    const std::atomic_bool *cancel = nullptr;
};

struct Report {
    int linked = 0;
    int removed = 0;
    int unchanged = 0;
    int backedUp = 0;
    int restored = 0;
    int captured = 0;
    QStringList warnings;
};

/* Anything standing in the way of deploying to this game folder. Refuses a
 * folder Vortex has deployed to: its links would be displaced without Vortex
 * knowing, and both tools would then believe they own the same files. */
QString blocker(const Instance &instance);

bool deploy(Instance &instance, const FileIndex &index, const Options &options,
            Report *report, QString *error);

/* Removes everything fanMO deployed and restores what it displaced. */
bool purge(Instance &instance, const Options &options, Report *report, QString *error);

/* Files in Data that no manifest or the baseline accounts for: what the game
 * or a tool created since the last deployment. */
QStringList unmanagedFiles(const Instance &instance);

/* Moves those files into destDir, deepest paths first in case-folded form. */
int captureInto(const Instance &instance, const QStringList &files, const QString &destDir,
                QString *error);

} /* namespace Deploy */

#endif
