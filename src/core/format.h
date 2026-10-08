#pragma once

#include <QString>

// Formats a rate given in Mbit/s, e.g. "950.00 Mbps" or "1.250 Gbps".
QString formatRate(double mbps);
// Formats a byte count, e.g. "150.00 MB".
QString formatBytes(quint64 bytes);
