/* The two binary delta formats mod lists ship: BSDIFF40, which Vortex
 * collections use, and Octodiff, which Wabbajack uses. Both return the
 * patched bytes, or an empty array with *error set. */
#ifndef FANMO_PATCH_H
#define FANMO_PATCH_H

#include <QByteArray>
#include <QString>

namespace Patch {

QByteArray bsdiff(const QByteArray &source, const QByteArray &patch, QString *error);
QByteArray octodiff(const QByteArray &source, const QByteArray &delta, QString *error);

} /* namespace Patch */

#endif
