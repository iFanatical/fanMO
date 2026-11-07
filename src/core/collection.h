/* A Nexus collection: what collection.json says, and the two orders that
 * can be worked out from it -- which mod wins a file, and plugin load order. */
#ifndef FANMO_COLLECTION_H
#define FANMO_COLLECTION_H

#include "fomod.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace Collection {

struct Source {
    QString type;        /* nexus, direct, browse, manual, bundle */
    qint64 modId = 0;
    qint64 fileId = 0;
    QString md5;
    qint64 size = 0;
    QString logicalFilename;
    QString fileExpression;
    QString url;
    QString updatePolicy;
    QString instructions;
};

struct Mod {
    QString name;
    QString version;
    bool optional = false;
    QString domain;
    Source source;
    QString author;
    QString category;
    QString type;        /* "dinput" deploys to the game root */
    int phase = 0;
    std::optional<Fomod::Preset> choices;
    QList<std::pair<QString, QString>> patches; /* path, source CRC */
    QStringList fileOverrides;
    QString instructions;
};

struct Reference {
    QString md5;
    QString fileExpression;
    QString logicalFilename;
    QString versionMatch;
};

struct Rule {
    QString type;
    Reference source;
    Reference reference;
};

struct Plugin {
    QString name;
    bool enabled = true;
};

struct PluginRule {
    QString name;
    QStringList after;
};

struct Manifest {
    QString name;
    QString author;
    QString installInstructions;
    QString domain;
    QStringList gameVersions;
    std::vector<Mod> mods;
    std::vector<Rule> rules;
    /* Which plugins are on. Not a load order: Vortex fills this by walking
     * mod folders, and leaves the order to LOOT plus pluginRules. */
    std::vector<Plugin> plugins;
    std::vector<PluginRule> pluginRules;
    /* The same rules as Vortex stored them: LOOT's userlist format. */
    QJsonObject pluginRulesJson;
    /* The unpacked bundle; bundled/, patches/ and INI Tweaks/ live under it. */
    QString rootDir;

    bool hasPatches() const;
};

bool load(const QString &collectionJson, Manifest *out, QString *error);

/* The mod a reference names, or -1. An MD5 is exact; the rest fall back the
 * way Vortex's testModReference does when a mod's file has moved on. */
int resolve(const Manifest &manifest, const Reference &ref);

struct Sorted {
    /* Mod indices, lowest priority first: a later mod's file wins. */
    std::vector<int> order;
    int unresolvedRules = 0;
    QStringList cycles;
};

/* Sorts `included` by the collection's before/after rules, with graphlib's
 * algorithm: the rules alone don't fix an order, and matching Vortex's walk
 * keeps ties falling the same way for any conflict the curator didn't rule on. */
Sorted sortMods(const Manifest &manifest, const std::vector<int> &included);

/* FOMOD's view of the finished collection: a plugin it enables is Active, one
 * it lists but leaves off is Inactive. */
Fomod::Environment environment(const Manifest &manifest, const QStringList &baseDataFiles);

} /* namespace Collection */

#endif
