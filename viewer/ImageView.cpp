#include "ImageView.h"

#include <QDragEnterEvent>
#include <QMimeData>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>

namespace {
const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 corner;
uniform vec2 viewSize;
uniform vec2 origin;
uniform vec2 drawSize;
out vec2 uv;
void main() {
    vec2 px = origin + corner * drawSize;
    gl_Position = vec4(px.x / viewSize.x * 2.0 - 1.0, 1.0 - px.y / viewSize.y * 2.0, 0.0, 1.0);
    uv = corner;
}
)";

const char* kFragmentHead = R"(#version 330 core
in vec2 uv;
out vec4 fragColor;
uniform sampler2D exrv_image;
uniform int exrv_channel;
uniform float exrv_invGamma;
)";

const char* kFragmentMain = R"(
void main() {
    vec4 c = texture(exrv_image, uv);
    if (any(isnan(c))) c = vec4(0.0);
    if (exrv_channel == 4) {
        float a = clamp(c.a, 0.0, 1.0);
        fragColor = vec4(a, a, a, 1.0);
        return;
    }
    if (exrv_channel == 1) c.rgb = vec3(c.r);
    else if (exrv_channel == 2) c.rgb = vec3(c.g);
    else if (exrv_channel == 3) c.rgb = vec3(c.b);
    else if (exrv_channel == 5) c.rgb = vec3(dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)));
    c = OCIODisplay(c);
    vec3 o = clamp(c.rgb, 0.0, 1.0);
    if (any(isnan(o))) o = vec3(0.0);
    fragColor = vec4(pow(o, vec3(exrv_invGamma)), 1.0);
}
)";
} // namespace

ImageView::ImageView(exrv::ColorPipeline* pipeline, QWidget* parent) : QOpenGLWidget(parent), m_pipe(pipeline)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
}

ImageView::~ImageView()
{
    if (!m_glOk) return;
    makeCurrent();
    freeLuts();
    if (m_imgTex) glDeleteTextures(1, &m_imgTex);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    delete m_prog;
    doneCurrent();
}

// ---------------------------------------------------------------- API
QSize ImageView::imageSize() const { return m_src ? QSize(m_src->width, m_src->height) : QSize(); }

void ImageView::setSource(std::shared_ptr<const exrv::Image> img, bool keepView)
{
    bool sizeChanged = !m_src || !img || img->width != m_src->width || img->height != m_src->height;
    m_src = std::move(img);
    m_texDirty = true;
    m_cpuDirty = true;
    m_message.clear();
    if (!keepView || sizeChanged) {
        if (m_fitMode || !keepView) fit();
    }
    update();
}

void ImageView::clearSource()
{
    m_src.reset();
    m_cpuImage = QImage();
    update();
}

void ImageView::setViewSettings(const exrv::ViewSettings& vs)
{
    if (vs.input != m_vs.input || vs.display != m_vs.display || vs.view != m_vs.view) m_shaderDirty = true;
    m_vs = vs;
    m_cpuDirty = true;
    update();
}

void ImageView::colorConfigChanged()
{
    m_shaderDirty = true;
    m_cpuDirty = true;
    update();
}

void ImageView::setMessage(const QString& msg)
{
    m_message = msg;
    update();
}

void ImageView::fit()
{
    m_fitMode = true;
    QSize s = imageSize();
    if (s.isEmpty()) return;
    double z = qMin((double)width() / s.width(), (double)height() / s.height());
    if (z <= 0) z = 1;
    m_zoom = z;
    m_offset = QPointF((width() - s.width() * z) / 2.0, (height() - s.height() * z) / 2.0);
    emit zoomChanged(m_zoom);
    update();
}

void ImageView::setZoom(double z, QPointF anchor)
{
    if (imageSize().isEmpty()) return;
    z = qBound(0.01, z, 64.0);
    if (anchor.x() < 0) anchor = QPointF(width() / 2.0, height() / 2.0);
    QPointF img = widgetToImage(anchor);
    m_zoom = z;
    m_offset = anchor - img * z;
    m_fitMode = false;
    emit zoomChanged(m_zoom);
    update();
}

QPointF ImageView::widgetToImage(QPointF p) const { return (p - m_offset) / m_zoom; }

// ---------------------------------------------------------------- GL
void ImageView::initializeGL()
{
    initializeOpenGLFunctions();
    QSurfaceFormat f = context()->format();
    bool versionOk = f.majorVersion() > 3 || (f.majorVersion() == 3 && f.minorVersion() >= 3);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    QString name = renderer ? QString::fromLatin1(renderer) : QStringLiteral("OpenGL");
    if (!versionOk || context()->isOpenGLES()) {
        m_glOk = false;
        setCpuMode(true, tr("CPU (OpenGL %1.%2 không đủ 3.3)").arg(f.majorVersion()).arg(f.minorVersion()));
        return;
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTex);
    static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    m_glOk = true;
    m_rendererInfo = QStringLiteral("GPU: ") + name;
    m_shaderDirty = m_texDirty = true;
    emit rendererChanged(m_rendererInfo);
}

