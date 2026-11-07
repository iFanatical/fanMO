/* Enough YAML to edit LOOT's userlist without losing anything in it.
 *
 * Reads block and flow collections and every scalar style, and refuses what it
 * can't carry through a rewrite: anchors, aliases, merge keys, tags, several
 * documents. A refused file stays as it is, so being strict is the safe side. */
#ifndef FANMO_YAML_H
#define FANMO_YAML_H

#include <QByteArray>
#include <QJsonValue>
#include <QString>

#include <vector>

namespace Yaml {

struct Node {
    enum Kind { Scalar, Sequence, Mapping };
    struct Entry;

    Kind kind = Scalar;
    QString text;              /* Scalar */
    bool plain = true;         /* unquoted: hex numbers and the like stay as written */
    std::vector<Node> items;   /* Sequence */
    std::vector<Entry> entries; /* Mapping, in file order */

    bool isNull() const;
    Node *get(const QString &key);
    const Node *get(const QString &key) const;
    Node &set(const QString &key, Node value);
    void remove(const QString &key);
};

struct Node::Entry {
    QString key;
    Node value;
};

Node sequence();
Node mapping();

/* Blank input is an empty mapping. */
bool parse(const QByteArray &data, Node *out, QString *error);
QByteArray dump(const Node &root);

Node fromJson(const QJsonValue &value);
QJsonValue toJson(const Node &node);

} /* namespace Yaml */

#endif
