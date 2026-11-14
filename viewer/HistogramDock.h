// HistogramDock — biểu đồ phân bố R/G/B/Luminance và thống kê giá trị float (Min/Max/Mean/NaN/Inf).
#pragma once

#include "exrcore.h"

#include <QDockWidget>
#include <QVector>

#include <memory>

class QCheckBox;
class QLabel;
class HistogramCanvas;

class HistogramDock : public QDockWidget {
    Q_OBJECT
public:
    explicit HistogramDock(QWidget* parent = nullptr);

    void setImage(std::shared_ptr<const exrv::Image> img, float exposure = 0.f);
    void clear();

private:
    void recompute();

    std::shared_ptr<const exrv::Image> m_img;
    float m_exposure = 0.f;
    HistogramCanvas* m_canvas = nullptr;
    QCheckBox* m_logCheck = nullptr;
    QLabel* m_statsLabel = nullptr;
};
