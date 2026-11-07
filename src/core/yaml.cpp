#include "yaml.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringDecoder>
#include <QStringList>

namespace Yaml {

bool Node::isNull() const
{
    return kind == Scalar && plain && (text.isEmpty() || text == QLatin1String("~")
                                       || text == QLatin1String("null"));
}

Node *Node::get(const QString &key)
{
    for (Entry &e : entries)
        if (e.key == key)
            return &e.value;
    return nullptr;
}

const Node *Node::get(const QString &key) const
{
    return const_cast<Node *>(this)->get(key);
}

Node &Node::set(const QString &key, Node value)
{
    if (Node *existing = get(key)) {
        *existing = std::move(value);
        return *existing;
    }
    entries.push_back({key, std::move(value)});
    return entries.back().value;
}

void Node::remove(const QString &key)
{
    for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it->key == key) {
            entries.erase(it);
            return;
        }
}

Node sequence()
{
    Node n;
    n.kind = Node::Sequence;
    return n;
}

Node mapping()
{
    Node n;
    n.kind = Node::Mapping;
    return n;
}

namespace {

struct Failure {
    QString message;
};

class Parser {
public:
    explicit Parser(const QString &text)
    {
        const QStringList raw = text.split(QLatin1Char('\n'));
        lines.reserve(size_t(raw.size()));
        for (qsizetype n = 0; n < raw.size(); ++n) {
            Line line;
            line.number = int(n) + 1;
            line.raw = raw[n];
            if (line.raw.endsWith(QLatin1Char('\r')))
                line.raw.chop(1);
            while (line.indent < line.raw.size() && line.raw[line.indent] == QLatin1Char(' '))
                ++line.indent;
            line.text = line.raw.mid(line.indent);
            while (!line.text.isEmpty() && line.text.back().isSpace())
                line.text.chop(1);
            lines.push_back(std::move(line));
        }
    }

    Node document()
    {
        skip();
        started = true;
        if (at >= lines.size())
            return mapping();
        Node root = block(0);
        skip();
        if (at < lines.size())
            fail(QStringLiteral("unexpected content"));
        return root;
    }

private:
    struct Line {
        int number = 0;
        int indent = 0;
        QString raw;
        QString text;
    };

    [[noreturn]] void fail(const QString &what) const
    {
        const int number = at < lines.size() ? lines[at].number : int(lines.size());
        throw Failure{QStringLiteral("line %1: %2").arg(number).arg(what)};
    }

    bool blank(size_t n) const { return lines[n].text.isEmpty(); }

    /* Past blank lines, comments and document markers. */
    void skip()
    {
        while (at < lines.size()) {
            const Line &line = lines[at];
            if (line.text.isEmpty() || line.text[0] == QLatin1Char('#')) {
                ++at;
                continue;
            }
            if (line.text[0] == QLatin1Char('\t'))
                fail(QStringLiteral("tab used for indentation"));
            if (line.indent == 0 && line.text[0] == QLatin1Char('%'))
                fail(QStringLiteral("directives aren't supported"));
            if (line.indent == 0 && line.text == QLatin1String("---")) {
                if (started || seenMarker)
                    fail(QStringLiteral("several documents in one file"));
                seenMarker = true;
                ++at;
                continue;
            }
            if (line.indent == 0 && line.text.startsWith(QLatin1String("--- ")))
                fail(QStringLiteral("content on a document marker"));
            if (line.indent == 0 && line.text == QLatin1String("...")) {
                ++at;
                skip();
                if (at < lines.size())
                    fail(QStringLiteral("several documents in one file"));
                return;
            }
            return;
        }
    }

    static bool isItem(const QString &text)
    {
        return text == QLatin1String("-") || text.startsWith(QLatin1String("- "));
    }

