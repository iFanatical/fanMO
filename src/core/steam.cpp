#include "steam.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace Steam {

QString steamRoot()
{
    for (const QString &candidate : {QDir::homePath() + QStringLiteral("/.local/share/Steam"),
                                     QDir::homePath() + QStringLiteral("/.steam/steam"),
                                     QDir::homePath() + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam")})
        if (QFileInfo(candidate).isDir())
            return QFileInfo(candidate).canonicalFilePath();
    return {};
}

static QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QStringList libraries()
{
    const QString root = steamRoot();
    QStringList out;
    if (root.isEmpty())
        return out;
    out << root;
    static const QRegularExpression pathLine(QStringLiteral("\"path\"\\s+\"([^\"]+)\""));
    for (const QString &vdf : {root + QStringLiteral("/config/libraryfolders.vdf"),
                               root + QStringLiteral("/steamapps/libraryfolders.vdf")}) {
        auto matches = pathLine.globalMatch(readText(vdf));
        while (matches.hasNext()) {
            QString path = matches.next().captured(1);
            path.replace(QLatin1String("\\\\"), QLatin1String("\\"));
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (!canonical.isEmpty() && !out.contains(canonical))
                out << canonical;
        }
    }
    return out;
}

std::vector<Install> findSkyrim()
{
    std::vector<Install> out;
    static const QRegularExpression dirLine(QStringLiteral("\"installdir\"\\s+\"([^\"]+)\""));
    for (const QString &library : libraries()) {
        const QString manifest = library + QStringLiteral("/steamapps/appmanifest_%1.acf").arg(skyrimAppId);
        const auto match = dirLine.match(readText(manifest));
        if (!match.hasMatch())
            continue;
        const QString game = library + QStringLiteral("/steamapps/common/") + match.captured(1);
        if (!QFileInfo::exists(game + QStringLiteral("/SkyrimSE.exe")))
            continue;
        const QString compat = library + QStringLiteral("/steamapps/compatdata/%1").arg(skyrimAppId);
        out.push_back({game, QFileInfo(compat).isDir() ? compat : QString(), library});
    }
    return out;
}

std::vector<ProtonVersion> protonVersions()
{
    std::vector<ProtonVersion> custom, official;
    const QString root = steamRoot();
    if (root.isEmpty())
        return {};
    for (const QFileInfo &dir : QDir(root + QStringLiteral("/compatibilitytools.d"))
                                    .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo::exists(dir.filePath() + QStringLiteral("/proton")))
            custom.push_back({dir.fileName(), dir.filePath() + QStringLiteral("/proton")});
    for (const QString &library : libraries())
        for (const QFileInfo &dir : QDir(library + QStringLiteral("/steamapps/common"))
                                        .entryInfoList({QStringLiteral("Proton*")}, QDir::Dirs))
            if (QFileInfo::exists(dir.filePath() + QStringLiteral("/proton")))
                official.push_back({dir.fileName(), dir.filePath() + QStringLiteral("/proton")});
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(custom.begin(), custom.end(), [&](const auto &a, const auto &b) {
        return collator.compare(a.name, b.name) > 0;
    });
    custom.insert(custom.end(), official.begin(), official.end());
    return custom;
}

QString defaultProton()
{
    const auto versions = protonVersions();
    const QString config = readText(steamRoot() + QStringLiteral("/config/config.vdf"));
    /* CompatToolMapping: "489830" { "name" "GE-Proton11-1" ... } */
    const QRegularExpression entry(QStringLiteral("\"%1\"\\s*\\{\\s*\"name\"\\s+\"([^\"]*)\"").arg(skyrimAppId));
    const auto match = entry.match(config);
    if (match.hasMatch())
        for (const ProtonVersion &v : versions)
            if (v.name == match.captured(1))
                return v.path;
    return versions.empty() ? QString() : versions.front().path;
}

} /* namespace Steam */
