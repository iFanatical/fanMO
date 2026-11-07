#include "fomod.h"

#include "util.h"

#include <QHash>
#include <QJsonObject>
#include <QStringDecoder>
#include <QVersionNumber>
#include <QXmlStreamReader>

#include <algorithm>

namespace Fomod {

namespace {

/* Just enough of a DOM: installers are small, and the conditions need the
 * tree. */
struct Node {
    QString name;
    QHash<QString, QString> attrs;
    std::vector<std::unique_ptr<Node>> children;
    QString text;

    const Node *child(const QString &n) const
    {
        for (const auto &c : children)
            if (c->name == n)
                return c.get();
        return nullptr;
    }
    QString attr(const char *n) const { return attrs.value(QLatin1String(n)); }
};

QString decodeXml(const QByteArray &bytes)
{
    if (bytes.startsWith("\xff\xfe"))
        return QStringDecoder(QStringDecoder::Utf16LE)(bytes.mid(2));
    if (bytes.startsWith("\xfe\xff"))
        return QStringDecoder(QStringDecoder::Utf16BE)(bytes.mid(2));
    if (bytes.startsWith("\xef\xbb\xbf"))
        return QString::fromUtf8(bytes.mid(3));
    /* UTF-16 with no BOM shows as NULs in every other byte of the ASCII "<?xml". */
    if (bytes.size() > 4 && bytes[1] == 0 && bytes[3] == 0)
        return QStringDecoder(QStringDecoder::Utf16LE)(bytes);
    if (bytes.size() > 4 && bytes[0] == 0 && bytes[2] == 0)
        return QStringDecoder(QStringDecoder::Utf16BE)(bytes);
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8(bytes);
    if (utf8.hasError())
        text = QString::fromLatin1(bytes);
    return text;
}

std::unique_ptr<Node> parseTree(const QByteArray &bytes, QString *error)
{
    QString text = decodeXml(bytes);
    /* The declaration's encoding is already dealt with, and QXmlStreamReader
     * rejects a QString whose declaration names anything it can't reconcile. */
    const qsizetype start = text.indexOf(QLatin1Char('<'));
    if (start > 0)
        text.remove(0, start);
    if (text.startsWith(QLatin1String("<?xml"))) {
        const qsizetype end = text.indexOf(QLatin1String("?>"));
        if (end > 0)
            text.remove(0, end + 2);
    }

    QXmlStreamReader xml(text);
    auto root = std::make_unique<Node>();
    std::vector<Node *> stack = {root.get()};
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            auto node = std::make_unique<Node>();
            node->name = xml.name().toString();
            for (const auto &a : xml.attributes())
                node->attrs.insert(a.name().toString(), a.value().toString());
            Node *raw = node.get();
            stack.back()->children.push_back(std::move(node));
            stack.push_back(raw);
            break;
        }
        case QXmlStreamReader::EndElement:
            stack.pop_back();
            break;
        case QXmlStreamReader::Characters:
            stack.back()->text += xml.text();
            break;
        default:
            break;
        }
    }
    if (xml.hasError()) {
        *error = QStringLiteral("line %1: %2").arg(xml.lineNumber()).arg(xml.errorString());
        return nullptr;
    }
    return root;
}

bool parseBool(const QString &s)
{
    return s == QLatin1String("true") || s == QLatin1String("1");
}

OptionType parseType(const QString &s)
{
    if (s == QLatin1String("Required"))
        return OptionType::Required;
    if (s == QLatin1String("Recommended"))
        return OptionType::Recommended;
    if (s == QLatin1String("NotUsable"))
        return OptionType::NotUsable;
    if (s == QLatin1String("CouldBeUsable"))
        return OptionType::CouldBeUsable;
    return OptionType::Optional;
}

GroupType parseGroupType(const QString &s)
{
    if (s == QLatin1String("SelectAtLeastOne"))
        return GroupType::AtLeastOne;
    if (s == QLatin1String("SelectAtMostOne"))
        return GroupType::AtMostOne;
    if (s == QLatin1String("SelectExactlyOne"))
        return GroupType::ExactlyOne;
    if (s == QLatin1String("SelectAll"))
        return GroupType::All;
    return GroupType::Any;
}

