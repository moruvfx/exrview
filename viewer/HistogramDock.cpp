#include "HistogramDock.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>

class HistogramCanvas : public QWidget {
public:
    explicit HistogramCanvas(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(140);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setData(const QVector<int>& r, const QVector<int>& g, const QVector<int>& b, const QVector<int>& l, bool logScale)
    {
        m_r = r;
        m_g = g;
        m_b = b;
        m_l = l;
        m_log = logScale;
        update();
    }

    void setLog(bool on)
    {
        m_log = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(24, 24, 26));
        const QRectF area = rect().adjusted(6, 6, -6, -6);
        if (area.width() <= 10 || area.height() <= 10) return;

        // Lưới tham chiếu 0.25 / 0.5 / 0.75 (0.0 -> 1.0)
        p.setPen(QPen(QColor(48, 48, 52), 1));
        for (int i = 1; i <= 3; ++i) {
            double x = area.left() + area.width() * (i * 0.25);
            p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        }
        p.drawRect(area);

        if (m_l.isEmpty()) {
            p.setPen(QColor(120, 120, 125));
            p.drawText(area, Qt::AlignCenter, tr("Không có dữ liệu"));
            return;
        }

        const int bins = m_l.size();
        int maxVal = 1;
        for (int i = 0; i < bins; ++i)
            maxVal = std::max({maxVal, m_r[i], m_g[i], m_b[i], m_l[i]});

        const double denom = m_log ? std::log1p((double)maxVal) : (double)maxVal;
        auto normY = [&](int v) -> double {
            double n = m_log ? std::log1p((double)v) / denom : (double)v / denom;
            return area.bottom() - qBound(0.0, n, 1.0) * area.height();
        };

        p.setRenderHint(QPainter::Antialiasing, true);
        auto drawCurve = [&](const QVector<int>& data, QColor stroke, QColor fill) {
            QPainterPath path;
            path.moveTo(area.left(), area.bottom());
            for (int i = 0; i < bins; ++i) {
                double x = area.left() + (double)i / (bins - 1) * area.width();
                path.lineTo(x, normY(data[i]));
            }
            path.lineTo(area.right(), area.bottom());
            path.closeSubpath();
            p.fillPath(path, fill);
            p.setPen(QPen(stroke, 1.3));
            QPainterPath line;
            for (int i = 0; i < bins; ++i) {
                double x = area.left() + (double)i / (bins - 1) * area.width();
                double y = normY(data[i]);
                if (i == 0) line.moveTo(x, y);
                else line.lineTo(x, y);
            }
            p.drawPath(line);
        };

        drawCurve(m_r, QColor(235, 85, 85, 220), QColor(235, 85, 85, 38));
        drawCurve(m_g, QColor(90, 215, 105, 220), QColor(90, 215, 105, 38));
        drawCurve(m_b, QColor(90, 155, 245, 220), QColor(90, 155, 245, 38));
        drawCurve(m_l, QColor(225, 225, 230, 190), QColor(225, 225, 230, 25));
    }

private:
    QVector<int> m_r, m_g, m_b, m_l;
    bool m_log = true;
};

HistogramDock::HistogramDock(QWidget* parent) : QDockWidget(tr("Histogram & Thống kê"), parent)
{
    setObjectName("histogramDock");
    auto* root = new QWidget(this);
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    auto* top = new QHBoxLayout;
    auto* title = new QLabel(tr("Phân bố 0.0 → 1.0 (sau exposure)"));
    title->setStyleSheet("color: #aaa; font-size: 11px;");
    top->addWidget(title, 1);
    m_logCheck = new QCheckBox(tr("Log"));
    m_logCheck->setChecked(true);
    connect(m_logCheck, &QCheckBox::toggled, this, [this](bool on) { m_canvas->setLog(on); });
    top->addWidget(m_logCheck);
    lay->addLayout(top);

    m_canvas = new HistogramCanvas(root);
    lay->addWidget(m_canvas, 1);

    m_statsLabel = new QLabel;
    m_statsLabel->setWordWrap(true);
    m_statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statsLabel->setStyleSheet("font-family: Consolas, monospace; font-size: 11px; color: #ccc;");
    lay->addWidget(m_statsLabel);

    setWidget(root);
}

