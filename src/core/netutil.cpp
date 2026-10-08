#include "netutil.h"

#include <QHostInfo>
#include <QNetworkInterface>

// Returns this machine's IPv4 addresses as text.
QStringList localIPv4Addresses(bool includeLoopback)
{
    QStringList list;
    for (const QHostAddress &a : QNetworkInterface::allAddresses()) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol && (includeLoopback || !a.isLoopback()))
            list << a.toString();
    }
    return list;
}

// Resolves an IP address or host name, preferring IPv4; null address on failure.
QHostAddress resolveHost(const QString &host)
{
    const QString name = host.trimmed();
    QHostAddress addr;
    if (addr.setAddress(name))
        return addr;

    const QHostInfo info = QHostInfo::fromName(name);
    for (const QHostAddress &a : info.addresses()) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol)
            return a;
    }
    return info.addresses().isEmpty() ? QHostAddress() : info.addresses().first();
}