Condition parseCondition(const Node &n)
{
    Condition c;
    if (n.name == QLatin1String("flagDependency")) {
        c.kind = Condition::Flag;
        c.name = n.attr("flag");
        c.value = n.attr("value");
    } else if (n.name == QLatin1String("fileDependency")) {
        c.kind = Condition::File;
        c.name = n.attr("file");
        const QString state = n.attr("state");
        c.state = state == QLatin1String("Missing") ? FileState::Missing
                : state == QLatin1String("Inactive") ? FileState::Inactive
                                                     : FileState::Active;
    } else if (n.name == QLatin1String("gameDependency")) {
        c.kind = Condition::GameVersion;
        c.value = n.attr("version");
    } else if (n.name.endsWith(QLatin1String("Dependency"))) {
        /* fomm, fose, nvse, skse: versions of tools Vortex always reports
         * as present. */
        c.kind = Condition::OtherVersion;
    } else {
        c.kind = Condition::Composite;
        c.requireAll = n.attr("operator") != QLatin1String("Or");
        for (const auto &child : n.children)
            c.children.push_back(parseCondition(*child));
    }
    return c;
}

std::vector<FileItem> parseFiles(const Node *parent)
{
    std::vector<FileItem> out;
    if (!parent)
        return out;
    for (const auto &n : parent->children) {
        if (n->name != QLatin1String("file") && n->name != QLatin1String("folder"))
            continue;
        FileItem f;
        f.folder = n->name == QLatin1String("folder");
        f.source = n->attr("source");
        /* A missing destination means "same as source", an empty one means
         * the mod's root -- the attribute's presence is what differs. */
        f.destination = n->attrs.contains(QStringLiteral("destination")) ? n->attr("destination")
                                                                         : f.source;
        f.alwaysInstall = parseBool(n->attr("alwaysInstall"));
        f.installIfUsable = parseBool(n->attr("installIfUsable"));
        f.priority = n->attr("priority").toInt();
        out.push_back(std::move(f));
    }
    return out;
}

Option parseOption(const Node &n)
{
    Option o;
    o.name = n.attr("name");
    if (const Node *d = n.child(QStringLiteral("description")))
        o.description = d->text.trimmed();
    if (const Node *img = n.child(QStringLiteral("image")))
        o.image = img->attr("path");
    if (const Node *flags = n.child(QStringLiteral("conditionFlags")))
        for (const auto &f : flags->children)
            if (f->name == QLatin1String("flag"))
                o.flags.emplace_back(f->attr("name"), f->text);
    o.files = parseFiles(n.child(QStringLiteral("files")));
    if (const Node *desc = n.child(QStringLiteral("typeDescriptor"))) {
        if (const Node *t = desc->child(QStringLiteral("type"))) {
            o.defaultType = parseType(t->attr("name"));
        } else if (const Node *dep = desc->child(QStringLiteral("dependencyType"))) {
            if (const Node *dt = dep->child(QStringLiteral("defaultType")))
                o.defaultType = parseType(dt->attr("name"));
            if (const Node *patterns = dep->child(QStringLiteral("patterns")))
                for (const auto &p : patterns->children) {
                    TypePattern tp;
                    const Node *deps = p->child(QStringLiteral("dependencies"));
                    if (!deps)
                        deps = p->child(QStringLiteral("dependancies"));
                    if (deps)
                        tp.condition = parseCondition(*deps);
                    if (const Node *t = p->child(QStringLiteral("type")))
                        tp.type = parseType(t->attr("name"));
                    o.patterns.push_back(std::move(tp));
                }
        }
    }
    return o;
}

/* Nexus's parser treats a missing order attribute as Explicit (the schema
 * says Ascending). Which option a SelectExactlyOne falls back to depends on
 * this, so follow the parser. */
template <typename T, typename Name>
void applyOrder(std::vector<T> &items, const QString &order, Name name)
{
    const bool descending = order == QLatin1String("Descending");
    if (order != QLatin1String("Ascending") && !descending)
        return;
    std::stable_sort(items.begin(), items.end(), [&](const T &a, const T &b) {
        const int cmp = QString::compare(name(a), name(b), Qt::CaseInsensitive);
        return descending ? cmp > 0 : cmp < 0;
    });
}