void HistogramDock::setImage(std::shared_ptr<const exrv::Image> img, float exposure)
{
    m_img = std::move(img);
    m_exposure = exposure;
    if (isVisible()) recompute();
}

void HistogramDock::clear()
{
    m_img.reset();
    m_canvas->setData({}, {}, {}, {}, m_logCheck->isChecked());
    m_statsLabel->clear();
}

void HistogramDock::recompute()
{
    if (!m_img || m_img->empty()) {
        clear();
        return;
    }

    const int kBins = 128;
    QVector<int> hr(kBins, 0), hg(kBins, 0), hb(kBins, 0), hl(kBins, 0);

    const int w = m_img->width;
    const int h = m_img->height;
    const size_t totalPx = (size_t)w * h;
    // Lấy mẫu đều để ảnh 4K/8K tính trong < 3ms
    const int step = (int)std::max<size_t>(1, (size_t)std::sqrt((double)totalPx / 250000.0));

    float minV = std::numeric_limits<float>::infinity();
    float maxV = -std::numeric_limits<float>::infinity();
    double sumR = 0, sumG = 0, sumB = 0;
    size_t validSamples = 0;
    size_t nanCount = 0, infCount = 0, negCount = 0, hdrCount = 0;

    const float gain = std::exp2(m_exposure);
    float px[4];
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            m_img->pixel(x, y, px);
            bool anyNan = false, anyInf = false, anyNeg = false, anyHdr = false;
            for (int c = 0; c < 3; ++c) {
                float v = px[c];
                if (std::isnan(v)) { anyNan = true; continue; }
                if (std::isinf(v)) { anyInf = true; continue; }
                if (v < 0.f) anyNeg = true;
                if (v * gain > 1.f) anyHdr = true;
                minV = std::min(minV, v);
                maxV = std::max(maxV, v);
            }
            if (anyNan) { ++nanCount; continue; }
            if (anyInf) { ++infCount; continue; }
            if (anyNeg) ++negCount;
            if (anyHdr) ++hdrCount;

            sumR += px[0];
            sumG += px[1];
            sumB += px[2];
            ++validSamples;

            auto toBin = [&](float v) {
                float s = v * gain;
                // Áp dụng gamma ~2.2 nhẹ khi hiển thị histogram để dễ nhìn vùng tối
                float g = s > 0.f ? std::pow(std::min(s, 1.f), 1.f / 2.2f) : 0.f;
                return qBound(0, (int)(g * (kBins - 1) + 0.5f), kBins - 1);
            };
            float lum = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
            ++hr[toBin(px[0])];
            ++hg[toBin(px[1])];
            ++hb[toBin(px[2])];
            ++hl[toBin(lum)];
        }
    }

    m_canvas->setData(hr, hg, hb, hl, m_logCheck->isChecked());

    if (validSamples == 0) {
        m_statsLabel->setText(tr("Không có pixel hợp lệ (NaN: %1, Inf: %2)").arg(nanCount).arg(infCount));
        return;
    }

    auto fmt = [](float v) {
        return std::isfinite(v) ? QString::number(v, 'g', 4) : QStringLiteral("—");
    };
    const double invN = 1.0 / (double)validSamples;
    const double hdrPct = 100.0 * hdrCount * invN;
    m_statsLabel->setText(
        tr("Min: %1   Max: %2\nMean RGB: %3  %4  %5\n>1.0 (HDR): %6%   Âm (<0): %7\nNaN: %8   Inf: %9   (mẫu %10×%10)")
            .arg(fmt(minV), fmt(maxV))
            .arg(sumR * invN, 0, 'f', 4)
            .arg(sumG * invN, 0, 'f', 4)
            .arg(sumB * invN, 0, 'f', 4)
            .arg(hdrPct, 0, 'f', 1)
            .arg(negCount)
            .arg(nanCount)
            .arg(infCount)
            .arg(step));
}
