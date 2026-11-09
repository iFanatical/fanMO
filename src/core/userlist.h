/* LOOT's userlist.yaml, shared between you and fanMO.
 *
 * LOOT reads one userlist per game, holding your own rules. A collection's
 * rules have to go in the same file, so fanMO adds them next to yours and
 * records exactly what it added. The next deploy takes back only those, which
 * leaves your rules alone when a collection drops a rule or a profile has none.
 *
 * When you and a collection disagree on a value that can only have one answer,
 * such as a plugin's group, yours stays. */
#ifndef FANMO_USERLIST_H
#define FANMO_USERLIST_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class Instance;

namespace Userlist {

struct Merged {
    QByteArray text;
    bool changed = false;      /* false: the file already says this; leave its bytes alone */
    QJsonObject added;         /* what fanMO put in, and nothing that was already there */
    QStringList notes;         /* disagreements, for the deploy report */
};

/* current: the file as it stands (may be empty). rules: what should be in it,
 * in LOOT's format. previous: what earlier runs added, to take back first. */
bool merge(const QByteArray &current, const QJsonObject &rules, const QList<QJsonObject> &previous,
           Merged *out, QString *error);

/* Brings every LOOT's userlist up to date with this profile: the one in the
 * game's prefix, for Windows LOOT, and each native LOOT tool's. A native LOOT
 * that has none yet starts from a copy of the prefix's. A file that can't be
 * read safely stays untouched and comes back as an error; the others are
 * still brought up to date. */
bool sync(const Instance &instance, const QString &profile, QStringList *notes, QString *error);

} /* namespace Userlist */

#endif
