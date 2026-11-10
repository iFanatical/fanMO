/* A secret kept under a password: AES-256-GCM with a key scrypt derives from
 * the password and a random salt. What it writes is JSON carrying the salt,
 * nonce, scrypt cost and tag, so a later version can raise the cost and still
 * open what an earlier one sealed. */
#ifndef FANMO_KEYSTORE_H
#define FANMO_KEYSTORE_H

#include <QByteArray>
#include <QString>

#include <optional>

namespace KeyStore {

QByteArray seal(const QString &secret, const QString &password, QString *error);

/* nullopt with *error set: a wrong password and a damaged file look the same,
 * which is the point of the tag. */
std::optional<QString> open(const QByteArray &sealed, const QString &password, QString *error);

} /* namespace KeyStore */

#endif