std::vector<Group> parseGroups(const Node &parent)
{
    std::vector<Group> groups;
    for (const auto &g : parent.children) {
        if (g->name != QLatin1String("group"))
            continue;
        Group group;
        group.name = g->attr("name");
        group.type = parseGroupType(g->attr("type"));
        if (const Node *plugins = g->child(QStringLiteral("plugins"))) {
            for (const auto &p : plugins->children)
                if (p->name == QLatin1String("plugin"))
                    group.options.push_back(parseOption(*p));
            applyOrder(group.options, plugins->attr("order"),
                       [](const Option &o) { return o.name; });
        }
        groups.push_back(std::move(group));
    }
    applyOrder(groups, parent.attr("order"), [](const Group &g) { return g.name; });
    return groups;
}

} /* namespace */

bool parse(const QByteArray &xml, Script *out, QString *error)
{
    auto tree = parseTree(xml, error);
    if (!tree)
        return false;
    const Node *config = tree->child(QStringLiteral("config"));
    if (!config) {
        *error = QStringLiteral("no <config> element");
        return false;
    }
    Script s;
    if (const Node *n = config->child(QStringLiteral("moduleName")))
        s.moduleName = n->text.trimmed();
    if (const Node *n = config->child(QStringLiteral("moduleImage")))
        s.moduleImage = n->attr("path");

    const Node *deps = config->child(QStringLiteral("moduleDependencies"));
    if (!deps)
        deps = config->child(QStringLiteral("moduleDependancies"));
    if (deps) {
        s.hasPrerequisites = true;
        s.prerequisites = parseCondition(*deps);
    }
    s.requiredFiles = parseFiles(config->child(QStringLiteral("requiredInstallFiles")));

    if (const Node *steps = config->child(QStringLiteral("installSteps"))) {
        for (const auto &n : steps->children) {
            if (n->name != QLatin1String("installStep"))
                continue;
            Step step;
            step.name = n->attr("name");
            if (const Node *visible = n->child(QStringLiteral("visible"))) {
                step.hasVisibility = true;
                step.visibility = parseCondition(*visible);
            }
            if (const Node *groups = n->child(QStringLiteral("optionalFileGroups")))
                step.groups = parseGroups(*groups);
            s.steps.push_back(std::move(step));
        }
        applyOrder(s.steps, steps->attr("order"), [](const Step &st) { return st.name; });
    } else if (const Node *groups = config->child(QStringLiteral("optionalFileGroups"))) {
        /* Format 1.0: one implicit step. */
        Step step;
        step.groups = parseGroups(*groups);
        s.steps.push_back(std::move(step));
    }

    if (const Node *cond = config->child(QStringLiteral("conditionalFileInstalls")))
        if (const Node *patterns = cond->child(QStringLiteral("patterns")))
            for (const auto &p : patterns->children) {
                ConditionalFiles set;
                if (const Node *d = p->child(QStringLiteral("dependencies")))
                    set.condition = parseCondition(*d);
                set.files = parseFiles(p->child(QStringLiteral("files")));
                s.conditionalFiles.push_back(std::move(set));
            }
    *out = std::move(s);
    return true;
}

Preset presetFromJson(const QJsonArray &options)
{
    Preset preset;
    for (const auto &stepValue : options) {
        const QJsonObject stepObj = stepValue.toObject();
        PresetStep step;
        step.name = stepObj.value(QLatin1String("name")).toString();
        for (const auto &groupValue : stepObj.value(QLatin1String("groups")).toArray()) {
            const QJsonObject groupObj = groupValue.toObject();
            PresetGroup group;
            group.name = groupObj.value(QLatin1String("name")).toString();
            for (const auto &c : groupObj.value(QLatin1String("choices")).toArray()) {
                const QJsonObject choice = c.toObject();
                group.choices.push_back({choice.value(QLatin1String("name")).toString(),
                                         choice.value(QLatin1String("idx")).toInt()});
            }
            step.groups.push_back(std::move(group));
        }
        preset.push_back(std::move(step));
    }
    return preset;
}

