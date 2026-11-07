/* Plugin files: reading their headers, and the load order files the game and
 * tools like LOOT share inside the Proton prefix. */
#ifndef FANMO_PLUGINS_H
#define FANMO_PLUGINS_H

#include "deploy.h"
#include "instance.h"

#include <QString>
#include <QStringList>

namespace Plugins {

struct Header {
    bool valid = false;
    bool master = false;
    bool light = false;
    QStringList masters;
    QString author;
    QString description;
};

Header readHeader(const QString &path);

bool isPluginName(const QString &name);

/* The game's plugins, always first and in this order. */
QStringList baseMasters();

/* Everything that loads whether Plugins.txt says so or not, in load order:
 * the game's masters, then Skyrim.ccc's list. */
QStringList earlyLoaders(const Instance &instance);

struct Available {
    QString name;
    QString path;     /* the winning file */
    QString provider; /* layer id, or empty for a file in Data */
};

/* Every plugin the winning files provide, plus those already in Data. */
std::vector<Available> available(const Instance &instance, const Deploy::FileIndex &index);

/* Brings a profile's plugin list up to date with what exists: gone plugins
 * drop out, new ones join the end, enabled, and early loaders go first.
 * Returns the number changed. */
int reconcile(const Instance &instance, Profile *profile, const std::vector<Available> &present);

struct OrderRule {
    QString plugin;
    QStringList after;
};

/* An order the game can load, not one that's right: early loaders, then
 * ESM-flagged plugins, each after its masters and anything a rule names,
 * otherwise as listed. Sorting properly takes LOOT's masterlist. */
void sortSafely(const Instance &instance, Profile *profile, const std::vector<Available> &present,
                const std::vector<OrderRule> &rules);

/* Reorders to match `reference` (names, best first), as far as it goes:
 * plugins it doesn't know keep their place in line. Returns how many it knew. */
int adoptOrder(Profile *profile, const QStringList &reference);

/* A loadorder.txt or Plugins.txt as plain names. */
QStringList readOrderFile(const QString &path, QString *error);

/* Where LOOT looks for its user rules inside the prefix. */
QString lootUserlistPath(const Instance &instance);

/* Writes Plugins.txt and loadorder.txt. The first write keeps a copy of the
 * files that were there as *.fanmo-backup. */
bool write(const Instance &instance, const Profile &profile, QString *error);

/* Reads the load order back into the profile, after LOOT or the like. */
bool read(const Instance &instance, Profile *profile, QString *error);

} /* namespace Plugins */

#endif
