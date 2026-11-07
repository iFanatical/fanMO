/* Finding Skyrim, its Proton prefix and a Proton to run things with. */
#ifndef FANMO_STEAM_H
#define FANMO_STEAM_H

#include <QString>
#include <QStringList>

#include <vector>

namespace Steam {

inline constexpr int skyrimAppId = 489830;

struct Install {
    QString gamePath;
    QString compatDataPath;  /* empty until the game has been run once */
    QString library;
};

QString steamRoot();
QStringList libraries();

/* Each library's Skyrim Special Edition, from its Steam manifest. */
std::vector<Install> findSkyrim();

struct ProtonVersion {
    QString name;
    QString path;  /* the `proton` script */
};

/* Custom tools and Steam's own, newest custom first. */
std::vector<ProtonVersion> protonVersions();

/* The Proton Steam is set to use for Skyrim, or the first we can find. */
QString defaultProton();

} /* namespace Steam */

#endif