QJsonArray presetToJson(const Preset &preset)
{
    QJsonArray steps;
    for (const PresetStep &step : preset) {
        QJsonArray groups;
        for (const PresetGroup &group : step.groups) {
            QJsonArray choices;
            for (const PresetChoice &c : group.choices)
                choices.append(QJsonObject{{QStringLiteral("name"), c.name},
                                           {QStringLiteral("idx"), c.idx}});
            groups.append(QJsonObject{{QStringLiteral("name"), group.name},
                                      {QStringLiteral("choices"), choices}});
        }
        steps.append(QJsonObject{{QStringLiteral("name"), step.name},
                                 {QStringLiteral("groups"), groups}});
    }
    return steps;
}

Session::Session(const Script &script, Environment env)
    : m_script(script), m_env(std::move(env))
{
    /* Two required options can't sit in a pick-one group; loosen the group
     * rather than fail. This reads conditions at their initial state. */
    for (Step &step : m_script.steps)
        for (Group &group : step.groups) {
            int required = 0;
            for (const Option &o : group.options)
                if (resolve(o) == OptionType::Required)
                    ++required;
            if (required > 1 && group.type == GroupType::AtMostOne)
                group.type = GroupType::Any;
            else if (required > 1 && group.type == GroupType::ExactlyOne)
                group.type = GroupType::AtLeastOne;
        }
}

bool Session::fulfilled(const Condition &c) const
{
    switch (c.kind) {
    case Condition::Composite: {
        if (c.children.empty())
            return true;
        for (const Condition &child : c.children) {
            const bool ok = fulfilled(child);
            if (c.requireAll && !ok)
                return false;
            if (!c.requireAll && ok)
                return true;
        }
        return c.requireAll;
    }
    case Condition::Flag: {
        const auto it = m_flags.find(c.name);
        const QString value = it == m_flags.end() ? QString() : it->second.value;
        /* An unset flag equals an empty value; anything else is exact. */
        if (c.value.isEmpty())
            return value.isEmpty();
        return it != m_flags.end() && value == c.value;
    }
    case Condition::File:
        return (m_env.fileState ? m_env.fileState(c.name) : FileState::Missing) == c.state;
    case Condition::GameVersion: {
        const QVersionNumber need = QVersionNumber::fromString(c.value);
        const QVersionNumber have = QVersionNumber::fromString(m_env.gameVersion);
        return QVersionNumber::compare(have, need) >= 0;
    }
    case Condition::OtherVersion:
        return true;
    }
    return true;
}

OptionType Session::resolve(const Option &o) const
{
    for (const TypePattern &p : o.patterns)
        if (fulfilled(p.condition))
            return p.type;
    return o.defaultType;
}

bool Session::prerequisitesMet() const
{
    return !m_script.hasPrerequisites || fulfilled(m_script.prerequisites);
}

bool Session::stepVisible(int step) const
{
    const Step &s = m_script.steps[size_t(step)];
    return !s.hasVisibility || fulfilled(s.visibility);
}

OptionType Session::optionType(int step, int group, int option) const
{
    if (m_forcedUsable.count({step, group, option}))
        return OptionType::CouldBeUsable;
    return resolve(m_script.steps[size_t(step)].groups[size_t(group)].options[size_t(option)]);
}

bool Session::isSelected(int step, int group, int option) const
{
    return m_selected.count({step, group, option}) > 0;
}

void Session::enable(const OptionId &id)
{
    m_selected.insert(id);
    const Option &o = m_script.steps[size_t(id.step)].groups[size_t(id.group)].options[size_t(id.option)];
    for (const auto &[name, value] : o.flags)
        m_flags[name] = {value, id};
}

void Session::disable(const OptionId &id)
{
    m_selected.erase(id);
    for (auto it = m_flags.begin(); it != m_flags.end();) {
        if (it->second.owner == id)
            it = m_flags.erase(it);
        else
            ++it;
    }
}

