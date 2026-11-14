// TimelineSlider — thanh timeline hiển thị các frame đã cache trên RAM và vùng In/Out.
#pragma once

#include <QPainter>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QVector>

class TimelineSlider : public QSlider {
    Q_OBJECT
public:
    explicit TimelineSlider(QWidget* parent = nullptr) : QSlider(Qt::Horizontal, parent) {}

    void setCachedMask(const QVector<bool>& mask)
    {
        if (m_cached == mask) return;
        m_cached = mask;
        update();
    }

    void setInOutRange(int inIdx, int outIdx)
    {
        if (m_in == inIdx && m_out == outIdx) return;
        m_in = inIdx;
        m_out = outIdx;
        update();
    }

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QSlider::paintEvent(e);
        const int total = maximum() - minimum() + 1;
        if (total <= 1) return;

        QStyleOptionSlider opt;
        initStyleOption(&opt);
        QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        if (groove.width() <= 4) return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        // Vùng In/Out (nếu có đặt)
        if (m_in > 0 || (m_out >= 0 && m_out < total - 1)) {
            int inClamped = qBound(0, m_in, total - 1);
            int outClamped = (m_out < 0) ? total - 1 : qBound(inClamped, m_out, total - 1);
            int x0 = groove.left() + (int)((double)inClamped / total * groove.width());
            int x1 = groove.left() + (int)((double)(outClamped + 1) / total * groove.width());
            p.fillRect(QRect(x0, groove.top() - 2, qMax(2, x1 - x0), 2), QColor(232, 140, 40, 210));
        }

        // Vạch xanh dưới groove cho các frame đã cache
        if (m_cached.size() == total) {
            const int y = groove.bottom() + 2;
            const double step = (double)groove.width() / total;
            int runStart = -1;
            for (int i = 0; i <= total; ++i) {
                bool on = (i < total) && m_cached[i];
                if (on && runStart < 0) {
                    runStart = i;
                } else if (!on && runStart >= 0) {
                    int x0 = groove.left() + (int)(runStart * step);
                    int x1 = groove.left() + (int)(i * step);
                    p.fillRect(QRect(x0, y, qMax(1, x1 - x0), 2), QColor(76, 175, 80, 220));
                    runStart = -1;
                }
            }
        }
    }

private:
    QVector<bool> m_cached;
    int m_in = 0;
    int m_out = -1;
};
