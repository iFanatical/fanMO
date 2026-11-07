/* The Nexus Mods API: enough to fetch a collection and its files.
 *
 * Each call blocks the calling thread; the worker pool is where they run.
 * Use a client from one thread only. Limits are read from every response and
 * shared between clients, so a pool of downloaders stops together. */
#ifndef FANMO_NEXUS_H
#define FANMO_NEXUS_H

#include "util.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QString>
#include <QUrl>

#include <atomic>
#include <optional>

namespace Nexus {

/* Where the key lives: a 0600 file, read by nothing else. */
QString keyFile();
QString loadKey();
bool saveKey(const QString &key, QString *error);

struct NxmLink {
    enum Kind { Invalid, ModFile, Collection } kind = Invalid;
    QString domain;
    qint64 modId = 0, fileId = 0;
    QString key;
    QString expires;
    QString collectionSlug;
    int revision = 0;
};

/* nxm://skyrimspecialedition/mods/1/files/2?key=..&expires=..&user_id=..
 * nxm://skyrimspecialedition/collections/qdurkx/revisions/118 */
NxmLink parseNxm(const QString &url);

/* https://www.nexusmods.com/games/skyrimspecialedition/collections/qdurkx
 * as well, since that's what's in a browser's address bar. */
NxmLink parseCollectionUrl(const QString &url);

struct Account {
    QString name;
    bool premium = false;
};

struct CollectionInfo {
    QString name;
    QString slug;
    int revision = 0;
    int modCount = 0;
    QString downloadLink;
};

/* Remaining quota, shared process-wide. */
struct Limits {
    std::atomic_int daily{-1};
    std::atomic_int hourly{-1};
};
Limits &limits();

class Client {
public:
    explicit Client(QString apiKey);

    std::optional<Account> validate(QString *error);
    std::optional<CollectionInfo> collectionRevision(const QString &slug, int revision, QString *error);
    int latestRevision(const QString &slug, QString *error);
    /* CDN URLs. key/expires come from an nxm link; a Premium account doesn't
     * need them. A free account without them gets an empty list and
     * *premiumRequired set. */
    QStringList downloadLinks(const QString &domain, qint64 modId, qint64 fileId,
                              const QString &key, const QString &expires,
                              bool *premiumRequired, QString *error);
    QStringList collectionLinks(const QString &downloadLink, QString *error);

    /* Downloads to dest, resuming a partial dest.part. progress gets bytes. */
    bool download(const QString &url, const QString &dest, const Util::Progress &progress,
                  const std::atomic_bool *cancel, QString *error);

private:
    QNetworkRequest request(const QUrl &url) const;
    QByteArray get(const QUrl &url, int *status, QString *error);
    QJsonDocument graphql(const QByteArray &query, const QJsonObject &variables, QString *error);

    QString m_key;
    QNetworkAccessManager m_network;
};

} /* namespace Nexus */

#endif
