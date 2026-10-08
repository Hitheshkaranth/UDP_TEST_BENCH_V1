#include "payload.h"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

namespace {

// Stores msg in *error (if requested) and returns false, to keep validation code short.
bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

// Parses a value that must be exactly one byte (e.g. "A5" or "0xA5").
bool parseSingleByte(const QString &text, quint8 &value, QString *error)
{
    QByteArray bytes;
    if (!Payload::parseHexBytes(text, bytes, error))
        return false;
    if (bytes.size() != 1)
        return fail(error, QStringLiteral("enter a single byte in hex, 00 to FF"));
    value = quint8(bytes.at(0));
    return true;
}

} // namespace

namespace Payload {

// Display name of a pattern ("Fixed byte", ...).
QString patternName(PayloadPattern pattern)
{
    switch (pattern) {
    case PayloadPattern::Incrementing: return QStringLiteral("Incrementing bytes");
    case PayloadPattern::FixedByte:    return QStringLiteral("Fixed byte");
    case PayloadPattern::PseudoRandom: return QStringLiteral("Pseudo-random");
    case PayloadPattern::CustomHex:    return QStringLiteral("Custom hex bytes");
    case PayloadPattern::Text:         return QStringLiteral("Text");
    }
    return QString();
}

// Keyword used by the CLI --pattern option.
QString patternKeyword(PayloadPattern pattern)
{
    switch (pattern) {
    case PayloadPattern::Incrementing: return QStringLiteral("inc");
    case PayloadPattern::FixedByte:    return QStringLiteral("fixed");
    case PayloadPattern::PseudoRandom: return QStringLiteral("random");
    case PayloadPattern::CustomHex:    return QStringLiteral("hex");
    case PayloadPattern::Text:         return QStringLiteral("text");
    }
    return QString();
}

// Converts a CLI keyword back to a pattern; returns false if unknown.
bool patternFromKeyword(const QString &keyword, PayloadPattern &pattern)
{
    for (PayloadPattern p : kAllPayloadPatterns) {
        if (keyword.compare(patternKeyword(p), Qt::CaseInsensitive) == 0) {
            pattern = p;
            return true;
        }
    }
    return false;
}

// Describes what the value field means for a pattern.
QString valueHint(PayloadPattern pattern)
{
    switch (pattern) {
    case PayloadPattern::Incrementing: return QStringLiteral("first byte in hex, optional (default 00)");
    case PayloadPattern::FixedByte:    return QStringLiteral("byte value in hex, e.g. A5");
    case PayloadPattern::PseudoRandom: return QStringLiteral("seed 1-4294967295, optional (default 1)");
    case PayloadPattern::CustomHex:    return QStringLiteral("hex bytes, e.g. DE AD BE EF (repeated to fill)");
    case PayloadPattern::Text:         return QStringLiteral("text, e.g. HELLO (repeated to fill)");
    }
    return QString();
}

// Parses hex bytes; separators may be spaces, commas, semicolons, colons or dashes,
// each token may have a 0x prefix, and long tokens are read two digits at a time.
bool parseHexBytes(const QString &text, QByteArray &out, QString *error)
{
    out.clear();
    static const QRegularExpression separators(QStringLiteral("[\\s,;:\\-]+"));
    const QStringList tokens = text.split(separators, Qt::SkipEmptyParts);
    for (QString token : tokens) {
        if (token.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
            token.remove(0, 2);
        if (token.isEmpty())
            return fail(error, QStringLiteral("\"0x\" without hex digits"));
        if (token.size() > 2 && token.size() % 2 != 0)
            return fail(error, QStringLiteral("\"%1\" has an odd number of hex digits").arg(token));
        for (int i = 0; i < token.size(); i += 2) {
            const QString digits = token.mid(i, 2);
            bool ok = false;
            const uint v = digits.toUInt(&ok, 16);
            if (!ok || digits.startsWith(QLatin1Char('+')))
                return fail(error, QStringLiteral("\"%1\" is not a hex byte").arg(digits));
            out.append(char(v));
        }
    }
    if (out.isEmpty())
        return fail(error, QStringLiteral("no hex bytes given"));
    return true;
}

// Validates cfg and fills out with exactly `length` data-field bytes.
bool build(const PayloadConfig &cfg, int length, QByteArray &out, QString *error)
{
    if (length <= 0)
        return fail(error, QStringLiteral("the datagram is too small to have a data field"));

    const QString value = cfg.value.trimmed();
    out = QByteArray(length, Qt::Uninitialized);

    switch (cfg.pattern) {
    case PayloadPattern::Incrementing: {
        quint8 first = 0;
        if (!value.isEmpty() && !parseSingleByte(value, first, error))
            return false;
        for (int i = 0; i < length; ++i)
            out[i] = char(quint8(first + i));
        return true;
    }
    case PayloadPattern::FixedByte: {
        if (value.isEmpty())
            return fail(error, QStringLiteral("enter the byte value in hex, e.g. A5"));
        quint8 byte = 0;
        if (!parseSingleByte(value, byte, error))
            return false;
        out.fill(char(byte));
        return true;
    }
    case PayloadPattern::PseudoRandom: {
        quint32 seed = 1;
        if (!value.isEmpty()) {
            bool ok = false;
            seed = value.toUInt(&ok, 0);   // decimal, or hex with 0x prefix
            if (!ok || seed == 0)
                return fail(error, QStringLiteral("the seed must be a number from 1 to 4294967295"));
        }
        quint32 x = seed;
        for (int i = 0; i < length; ++i) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            out[i] = char(x & 0xff);
        }
        return true;
    }
    case PayloadPattern::CustomHex:
    case PayloadPattern::Text: {
        QByteArray unit;
        if (cfg.pattern == PayloadPattern::CustomHex) {
            if (!parseHexBytes(value, unit, error))
                return false;
        } else {
            if (cfg.value.isEmpty())
                return fail(error, QStringLiteral("enter the text to send"));
            unit = cfg.value.toUtf8();   // untrimmed: spaces in the text are data
        }
        if (unit.size() > length) {
            return fail(error, QStringLiteral("%1 bytes do not fit in the %2-byte data field; "
                                              "increase the data-field size")
                                       .arg(unit.size())
                                       .arg(length));
        }
        for (int i = 0; i < length; ++i)
            out[i] = unit.at(i % unit.size());
        return true;
    }
    }
    return fail(error, QStringLiteral("unknown data pattern"));
}

// Formats the first maxBytes bytes as "DE AD BE EF ..." for previews and logs.
QString hexPreview(const QByteArray &data, int maxBytes)
{
    const int n = std::min<int>(maxBytes, data.size());
    QString text = QString::fromLatin1(data.left(n).toHex(' ').toUpper());
    if (data.size() > n)
        text += QStringLiteral(" ...");
    return text;
}

} // namespace Payload
