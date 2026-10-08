#pragma once

#include <QColor>
#include <QPointF>
#include <QVector>
#include <QWidget>

// Minimal scrolling line chart of throughput over time (no Qt Charts dependency).
class RateChart : public QWidget
{
    Q_OBJECT
public:
    explicit RateChart(QWidget *parent = nullptr);

    // Adds a line; returns its index.
    int addSeries(const QString &name, const QColor &color);
    // Adds a point (seconds, Mbps) to a series.
    void append(int series, double xSec, double mbps);
    // Removes all points.
    void clear();
    // Visible time span in seconds.
    void setWindowSeconds(double seconds);

    QSize minimumSizeHint() const override { return {320, 180}; }
    QSize sizeHint() const override { return {700, 260}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Series
    {
        QString name;
        QColor color;
        QVector<QPointF> points;
    };

    QVector<Series> m_series;
    double m_windowSec = 60.0;
};
