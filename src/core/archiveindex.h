/* Finds archives by content. A collection names each file by MD5, so anything
 * already on disk -- from MO2, Vortex or an earlier run -- saves a download.
 * Only files whose size matches something wanted get hashed, and the results
 * are cached, so a second scan of 60 GB costs a directory walk. */
#ifndef FANMO_ARCHIVEINDEX_H
#define FANMO_ARCHIVEINDEX_H

#include "util.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace ArchiveIndex {

enum class Hash { Md5, Wabbajack };

struct Wanted {
    QString hash;
    qint64 size = 0;
};

/* Returns hash -> path for every wanted file found under folders. A cache
 * file holds one kind of hash, so give each kind its own. */
QHash<QString, QString> scan(const QStringList &folders, const QList<Wanted> &wanted,
                             const QString &cacheFile, const Util::Progress &progress = {},
                             const std::atomic_bool *cancel = nullptr, Hash kind = Hash::Md5);

} /* namespace ArchiveIndex */

#endif
