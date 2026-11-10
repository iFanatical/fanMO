#include "keystore.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>


namespace KeyStore {

static const QLatin1String format("fanmo-sealed-1");
/* Bound to the ciphertext, so the header can't be swapped onto another file. */
static const QByteArray aad("fanMO sealed secret v1");

/* OWASP's scrypt baseline: 128 MiB and a fraction of a second, once a session. */
static constexpr quint64 defaultN = 1u << 17;
static constexpr quint64 defaultR = 8;
static constexpr quint64 defaultP = 1;
/* What open() will spend on a file's own parameters, so a file can't ask for
 * gigabytes. */
static constexpr quint64 maxN = 1u << 20;
static constexpr quint64 maxMemory = 1100ull * 1024 * 1024;

namespace {
struct Cipher {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    ~Cipher() { EVP_CIPHER_CTX_free(ctx); }
};

/* The derived key, wiped however the function leaves. */
struct DerivedKey {
    unsigned char bytes[32];
    ~DerivedKey() { OPENSSL_cleanse(bytes, sizeof bytes); }
};
} // namespace

static bool derive(const QString &password, const QByteArray &salt, quint64 n, quint64 r, quint64 p,
                   DerivedKey *key, QString *error)
{
    QByteArray pass = password.toUtf8();
    const quint64 memory = 128 * n * r * p + 64 * 1024 * 1024;
    const int ok = EVP_PBE_scrypt(pass.constData(), size_t(pass.size()),
                                  reinterpret_cast<const unsigned char *>(salt.constData()), size_t(salt.size()),
                                  n, r, p, memory, key->bytes, sizeof key->bytes);
    OPENSSL_cleanse(pass.data(), size_t(pass.size()));
    if (ok != 1) {
        *error = QStringLiteral("couldn't derive a key from the password");
        return false;
    }
    return true;
}

QByteArray seal(const QString &secret, const QString &password, QString *error)
{
    QByteArray salt(16, '\0'), nonce(12, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), int(salt.size())) != 1
        || RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), int(nonce.size())) != 1) {
        *error = QStringLiteral("no random numbers to seal with");
        return {};
    }
    DerivedKey key;
    if (!derive(password, salt, defaultN, defaultR, defaultP, &key, error))
        return {};

    QByteArray plain = secret.toUtf8();
    QByteArray data(plain.size() + 16, '\0'), tag(16, '\0');
    Cipher c;
    int len = 0, total = 0;
    const bool ok = c.ctx
        && EVP_EncryptInit_ex(c.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_GCM_SET_IVLEN, int(nonce.size()), nullptr) == 1
        && EVP_EncryptInit_ex(c.ctx, nullptr, nullptr, key.bytes,
                              reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_EncryptUpdate(c.ctx, nullptr, &len, reinterpret_cast<const unsigned char *>(aad.constData()),
                             int(aad.size())) == 1
        && EVP_EncryptUpdate(c.ctx, reinterpret_cast<unsigned char *>(data.data()), &len,
                             reinterpret_cast<const unsigned char *>(plain.constData()), int(plain.size())) == 1
        && (total = len, EVP_EncryptFinal_ex(c.ctx, reinterpret_cast<unsigned char *>(data.data()) + total, &len) == 1)
        && EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_GCM_GET_TAG, int(tag.size()), tag.data()) == 1;
    OPENSSL_cleanse(plain.data(), size_t(plain.size()));
    if (!ok) {
        *error = QStringLiteral("couldn't encrypt the secret");
        return {};
    }
    data.resize(total + len);
    const QJsonObject o{{QStringLiteral("format"), format},
                        {QStringLiteral("kdf"), QStringLiteral("scrypt")},
                        {QStringLiteral("n"), qint64(defaultN)},
                        {QStringLiteral("r"), qint64(defaultR)},
                        {QStringLiteral("p"), qint64(defaultP)},
                        {QStringLiteral("salt"), QString::fromLatin1(salt.toBase64())},
                        {QStringLiteral("nonce"), QString::fromLatin1(nonce.toBase64())},
                        {QStringLiteral("data"), QString::fromLatin1(data.toBase64())},
                        {QStringLiteral("tag"), QString::fromLatin1(tag.toBase64())}};
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

std::optional<QString> open(const QByteArray &sealed, const QString &password, QString *error)
{
    const QJsonObject o = QJsonDocument::fromJson(sealed).object();
    if (o.value(QLatin1String("format")).toString() != format
        || o.value(QLatin1String("kdf")).toString() != QLatin1String("scrypt")) {
        *error = QStringLiteral("not a key fanMO sealed");
        return std::nullopt;
    }
    const quint64 n = quint64(o.value(QLatin1String("n")).toInteger());
    const quint64 r = quint64(o.value(QLatin1String("r")).toInteger());
    const quint64 p = quint64(o.value(QLatin1String("p")).toInteger());
    const QByteArray salt = QByteArray::fromBase64(o.value(QLatin1String("salt")).toString().toLatin1());
    const QByteArray nonce = QByteArray::fromBase64(o.value(QLatin1String("nonce")).toString().toLatin1());
    const QByteArray data = QByteArray::fromBase64(o.value(QLatin1String("data")).toString().toLatin1());
    const QByteArray tag = QByteArray::fromBase64(o.value(QLatin1String("tag")).toString().toLatin1());
    if (n < 2 || (n & (n - 1)) != 0 || n > maxN || r < 1 || r > 32 || p < 1 || p > 16
        || 128 * n * r * p > maxMemory || salt.size() < 16 || nonce.size() != 12 || tag.size() != 16) {
        *error = QStringLiteral("the sealed key is damaged");
        return std::nullopt;
    }
    DerivedKey key;
    if (!derive(password, salt, n, r, p, &key, error))
        return std::nullopt;

    QByteArray plain(data.size() + 16, '\0');
    Cipher c;
    int len = 0, total = 0;
    const bool ok = c.ctx
        && EVP_DecryptInit_ex(c.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_GCM_SET_IVLEN, int(nonce.size()), nullptr) == 1
        && EVP_DecryptInit_ex(c.ctx, nullptr, nullptr, key.bytes,
                              reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_DecryptUpdate(c.ctx, nullptr, &len, reinterpret_cast<const unsigned char *>(aad.constData()),
                             int(aad.size())) == 1
        && EVP_DecryptUpdate(c.ctx, reinterpret_cast<unsigned char *>(plain.data()), &len,
                             reinterpret_cast<const unsigned char *>(data.constData()), int(data.size())) == 1
        && (total = len, EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_GCM_SET_TAG, int(tag.size()),
                                             const_cast<char *>(tag.constData())) == 1)
        && EVP_DecryptFinal_ex(c.ctx, reinterpret_cast<unsigned char *>(plain.data()) + total, &len) == 1;
    if (!ok) {
        OPENSSL_cleanse(plain.data(), size_t(plain.size()));
        *error = QStringLiteral("wrong password");
        return std::nullopt;
    }
    const QString out = QString::fromUtf8(plain.constData(), total + len);
    OPENSSL_cleanse(plain.data(), size_t(plain.size()));
    return out;
}

} /* namespace KeyStore */
