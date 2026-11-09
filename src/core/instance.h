/* An instance is one managed game: its mods, downloads, profiles and
 * deployment state, in a folder that must share a filesystem with the game
 * so files can be hard-linked into it. */
#ifndef FANMO_INSTANCE_H
#define FANMO_INSTANCE_H

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

struct ModInfo {
    QString folder;     /* key; name of the folder in mods/ */
    QString name;
    QString version;
    QString category;
    QString author;
    qint64 nexusModId = 0;
    qint64 nexusFileId = 0;
    QString md5;
    QString archive;
    QString collection;
    bool rootDeploy = false;  /* files go to the game folder, not Data */
    QString choicesKey;       /* hash of the choices it was installed with */
    QJsonObject choices;      /* {options: [...]} when it came from a FOMOD */
    qint64 installed = 0;
};

struct ProfileMod {
    QString folder;
    bool enabled = true;
};

struct ProfilePlugin {
    QString name;
    bool enabled = true;
};

struct Profile {
    QString name;
    std::vector<ProfileMod> mods;       /* lowest priority first */
    std::vector<ProfilePlugin> plugins; /* load order */
    bool localSaves = false;
    bool localInis = false;
    QString collection;                 /* e.g. "Gate To Sovngarde rev 118" */
    QStringList iniTweaks;              /* files in the profile's folder */
    /* The order is only safe, not sorted: a collection needs LOOT, or an
     * order imported from a run of it, before it plays right. */
    bool needsSorting = false;
};

/* An external program run through Proton. */
struct Tool {
    QString name;
    QString exe;             /* absolute, or relative to the game folder */
    QString arguments;
    QString workingDir;      /* empty: the exe's own folder */
    QString outputMod;       /* new files in Data become this mod */
    bool syncPlugins = false; /* re-read the load order afterwards (LOOT) */
};

class Instance {
public:
    bool load(const QString &root, QString *error);
    bool save(QString *error) const;
    static bool create(const QString &root, const QString &gamePath, const QString &compatDataPath,
                       const QString &protonPath, QString *error);

    QString root() const { return m_root; }
    QString gamePath() const { return m_gamePath; }
    QString compatDataPath() const { return m_compatDataPath; }
    QString protonPath() const { return m_protonPath; }
    void setProtonPath(const QString &path) { m_protonPath = path; }

    QString dataPath() const { return m_gamePath + QStringLiteral("/Data"); }
    QString modsPath() const { return m_root + QStringLiteral("/mods"); }
    QString downloadsPath() const { return m_root + QStringLiteral("/downloads"); }
    QString overwritePath() const { return m_root + QStringLiteral("/overwrite"); }
    QString statePath() const { return m_root + QStringLiteral("/state"); }
    QString tempPath() const { return m_root + QStringLiteral("/tmp"); }
    QString profilesPath() const { return m_root + QStringLiteral("/profiles"); }
    QString collectionsPath() const { return m_root + QStringLiteral("/collections"); }

    /* The Windows side of the Proton prefix. */
    QString appDataPath() const;
    QString myGamesPath() const;

    /* Other folders searched for archives before downloading. Read only. */
    QStringList archiveFolders() const { return m_archiveFolders; }
    void setArchiveFolders(const QStringList &folders) { m_archiveFolders = folders; }

    QHash<QString, ModInfo> &mods() { return m_mods; }
    const QHash<QString, ModInfo> &mods() const { return m_mods; }
    std::vector<Tool> &tools() { return m_tools; }
    const std::vector<Tool> &tools() const { return m_tools; }

    QStringList profileNames() const;
    bool loadProfile(const QString &name, Profile *out, QString *error) const;
    bool saveProfile(const Profile &profile, QString *error) const;
    bool deleteProfile(const QString &name, QString *error);
    QString activeProfile() const { return m_activeProfile; }
    void setActiveProfile(const QString &name) { m_activeProfile = name; }

    QString profileDir(const QString &name) const;

    /* A folder in mods/ for a new mod, unique against what exists. */
    QString uniqueFolder(const QString &wanted) const;

private:
    QString m_root;
    QString m_gamePath;
    QString m_compatDataPath;
    QString m_protonPath;
    QString m_activeProfile;
    QStringList m_archiveFolders;
    QHash<QString, ModInfo> m_mods;
    std::vector<Tool> m_tools;
};

#endif