void Session::preselect(int stepIdx, const Preset *preset)
{
    const Step &step = m_script.steps[size_t(stepIdx)];
    for (size_t g = 0; g < step.groups.size(); ++g) {
        const Group &group = step.groups[g];

        /* Step and group names aren't unique, so every match counts. */
        std::vector<const PresetGroup *> groupPresets;
        if (preset)
            for (const PresetStep &ps : *preset)
                if (ps.name == step.name)
                    for (const PresetGroup &pg : ps.groups)
                        if (pg.name == group.name)
                            groupPresets.push_back(&pg);

        bool anySelected = false;
        for (size_t o = 0; o < group.options.size(); ++o)
            if (isSelected(stepIdx, int(g), int(o)))
                anySelected = true;
        if (anySelected)
            continue;

        bool setFirst = group.type == GroupType::ExactlyOne;
        for (size_t o = 0; o < group.options.size(); ++o) {
            const Option &option = group.options[o];
            const OptionId id{stepIdx, int(g), int(o)};
            OptionType type = optionType(stepIdx, int(g), int(o));
            bool isPreset = false;
            for (const PresetGroup *pg : groupPresets)
                for (const PresetChoice &c : pg->choices)
                    if (c.name == option.name)
                        isPreset = true;
            if (isPreset && type == OptionType::NotUsable) {
                /* A recorded choice beats the option's own availability. */
                m_forcedUsable.insert(id);
                type = OptionType::CouldBeUsable;
            }
            if (type == OptionType::Required
                || (!preset && type == OptionType::Recommended)
                || group.type == GroupType::All
                || isPreset) {
                if (!preset && type == OptionType::Recommended
                    && (group.type == GroupType::ExactlyOne || group.type == GroupType::AtMostOne))
                    for (size_t i = 0; i < group.options.size(); ++i)
                        disable({stepIdx, int(g), int(i)});
                setFirst = false;
                if (!groupPresets.empty() && !isPreset)
                    disable(id);
                else
                    enable(id);
            }
        }
        if (setFirst && !group.options.empty())
            enable({stepIdx, int(g), 0});
    }
}

void Session::fixSelected(int stepIdx)
{
    const Step &step = m_script.steps[size_t(stepIdx)];
    for (size_t g = 0; g < step.groups.size(); ++g)
        for (size_t o = 0; o < step.groups[g].options.size(); ++o) {
            const OptionType type = optionType(stepIdx, int(g), int(o));
            if (type == OptionType::Required)
                enable({stepIdx, int(g), int(o)});
            else if (type == OptionType::NotUsable)
                disable({stepIdx, int(g), int(o)});
        }
}

void Session::preselectDefaults(int step)
{
    preselect(step, nullptr);
}

void Session::select(int step, int group, const QList<int> &options)
{
    const Group &g = m_script.steps[size_t(step)].groups[size_t(group)];
    for (size_t o = 0; o < g.options.size(); ++o) {
        const OptionId id{step, group, int(o)};
        if (options.contains(int(o)) || optionType(step, group, int(o)) == OptionType::Required)
            enable(id);
        else
            disable(id);
    }
}

void Session::applyPreset(const Preset &preset)
{
    for (size_t s = 0; s < m_script.steps.size(); ++s) {
        if (!stepVisible(int(s)))
            continue;
        preselect(int(s), &preset);
        fixSelected(int(s));
    }
}

Preset Session::currentChoices() const
{
    Preset out;
    for (size_t s = 0; s < m_script.steps.size(); ++s) {
        if (!stepVisible(int(s)))
            continue;
        const Step &step = m_script.steps[s];
        PresetStep ps;
        ps.name = step.name;
        for (size_t g = 0; g < step.groups.size(); ++g) {
            PresetGroup pg;
            pg.name = step.groups[g].name;
            for (size_t o = 0; o < step.groups[g].options.size(); ++o)
                if (isSelected(int(s), int(g), int(o)))
                    pg.choices.push_back({step.groups[g].options[o].name, int(o)});
            ps.groups.push_back(std::move(pg));
        }
        out.push_back(std::move(ps));
    }
    return out;
}

namespace {

/* Builds the destination -> source map, with Nexus's collision rule. */
class Collector {
public:
    Collector(const QStringList &files, const QString &prefix, QStringList *warnings)
        : m_prefix(prefix), m_warnings(warnings)
    {
        m_folded.reserve(files.size());
        for (const QString &f : files)
            m_folded.push_back({Util::foldKey(f), f});
        std::sort(m_folded.begin(), m_folded.end());
        for (const auto &[key, name] : m_folded)
            m_exact.insert(key, name);
    }

