#pragma once

// Generation and input validation of the data field: the bytes between the
// 24-byte header and the 0x33 stop byte of every data datagram.

#include <QByteArray>
#include <QString>

enum class PayloadPattern {
    Incrementing,   // 00 01 02 ... FF 00 01 ... (optional first value)
    FixedByte,      // every byte the same value
    PseudoRandom,   // xorshift32 sequence from a seed (same in every datagram)
    CustomHex,      // user hex bytes, repeated to fill the field
    Text,           // user text (UTF-8), repeated to fill the field
};

inline constexpr PayloadPattern kAllPayloadPatterns[] = {
    PayloadPattern::Incrementing, PayloadPattern::FixedByte, PayloadPattern::PseudoRandom,
    PayloadPattern::CustomHex,    PayloadPattern::Text,
};

struct PayloadConfig
{
    PayloadPattern pattern = PayloadPattern::Incrementing;
    QString value;   // pattern parameter as typed: first byte, fixed byte, seed, hex bytes or text
};

namespace Payload {

// Display name of a pattern ("Fixed byte", ...).
QString patternName(PayloadPattern pattern);

// Keyword used by the CLI --pattern option ("inc", "fixed", "random", "hex", "text").
QString patternKeyword(PayloadPattern pattern);

// Converts a CLI keyword back to a pattern; returns false if unknown.
bool patternFromKeyword(const QString &keyword, PayloadPattern &pattern);

// Describes what the value field means for a pattern (used as placeholder / help text).
QString valueHint(PayloadPattern pattern);

// Parses hex bytes such as "DE AD BE EF", "0xDE,0xAD" or "DEADBEEF".
// Returns false and sets *error if the text is not valid hex.
bool parseHexBytes(const QString &text, QByteArray &out, QString *error = nullptr);

// Validates cfg and fills out with exactly `length` data-field bytes.
// Returns false and sets *error with a user-readable reason if the input is invalid.
bool build(const PayloadConfig &cfg, int length, QByteArray &out, QString *error = nullptr);

// Formats the first maxBytes bytes as "DE AD BE EF ..." for previews and logs.
QString hexPreview(const QByteArray &data, int maxBytes = 24);

} // namespace Payload
