#include "format.h"

// Formats a rate given in Mbit/s as kbps, Mbps or Gbps with sensible precision.
QString formatRate(double mbps)
{
    if (mbps >= 1000.0)
        return QStringLiteral("%1 Gbps").arg(mbps / 1000.0, 0, 'f', 3);
    if (mbps >= 1.0)
        return QStringLiteral("%1 Mbps").arg(mbps, 0, 'f', 2);
    return QStringLiteral("%1 kbps").arg(mbps * 1000.0, 0, 'f', 1);
}

// Formats a byte count as B, kB, MB or GB (decimal units, consistent with Mbps).
QString formatBytes(quint64 bytes)
{
    const double b = double(bytes);
    if (b >= 1e9)
        return QStringLiteral("%1 GB").arg(b / 1e9, 0, 'f', 3);
    if (b >= 1e6)
        return QStringLiteral("%1 MB").arg(b / 1e6, 0, 'f', 2);
    if (b >= 1e3)
        return QStringLiteral("%1 kB").arg(b / 1e3, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}
