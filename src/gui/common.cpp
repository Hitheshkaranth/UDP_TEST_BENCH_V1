#include "common.h"

#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QScrollBar>
#include <QTableWidget>
#include <QTextStream>

// Creates a bold, selectable label used to display a live result value.
QLabel *makeValueLabel(QWidget *parent)
{
    auto *label = new QLabel(QStringLiteral("–"), parent);
    QFont f = label->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() + 1.5);
    label->setFont(f);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setMinimumWidth(130);
    return label;
}

// Creates a read-only, row-selectable table with the given column headers.
QTableWidget *makeStatsTable(const QStringList &headers, QWidget *parent)
{
    auto *table = new QTableWidget(0, headers.size(), parent);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    return table;
}

// Appends a right-aligned row; keeps the view scrolled to the bottom if it already was.
void appendTableRow(QTableWidget *table, const QStringList &cells)
{
    QScrollBar *bar = table->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 2;

    const int row = table->rowCount();
    table->insertRow(row);
    for (int col = 0; col < cells.size() && col < table->columnCount(); ++col) {
        auto *item = new QTableWidgetItem(cells.at(col));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->setItem(row, col, item);
    }
    if (atBottom)
        table->scrollToBottom();
}

// Quotes a CSV field if it contains a comma or a double quote.
static QString csvField(QString s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"'))) {
        s.replace(QLatin1String("\""), QLatin1String("\"\""));
        return QLatin1Char('"') + s + QLatin1Char('"');
    }
    return s;
}

// Asks for a file name and writes the table (header + rows) as CSV. Returns true on success.
bool exportTableCsv(QWidget *parent, const QTableWidget *table, const QString &defaultName)
{
    if (table->rowCount() == 0) {
        QMessageBox::information(parent, QObject::tr("Export CSV"), QObject::tr("There is no data to export yet."));
        return false;
    }
    const QString path = QFileDialog::getSaveFileName(parent, QObject::tr("Export CSV"), defaultName,
                                                      QObject::tr("CSV files (*.csv)"));
    if (path.isEmpty())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(parent, QObject::tr("Export CSV"),
                             QObject::tr("Cannot write %1:\n%2").arg(path, file.errorString()));
        return false;
    }

    QTextStream out(&file);
    QStringList fields;
    for (int c = 0; c < table->columnCount(); ++c)
        fields << csvField(table->horizontalHeaderItem(c)->text());
    out << fields.join(QLatin1Char(',')) << '\n';
    for (int r = 0; r < table->rowCount(); ++r) {
        fields.clear();
        for (int c = 0; c < table->columnCount(); ++c) {
            const QTableWidgetItem *item = table->item(r, c);
            fields << csvField(item ? item->text() : QString());
        }
        out << fields.join(QLatin1Char(',')) << '\n';
    }
    return true;
}