void ImageView::setCpuMode(bool on, const QString& reason)
{
    QString info = on ? reason : QStringLiteral("GPU: ") + QString::fromLatin1((const char*)glGetString(GL_RENDERER));
    m_cpuMode = on;
    if (info != m_rendererInfo) {
        m_rendererInfo = info;
        emit rendererChanged(info);
    }
}

void ImageView::freeLuts()
{
    for (auto& l : m_luts) glDeleteTextures(1, &l.id);
    m_luts.clear();
}

bool ImageView::rebuildShader(QString* err)
{
    exrv::GpuShader sh;
    std::string e;
    if (!m_pipe->buildGpuShader(m_vs.input, m_vs.display, m_vs.view, sh, &e)) {
        *err = QString::fromStdString(e);
        return false;
    }
    auto* prog = new QOpenGLShaderProgram;
    QByteArray frag = QByteArray(kFragmentHead) + QByteArray::fromStdString(sh.text) + kFragmentMain;
    if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex) ||
        !prog->addShaderFromSourceCode(QOpenGLShader::Fragment, frag) || !prog->link()) {
        *err = prog->log();
        delete prog;
        return false;
    }
    delete m_prog;
    m_prog = prog;

    freeLuts();
    for (const auto& lut : sh.luts) {
        LutTex t{};
        t.sampler = QByteArray::fromStdString(lut.samplerName);
        glGenTextures(1, &t.id);
        GLint filt = lut.linear ? GL_LINEAR : GL_NEAREST;
        if (lut.depth > 0) {
            t.target = GL_TEXTURE_3D;
            glBindTexture(GL_TEXTURE_3D, t.id);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, filt);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, filt);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F, lut.width, lut.height, lut.depth, 0, GL_RGB, GL_FLOAT,
                         lut.values.data());
        } else {
            t.target = GL_TEXTURE_2D;
            glBindTexture(GL_TEXTURE_2D, t.id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            if (lut.channels == 1)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, lut.width, lut.height, 0, GL_RED, GL_FLOAT, lut.values.data());
            else
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, lut.width, lut.height, 0, GL_RGB, GL_FLOAT, lut.values.data());
        }
        m_luts.push_back(t);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void ImageView::uploadImage()
{
    if (!m_imgTex) glGenTextures(1, &m_imgTex);
    glBindTexture(GL_TEXTURE_2D, m_imgTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // RGBA32F: giữ nguyên giá trị lớn (depth, position) — RGBA16F sẽ tràn thành Inf > 65504.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, m_src->width, m_src->height, 0, GL_RGBA, GL_FLOAT, m_src->rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_texSize = QSize(m_src->width, m_src->height);
    m_texDirty = false;
}

void ImageView::paintGL()
{
    QColor bg(38, 38, 40);
    if (m_glOk) {
        glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (m_src && !m_src->empty()) {
        bool useCpu = !m_glOk;
        if (m_glOk) {
            bool tooBig = m_src->width > m_maxTex || m_src->height > m_maxTex;
            if (tooBig) {
                setCpuMode(true, tr("CPU (ảnh lớn hơn %1 px)").arg(m_maxTex));
                useCpu = true;
            } else {
                if (m_shaderDirty) {
                    QString err;
                    m_shaderDirty = false;
                    if (!rebuildShader(&err)) {
                        delete m_prog;
                        m_prog = nullptr;
                        qWarning("OCIO GPU shader: %s", qPrintable(err));
                    }
                }
                if (!m_prog) {
                    setCpuMode(true, tr("CPU (shader GPU lỗi)"));
                    useCpu = true;
                } else {
                    setCpuMode(false, QString());
                }
            }
        }
        if (useCpu) drawCpu();
        else drawGpu();
    }
    drawOverlay();
}

void ImageView::drawGpu()
{
    if (m_texDirty) uploadImage();
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, (GLsizei)(width() * dpr), (GLsizei)(height() * dpr));
    m_prog->bind();
    m_prog->setUniformValue("viewSize", QVector2D(width(), height()));
    m_prog->setUniformValue("origin", QVector2D(m_offset));
    m_prog->setUniformValue("drawSize", QVector2D(m_src->width * m_zoom, m_src->height * m_zoom));
    m_prog->setUniformValue("exrv_channel", (int)m_vs.channel);
    m_prog->setUniformValue("exrv_invGamma", 1.0f / qMax(0.01f, m_vs.gamma));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_imgTex);
    // Thu nhỏ: mipmap (lọc trong không gian tuyến tính, trước OCIO). Phóng to: giữ pixel sắc nét.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_zoom < 1.0 ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_zoom < 1.0 ? GL_LINEAR : GL_NEAREST);
    m_prog->setUniformValue("exrv_image", 0);
    for (size_t i = 0; i < m_luts.size(); ++i) {
        glActiveTexture(GL_TEXTURE1 + (GLenum)i);
        glBindTexture(m_luts[i].target, m_luts[i].id);
        m_prog->setUniformValue(m_luts[i].sampler.constData(), (int)(i + 1));
    }
    GLuint pid = m_prog->programId();
    for (const auto& u : m_pipe->gpuUniforms(m_vs.exposure)) {
        GLint loc = glGetUniformLocation(pid, u.name.c_str());
        if (loc < 0) continue;
        switch (u.type) {
        case exrv::GpuUniformValue::Double:
            if (!u.f.empty()) glUniform1f(loc, u.f[0]);
            break;
        case exrv::GpuUniformValue::Bool:
            if (!u.f.empty()) glUniform1i(loc, u.f[0] != 0.f);
            break;
        case exrv::GpuUniformValue::Float3:
            if (u.f.size() >= 3) glUniform3f(loc, u.f[0], u.f[1], u.f[2]);
            break;
        case exrv::GpuUniformValue::VectorFloat:
            if (!u.f.empty()) glUniform1fv(loc, (GLsizei)u.f.size(), u.f.data());
            break;
        case exrv::GpuUniformValue::VectorInt:
            if (!u.i.empty()) glUniform1iv(loc, (GLsizei)u.i.size(), u.i.data());
            break;
        }
    }
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    m_prog->release();
    for (size_t i = 0; i < m_luts.size(); ++i) {
        glActiveTexture(GL_TEXTURE1 + (GLenum)i);
        glBindTexture(m_luts[i].target, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ImageView::drawCpu()
{
    if (m_cpuDirty || m_cpuImage.size() != imageSize()) {
        std::vector<uint8_t> bgra;
        std::string err;
        if (m_pipe->render(*m_src, m_vs, bgra, &err)) {
            QImage q((const uchar*)bgra.data(), m_src->width, m_src->height, m_src->width * 4, QImage::Format_RGB32);
            m_cpuImage = q.copy();
        } else {
            m_cpuImage = QImage();
            m_message = tr("Lỗi OCIO: %1").arg(QString::fromStdString(err));
        }
        m_cpuDirty = false;
    }
    if (m_cpuImage.isNull()) return;
    QPainter p(this);
    if (!m_glOk) p.fillRect(rect(), QColor(38, 38, 40));
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
    p.drawImage(QRectF(m_offset, QSizeF(m_src->width * m_zoom, m_src->height * m_zoom)), m_cpuImage);
}

void ImageView::drawOverlay()
{
    QPainter p(this);
    if (!m_glOk && (!m_src || m_src->empty())) p.fillRect(rect(), QColor(38, 38, 40));
    if (!m_src || m_src->empty()) {
        p.setPen(QColor(150, 150, 155));
        p.drawText(rect(), Qt::AlignCenter,
                   m_message.isEmpty() ? tr("Kéo thả file .exr vào đây hoặc nhấn Ctrl+O") : m_message);
        return;
    }
    QRectF target(m_offset, QSizeF(m_src->width * m_zoom, m_src->height * m_zoom));
    p.setPen(QPen(QColor(90, 90, 95), 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(target.adjusted(-0.5, -0.5, 0.5, 0.5));
    if (!m_message.isEmpty()) {
        p.setPen(QColor(255, 180, 80));
        p.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignTop | Qt::AlignLeft, m_message);
    }
}

// ---------------------------------------------------------------- Chuột
void ImageView::wheelEvent(QWheelEvent* e)
{
    double steps = e->angleDelta().y() / 120.0;
    if (steps == 0) return;
    setZoom(m_zoom * qPow(1.25, steps), e->position());
    emitHover(e->position());
}

void ImageView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
        m_dragging = true;
        m_dragStart = e->position();
        m_offsetStart = m_offset;
        setCursor(Qt::ClosedHandCursor);
    }
}

void ImageView::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging) {
        m_offset = m_offsetStart + (e->position() - m_dragStart);
        m_fitMode = false;
        update();
    }
    emitHover(e->position());
}

void ImageView::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
    unsetCursor();
}

void ImageView::mouseDoubleClickEvent(QMouseEvent*) { fit(); }

void ImageView::resizeEvent(QResizeEvent* e)
{
    QOpenGLWidget::resizeEvent(e);
    if (m_fitMode) fit();
}

void ImageView::leaveEvent(QEvent*) { emit hovered(-1, -1, false); }

void ImageView::emitHover(QPointF widgetPos)
{
    QSize s = imageSize();
    if (s.isEmpty()) return;
    QPointF ip = widgetToImage(widgetPos);
    int x = (int)qFloor(ip.x()), y = (int)qFloor(ip.y());
    emit hovered(x, y, x >= 0 && y >= 0 && x < s.width() && y < s.height());
}

void ImageView::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void ImageView::dropEvent(QDropEvent* e)
{
    for (const QUrl& u : e->mimeData()->urls()) {
        QString f = u.toLocalFile();
        if (!f.isEmpty()) {
            emit fileDropped(f);
            break;
        }
    }
}
