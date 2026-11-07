#include "archiveindex.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QThreadPool>
#include <QtConcurrent>

#include <dirent.h>
#include <sys/stat.h>

namespace ArchiveIndex {

namespace {

struct Candidate {
    QString path;
    qint64 size;
    qint64 mtime;
};

void collect(const QByteArray &dir, const QSet<qint64> &sizes, QList<Candidate> &out)
{
    DIR *d = opendir(dir.constData());
    if (!d)
        return;
    while (dirent *e = readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)))
            continue;
        const QByteArray child = dir + '/' + n;
        struct stat st;
        if (stat(child.constData(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            collect(child, sizes, out);
        else if (S_ISREG(st.st_mode) && sizes.contains(st.st_size))
            out.append({QFile::decodeName(child), qint64(st.st_size), qint64(st.st_mtime)});
    }
    closedir(d);
}

} /* namespace */

QHash<QString, QString> scan(const QStringList &folders, const QList<Wanted> &wanted,
                             const QString &cacheFile, const Util::Progress &progress,
                             const std::atomic_bool *cancel, Hash kind)
{
    QSet<qint64> sizes;
    QSet<QString> md5s;
    for (const Wanted &w : wanted) {
        sizes.insert(w.size);
        md5s.insert(w.hash);
    }

    QList<Candidate> candidates;
    for (const QString &folder : folders)
        collect(QFile::encodeName(folder), sizes, candidates);

    /* path -> {size, mtime, md5} from last time */
    QHash<QString, QJsonObject> cache;
    const QJsonDocument cached = Util::readJson(cacheFile);
    for (const auto &v : cached.array()) {
        const QJsonObject o = v.toObject();
        cache.insert(o.value(QLatin1String("path")).toString(), o);
    }

    QHash<QString, QString> found;
    QJsonArray fresh;
    QList<Candidate> toHash;
    for (const Candidate &c : std::as_const(candidates)) {
        const auto it = cache.constFind(c.path);
        if (it != cache.constEnd()
            && it->value(QLatin1String("size")).toInteger() == c.size
            && it->value(QLatin1String("mtime")).toInteger() == c.mtime) {
            const QString md5 = it->value(QLatin1String("md5")).toString();
            fresh.append(*it);
            if (md5s.contains(md5) && !found.contains(md5))
                found.insert(md5, c.path);
        } else {
            toHash.append(c);
        }
    }

    QMutex lock;
    qint64 done = 0;
    const qint64 total = toHash.size();
    QThreadPool pool;
    pool.setMaxThreadCount(qMax(2, QThread::idealThreadCount() / 2));
    QtConcurrent::blockingMap(&pool, toHash, [&](const Candidate &c) {
        const QString text = kind == Hash::Md5 ? QString::fromLatin1(Util::md5File(c.path, cancel))
                                               : Util::wabbajackHash(Util::xxh64File(c.path));
        QMutexLocker guard(&lock);
        ++done;
        if (progress)
            progress(done, total, QFileInfo(c.path).fileName());
        if (text.isEmpty())
            return;
        fresh.append(QJsonObject{{QStringLiteral("path"), c.path},
                                 {QStringLiteral("size"), c.size},
                                 {QStringLiteral("mtime"), c.mtime},
                                 {QStringLiteral("md5"), text}});
        if (md5s.contains(text) && !found.contains(text))
            found.insert(text, c.path);
    });
    Util::writeJson(cacheFile, QJsonDocument(fresh));
    return found;
}

} /* namespace ArchiveIndex */
