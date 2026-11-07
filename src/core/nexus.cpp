#include "nexus.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

namespace Nexus {

static const char *const apiBase = "https://api.nexusmods.com";
static const char *const appVersion = "0.1.0";

QString keyFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/fanMO/nexus.key");
}

QString loadKey()
{
    QFile f(keyFile());
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

bool saveKey(const QString &key, QString *error)
{
    if (!Util::writeFile(keyFile(), key.trimmed().toUtf8(), error))
        return false;
    QFile::setPermissions(keyFile(), QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

NxmLink parseNxm(const QString &text)
{
    NxmLink link;
    const QUrl url(text);
    if (url.scheme().compare(QLatin1String("nxm"), Qt::CaseInsensitive) != 0)
        return link;
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    link.domain = url.host().toLower();
    if (parts.size() >= 4 && parts[0] == QLatin1String("mods") && parts[2] == QLatin1String("files")) {
        link.kind = NxmLink::ModFile;
        link.modId = parts[1].toLongLong();
        link.fileId = parts[3].toLongLong();
        const QUrlQuery query(url);
        link.key = query.queryItemValue(QStringLiteral("key"));
        link.expires = query.queryItemValue(QStringLiteral("expires"));
    } else if (parts.size() >= 4 && parts[0] == QLatin1String("collections")
               && parts[2] == QLatin1String("revisions")) {
        link.kind = NxmLink::Collection;
        link.collectionSlug = parts[1];
        link.revision = parts[3].toInt();
    }
    return link;
}

NxmLink parseCollectionUrl(const QString &text)
{
    NxmLink link = parseNxm(text);
    if (link.kind != NxmLink::Invalid)
        return link;
    static const QRegularExpression re(
        QStringLiteral("/games/([^/]+)/collections/([a-z0-9]+)(?:/revisions/(\\d+))?"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(text);
    if (m.hasMatch()) {
        link.kind = NxmLink::Collection;
        link.domain = m.captured(1).toLower();
        link.collectionSlug = m.captured(2);
        link.revision = m.captured(3).toInt();
    }
    return link;
}

Limits &limits()
{
    static Limits l;
    return l;
}

Client::Client(QString apiKey) : m_key(std::move(apiKey))
{
}

QNetworkRequest Client::request(const QUrl &url) const
{
    QNetworkRequest r(url);
    r.setRawHeader("APIKEY", m_key.toUtf8());
    r.setRawHeader("Application-Name", "fanMO");
    r.setRawHeader("Application-Version", appVersion);
    r.setRawHeader("User-Agent", QByteArray("fanMO/") + appVersion);
    r.setRawHeader("Accept", "application/json");
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return r;
}

static void recordLimits(QNetworkReply *reply)
{
    const QByteArray daily = reply->rawHeader("x-rl-daily-remaining");
    const QByteArray hourly = reply->rawHeader("x-rl-hourly-remaining");
    if (!daily.isEmpty())
        limits().daily = daily.toInt();
    if (!hourly.isEmpty())
        limits().hourly = hourly.toInt();
}

static void waitFor(QNetworkReply *reply, int timeoutMs)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    if (!reply->isFinished())
        loop.exec();
}

QByteArray Client::get(const QUrl &url, int *status, QString *error)
{
    /* A 429 means slow down, not stop: the quota refills as it runs. */
    for (int attempt = 0; attempt < 5; ++attempt) {
        QNetworkReply *reply = m_network.get(request(url));
        waitFor(reply, 30000);
        recordLimits(reply);
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QString message = reply->errorString();
        const bool failed = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (*status == 429) {
            QThread::sleep(1u << attempt);
            continue;
        }
        if (failed && *status == 0) {
            *error = message;
            return {};
        }
        return body;
    }
    *error = QStringLiteral("Nexus rate limit reached (%1 left today)").arg(limits().daily.load());
    return {};
}

std::optional<Account> Client::validate(QString *error)
{
    int status = 0;
    const QByteArray body = get(QUrl(QString::fromLatin1(apiBase) + QStringLiteral("/v1/users/validate.json")),
                                &status, error);
    if (status == 401) {
        *error = QStringLiteral("Nexus rejected this API key");
        return std::nullopt;
    }
    if (status != 200) {
        if (error->isEmpty())
            *error = QStringLiteral("Nexus answered %1").arg(status);
        return std::nullopt;
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    Account a;
    a.name = o.value(QLatin1String("name")).toString();
    a.premium = o.value(QLatin1String("is_premium")).toBool();
    return a;
}

QJsonDocument Client::graphql(const QByteArray &query, const QJsonObject &variables, QString *error)
{
    QNetworkRequest r = request(QUrl(QString::fromLatin1(apiBase) + QStringLiteral("/v2/graphql")));
    r.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    const QByteArray payload = QJsonDocument(QJsonObject{
        {QStringLiteral("query"), QString::fromUtf8(query)},
        {QStringLiteral("variables"), variables}}).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network.post(r, payload);
    waitFor(reply, 30000);
    recordLimits(reply);
    const QByteArray body = reply->readAll();
    const bool failed = reply->error() != QNetworkReply::NoError;
    const QString message = reply->errorString();
    reply->deleteLater();
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    const QJsonArray errors = doc.object().value(QLatin1String("errors")).toArray();
    if (!errors.isEmpty()) {
        *error = errors.first().toObject().value(QLatin1String("message")).toString();
        return {};
    }
    if (failed && !doc.isObject()) {
        *error = message;
        return {};
    }
    return doc;
}

std::optional<CollectionInfo> Client::collectionRevision(const QString &slug, int revision, QString *error)
{
    static const QByteArray query =
        "query collectionRevision($slug: String, $revision: Int, $viewAdultContent: Boolean) {"
        " collectionRevision(slug: $slug, revision: $revision, viewAdultContent: $viewAdultContent) {"
        " revisionNumber modCount downloadLink collection { name slug } } }";
    const QJsonDocument doc = graphql(query,
        {{QStringLiteral("slug"), slug}, {QStringLiteral("revision"), revision},
         {QStringLiteral("viewAdultContent"), true}}, error);
    const QJsonObject o = doc.object().value(QLatin1String("data")).toObject()
                              .value(QLatin1String("collectionRevision")).toObject();
    if (o.isEmpty()) {
        if (error->isEmpty())
            *error = QStringLiteral("Nexus has no revision %1 of %2").arg(revision).arg(slug);
        return std::nullopt;
    }
    CollectionInfo info;
    info.slug = slug;
    info.name = o.value(QLatin1String("collection")).toObject().value(QLatin1String("name")).toString();
    info.revision = o.value(QLatin1String("revisionNumber")).toInt();
    info.modCount = o.value(QLatin1String("modCount")).toInt();
    info.downloadLink = o.value(QLatin1String("downloadLink")).toString();
    return info;
}

int Client::latestRevision(const QString &slug, QString *error)
{
    static const QByteArray query =
        "query collection($slug: String, $viewAdultContent: Boolean) {"
        " collection(slug: $slug, viewAdultContent: $viewAdultContent) {"
        " latestPublishedRevision { revisionNumber } } }";
    const QJsonDocument doc = graphql(query, {{QStringLiteral("slug"), slug},
                                              {QStringLiteral("viewAdultContent"), true}}, error);
    return doc.object().value(QLatin1String("data")).toObject()
        .value(QLatin1String("collection")).toObject()
        .value(QLatin1String("latestPublishedRevision")).toObject()
        .value(QLatin1String("revisionNumber")).toInt();
}

static QStringList linksFrom(const QJsonDocument &doc)
{
    QStringList out;
    const QJsonArray list = doc.isArray() ? doc.array()
                                          : doc.object().value(QLatin1String("download_links")).toArray();
    for (const auto &v : list) {
        const QString uri = v.toObject().value(QLatin1String("URI")).toString();
        if (!uri.isEmpty())
            out << uri;
    }
    return out;
}

QStringList Client::downloadLinks(const QString &domain, qint64 modId, qint64 fileId,
                                  const QString &key, const QString &expires,
                                  bool *premiumRequired, QString *error)
{
    QUrl url(QStringLiteral("%1/v1/games/%2/mods/%3/files/%4/download_link.json")
                 .arg(QString::fromLatin1(apiBase), domain).arg(modId).arg(fileId));
    if (!key.isEmpty()) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("key"), key);
        query.addQueryItem(QStringLiteral("expires"), expires);
        url.setQuery(query);
    }
    int status = 0;
    const QByteArray body = get(url, &status, error);
    if (premiumRequired)
        *premiumRequired = status == 403;
    if (status == 403)
        return {};
    if (status != 200) {
        if (error->isEmpty())
            *error = QStringLiteral("Nexus answered %1 for file %2").arg(status).arg(fileId);
        return {};
    }
    return linksFrom(QJsonDocument::fromJson(body));
}

QStringList Client::collectionLinks(const QString &downloadLink, QString *error)
{
    int status = 0;
    const QByteArray body = get(QUrl(QString::fromLatin1(apiBase) + downloadLink), &status, error);
    if (status != 200) {
        if (error->isEmpty())
            *error = QStringLiteral("Nexus answered %1 for the collection archive").arg(status);
        return {};
    }
    return linksFrom(QJsonDocument::fromJson(body));
}

bool Client::download(const QString &urlText, const QString &dest, const Util::Progress &progress,
                      const std::atomic_bool *cancel, QString *error)
{
    const QString partial = dest + QStringLiteral(".part");
    QFile out(partial);
    const qint64 resumeFrom = QFileInfo(partial).size();
    if (!out.open(QIODevice::WriteOnly | QIODevice::Append)) {
        *error = out.errorString();
        return false;
    }

    /* The API key must not travel to a CDN or to anyone else. */
    QNetworkRequest r{QUrl(urlText)};
    r.setRawHeader("User-Agent", QByteArray("fanMO/") + appVersion);
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (resumeFrom > 0)
        r.setRawHeader("Range", "bytes=" + QByteArray::number(resumeFrom) + "-");

    QNetworkReply *reply = m_network.get(r);
    QEventLoop loop;
    QTimer stall;
    stall.setSingleShot(true);
    QTimer cancelPoll;
    bool cancelled = false;
    QObject::connect(&stall, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(&cancelPoll, &QTimer::timeout, reply, [&] {
        if (cancel && cancel->load()) {
            cancelled = true;
            reply->abort();
        }
    });
    qint64 written = resumeFrom;
    bool truncatedForRestart = false;
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&] {
        stall.start(120000);
        /* The server ignored the Range: start the file over. */
        if (resumeFrom > 0 && !truncatedForRestart
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            out.resize(0);
            out.seek(0);
            written = 0;
            truncatedForRestart = true;
        }
        const QByteArray chunk = reply->readAll();
        out.write(chunk);
        written += chunk.size();
        if (progress) {
            const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
            progress(written, length.toLongLong() + (truncatedForRestart ? 0 : resumeFrom), QString());
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    stall.start(120000);
    cancelPoll.start(500);
    if (!reply->isFinished())
        loop.exec();
    const QByteArray rest = reply->readAll();
    out.write(rest);
    out.close();
    const auto failure = reply->error();
    const QString message = reply->errorString();
    reply->deleteLater();
    if (cancelled) {
        *error = QStringLiteral("cancelled");
        return false;
    }
    if (failure != QNetworkReply::NoError) {
        *error = message;
        return false;
    }
    QFile::remove(dest);
    if (!QFile::rename(partial, dest)) {
        *error = QStringLiteral("cannot finish %1").arg(dest);
        return false;
    }
    return true;
}

} /* namespace Nexus */