    /* Note the asymmetry, kept from the original: a <folder> ignores the
     * offset that orders required, optional and conditional files. A folder
     * installed by requiredInstallFiles therefore competes on its raw
     * priority, and a mod relying on that gets the same answer here. */
    void install(const FileItem &item, int priorityOffset)
    {
        const QString source = Util::posixPath(m_prefix + item.source);
        if (item.folder) {
            const QString base = source.isEmpty() ? source : source + QLatin1Char('/');
            const QString key = Util::foldKey(base);
            QString to = Util::posixPath(item.destination);
            if (!to.isEmpty())
                to += QLatin1Char('/');
            auto it = std::lower_bound(m_folded.begin(), m_folded.end(),
                                       std::make_pair(key, QString()));
            for (; it != m_folded.end() && it->first.startsWith(key); ++it)
                add(it->second, to + it->second.mid(base.size()), item.priority);
            return;
        }
        const auto found = m_exact.constFind(Util::foldKey(source));
        if (found == m_exact.constEnd()) {
            if (m_warnings)
                *m_warnings << QStringLiteral("installer names a missing file: %1").arg(source);
            return;
        }
        /* A <file> whose destination ends in a separator makes that folder and
         * installs nothing. It reads like a typo for "put it in here", and
         * authors do write that, but what the curator tested is this. */
        if (item.destination.endsWith(QLatin1Char('/')) || item.destination.endsWith(QLatin1Char('\\')))
            return;
        QString to = Util::posixPath(item.destination);
        if (to.isEmpty())
            to = source.section(QLatin1Char('/'), -1);
        add(*found, to, item.priority + priorityOffset);
    }

    std::vector<Copy> take()
    {
        std::vector<Copy> out;
        out.reserve(m_copies.size());
        for (Copy &c : m_copies)
            out.push_back(std::move(c));
        return out;
    }

private:
    void add(const QString &from, const QString &to, int priority)
    {
        if (to.isEmpty())
            return;
        /* Exact, not case-folded, as in the original: "scripts/Source/x" and
         * "scripts/source/x" are two instructions, and whichever is copied
         * later wins once they reach a case-insensitive folder. */
        const auto idx = m_index.constFind(to);
        if (idx == m_index.constEnd()) {
            m_index.insert(to, m_copies.size());
            m_copies.push_back({from, to, priority});
            return;
        }
        Copy &old = m_copies[*idx];
        const bool update = priority != old.priority ? priority > old.priority
                                                     : from.compare(old.source) > 0;
        if (update)
            old = {from, to, priority};
    }

    QString m_prefix;
    QStringList *m_warnings;
    std::vector<std::pair<QString, QString>> m_folded;
    QHash<QString, QString> m_exact;
    QHash<QString, qsizetype> m_index;
    std::vector<Copy> m_copies;
};

} /* namespace */

std::vector<Copy> Session::instructions(const QStringList &files, const QString &prefix,
                                        QStringList *warnings) const
{
    /* The original's three phases, kept apart by a large priority offset. */
    constexpr int offset = 1000000000;
    Collector collector(files, prefix, warnings);

    for (const FileItem &f : m_script.requiredFiles)
        collector.install(f, -offset);

    for (size_t s = 0; s < m_script.steps.size(); ++s) {
        const Step &step = m_script.steps[s];
        for (size_t g = 0; g < step.groups.size(); ++g)
            for (size_t o = 0; o < step.groups[g].options.size(); ++o) {
                const Option &option = step.groups[g].options[o];
                const bool selected = isSelected(int(s), int(g), int(o));
                for (const FileItem &f : option.files) {
                    const bool usable = optionType(int(s), int(g), int(o)) != OptionType::NotUsable;
                    if (selected || f.alwaysInstall || (f.installIfUsable && usable))
                        collector.install(f, 0);
                }
            }
    }

    for (const ConditionalFiles &set : m_script.conditionalFiles)
        if (fulfilled(set.condition))
            for (const FileItem &f : set.files)
                collector.install(f, offset);

    return collector.take();
}

} /* namespace Fomod */