    /* Where the colon ending a mapping key sits, or -1. */
    static int keyEnd(const QString &text)
    {
        if (text.isEmpty() || text[0] == QLatin1Char('[') || text[0] == QLatin1Char('{'))
            return -1;
        qsizetype pos = 0;
        if (text[0] == QLatin1Char('"') || text[0] == QLatin1Char('\'')) {
            const QChar quote = text[0];
            pos = 1;
            for (;; ++pos) {
                if (pos >= text.size())
                    return -1;
                if (quote == QLatin1Char('"') && text[pos] == QLatin1Char('\\'))
                    ++pos;
                else if (quote == QLatin1Char('\'') && text[pos] == quote
                         && pos + 1 < text.size() && text[pos + 1] == quote)
                    ++pos;
                else if (text[pos] == quote)
                    break;
            }
            ++pos;
            while (pos < text.size() && text[pos] == QLatin1Char(' '))
                ++pos;
            if (pos < text.size() && text[pos] == QLatin1Char(':')
                && (pos + 1 == text.size() || text[pos + 1] == QLatin1Char(' ')))
                return int(pos);
            return -1;
        }
        for (; pos < text.size(); ++pos) {
            if (text[pos] == QLatin1Char('#') && pos > 0 && text[pos - 1] == QLatin1Char(' '))
                return -1;
            if (text[pos] == QLatin1Char(':')
                && (pos + 1 == text.size() || text[pos + 1] == QLatin1Char(' ')))
                return int(pos);
        }
        return -1;
    }

    static QString withoutComment(const QString &text)
    {
        if (text.startsWith(QLatin1Char('#')))
            return QString();
        const qsizetype hash = text.indexOf(QLatin1String(" #"));
        return (hash < 0 ? text : text.left(hash)).trimmed();
    }

    void refuseIndicator(const QString &text) const
    {
        if (text.isEmpty())
            return;
        const QChar c = text[0];
        if (c == QLatin1Char('&') || c == QLatin1Char('*') || c == QLatin1Char('!'))
            fail(QStringLiteral("anchors, aliases and tags aren't supported"));
        if (c == QLatin1Char('%') || c == QLatin1Char('@') || c == QLatin1Char('`')
            || text == QLatin1String("?") || text.startsWith(QLatin1String("? ")))
            fail(QStringLiteral("unsupported syntax"));
    }

    Node block(int minimum)
    {
        if (++depth > 64)
            fail(QStringLiteral("nested too deeply"));
        skip();
        Node node;
        if (at < lines.size() && lines[at].indent >= minimum) {
            const Line &line = lines[at];
            if (isItem(line.text))
                node = readSequence(line.indent);
            else if (keyEnd(line.text) >= 0)
                node = readMapping(line.indent);
            else
                node = value(line.text, minimum - 1);
        }
        --depth;
        return node;
    }

    Node readSequence(int indent)
    {
        Node node = sequence();
        for (;;) {
            skip();
            if (at >= lines.size())
                break;
            Line &line = lines[at];
            if (line.indent < indent || (line.indent == indent && !isItem(line.text)))
                break;
            if (line.indent > indent)
                fail(QStringLiteral("bad indentation"));
            int offset = 1;
            while (offset < line.text.size() && line.text[offset] == QLatin1Char(' '))
                ++offset;
            const QString rest = line.text.mid(offset);
            if (rest.isEmpty() || rest[0] == QLatin1Char('#')) {
                ++at;
                skip();
                node.items.push_back(at < lines.size() && lines[at].indent > indent ? block(indent + 1) : Node());
            } else if (isItem(rest) || keyEnd(rest) >= 0) {
                /* "- name: x" opens a collection on the dash's own line. */
                line.text = rest;
                line.indent = indent + offset;
                node.items.push_back(block(indent + 1));
            } else {
                node.items.push_back(value(rest, indent));
            }
        }
        return node;
    }

    Node readMapping(int indent)
    {
        Node node = mapping();
        for (;;) {
            skip();
            if (at >= lines.size())
                break;
            const Line &line = lines[at];
            if (line.indent < indent)
                break;
            if (line.indent > indent)
                fail(QStringLiteral("bad indentation"));
            const int colon = keyEnd(line.text);
            if (colon < 0)
                fail(QStringLiteral("expected a key"));
            const QString key = readKey(line.text.left(colon).trimmed());
            if (node.get(key))
                fail(QStringLiteral("key \"%1\" appears twice").arg(key));
            const QString rest = withoutCommentAfterColon(line.text.mid(colon + 1).trimmed());
            if (rest.isEmpty()) {
                ++at;
                skip();
                if (at < lines.size() && lines[at].indent > indent)
                    node.entries.push_back({key, block(indent + 1)});
                else if (at < lines.size() && lines[at].indent == indent && isItem(lines[at].text))
                    node.entries.push_back({key, readSequence(indent)});
                else
                    node.entries.push_back({key, Node()});
            } else {
                node.entries.push_back({key, value(rest, indent)});
            }
        }
        return node;
    }

