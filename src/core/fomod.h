/* FOMOD XML installers: parsing, the option/flag state machine, and turning
 * a set of choices into copy instructions.
 *
 * The behaviour follows Nexus's own installer (fomod-installer's XmlScript)
 * and not the schema where the two differ, because a collection's recorded
 * choices were produced by that code and have to replay identically. Those
 * places are noted where they occur. */
#ifndef FANMO_FOMOD_H
#define FANMO_FOMOD_H

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace Fomod {

enum class GroupType { AtLeastOne, AtMostOne, ExactlyOne, All, Any };
enum class OptionType { Required, Optional, Recommended, NotUsable, CouldBeUsable };
enum class FileState { Missing, Inactive, Active };

struct Condition {
    enum Kind { Composite, Flag, File, GameVersion, OtherVersion } kind = Composite;
    bool requireAll = true;          /* Composite: And rather than Or */
    std::vector<Condition> children; /* Composite */
    QString name;                    /* Flag: its name. File: the file. */
    QString value;                   /* Flag: its value. Version: minimum. */
    FileState state = FileState::Active; /* File */
};

struct FileItem {
    QString source;
    QString destination;
    bool folder = false;
    bool alwaysInstall = false;
    bool installIfUsable = false;
    int priority = 0;
};

struct TypePattern {
    Condition condition;
    OptionType type = OptionType::Optional;
};

struct Option {
    QString name;
    QString description;
    QString image;
    std::vector<std::pair<QString, QString>> flags;
    std::vector<FileItem> files;
    OptionType defaultType = OptionType::Optional;
    std::vector<TypePattern> patterns;
};

struct Group {
    QString name;
    GroupType type = GroupType::Any;
    std::vector<Option> options;
};

struct Step {
    QString name;
    bool hasVisibility = false;
    Condition visibility;
    std::vector<Group> groups;
};

struct ConditionalFiles {
    Condition condition;
    std::vector<FileItem> files;
};

struct Script {
    QString moduleName;
    QString moduleImage;
    bool hasPrerequisites = false;
    Condition prerequisites;
    std::vector<FileItem> requiredFiles;
    std::vector<Step> steps;
    std::vector<ConditionalFiles> conditionalFiles;
};

/* Decodes the bytes itself (BOM first, then the XML declaration is ignored)
 * because plenty of mods declare an encoding they aren't written in. */
bool parse(const QByteArray &xml, Script *out, QString *error);

/* What the installer asks of the outside world. */
struct Environment {
    std::function<FileState(const QString &file)> fileState;
    QString gameVersion = QStringLiteral("1.6.1170.0");
};

/* A curator's recorded answers: Vortex's choices.options. */
struct PresetChoice {
    QString name;
    int idx = 0;
};
struct PresetGroup {
    QString name;
    std::vector<PresetChoice> choices;
};
struct PresetStep {
    QString name;
    std::vector<PresetGroup> groups;
};
using Preset = std::vector<PresetStep>;

Preset presetFromJson(const QJsonArray &options);
QJsonArray presetToJson(const Preset &preset);

struct Copy {
    QString source;      /* relative to the extraction folder */
    QString destination; /* relative to the mod's root */
    int priority = 0;
};

struct OptionId {
    int step = 0, group = 0, option = 0;
    auto operator<=>(const OptionId &) const = default;
};

/* One run through an installer. The wizard and the unattended replay drive
 * the same state, so they can't disagree about what a choice installs. */
class Session {
public:
    Session(const Script &script, Environment env);

    const Script &script() const { return m_script; }
    bool prerequisitesMet() const;
    bool stepVisible(int step) const;
    OptionType optionType(int step, int group, int option) const;
    bool isSelected(int step, int group, int option) const;

    /* The defaults a person sees on opening a step. */
    void preselectDefaults(int step);
    /* Sets a group's selection; required options stay on. */
    void select(int step, int group, const QList<int> &options);
    /* Replays recorded choices across every visible step. */
    void applyPreset(const Preset &preset);

    /* files: everything in the extraction folder. prefix: the folder holding
     * fomod/, relative to it, empty or ending in '/'. */
    std::vector<Copy> instructions(const QStringList &files, const QString &prefix,
                                   QStringList *warnings) const;
    /* The current selection, in the shape applyPreset reads back. */
    Preset currentChoices() const;

private:
    bool fulfilled(const Condition &c) const;
    OptionType resolve(const Option &o) const;
    void enable(const OptionId &id);
    void disable(const OptionId &id);
    void preselect(int step, const Preset *preset);
    void fixSelected(int step);

    struct FlagValue {
        QString value;
        OptionId owner;
    };

    Script m_script;
    Environment m_env;
    std::set<OptionId> m_selected;
    std::map<QString, FlagValue> m_flags;
    std::set<OptionId> m_forcedUsable;
};

} /* namespace Fomod */

#endif
