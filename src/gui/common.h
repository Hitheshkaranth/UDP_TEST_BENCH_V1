#pragma once

#include "format.h"

#include <QString>
#include <QStringList>

class QLabel;
class QTableWidget;
class QWidget;

// Bold, selectable label for a live result value.
QLabel *makeValueLabel(QWidget *parent = nullptr);
// Read-only results table with the given column headers.
QTableWidget *makeStatsTable(const QStringList &headers, QWidget *parent = nullptr);
// Appends a row, auto-scrolling when the view is at the bottom.
void appendTableRow(QTableWidget *table, const QStringList &cells);
// Saves the table to a CSV file chosen by the user.
bool exportTableCsv(QWidget *parent, const QTableWidget *table, const QString &defaultName);