    static QString withoutCommentAfterColon(const QString &text)
    {
        return text.startsWith(QLatin1Char('#')) ? QString() : text;
    }

    QString readKey(const QString &text)
    {
        if (text == QLatin1String("<<"))
            fail(QStringLiteral("merge keys aren't supported"));
        refuseIndicator(text);
        if (text.startsWith(QLatin1Char('"')) || text.startsWith(QLatin1Char('\''))) {
            QString buffer = text;
            qsizetype pos = 0;
            return quoted(buffer, pos);
        }
        return text;
    }

    /* A value that starts on the current line, which it consumes. */
    Node value(const QString &text, int parent)
    {
        const QChar c = text[0];
        if (c == QLatin1Char('|') || c == QLatin1Char('>'))
            return blockScalar(text, parent);
        if (c == QLatin1Char('[') || c == QLatin1Char('{')) {
            QString buffer = text;
            qsizetype pos = 0;
            Node node = flow(buffer, pos);
            finishLine(buffer.mid(pos));
            return node;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            QString buffer = text;
            qsizetype pos = 0;
            Node node;
            node.plain = false;
            node.text = quoted(buffer, pos);
            finishLine(buffer.mid(pos));
            return node;
        }
        if (isItem(text))
            fail(QStringLiteral("unexpected sequence"));
        refuseIndicator(text);
        return plainScalar(text, parent);
    }

    void finishLine(const QString &rest)
    {
        const QString tail = rest.trimmed();
        if (!tail.isEmpty() && !tail.startsWith(QLatin1Char('#')))
            fail(QStringLiteral("unexpected text after a value"));
        ++at;
    }

    Node plainScalar(const QString &text, int parent)
    {
        Node node;
        node.text = withoutComment(text);
        const auto refuseMapping = [this](const QString &part) {
            if (part.contains(QLatin1String(": ")) || part.endsWith(QLatin1Char(':')))
                fail(QStringLiteral("a value can't hold \": \" without quotes"));
        };
        refuseMapping(node.text);
        const bool commented = node.text.size() < text.trimmed().size();
        ++at;
        if (commented)
            return node;
        /* Continuation lines, deeper than whatever owns this value, fold. */
        int blanks = 0;
        while (at < lines.size()) {
            if (blank(at)) {
                ++blanks;
                ++at;
                continue;
            }
            const Line &line = lines[at];
            if (line.indent <= parent || line.text[0] == QLatin1Char('#'))
                break;
            const QString part = withoutComment(line.text);
            refuseMapping(part);
            node.text += blanks ? QString(blanks, QLatin1Char('\n')) : QStringLiteral(" ");
            node.text += part;
            blanks = 0;
            const bool ended = part.size() < line.text.size();
            ++at;
            if (ended)
                break;
        }
        return node;
    }

    /* Leaves pos just past the closing quote; a scalar may run over lines,
     * in which case buffer becomes the line it ended on. */
    QString quoted(QString &buffer, qsizetype &pos)
    {
        const QChar quote = buffer[pos];
        const bool isDouble = quote == QLatin1Char('"');
        ++pos;
        QString out;
        qsizetype kept = 0;      /* escapes aren't trailing space to trim */
        bool joined = false;
        for (;;) {
            if (pos >= buffer.size()) {
                if (!joined)
                    while (out.size() > kept && (out.back() == QLatin1Char(' ') || out.back() == QLatin1Char('\t')))
                        out.chop(1);
                ++at;
                int blanks = 0;
                while (at < lines.size() && blank(at)) {
                    ++blanks;
                    ++at;
                }
                if (at >= lines.size())
                    fail(QStringLiteral("unterminated string"));
                buffer = lines[at].text;
                while (!buffer.isEmpty() && buffer[0].isSpace())
                    buffer.remove(0, 1);
                pos = 0;
                if (blanks)
                    out += QString(blanks, QLatin1Char('\n'));
                else if (!joined)
                    out += QLatin1Char(' ');
                joined = false;
                continue;
            }
            const QChar c = buffer[pos];
            if (c == quote) {
                if (!isDouble && pos + 1 < buffer.size() && buffer[pos + 1] == quote) {
                    out += quote;
                    pos += 2;
                    continue;
                }
                ++pos;
                return out;
            }
            if (isDouble && c == QLatin1Char('\\')) {
                ++pos;
                if (pos >= buffer.size()) {
                    joined = true;
                    continue;
                }
                escape(buffer, pos, &out);
                kept = out.size();
                continue;
            }
            out += c;
            ++pos;
        }
    }

