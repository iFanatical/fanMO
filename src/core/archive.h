/* Unpacking mod archives. 7-Zip does the work: it reads every format Nexus
 * serves (7z, zip, rar4, rar5) and has no library worth linking. */
#ifndef FANMO_ARCHIVE_H
#define FANMO_ARCHIVE_H

#include <QString>

namespace Archive {

/* The first of 7z, 7zz, 7za on PATH, or empty. */
QString sevenZip();

/* Unpacks into dest, which must not exist yet or be empty. The tree is then
 * sanitised: links dropped, permissions opened. A password-protected archive
 * fails rather than waiting on stdin. */
bool extract(const QString &archive, const QString &dest, QString *error);

} /* namespace Archive */

#endif
