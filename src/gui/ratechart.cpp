#include "ratechart.h"

#include "common.h"

#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kMaxPointsPerSeries = 7200;

// Smallest "round" number (1, 2, 2.5, 5 x 10^n) that is >= v.
double niceCeil(double v)
{
    if (v <= 0.0)
        return 1.0;
    const double mag = std::pow(10.0, std::floor(std::log10(v)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (m * mag >= v)
            return m * mag;
    }
    return 10.0 * mag;
}

// Picks a grid spacing (seconds) that gives at most ~8 vertical grid lines.
double timeStep(double windowSec)
{
    for (double step : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0}) {
        if (windowSec / step <= 8.0)
            return step;
    }
    return 3600.0;
}

} // namespace

// Creates an empty chart that expands to fill the available space.
RateChart::RateChart(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

// Adds a named line in the given colour; returns its index for append().
int RateChart::addSeries(const QString &name, const QColor &color)
{
    m_series.append(Series{name, color, {}});
    update();
    return m_series.size() - 1;
}

// Adds a point (time in seconds, rate in Mbps) to a series and repaints.
void RateChart::append(int series, double xSec, double mbps)
{
    if (series < 0 || series >= m_series.size())
        return;
    QVector<QPointF> &pts = m_series[series].points;
    pts.append(QPointF(xSec, mbps));
    if (pts.size() > kMaxPointsPerSeries)
        pts.remove(0, pts.size() - kMaxPointsPerSeries);
    update();
}

// Removes all points from every series (the series themselves are kept).
void RateChart::clear()
{
    for (Series &s : m_series)
        s.points.clear();
    update();
}

// Sets how many seconds of history are visible (minimum 5).
void RateChart::setWindowSeconds(double seconds)
{
    m_windowSec = std::max(5.0, seconds);
    update();
}

// Draws the grid, axis labels, data lines and the legend with the latest values.
void RateChart::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QPalette pal = palette();
    const QColor textColor = pal.color(QPalette::Text);
    QColor gridColor = textColor;
    gridColor.setAlpha(45);
    p.fillRect(rect(), pal.color(QPalette::Base));

    const QFontMetrics fm = p.fontMetrics();
    const int legendHeight = fm.height() + 8;
    const int leftMargin = fm.horizontalAdvance(QStringLiteral("000.000 Gbps")) + 10;
    const QRect plot = rect().adjusted(leftMargin, legendHeight + 4, -16, -(fm.height() + 10));
    if (plot.width() < 40 || plot.height() < 30)
        return;

    bool anyData = false;
    double lastX = 0.0;
    for (const Series &s : m_series) {
        if (!s.points.isEmpty()) {
            anyData = true;
            lastX = std::max(lastX, s.points.last().x());
        }
    }
    const double xMax = std::max(m_windowSec, lastX);
    const double xMin = xMax - m_windowSec;

    double yPeak = 0.0;
    for (const Series &s : m_series) {
        for (const QPointF &pt : s.points) {
            if (pt.x() >= xMin)
                yPeak = std::max(yPeak, pt.y());
        }
    }
    const double yMax = niceCeil(yPeak * 1.05);

    auto mapX = [&](double x) { return plot.left() + (x - xMin) / (xMax - xMin) * plot.width(); };
    auto mapY = [&](double y) { return plot.bottom() - y / yMax * plot.height(); };

    // Horizontal grid and rate labels.
    constexpr int yTicks = 5;
    for (int i = 0; i <= yTicks; ++i) {
        const double v = yMax * i / yTicks;
        const qreal y = mapY(v);
        p.setPen(gridColor);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(textColor);
        p.drawText(QRectF(0, y - fm.height() / 2.0, leftMargin - 6, fm.height()),
                   Qt::AlignRight | Qt::AlignVCenter, formatRate(v));
    }

    // Vertical grid and time labels.
    const double step = timeStep(m_windowSec);
    for (double x = std::ceil(xMin / step) * step; x <= xMax + 1e-9; x += step) {
        const qreal px = mapX(x);
        p.setPen(gridColor);
        p.drawLine(QPointF(px, plot.top()), QPointF(px, plot.bottom()));
        p.setPen(textColor);
        p.drawText(QRectF(px - 40, plot.bottom() + 4, 80, fm.height()), Qt::AlignHCenter | Qt::AlignTop,
                   QStringLiteral("%1 s").arg(x, 0, 'f', 0));
    }

    p.setPen(gridColor);
    p.drawRect(plot);

    // Data lines.
    p.save();
    p.setClipRect(plot.adjusted(-1, -1, 1, 1));
    for (const Series &s : m_series) {
        QPolygonF poly;
        for (int i = 0; i < s.points.size(); ++i) {
            const QPointF &pt = s.points.at(i);
            const bool nextVisible = i + 1 < s.points.size() && s.points.at(i + 1).x() >= xMin;
            if (pt.x() < xMin && !nextVisible)
                continue;
            poly << QPointF(mapX(pt.x()), mapY(pt.y()));
        }
        p.setPen(QPen(s.color, 2.0));
        p.setBrush(Qt::NoBrush);
        if (poly.size() == 1)
            p.drawEllipse(poly.first(), 2.5, 2.5);
        else
            p.drawPolyline(poly);
    }
    p.restore();

    // Legend with the most recent value of each series.
    int lx = plot.left();
    for (const Series &s : m_series) {
        const QString label = s.points.isEmpty()
                ? s.name
                : QStringLiteral("%1: %2").arg(s.name, formatRate(s.points.last().y()));
        p.fillRect(QRect(lx, 4 + (fm.height() - 10) / 2, 10, 10), s.color);
        p.setPen(textColor);
        p.drawText(lx + 14, 4 + fm.ascent(), label);
        lx += 14 + fm.horizontalAdvance(label) + 20;
    }

    if (!anyData) {
        p.setPen(textColor);
        p.drawText(plot, Qt::AlignCenter, tr("No data yet"));
    }
}
