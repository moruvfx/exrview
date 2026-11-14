// ImageView — vùng hiển thị ảnh bằng OpenGL: OCIO chạy trên GPU (shader), exposure/gamma/kênh
// đổi tức thì. Tự chuyển sang render CPU nếu máy không có OpenGL 3.3 hoặc ảnh vượt giới hạn texture.
// Zoom quanh con trỏ, kéo để pan, double-click để fit, Shift+kéo để đo vùng pixel.
// Chế độ so sánh trong cùng một khung: Wipe (A bên trái, B bên phải đường chia kéo được)
// và Difference (|A − B| × 2^exposure, giá trị tuyến tính không qua OCIO).
#pragma once

#include "exrcore.h"

#include <QHash>
#include <QImage>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>
#include <QPointF>
#include <QRect>

#include <memory>

class QOpenGLShaderProgram;

class ImageView : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT
public:
    enum class CompareMode { Off = 0, Wipe, Difference };
    enum class BgMode { Dark = 0, Black, Gray, Checker };
    enum class InspectMode { Normal = 0, FalseColor, Clipping };

    explicit ImageView(exrv::ColorPipeline* pipeline, QWidget* parent = nullptr);
    ~ImageView() override;

    // keepView = true: giữ zoom/pan (khi chuyển frame cùng kích thước).
    void setSource(std::shared_ptr<const exrv::Image> img, bool keepView);
    void clearSource();
    void setViewSettings(const exrv::ViewSettings& vs);
    void colorConfigChanged(); // sau khi nạp OCIO config khác

    // Lớp B cho Wipe/Difference (pipeline riêng của B).
    void setCompareMode(CompareMode mode);
    CompareMode compareMode() const { return m_mode; }
    void setSecondPipeline(exrv::ColorPipeline* pipeline);
    void setSecondSource(std::shared_ptr<const exrv::Image> img);
    void setSecondViewSettings(const exrv::ViewSettings& vs);
    void secondColorConfigChanged();

    // Hiển thị & kiểm tra ảnh chuyên nghiệp
    void setBgMode(BgMode m);
    BgMode bgMode() const { return m_bgMode; }
    void setInspectMode(InspectMode m);
    InspectMode inspectMode() const { return m_inspectMode; }
    void setPixelGrid(bool on);
    bool pixelGrid() const { return m_pixelGrid; }
    void setFlipH(bool on);
    bool flipH() const { return m_flipH; }
    void setFlipV(bool on);
    bool flipV() const { return m_flipV; }
    void setRot90(int steps); // 0, 1, 2, 3 (chiều kim đồng hồ)
    int rot90() const { return m_rot90; }

    void fit();
    void setZoom(double z, QPointF anchorWidget = QPointF(-1, -1));
    double zoom() const { return m_zoom; }
    void setMessage(const QString& msg);
    bool gpuMode() const { return m_glOk && !m_cpuMode; }
    QString rendererInfo() const { return m_rendererInfo; }
    QPointF viewOffset() const { return m_offset; }
    bool fitMode() const { return m_fitMode; }
    // Đặt zoom/pan từ khung khác (chế độ so sánh) — không phát lại viewChanged.
    void setViewTransform(double zoom, QPointF offset, bool fitMode);
    void setLabel(const QString& label); // nhãn góc khung, vd "A" / "B"
    void setPlayback(bool on);            // đang play sequence: ưu tiên tốc độ (không tạo mipmap mỗi frame)

signals:
    void hovered(int x, int y, bool inside); // toạ độ pixel ảnh gốc (đã tính flip/rotate)
    void regionSelected(const QRect& imageRect);
    void zoomChanged(double zoom);
    void fileDropped(const QString& path);
    void rendererChanged(const QString& info);
    void viewChanged(double zoom, QPointF offset, bool fitMode); // zoom/pan do người dùng đổi

protected:
    void initializeGL() override;
    void paintGL() override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void leaveEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    struct LutTex { unsigned id; unsigned target; QByteArray sampler; };
    // Một ảnh + cài đặt màu + tài nguyên GPU/CPU của nó. [0] = ảnh chính (A), [1] = B.
    struct Layer {
        exrv::ColorPipeline* pipe = nullptr;
        std::shared_ptr<const exrv::Image> src;
        exrv::ViewSettings vs;
        bool shaderDirty = true;
        bool texDirty = false;
        bool cpuDirty = true;
        QOpenGLShaderProgram* prog = nullptr;
        QHash<QByteArray, GLint> uniformLocs;
        unsigned tex = 0;
        unsigned pbo[2] = {0, 0};
        size_t pboBytes[2] = {0, 0};
        int pboIdx = 0;
        int texW = 0, texH = 0;   // kích thước/kiểu texture đang cấp phát (để ghi đè thay vì cấp lại)
        bool texHalf = false;
        bool mipsValid = false;
        std::vector<LutTex> luts;
        QImage cpuImage;
        bool valid() const { return src && !src->empty(); }
    };

    QPointF widgetToSourceImage(QPointF p) const; // toạ độ trên ảnh gốc (đã đảo flip/rotate)
    QPointF displayToSource(QPointF dp) const;
    void emitHover(QPointF widgetPos);
    QSize imageSize() const; // kích thước hiển thị (sau khi tính rot90)
    bool twoLayers() const { return m_mode != CompareMode::Off && m_layers[1].valid(); }
    bool rebuildShader(Layer& L, QString* err);
    bool buildDiffShader(QString* err);
    void uploadImage(Layer& L);
    void bindFiltered(Layer& L);
    void freeLayerGl(Layer& L, bool texToo);
    void drawLayerGpu(Layer& L);
    void drawDiffGpu();
    void drawCpu();
    bool updateCpuImage(Layer& L);
    void updateDiffCpu();
    void drawOverlay();
    void setCpuMode(bool on, const QString& reason);
    void fitSilently(); // như fit() nhưng không phát viewChanged
    double wipeX() const { return m_wipe * width(); }
    bool nearWipe(QPointF p) const;

    Layer m_layers[2];
    CompareMode m_mode = CompareMode::Off;
    BgMode m_bgMode = BgMode::Dark;
    InspectMode m_inspectMode = InspectMode::Normal;
    bool m_pixelGrid = true;
    bool m_flipH = false;
    bool m_flipV = false;
    int m_rot90 = 0;
    double m_wipe = 0.5; // vị trí đường wipe theo tỉ lệ bề ngang khung
    bool m_draggingWipe = false;
    bool m_playback = false;

    // Chọn vùng (Shift + kéo)
    bool m_selecting = false;
    QPointF m_selStart, m_selEnd;

    // GL
    bool m_glOk = false;
    bool m_cpuMode = false;
    int m_maxTex = 0;
    unsigned m_vao = 0, m_vbo = 0;
    QOpenGLShaderProgram* m_diffProg = nullptr;
    QString m_rendererInfo;

    // CPU fallback cho Difference
    QImage m_diffImage;
    bool m_diffDirty = true;

    double m_zoom = 1.0;
    QPointF m_offset;
    bool m_fitMode = true;
    bool m_dragging = false;
    QPointF m_dragStart, m_offsetStart;
    QString m_message;
    QString m_label;
};
