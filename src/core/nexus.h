/* The Nexus Mods API: enough to fetch a collection and its files.
 *
 * Each call blocks the calling thread; the worker pool is where they run.
 * Use a client from one thread only. Limits are read from every response and
 * shared between clients, so a pool of downloaders stops together. */
#ifndef FANMO_NEXUS_H
#define FANMO_NEXUS_H

#include "util.h"

#include <QHash>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QString>
#include <QUrl>

#include <atomic>
#include <optional>

namespace Nexus {

/* The API key at rest is sealed under a password of the user's
 * (KeyStore), in sealedKeyFile(). keyFile() is where 0.1 kept it in plain
 * text; it's read only to seal it, and removed once sealed. */
QString keyFile();
QString sealedKeyFile();
enum class KeyState { None, Plain, Sealed };
KeyState keyState();

/* The key for this session: empty until unlockKey() or usePlainKey(). Safe
 * from any thread. */
QString loadKey();
bool unlockKey(const QString &password, QString *error);
/* An unsealed 0.1 key, for this session only. */
bool usePlainKey();
/* Seals key under password, removes any plain copy, and unlocks it. */
bool saveKey(const QString &key, const QString &password, QString *error);
/* Removes the key from disk and from the session. */
void forgetKey();

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

/* What a downloaded archive is: kept beside it as "<archive>.meta.json" so
 * installing it later knows its Nexus mod, its name and its version. */
struct DownloadMeta {
    QString domain;
    qint64 modId = 0;
    qint64 fileId = 0;
    QString name;       /* the mod's, with the file's when that says more */
    QString version;
    QString author;
    QString category;
};
QString metaPath(const QString &archive);
std::optional<DownloadMeta> loadDownloadMeta(const QString &archive);
bool saveDownloadMeta(const QString &archive, const DownloadMeta &meta, QString *error);

/* Nexus names its downloads "<name>-<mod id>-<version>-<upload time>.<ext>",
 * the version's dots turned to dashes. Read back, that's a mod id, a name and
 * a version for an archive that came without its meta file. */
std::optional<DownloadMeta> guessFromFileName(const QString &fileName);

/* The mod's, file's and game's answers from the v1 API made into one. */
DownloadMeta parseFileDetails(const QByteArray &modJson, const QByteArray &fileJson, const QByteArray &gameJson);

struct Account {
    QString name;
    bool premium = false;
};

/* Where you stand on a mod. Undecided is what Nexus calls never having voted. */
enum class Endorsement { Undecided, Endorsed, Abstained };

/* Your endorsements in one game, by mod id, from /v1/user/endorsements.
 * Undecided ones are left out. */
QHash<qint64, Endorsement> parseEndorsements(const QByteArray &json, const QString &domain);

/* Kept between runs, so the list shows them before Nexus has answered.
 * load returns false when there's nothing saved yet: nothing is known. */
bool loadEndorsements(const QString &path, QHash<qint64, Endorsement> *out);
bool saveEndorsements(const QString &path, const QHash<qint64, Endorsement> &endorsements, QString *error);

/* Nexus's reason for refusing a vote, such as NOT_DOWNLOADED_MOD, in words. */
QString endorseRefusal(const QString &code);

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

    /* Name, author, version and category for one file of a mod. */
    std::optional<DownloadMeta> fileDetails(const QString &domain, qint64 modId, qint64 fileId, QString *error);

    std::optional<QHash<qint64, Endorsement>> endorsements(const QString &domain, QString *error);
    /* Endorses, or abstains when endorse is false; the answer is where you
     * stand now. Nexus wants the version you have. */
    std::optional<Endorsement> endorse(const QString &domain, qint64 modId, const QString &version,
                                       bool endorse, QString *error);

    /* Downloads to dest, resuming a partial dest.part. progress gets bytes. */
    bool download(const QString &url, const QString &dest, const Util::Progress &progress,
                  const std::atomic_bool *cancel, QString *error);

private:
    QNetworkRequest request(const QUrl &url) const;
    QByteArray get(const QUrl &url, int *status, QString *error);
    QByteArray post(const QUrl &url, const QByteArray &json, int *status, QString *error);
    QJsonDocument graphql(const QByteArray &query, const QJsonObject &variables, QString *error);

    QString m_key;
    QNetworkAccessManager m_network;
};

} /* namespace Nexus */

#endif
