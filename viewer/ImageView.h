// ImageView — vùng hiển thị ảnh bằng OpenGL: OCIO chạy trên GPU (shader), exposure/gamma/kênh
// đổi tức thì. Tự chuyển sang render CPU nếu máy không có OpenGL 3.3 hoặc ảnh vượt giới hạn texture.
// Zoom quanh con trỏ, kéo để pan, double-click để fit.
#pragma once

#include "exrcore.h"

#include <QImage>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>
#include <QPointF>

#include <memory>

class QOpenGLShaderProgram;

class ImageView : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT
public:
    explicit ImageView(exrv::ColorPipeline* pipeline, QWidget* parent = nullptr);
    ~ImageView() override;

    // keepView = true: giữ zoom/pan (khi chuyển frame cùng kích thước).
    void setSource(std::shared_ptr<const exrv::Image> img, bool keepView);
    void clearSource();
    void setViewSettings(const exrv::ViewSettings& vs);
    void colorConfigChanged(); // sau khi nạp OCIO config khác

    void fit();
    void setZoom(double z, QPointF anchorWidget = QPointF(-1, -1));
    double zoom() const { return m_zoom; }
    void setMessage(const QString& msg);
    bool gpuMode() const { return m_glOk && !m_cpuMode; }
    QString rendererInfo() const { return m_rendererInfo; }

signals:
    void hovered(int x, int y, bool inside); // toạ độ pixel ảnh
    void zoomChanged(double zoom);
    void fileDropped(const QString& path);
    void rendererChanged(const QString& info);

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
    QPointF widgetToImage(QPointF p) const;
    void emitHover(QPointF widgetPos);
    QSize imageSize() const;
    bool rebuildShader(QString* err);
    void uploadImage();
    void freeLuts();
    void drawGpu();
    void drawCpu();
    void drawOverlay();
    void setCpuMode(bool on, const QString& reason);

    exrv::ColorPipeline* m_pipe;
    std::shared_ptr<const exrv::Image> m_src;
    exrv::ViewSettings m_vs;

    // GL
    bool m_glOk = false;
    bool m_cpuMode = false;
    bool m_shaderDirty = true;
    bool m_texDirty = false;
    int m_maxTex = 0;
    QOpenGLShaderProgram* m_prog = nullptr;
    unsigned m_vao = 0, m_vbo = 0, m_imgTex = 0;
    QSize m_texSize;
    struct LutTex { unsigned id; unsigned target; QByteArray sampler; };
    std::vector<LutTex> m_luts;
    QString m_rendererInfo;

    // CPU fallback
    QImage m_cpuImage;
    bool m_cpuDirty = true;

    double m_zoom = 1.0;
    QPointF m_offset;
    bool m_fitMode = true;
    bool m_dragging = false;
    QPointF m_dragStart, m_offsetStart;
    QString m_message;
};
