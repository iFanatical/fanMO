/* Small helpers every part of the core uses: paths, hashes, JSON files. */
#ifndef FANMO_UTIL_H
#define FANMO_UTIL_H

#include <QJsonDocument>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

namespace Util {

/* Forward slashes, no leading "./" or "/", no doubled or trailing slashes. */
QString posixPath(const QString &path);

/* The key two paths share when Windows would treat them as the same file.
 * Skyrim and everything it loads are case-insensitive; ext4 is not. */
QString foldKey(const QString &path);

/* Every regular file under root, as paths relative to it, sorted. Symlinks
 * are not followed and not returned. */
QStringList walkFiles(const QString &root);

bool sameFilesystem(const QString &a, const QString &b);

/* Removes symlinks anywhere under root, makes every entry writable, and turns
 * names holding backslashes into real folders. Archives are untrusted: a link
 * must not let a later write land outside the extraction folder, and read-only
 * folders must not stop us clearing it. */
int sanitizeTree(const QString &root);

QByteArray md5File(const QString &path, const std::atomic_bool *cancel = nullptr);
quint32 crc32Bytes(const QByteArray &data);
QString crcHex(quint32 crc);
quint64 xxh64File(const QString &path);
quint64 xxh64Bytes(const QByteArray &data);
/* Wabbajack's text form of a hash: base64 of the little-endian bytes. */
QString wabbajackHash(quint64 hash);

QJsonDocument readJson(const QString &path, QString *error = nullptr);
bool writeJson(const QString &path, const QJsonDocument &doc, QString *error = nullptr);
bool writeFile(const QString &path, const QByteArray &data, QString *error = nullptr);

/* A name safe to use as one folder on ext4 and inside a Proton prefix. */
QString safeFolderName(const QString &name);

QString humanSize(qint64 bytes);

/* Reported from worker threads; `done` and `total` are items, not bytes. */
using Progress = std::function<void(qint64 done, qint64 total, const QString &what)>;
using Logger = std::function<void(const QString &line)>;

} /* namespace Util */

#endif
