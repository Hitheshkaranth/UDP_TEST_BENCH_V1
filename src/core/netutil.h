#pragma once

#include <QHostAddress>
#include <QString>
#include <QStringList>

// Resolves an IP address or host name, preferring IPv4. Returns a null address if the
// name cannot be resolved. Blocks while a DNS lookup is in progress.
QHostAddress resolveHost(const QString &host);

// Returns this machine's IPv4 addresses as text (loopback included only if includeLoopback).
QStringList localIPv4Addresses(bool includeLoopback);