    void escape(const QString &buffer, qsizetype &pos, QString *out)
    {
        const char c = buffer[pos].toLatin1();
        ++pos;
        int digits = 0;
        switch (c) {
        case '0': *out += QChar(0); return;
        case 'a': *out += QChar(7); return;
        case 'b': *out += QChar(8); return;
        case 't': case '\t': *out += QChar(9); return;
        case 'n': *out += QChar(10); return;
        case 'v': *out += QChar(11); return;
        case 'f': *out += QChar(12); return;
        case 'r': *out += QChar(13); return;
        case 'e': *out += QChar(27); return;
        case ' ': *out += QLatin1Char(' '); return;
        case '"': *out += QLatin1Char('"'); return;
        case '/': *out += QLatin1Char('/'); return;
        case '\\': *out += QLatin1Char('\\'); return;
        case 'N': *out += QChar(0x85); return;
        case '_': *out += QChar(0xA0); return;
        case 'L': *out += QChar(0x2028); return;
        case 'P': *out += QChar(0x2029); return;
        case 'x': digits = 2; break;
        case 'u': digits = 4; break;
        case 'U': digits = 8; break;
        default: fail(QStringLiteral("unknown escape"));
        }
        bool ok = false;
        const uint code = buffer.mid(pos, digits).toUInt(&ok, 16);
        if (!ok || pos + digits > buffer.size() || code > 0x10FFFF)
            fail(QStringLiteral("bad escape"));
        pos += digits;
        *out += QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
    }

    Node blockScalar(const QString &header, int parent)
    {
        const bool folded = header[0] == QLatin1Char('>');
        QChar chomp;
        int declared = 0;
        const QString options = withoutComment(header.mid(1));
        for (const QChar c : options) {
            if (c == QLatin1Char('+') || c == QLatin1Char('-'))
                chomp = c;
            else if (c.isDigit() && c != QLatin1Char('0'))
                declared = c.digitValue();
            else
                fail(QStringLiteral("bad block scalar header"));
        }
        ++at;
        int indent = declared ? qMax(parent, 0) + declared : -1;
        QStringList body;
        for (; at < lines.size(); ++at) {
            const Line &line = lines[at];
            if (line.text.isEmpty()) {
                body << QString();
                continue;
            }
            if (indent < 0) {
                if (line.indent <= parent)
                    break;
                indent = line.indent;
            }
            if (line.indent < indent)
                break;
            body << line.raw.mid(indent);
        }
        int trailing = 0;
        while (!body.isEmpty() && body.back().isEmpty()) {
            body.removeLast();
            ++trailing;
        }

        Node node;
        node.plain = false;
        if (!folded) {
            node.text = body.join(QLatin1Char('\n'));
        } else {
            int breaks = 0;
            bool first = true, previousDeeper = false;
            for (const QString &line : std::as_const(body)) {
                if (line.isEmpty()) {
                    ++breaks;
                    continue;
                }
                const bool deeper = line[0] == QLatin1Char(' ') || line[0] == QLatin1Char('\t');
                if (first)
                    node.text = line;
                else if (deeper || previousDeeper)
                    node.text += QString(breaks + 1, QLatin1Char('\n')) + line;
                else if (breaks)
                    node.text += QString(breaks, QLatin1Char('\n')) + line;
                else
                    node.text += QLatin1Char(' ') + line;
                breaks = 0;
                first = false;
                previousDeeper = deeper;
            }
        }
        if (chomp == QLatin1Char('+'))
            node.text += QString(trailing + (body.isEmpty() ? 0 : 1), QLatin1Char('\n'));
        else if (chomp.isNull() && !body.isEmpty())
            node.text += QLatin1Char('\n');
        return node;
    }

    /* Next token of a flow collection, pulling in lines as they're needed. */
    void flowSkip(QString &buffer, qsizetype &pos)
    {
        for (;;) {
            while (pos < buffer.size() && buffer[pos].isSpace())
                ++pos;
            const bool comment = pos < buffer.size() && buffer[pos] == QLatin1Char('#')
                && (pos == 0 || buffer[pos - 1].isSpace());
            if (pos < buffer.size() && !comment)
                return;
            ++at;
            if (at >= lines.size())
                fail(QStringLiteral("unterminated flow collection"));
            buffer = lines[at].raw;
            pos = 0;
        }
    }

