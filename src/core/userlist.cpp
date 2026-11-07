#include "userlist.h"

#include "instance.h"
#include "plugins.h"
#include "util.h"
#include "yaml.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

using Yaml::Node;

namespace Userlist {

namespace {

const QString kName = QStringLiteral("name");
const QString kCreated = QStringLiteral("created");
const QString kGlobals = QStringLiteral("globals");
const QStringList kNamedSections{QStringLiteral("plugins"), QStringLiteral("groups")};

/* LOOT compares plugin names without regard to case. */
QString identity(const Node &node)
{
    if (node.kind == Node::Scalar)
        return Util::foldKey(node.text);
    const QJsonValue json = Yaml::toJson(node);
    const QJsonDocument doc = json.isArray() ? QJsonDocument(json.toArray()) : QJsonDocument(json.toObject());
    return Util::foldKey(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

bool holds(const Node &list, const Node &item)
{
    const QString key = identity(item);
    for (const Node &existing : list.items)
        if (identity(existing) == key)
            return true;
    return false;
}

void removeItems(Node &list, const QJsonArray &items)
{
    for (const QJsonValue &value : items) {
        const QString key = identity(Yaml::fromJson(value));
        for (auto it = list.items.begin(); it != list.items.end(); ++it)
            if (identity(*it) == key) {
                list.items.erase(it);
                break;
            }
    }
}

int findEntry(const Node &list, const QString &name)
{
    const QString key = Util::foldKey(name);
    for (size_t i = 0; i < list.items.size(); ++i) {
        const Node *entryName = list.items[i].get(kName);
        if (entryName && Util::foldKey(entryName->text) == key)
            return int(i);
    }
    return -1;
}

/* Anything stranger than this isn't a list fanMO should be rewriting. */
bool validate(Node &top, QString *error)
{
    if (top.kind != Node::Mapping) {
        *error = QStringLiteral("it isn't a mapping");
        return false;
    }
    QStringList sections = kNamedSections;
    sections << kGlobals;
    for (const QString &name : std::as_const(sections)) {
        Node *section = top.get(name);
        if (!section)
            continue;
        if (section->isNull()) {
            *section = Yaml::sequence();
            continue;
        }
        if (section->kind != Node::Sequence) {
            *error = QStringLiteral("\"%1\" isn't a list").arg(name);
            return false;
        }
    }
    for (const QString &name : kNamedSections) {
        const Node *section = top.get(name);
        if (!section)
            continue;
        for (const Node &entry : section->items) {
            const Node *entryName = entry.kind == Node::Mapping ? entry.get(kName) : nullptr;
            if (!entryName || entryName->kind != Node::Scalar) {
                *error = QStringLiteral("an entry in \"%1\" has no name").arg(name);
                return false;
            }
        }
    }
    return true;
}

void takeBack(Node &top, const QJsonObject &added)
{
    for (const QString &name : kNamedSections) {
        Node *section = top.get(name);
        if (!section)
            continue;
        const QJsonArray entries = added.value(name).toArray();
        for (const QJsonValue &value : entries) {
            const QJsonObject record = value.toObject();
            const int index = findEntry(*section, record.value(kName).toString());
            if (index < 0)
                continue;
            Node &entry = section->items[size_t(index)];
            for (auto it = record.begin(); it != record.end(); ++it) {
                if (it.key() == kName || it.key() == kCreated)
                    continue;
                Node *field = entry.get(it.key());
                if (!field)
                    continue;
                if (it.value().isArray()) {
                    if (field->kind == Node::Sequence) {
                        removeItems(*field, it.value().toArray());
                        if (field->items.empty())
                            entry.remove(it.key());
                    }
                } else if (identity(*field) == identity(Yaml::fromJson(it.value()))) {
                    entry.remove(it.key());
                }
            }
            if (record.value(kCreated).toBool() && entry.entries.size() == 1)
                section->items.erase(section->items.begin() + index);
        }
    }
    if (Node *globals = top.get(kGlobals))
        removeItems(*globals, added.value(kGlobals).toArray());
}

QString describe(const Node &node)
{
    return node.kind == Node::Scalar ? node.text : QStringLiteral("a different value");
}

/* Adds to one named entry and returns what was new, or an empty object. */
QJsonObject addEntry(Node &section, const QJsonObject &rule, bool keepBare, QStringList *notes)
{
    const QString name = rule.value(kName).toString();
    bool hasContent = false;
    for (auto it = rule.begin(); it != rule.end(); ++it)
        if (it.key() != kName && !it.value().isNull()
            && !(it.value().isArray() && it.value().toArray().isEmpty()))
            hasContent = true;
    if (name.isEmpty() || (!hasContent && !keepBare))
        return {};

    QJsonObject record{{kName, name}};
    int index = findEntry(section, name);
    if (index < 0) {
        Node entry = Yaml::mapping();
        entry.set(kName, Yaml::fromJson(name));
        section.items.push_back(std::move(entry));
        index = int(section.items.size()) - 1;
        record.insert(kCreated, true);
    }
    Node &entry = section.items[size_t(index)];
    for (auto it = rule.begin(); it != rule.end(); ++it) {
        if (it.key() == kName || it.value().isNull())
            continue;
        Node *field = entry.get(it.key());
        if (it.value().isArray()) {
            const QJsonArray items = it.value().toArray();
            if (items.isEmpty())
                continue;
            if (!field)
                field = &entry.set(it.key(), Yaml::sequence());
            if (field->kind != Node::Sequence) {
                *notes << QStringLiteral("%1: keeping your \"%2\"; the collection's rule for it wasn't added")
                              .arg(name, it.key());
                continue;
            }
            QJsonArray fresh;
            for (const QJsonValue &item : items) {
                Node node = Yaml::fromJson(item);
                if (!holds(*field, node)) {
                    field->items.push_back(std::move(node));
                    fresh.append(item);
                }
            }
            if (!fresh.isEmpty())
                record.insert(it.key(), fresh);
        } else {
            const Node wanted = Yaml::fromJson(it.value());
            if (!field) {
                entry.set(it.key(), wanted);
                record.insert(it.key(), it.value());
            } else if (identity(*field) != identity(wanted)) {
                *notes << QStringLiteral("%1: keeping your %2 \"%3\"; the collection sets \"%4\"")
                              .arg(name, it.key(), describe(*field), describe(wanted));
            }
        }
    }
    const int bookkeeping = record.contains(kCreated) ? 2 : 1;
    return record.size() > bookkeeping || record.contains(kCreated) ? record : QJsonObject();
}

Node &section(Node &top, const QString &name)
{
    Node *found = top.get(name);
    return found ? *found : top.set(name, Yaml::sequence());
}

QJsonObject add(Node &top, const QJsonObject &rules, QStringList *notes)
{
    QJsonObject added;
    for (const QString &name : kNamedSections) {
        const QJsonArray entries = rules.value(name).toArray();
        QJsonArray records;
        /* A group with no rules still has to exist for plugins to join it. */
        const bool keepBare = name == QLatin1String("groups");
        for (const QJsonValue &value : entries) {
            if (!value.isObject())
                continue;
            const QJsonObject record = addEntry(section(top, name), value.toObject(), keepBare, notes);
            if (!record.isEmpty())
                records.append(record);
        }
        if (!records.isEmpty())
            added.insert(name, records);
    }
    const QJsonArray globals = rules.value(kGlobals).toArray();
    QJsonArray fresh;
    for (const QJsonValue &value : globals) {
        Node &list = section(top, kGlobals);
        Node node = Yaml::fromJson(value);
        if (!holds(list, node)) {
            list.items.push_back(std::move(node));
            fresh.append(value);
        }
    }
    if (!fresh.isEmpty())
        added.insert(kGlobals, fresh);
    return added;
}

QJsonDocument recordOf(const QList<QJsonObject> &sets)
{
    QJsonArray array;
    for (const QJsonObject &set : sets)
        array.append(set);
    return QJsonDocument(QJsonObject{{QStringLiteral("sets"), array}});
}

} /* namespace */

bool merge(const QByteArray &current, const QJsonObject &rules, const QList<QJsonObject> &previous,
           Merged *out, QString *error)
{
    Node top;
    QString why;
    if (!Yaml::parse(current, &top, &why) || !validate(top, &why)) {
        *error = why;
        return false;
    }
    const bool blank = top.kind == Node::Mapping && top.entries.empty();
    const QByteArray before = Yaml::dump(top);

    for (const QJsonObject &set : previous)
        takeBack(top, set);
    Merged result;
    result.added = add(top, rules, &result.notes);
    if (blank && !top.entries.empty())
        section(top, kGlobals);

    result.text = Yaml::dump(top);
    result.changed = result.text != before;
    *out = std::move(result);
    return true;
}

bool sync(const Instance &instance, const QString &profile, QStringList *notes, QString *error)
{
    if (instance.compatDataPath().isEmpty())
        return true;

    const QString rulesFile = instance.profileDir(profile) + QStringLiteral("/userlist.yaml");
    QJsonObject rules;
    if (QFileInfo::exists(rulesFile)) {
        const QJsonDocument doc = Util::readJson(rulesFile, error);
        if (!doc.isObject())
            return false;
        rules = doc.object();
    }

    const QString recordFile = instance.statePath() + QStringLiteral("/loot-userlist.json");
    QList<QJsonObject> previous;
    if (QFileInfo::exists(recordFile)) {
        const QJsonDocument doc = Util::readJson(recordFile, error);
        if (!doc.isObject())
            return false;
        const QJsonArray sets = doc.object().value(QLatin1String("sets")).toArray();
        for (const QJsonValue &set : sets)
            previous << set.toObject();
    }
    if (rules.isEmpty() && previous.isEmpty())
        return true;

    const QString target = Plugins::lootUserlistPath(instance);
    QByteArray current;
    if (QFileInfo::exists(target)) {
        QFile file(target);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = file.errorString();
            return false;
        }
        current = file.readAll();
    }

    Merged merged;
    if (!merge(current, rules, previous, &merged, error))
        return false;
    *notes += merged.notes;

    /* Until the list is written, either set could be what's in it. */
    QList<QJsonObject> pending = previous;
    pending << merged.added;
    if (!Util::writeJson(recordFile, recordOf(pending), error))
        return false;
    if (merged.changed) {
        QDir().mkpath(QFileInfo(target).absolutePath());
        const QString backup = target + QStringLiteral(".fanmo-backup");
        if (QFileInfo::exists(target) && !QFileInfo::exists(backup))
            QFile::copy(target, backup);
        if (!Util::writeFile(target, merged.text, error))
            return false;
    }
    return Util::writeJson(recordFile, recordOf({merged.added}), error);
}

} /* namespace Userlist */
