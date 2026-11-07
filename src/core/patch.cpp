#include "patch.h"

#include <bzlib.h>
#include <cstring>

namespace Patch {

/* bsdiff's offsets: sign in the top bit, not two's complement. */
static qint64 readOffset(const unsigned char *b)
{
    quint64 y = 0;
    for (int i = 7; i >= 0; --i)
        y = (y << 8) | b[i];
    const bool negative = y & (quint64(1) << 63);
    y &= ~(quint64(1) << 63);
    return negative ? -qint64(y) : qint64(y);
}

static bool bunzip(const char *data, qint64 size, QByteArray &out, QString *error)
{
    bz_stream s;
    std::memset(&s, 0, sizeof s);
    if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) {
        *error = QStringLiteral("bzip2 init failed");
        return false;
    }
    s.next_in = const_cast<char *>(data);
    s.avail_in = unsigned(size);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    int rc;
    do {
        s.next_out = buf.data();
        s.avail_out = unsigned(buf.size());
        rc = BZ2_bzDecompress(&s);
        if (rc != BZ_OK && rc != BZ_STREAM_END) {
            BZ2_bzDecompressEnd(&s);
            *error = QStringLiteral("corrupt bzip2 stream in patch");
            return false;
        }
        out.append(buf.constData(), buf.size() - qsizetype(s.avail_out));
    } while (rc != BZ_STREAM_END && (s.avail_in > 0 || s.avail_out == 0));
    BZ2_bzDecompressEnd(&s);
    return true;
}

QByteArray bsdiff(const QByteArray &source, const QByteArray &patch, QString *error)
{
    if (patch.size() < 32 || std::memcmp(patch.constData(), "BSDIFF40", 8) != 0) {
        *error = QStringLiteral("not a BSDIFF40 patch");
        return {};
    }
    const auto *header = reinterpret_cast<const unsigned char *>(patch.constData());
    const qint64 ctrlLen = readOffset(header + 8);
    const qint64 diffLen = readOffset(header + 16);
    const qint64 newSize = readOffset(header + 24);
    if (ctrlLen < 0 || diffLen < 0 || newSize < 0 || 32 + ctrlLen + diffLen > patch.size()) {
        *error = QStringLiteral("corrupt bsdiff header");
        return {};
    }
    QByteArray ctrl, diff, extra;
    const char *base = patch.constData() + 32;
    if (!bunzip(base, ctrlLen, ctrl, error)
        || !bunzip(base + ctrlLen, diffLen, diff, error)
        || !bunzip(base + ctrlLen + diffLen, patch.size() - 32 - ctrlLen - diffLen, extra, error))
        return {};

    QByteArray out(newSize, Qt::Uninitialized);
    qint64 oldPos = 0, newPos = 0, ctrlPos = 0, diffPos = 0, extraPos = 0;
    while (newPos < newSize) {
        if (ctrlPos + 24 > ctrl.size()) {
            *error = QStringLiteral("bsdiff control stream ended early");
            return {};
        }
        const auto *c = reinterpret_cast<const unsigned char *>(ctrl.constData()) + ctrlPos;
        const qint64 add = readOffset(c), copy = readOffset(c + 8), seek = readOffset(c + 16);
        ctrlPos += 24;
        if (add < 0 || copy < 0 || newPos + add > newSize || diffPos + add > diff.size()) {
            *error = QStringLiteral("corrupt bsdiff control data");
            return {};
        }
        for (qint64 i = 0; i < add; ++i) {
            char byte = diff[diffPos + i];
            if (oldPos + i >= 0 && oldPos + i < source.size())
                byte = char(byte + source[oldPos + i]);
            out[newPos + i] = byte;
        }
        diffPos += add;
        newPos += add;
        oldPos += add;
        if (newPos + copy > newSize || extraPos + copy > extra.size()) {
            *error = QStringLiteral("corrupt bsdiff extra data");
            return {};
        }
        std::memcpy(out.data() + newPos, extra.constData() + extraPos, size_t(copy));
        extraPos += copy;
        newPos += copy;
        oldPos += seek;
    }
    return out;
}

QByteArray octodiff(const QByteArray &source, const QByteArray &delta, QString *error)
{
    static const char magic[] = "OCTODELTA";
    if (delta.size() < 12 || std::memcmp(delta.constData(), magic, 9) != 0) {
        *error = QStringLiteral("not an Octodiff delta");
        return {};
    }
    qint64 pos = 9;
    const auto u8 = [&]() -> int { return pos < delta.size() ? quint8(delta[pos++]) : -1; };
    const auto i64 = [&](bool *ok) -> qint64 {
        if (pos + 8 > delta.size()) {
            *ok = false;
            return 0;
        }
        quint64 v = 0;
        for (int i = 7; i >= 0; --i)
            v = (v << 8) | quint8(delta[pos + i]);
        pos += 8;
        return qint64(v);
    };
    /* A .NET BinaryWriter string: one 7-bit length byte is enough for "SHA1". */
    if (u8() != 1) {
        *error = QStringLiteral("unknown Octodiff version");
        return {};
    }
    const int nameLen = u8();
    if (nameLen < 0 || nameLen > 64) {
        *error = QStringLiteral("corrupt Octodiff header");
        return {};
    }
    pos += nameLen;
    bool ok = true;
    const auto readInt32 = [&]() -> qint64 {
        if (pos + 4 > delta.size()) {
            ok = false;
            return 0;
        }
        quint32 v = 0;
        for (int i = 3; i >= 0; --i)
            v = (v << 8) | quint8(delta[pos + i]);
        pos += 4;
        return v;
    };
    const qint64 hashLen = readInt32();
    if (!ok || hashLen < 0 || pos + hashLen + 3 > delta.size()) {
        *error = QStringLiteral("corrupt Octodiff header");
        return {};
    }
    pos += hashLen;
    if (std::memcmp(delta.constData() + pos, ">>>", 3) != 0) {
        *error = QStringLiteral("Octodiff end-of-metadata marker missing");
        return {};
    }
    pos += 3;

    QByteArray out;
    while (pos < delta.size()) {
        const int command = u8();
        if (command == 0x60) { /* copy from the original */
            const qint64 offset = i64(&ok), length = i64(&ok);
            if (!ok || offset < 0 || length < 0 || offset + length > source.size()) {
                *error = QStringLiteral("Octodiff copy out of range");
                return {};
            }
            out.append(source.constData() + offset, length);
        } else if (command == 0x80) { /* literal data */
            const qint64 length = i64(&ok);
            if (!ok || length < 0 || pos + length > delta.size()) {
                *error = QStringLiteral("Octodiff literal out of range");
                return {};
            }
            out.append(delta.constData() + pos, length);
            pos += length;
        } else {
            *error = QStringLiteral("unknown Octodiff command 0x%1").arg(command, 2, 16, QLatin1Char('0'));
            return {};
        }
    }
    return out;
}

} /* namespace Patch */