    QString flowPlain(const QString &buffer, qsizetype &pos)
    {
        const qsizetype start = pos;
        for (; pos < buffer.size(); ++pos) {
            const QChar c = buffer[pos];
            if (c == QLatin1Char(',') || c == QLatin1Char('[') || c == QLatin1Char(']')
                || c == QLatin1Char('{') || c == QLatin1Char('}'))
                break;
            if (c == QLatin1Char(':') && (pos + 1 == buffer.size() || buffer[pos + 1].isSpace()
                                          || buffer[pos + 1] == QLatin1Char(',')
                                          || buffer[pos + 1] == QLatin1Char(']')
                                          || buffer[pos + 1] == QLatin1Char('}')))
                break;
            if (c == QLatin1Char('#') && pos > start && buffer[pos - 1].isSpace())
                break;
        }
        const QString text = buffer.mid(start, pos - start).trimmed();
        if (text.isEmpty())
            fail(QStringLiteral("empty value in a flow collection"));
        refuseIndicator(text);
        if (text == QLatin1String("<<"))
            fail(QStringLiteral("merge keys aren't supported"));
        return text;
    }

    Node flow(QString &buffer, qsizetype &pos)
    {
        if (++depth > 64)
            fail(QStringLiteral("nested too deeply"));
        flowSkip(buffer, pos);
        Node node;
        const QChar c = buffer[pos];
        if (c == QLatin1Char('[')) {
            node = sequence();
            ++pos;
            for (;;) {
                flowSkip(buffer, pos);
                if (buffer[pos] == QLatin1Char(']'))
                    break;
                node.items.push_back(flow(buffer, pos));
                flowSkip(buffer, pos);
                if (buffer[pos] == QLatin1Char(','))
                    ++pos;
                else if (buffer[pos] != QLatin1Char(']'))
                    fail(QStringLiteral("expected , or ]"));
            }
            ++pos;
        } else if (c == QLatin1Char('{')) {
            node = mapping();
            ++pos;
            for (;;) {
                flowSkip(buffer, pos);
                if (buffer[pos] == QLatin1Char('}'))
                    break;
                const bool isQuoted = buffer[pos] == QLatin1Char('"') || buffer[pos] == QLatin1Char('\'');
                const QString key = isQuoted ? quoted(buffer, pos) : flowPlain(buffer, pos);
                if (node.get(key))
                    fail(QStringLiteral("key \"%1\" appears twice").arg(key));
                flowSkip(buffer, pos);
                if (buffer[pos] == QLatin1Char(':')) {
                    ++pos;
                    node.entries.push_back({key, flow(buffer, pos)});
                    flowSkip(buffer, pos);
                } else {
                    node.entries.push_back({key, Node()});
                }
                if (buffer[pos] == QLatin1Char(','))
                    ++pos;
                else if (buffer[pos] != QLatin1Char('}'))
                    fail(QStringLiteral("expected , or }"));
            }
            ++pos;
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            node.plain = false;
            node.text = quoted(buffer, pos);
        } else {
            node.text = flowPlain(buffer, pos);
        }
        --depth;
        return node;
    }

    std::vector<Line> lines;
    size_t at = 0;
    int depth = 0;
    bool started = false;
    bool seenMarker = false;
};

void writeQuoted(QByteArray &out, const QString &text)
{
    QString escaped;
    escaped.reserve(text.size() + 2);
    escaped += QLatin1Char('"');
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if (c == QLatin1Char('"'))
            escaped += QLatin1String("\\\"");
        else if (c == QLatin1Char('\\'))
            escaped += QLatin1String("\\\\");
        else if (c == QLatin1Char('\n'))
            escaped += QLatin1String("\\n");
        else if (c == QLatin1Char('\t'))
            escaped += QLatin1String("\\t");
        else if (c == QLatin1Char('\r'))
            escaped += QLatin1String("\\r");
        else if (u < 0x20 || u == 0x7F || u == 0x85 || u == 0x2028 || u == 0x2029 || u == 0xFEFF)
            escaped += QStringLiteral("\\u%1").arg(u, 4, 16, QLatin1Char('0'));
        else
            escaped += c;
    }
    escaped += QLatin1Char('"');
    out += escaped.toUtf8();
}

void writeScalar(QByteArray &out, const Node &node)
{
    if (node.plain && node.text.isEmpty())
        out += '~';
    else if (node.plain && !node.text.contains(QLatin1Char('\n')))
        out += node.text.toUtf8();
    else
        writeQuoted(out, node.text);
}

void writeKey(QByteArray &out, const QString &key)
{
    bool simple = !key.isEmpty();
    for (const QChar c : key)
        simple = simple && c.unicode() < 0x80
            && (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-'));
    if (simple)
        out += key.toUtf8();
    else
        writeQuoted(out, key);
}

bool isInline(const Node &node)
{
    return node.kind == Node::Scalar || (node.kind == Node::Sequence && node.items.empty())
        || (node.kind == Node::Mapping && node.entries.empty());
}

void writeInline(QByteArray &out, const Node &node)
{
    if (node.kind == Node::Sequence)
        out += "[]";
    else if (node.kind == Node::Mapping)
        out += "{}";
    else
        writeScalar(out, node);
}

/* openOnLine: a "- " is already written, so the first line needs no indent. */
void writeBlock(QByteArray &out, const Node &node, int indent, bool openOnLine)
{
    const QByteArray pad(indent, ' ');
    bool first = true;
    const auto start = [&] {
        if (!first || !openOnLine)
            out += pad;
        first = false;
    };
    if (node.kind == Node::Mapping) {
        for (const Node::Entry &entry : node.entries) {
            start();
            writeKey(out, entry.key);
            out += ':';
            if (isInline(entry.value)) {
                out += ' ';
                writeInline(out, entry.value);
                out += '\n';
            } else {
                out += '\n';
                writeBlock(out, entry.value, indent + 2, false);
            }
        }
    } else {
        for (const Node &item : node.items) {
            start();
            out += "- ";
            if (isInline(item)) {
                writeInline(out, item);
                out += '\n';
            } else {
                writeBlock(out, item, indent + 2, true);
            }
        }
    }
}

} /* namespace */

bool parse(const QByteArray &data, Node *out, QString *error)
{
    QByteArray bytes = data;
    if (bytes.startsWith("\xEF\xBB\xBF"))
        bytes.remove(0, 3);
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder.decode(bytes);
    if (decoder.hasError() || text.contains(QChar(0))) {
        if (error)
            *error = QStringLiteral("not UTF-8 text");
        return false;
    }
    try {
        Parser parser(text);
        *out = parser.document();
    } catch (const Failure &failure) {
        if (error)
            *error = failure.message;
        return false;
    }
    return true;
}

QByteArray dump(const Node &root)
{
    QByteArray out;
    if (isInline(root)) {
        writeInline(out, root);
        out += '\n';
    } else {
        writeBlock(out, root, 0, false);
    }
    return out;
}

Node fromJson(const QJsonValue &value)
{
    Node node;
    if (value.isArray()) {
        node = sequence();
        const QJsonArray array = value.toArray();
        for (const QJsonValue &item : array)
            node.items.push_back(fromJson(item));
    } else if (value.isObject()) {
        node = mapping();
        const QJsonObject object = value.toObject();
        /* QJsonObject sorts its keys; a rule reads better led by its name. */
        if (object.contains(QLatin1String("name")))
            node.set(QStringLiteral("name"), fromJson(object.value(QLatin1String("name"))));
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key() != QLatin1String("name"))
                node.set(it.key(), fromJson(it.value()));
    } else if (value.isString()) {
        node.plain = false;
        node.text = value.toString();
    } else if (value.isBool()) {
        node.text = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    } else if (value.isDouble()) {
        const double number = value.toDouble();
        const qint64 whole = value.toInteger(Q_INT64_C(-9007199254740993));
        node.text = whole != Q_INT64_C(-9007199254740993) ? QString::number(whole)
                                                          : QString::number(number, 'g', 17);
    }
    return node;
}

QJsonValue toJson(const Node &node)
{
    if (node.kind == Node::Sequence) {
        QJsonArray array;
        for (const Node &item : node.items)
            array.append(toJson(item));
        return array;
    }
    if (node.kind == Node::Mapping) {
        QJsonObject object;
        for (const Node::Entry &entry : node.entries)
            object.insert(entry.key, toJson(entry.value));
        return object;
    }
    return node.isNull() ? QJsonValue() : QJsonValue(node.text);
}

} /* namespace Yaml */
