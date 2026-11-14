#include "ImageView.h"

#include <QDragEnterEvent>
#include <QMimeData>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QVector2D>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 corner;
uniform vec2 viewSize;
uniform vec2 origin;
uniform vec2 drawSize;
uniform int flipH;
uniform int flipV;
uniform int rot90;
out vec2 uv;
void main() {
    vec2 px = origin + corner * drawSize;
    gl_Position = vec4(px.x / viewSize.x * 2.0 - 1.0, 1.0 - px.y / viewSize.y * 2.0, 0.0, 1.0);
    vec2 t = corner;
    if (flipH != 0) t.x = 1.0 - t.x;
    if (flipV != 0) t.y = 1.0 - t.y;
    if (rot90 == 1) t = vec2(t.y, 1.0 - t.x);
    else if (rot90 == 2) t = vec2(1.0 - t.x, 1.0 - t.y);
    else if (rot90 == 3) t = vec2(1.0 - t.y, t.x);
    uv = t;
}
)";

const char* kFragmentHead = R"(#version 330 core
in vec2 uv;
out vec4 fragColor;
uniform sampler2D exrv_image;
uniform int exrv_channel;
uniform float exrv_invGamma;
uniform int exrv_bgMode;
uniform int exrv_inspectMode;
uniform float exrv_exposure;
)";

const char* kFragmentMain = R"(
vec3 exrv_falseColor(float lum) {
    // Bản đồ False Color theo stops quanh 18% gray (0.18)
    float stops = log2(max(lum, 1e-6) / 0.18);
    // -6 -> tím, -4 -> xanh dương, -2 -> cyan, 0 -> xanh lá, +1 -> hồng, +2.5 -> vàng, +4 -> đỏ
    vec3 c;
    if (stops < -4.0) {
        c = mix(vec3(0.18, 0.0, 0.35), vec3(0.0, 0.15, 0.85), clamp((stops + 6.0) / 2.0, 0.0, 1.0));
    } else if (stops < -2.0) {
        c = mix(vec3(0.0, 0.15, 0.85), vec3(0.0, 0.75, 0.85), (stops + 4.0) / 2.0);
    } else if (stops < -0.3) {
        c = mix(vec3(0.0, 0.75, 0.85), vec3(0.15, 0.75, 0.2), (stops + 2.0) / 1.7);
    } else if (stops < 0.3) {
        c = vec3(0.15, 0.8, 0.25); // 18% gray zone
    } else if (stops < 1.3) {
        c = mix(vec3(0.45, 0.75, 0.2), vec3(0.95, 0.55, 0.65), (stops - 0.3) / 1.0);
    } else if (stops < 2.8) {
        c = mix(vec3(0.95, 0.55, 0.65), vec3(0.98, 0.9, 0.15), (stops - 1.3) / 1.5);
    } else {
        c = mix(vec3(0.98, 0.9, 0.15), vec3(1.0, 0.1, 0.05), clamp((stops - 2.8) / 1.5, 0.0, 1.0));
    }
    return c;
}

void main() {
    vec4 raw = texture(exrv_image, uv);
    bool badNan = any(isnan(raw.rgb)) || any(isinf(raw.rgb));
    if (exrv_inspectMode == 2) {
        if (badNan) { fragColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
        if (any(lessThan(raw.rgb, vec3(0.0)))) { fragColor = vec4(0.0, 0.95, 1.0, 1.0); return; }
        vec3 scaled = raw.rgb * exp2(exrv_exposure);
        if (any(greaterThan(scaled, vec3(1.0)))) {
            float stripe = mod(floor((gl_FragCoord.x + gl_FragCoord.y) * 0.25), 2.0);
            fragColor = mix(vec4(1.0, 0.18, 0.05, 1.0), vec4(1.0, 0.85, 0.1, 1.0), stripe);
            return;
        }
    }

    vec4 c = raw;
    if (any(isnan(c))) c = vec4(0.0);
    if (exrv_channel == 4) {
        float a = clamp(c.a, 0.0, 1.0);
        fragColor = vec4(a, a, a, 1.0);
        return;
    }

    if (exrv_inspectMode == 1) {
        if (badNan) { fragColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
        float lum = dot(max(c.rgb * exp2(exrv_exposure), vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
        fragColor = vec4(exrv_falseColor(lum), 1.0);
        return;
    }

    if (exrv_channel == 1) c.rgb = vec3(c.r);
    else if (exrv_channel == 2) c.rgb = vec3(c.g);
    else if (exrv_channel == 3) c.rgb = vec3(c.b);
    else if (exrv_channel == 5) c.rgb = vec3(dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)));
    c = OCIODisplay(c);
    vec3 o = clamp(c.rgb, 0.0, 1.0);
    if (any(isnan(o))) o = vec3(0.0);
    o = pow(o, vec3(exrv_invGamma));
    if (exrv_inspectMode == 2) o *= 0.45;
    if (exrv_bgMode == 3) {
        ivec2 cell = ivec2(floor(gl_FragCoord.xy / 12.0));
        vec3 chk = ((cell.x + cell.y) & 1) == 0 ? vec3(0.18) : vec3(0.27);
        o = mix(chk, o, clamp(raw.a, 0.0, 1.0));
    }
    fragColor = vec4(o, 1.0);
}
)";

// |A − B| × gain trên giá trị gốc (tuyến tính, không qua OCIO): thấy đúng chỗ hai ảnh khác nhau.
// B được lấy mẫu theo toạ độ pixel của A (hai ảnh có thể khác kích thước).
const char* kDiffFragment = R"(#version 330 core
in vec2 uv;
out vec4 fragColor;
uniform sampler2D imgA;
uniform sampler2D imgB;
uniform vec2 scaleB;   // kích thước A / kích thước B
uniform float gain;
uniform int channel;
uniform float invGamma;
void main() {
    vec4 a = texture(imgA, uv);
    vec2 ub = uv * scaleB;
    vec4 b = (ub.x <= 1.0 && ub.y <= 1.0) ? texture(imgB, ub) : vec4(0.0);
    vec4 d = abs(a - b);
    if (any(isnan(d)) || any(isinf(d))) { fragColor = vec4(1.0, 0.0, 1.0, 1.0); return; } // NaN/Inf: tím
    vec3 v;
    if (channel == 1) v = vec3(d.r);
    else if (channel == 2) v = vec3(d.g);
    else if (channel == 3) v = vec3(d.b);
    else if (channel == 4) v = vec3(d.a);
    else if (channel == 5) v = vec3(dot(d.rgb, vec3(0.2126, 0.7152, 0.0722)));
    else v = d.rgb;
    fragColor = vec4(pow(clamp(v * gain, 0.0, 1.0), vec3(invGamma)), 1.0);
}
)";

float channelValue(const float* d, exrv::ChannelMode ch)
{
    switch (ch) {
    case exrv::ChannelMode::Red: return d[0];
    case exrv::ChannelMode::Green: return d[1];
    case exrv::ChannelMode::Blue: return d[2];
    case exrv::ChannelMode::Alpha: return d[3];
    case exrv::ChannelMode::Luminance: return 0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2];
    default: return 0.f;
    }
}

QColor bgClearColor(ImageView::BgMode m)
{
    switch (m) {
    case ImageView::BgMode::Black: return QColor(0, 0, 0);
    case ImageView::BgMode::Gray: return QColor(72, 72, 74);
    default: return QColor(34, 34, 37);
    }
}
} // namespace

ImageView::ImageView(exrv::ColorPipeline* pipeline, QWidget* parent) : QOpenGLWidget(parent)
{
    m_layers[0].pipe = pipeline;
    m_layers[1].pipe = pipeline;
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
}

ImageView::~ImageView()
{
    if (!m_glOk) return;
    makeCurrent();
    for (auto& L : m_layers) freeLayerGl(L, true);
    delete m_diffProg;
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    doneCurrent();
}

// ---------------------------------------------------------------- API
QSize ImageView::imageSize() const
{
    const auto& s = m_layers[0].src;
    if (!s) return QSize();
    return (m_rot90 & 1) ? QSize(s->height, s->width) : QSize(s->width, s->height);
}

void ImageView::setSource(std::shared_ptr<const exrv::Image> img, bool keepView)
{
    Layer& L = m_layers[0];
    bool sizeChanged = !L.src || !img || img->width != L.src->width || img->height != L.src->height;
    L.src = std::move(img);
    L.texDirty = true;
    L.cpuDirty = true;
    m_diffDirty = true;
    m_message.clear();
    if (!keepView || sizeChanged) {
        if (m_fitMode || !keepView) fit();
    }
    update();
}

void ImageView::clearSource()
{
    m_layers[0].src.reset();
    m_layers[0].cpuImage = QImage();
    m_diffImage = QImage();
    update();
}

void ImageView::setViewSettings(const exrv::ViewSettings& vs)
{
    Layer& L = m_layers[0];
    if (vs.input != L.vs.input || vs.display != L.vs.display || vs.view != L.vs.view) L.shaderDirty = true;
    if (vs.exposure != L.vs.exposure || vs.channel != L.vs.channel || vs.gamma != L.vs.gamma) m_diffDirty = true;
    L.vs = vs;
    L.cpuDirty = true;
    update();
}

void ImageView::colorConfigChanged()
{
    m_layers[0].shaderDirty = true;
    m_layers[0].cpuDirty = true;
    update();
}

void ImageView::setCompareMode(CompareMode mode)
{
    if (mode == m_mode) return;
    m_mode = mode;
    // Pipeline B có thể đã dựng shader cho khung khác trong lúc chạy cạnh nhau -> dựng lại.
    m_layers[1].shaderDirty = true;
    m_layers[1].cpuDirty = true;
    m_diffDirty = true;
    if (mode == CompareMode::Off) {
        m_layers[1].src.reset();
        m_layers[1].cpuImage = QImage();
        m_diffImage = QImage();
    }
    unsetCursor();
    update();
}

void ImageView::setSecondPipeline(exrv::ColorPipeline* pipeline)
{
    m_layers[1].pipe = pipeline;
    secondColorConfigChanged();
}

void ImageView::setSecondSource(std::shared_ptr<const exrv::Image> img)
{
    Layer& L = m_layers[1];
    L.src = std::move(img);
    L.texDirty = true;
    L.cpuDirty = true;
    m_diffDirty = true;
    update();
}

void ImageView::setSecondViewSettings(const exrv::ViewSettings& vs)
{
    Layer& L = m_layers[1];
    if (vs.input != L.vs.input || vs.display != L.vs.display || vs.view != L.vs.view) L.shaderDirty = true;
    L.vs = vs;
    L.cpuDirty = true;
    update();
}

void ImageView::secondColorConfigChanged()
{
    m_layers[1].shaderDirty = true;
    m_layers[1].cpuDirty = true;
    update();
}

void ImageView::setBgMode(BgMode m)
{
    if (m_bgMode == m) return;
    m_bgMode = m;
    update();
}

void ImageView::setInspectMode(InspectMode m)
{
    if (m_inspectMode == m) return;
    m_inspectMode = m;
    m_layers[0].cpuDirty = true;
    m_layers[1].cpuDirty = true;
    update();
}

void ImageView::setPixelGrid(bool on)
{
    if (m_pixelGrid == on) return;
    m_pixelGrid = on;
    update();
}

void ImageView::setFlipH(bool on)
{
    if (m_flipH == on) return;
    m_flipH = on;
    update();
}

void ImageView::setFlipV(bool on)
{
    if (m_flipV == on) return;
    m_flipV = on;
    update();
}

void ImageView::setRot90(int steps)
{
    steps = ((steps % 4) + 4) % 4;
    if (m_rot90 == steps) return;
    bool oddChanged = (m_rot90 & 1) != (steps & 1);
    m_rot90 = steps;
    if (oddChanged && m_fitMode) fit();
    update();
}

void ImageView::setMessage(const QString& msg)
{
    m_message = msg;
    update();
}

void ImageView::setViewTransform(double zoom, QPointF offset, bool fitMode)
{
    if (fitMode) {
        fitSilently();
        return;
    }
    m_zoom = zoom;
    m_offset = offset;
    m_fitMode = false;
    emit zoomChanged(m_zoom);
    update();
}

void ImageView::fitSilently()
{
    QSize s = imageSize();
    m_fitMode = true;
    if (s.isEmpty()) return;
    double z = qMin((double)width() / s.width(), (double)height() / s.height());
    if (z <= 0) z = 1;
    m_zoom = z;
    m_offset = QPointF((width() - s.width() * z) / 2.0, (height() - s.height() * z) / 2.0);
    emit zoomChanged(m_zoom);
    update();
}

void ImageView::setLabel(const QString& label)
{
    m_label = label;
    update();
}

void ImageView::fit()
{
    fitSilently();
    if (!imageSize().isEmpty()) emit viewChanged(m_zoom, m_offset, m_fitMode);
}

void ImageView::setZoom(double z, QPointF anchor)
{
    if (imageSize().isEmpty()) return;
    z = qBound(0.01, z, 64.0);
    if (anchor.x() < 0) anchor = QPointF(width() / 2.0, height() / 2.0);
    QPointF img = (anchor - m_offset) / m_zoom;
    m_zoom = z;
    m_offset = anchor - img * z;
    m_fitMode = false;
    emit zoomChanged(m_zoom);
    emit viewChanged(m_zoom, m_offset, m_fitMode);
    update();
}

QPointF ImageView::displayToSource(QPointF dp) const
{
    const auto& s = m_layers[0].src;
    if (!s || s->empty()) return dp;
    QSize ds = imageSize();
    double u = dp.x() / ds.width();
    double v = dp.y() / ds.height();
    if (m_flipH) u = 1.0 - u;
    if (m_flipV) v = 1.0 - v;
    double su = u, sv = v;
    if (m_rot90 == 1) { su = v; sv = 1.0 - u; }
    else if (m_rot90 == 2) { su = 1.0 - u; sv = 1.0 - v; }
    else if (m_rot90 == 3) { su = 1.0 - v; sv = u; }
    return QPointF(su * s->width, sv * s->height);
}

QPointF ImageView::widgetToSourceImage(QPointF p) const
{
    return displayToSource((p - m_offset) / m_zoom);
}

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
    for (auto& L : m_layers) L.shaderDirty = L.texDirty = true;
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

void ImageView::freeLayerGl(Layer& L, bool texToo)
{
    for (auto& l : L.luts) glDeleteTextures(1, &l.id);
    L.luts.clear();
    L.uniformLocs.clear();
    delete L.prog;
    L.prog = nullptr;
    if (texToo) {
        if (L.tex) {
            glDeleteTextures(1, &L.tex);
            L.tex = 0;
            L.texW = L.texH = 0;
        }
        for (int i = 0; i < 2; ++i) {
            if (L.pbo[i]) {
                glDeleteBuffers(1, &L.pbo[i]);
                L.pbo[i] = 0;
                L.pboBytes[i] = 0;
            }
        }
    }
}

bool ImageView::rebuildShader(Layer& L, QString* err)
{
    exrv::GpuShader sh;
    std::string e;
    if (!L.pipe || !L.pipe->buildGpuShader(L.vs.input, L.vs.display, L.vs.view, sh, &e)) {
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
    freeLayerGl(L, false);
    L.prog = prog;

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
        L.luts.push_back(t);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool ImageView::buildDiffShader(QString* err)
{
    auto* prog = new QOpenGLShaderProgram;
    if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex) ||
        !prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kDiffFragment) || !prog->link()) {
        *err = prog->log();
        delete prog;
        return false;
    }
    m_diffProg = prog;
    return true;
}

void ImageView::uploadImage(Layer& L)
{
    if (!L.tex) glGenTextures(1, &L.tex);
    glBindTexture(GL_TEXTURE_2D, L.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const bool half = L.src->isHalf();
    const GLenum type = half ? GL_HALF_FLOAT : GL_FLOAT;
    const void* data = half ? (const void*)L.src->rgbaHalf.data() : (const void*)L.src->rgba.data();
    const size_t dataBytes = L.src->bytes();

    if (L.texW == L.src->width && L.texH == L.src->height && L.texHalf == half) {
        // Dùng double-buffered PBO khi play sequence để DMA bất đồng bộ, tránh nghẽn CPU-GPU.
        bool uploadedViaPbo = false;
        if (m_playback && dataBytes > 0) {
            int idx = L.pboIdx;
            L.pboIdx = (L.pboIdx + 1) & 1;
            if (!L.pbo[idx]) glGenBuffers(1, &L.pbo[idx]);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, L.pbo[idx]);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)dataBytes, nullptr, GL_STREAM_DRAW);
            void* mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)dataBytes,
                                            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (mapped) {
                std::memcpy(mapped, data, dataBytes);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L.src->width, L.src->height, GL_RGBA, type, nullptr);
                uploadedViaPbo = true;
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        if (!uploadedViaPbo)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L.src->width, L.src->height, GL_RGBA, type, data);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, half ? GL_RGBA16F : GL_RGBA32F, L.src->width, L.src->height, 0, GL_RGBA, type,
                     data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        L.texW = L.src->width;
        L.texH = L.src->height;
        L.texHalf = half;
    }
    L.mipsValid = false;
    glBindTexture(GL_TEXTURE_2D, 0);
    L.texDirty = false;
}

void ImageView::bindFiltered(Layer& L)
{
    const bool minify = m_zoom < 1.0;
    const bool mips = minify && !m_playback;
    if (mips && !L.mipsValid) {
        glGenerateMipmap(GL_TEXTURE_2D);
        L.mipsValid = true;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mips ? GL_LINEAR_MIPMAP_LINEAR : (minify ? GL_LINEAR : GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, minify ? GL_LINEAR : GL_NEAREST);
}

void ImageView::setPlayback(bool on)
{
    if (on == m_playback) return;
    m_playback = on;
    update();
}

void ImageView::paintGL()
{
    QColor bg = bgClearColor(m_bgMode);
    if (m_glOk) {
        glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (m_layers[0].valid()) {
        const bool two = twoLayers();
        bool useCpu = !m_glOk;
        if (m_glOk) {
            QString reason;
            for (int i = 0; i < (two ? 2 : 1) && reason.isEmpty(); ++i) {
                Layer& L = m_layers[i];
                if (L.src->width > m_maxTex || L.src->height > m_maxTex) {
                    reason = tr("CPU (ảnh lớn hơn %1 px)").arg(m_maxTex);
                    break;
                }
                if (m_mode == CompareMode::Difference && two) continue;
                if (L.shaderDirty) {
                    QString err;
                    L.shaderDirty = false;
                    if (!rebuildShader(L, &err)) {
                        freeLayerGl(L, false);
                        qWarning("OCIO GPU shader: %s", qPrintable(err));
                    }
                }
                if (!L.prog) reason = tr("CPU (shader GPU lỗi)");
            }
            if (reason.isEmpty() && two && m_mode == CompareMode::Difference && !m_diffProg) {
                QString err;
                if (!buildDiffShader(&err)) {
                    qWarning("Difference shader: %s", qPrintable(err));
                    reason = tr("CPU (shader GPU lỗi)");
                }
            }
            useCpu = !reason.isEmpty();
            setCpuMode(useCpu, reason);
        }
        if (useCpu) {
            drawCpu();
        } else if (two && m_mode == CompareMode::Difference) {
            drawDiffGpu();
        } else if (two) {
            const qreal dpr = devicePixelRatioF();
            const int W = (int)std::lround(width() * dpr), H = (int)std::lround(height() * dpr);
            const int split = qBound(0, (int)std::lround(wipeX() * dpr), W);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, split, H);
            drawLayerGpu(m_layers[0]);
            glScissor(split, 0, W - split, H);
            drawLayerGpu(m_layers[1]);
            glDisable(GL_SCISSOR_TEST);
        } else {
            drawLayerGpu(m_layers[0]);
        }
    }
    drawOverlay();
}

void ImageView::drawLayerGpu(Layer& L)
{
    if (L.texDirty) uploadImage(L);
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, (GLsizei)(width() * dpr), (GLsizei)(height() * dpr));
    const QSize disp = (m_rot90 & 1) ? QSize(L.src->height, L.src->width) : QSize(L.src->width, L.src->height);
    L.prog->bind();
    L.prog->setUniformValue("viewSize", QVector2D(width(), height()));
    L.prog->setUniformValue("origin", QVector2D(m_offset));
    L.prog->setUniformValue("drawSize", QVector2D(disp.width() * m_zoom, disp.height() * m_zoom));
    L.prog->setUniformValue("flipH", m_flipH ? 1 : 0);
    L.prog->setUniformValue("flipV", m_flipV ? 1 : 0);
    L.prog->setUniformValue("rot90", m_rot90);
    L.prog->setUniformValue("exrv_channel", (int)L.vs.channel);
    L.prog->setUniformValue("exrv_invGamma", 1.0f / qMax(0.01f, L.vs.gamma));
    L.prog->setUniformValue("exrv_bgMode", (int)m_bgMode);
    L.prog->setUniformValue("exrv_inspectMode", (int)m_inspectMode);
    L.prog->setUniformValue("exrv_exposure", L.vs.exposure);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, L.tex);
    bindFiltered(L);
    L.prog->setUniformValue("exrv_image", 0);
    for (size_t i = 0; i < L.luts.size(); ++i) {
        glActiveTexture(GL_TEXTURE1 + (GLenum)i);
        glBindTexture(L.luts[i].target, L.luts[i].id);
        L.prog->setUniformValue(L.luts[i].sampler.constData(), (int)(i + 1));
    }
    GLuint pid = L.prog->programId();
    for (const auto& u : L.pipe->gpuUniforms(L.vs.exposure)) {
        QByteArray key = QByteArray::fromStdString(u.name);
        auto it = L.uniformLocs.constFind(key);
        GLint loc = (it != L.uniformLocs.constEnd()) ? *it : glGetUniformLocation(pid, u.name.c_str());
        if (it == L.uniformLocs.constEnd()) L.uniformLocs.insert(key, loc);
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
    L.prog->release();
    for (size_t i = 0; i < L.luts.size(); ++i) {
        glActiveTexture(GL_TEXTURE1 + (GLenum)i);
        glBindTexture(L.luts[i].target, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ImageView::drawDiffGpu()
{
    Layer& A = m_layers[0];
    Layer& B = m_layers[1];
    if (A.texDirty) uploadImage(A);
    if (B.texDirty) uploadImage(B);
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, (GLsizei)(width() * dpr), (GLsizei)(height() * dpr));
    const QSize disp = imageSize();
    m_diffProg->bind();
    m_diffProg->setUniformValue("viewSize", QVector2D(width(), height()));
    m_diffProg->setUniformValue("origin", QVector2D(m_offset));
    m_diffProg->setUniformValue("drawSize", QVector2D(disp.width() * m_zoom, disp.height() * m_zoom));
    m_diffProg->setUniformValue("flipH", m_flipH ? 1 : 0);
    m_diffProg->setUniformValue("flipV", m_flipV ? 1 : 0);
    m_diffProg->setUniformValue("rot90", m_rot90);
    m_diffProg->setUniformValue("scaleB", QVector2D((float)A.src->width / B.src->width, (float)A.src->height / B.src->height));
    m_diffProg->setUniformValue("gain", (float)std::exp2(A.vs.exposure));
    m_diffProg->setUniformValue("channel", (int)A.vs.channel);
    m_diffProg->setUniformValue("invGamma", 1.0f / qMax(0.01f, A.vs.gamma));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, A.tex);
    bindFiltered(A);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, B.tex);
    bindFiltered(B);
    m_diffProg->setUniformValue("imgA", 0);
    m_diffProg->setUniformValue("imgB", 1);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    m_diffProg->release();
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------- CPU fallback
bool ImageView::updateCpuImage(Layer& L)
{
    if (!L.cpuDirty && L.cpuImage.size() == QSize(L.src->width, L.src->height)) return !L.cpuImage.isNull();
    std::vector<uint8_t> bgra;
    std::string err;
    if (L.pipe && L.pipe->render(*L.src, L.vs, bgra, &err)) {
        QImage q((const uchar*)bgra.data(), L.src->width, L.src->height, L.src->width * 4, QImage::Format_RGB32);
        L.cpuImage = q.copy();
    } else {
        L.cpuImage = QImage();
        m_message = tr("Lỗi OCIO: %1").arg(QString::fromStdString(err));
    }
    L.cpuDirty = false;
    return !L.cpuImage.isNull();
}

void ImageView::updateDiffCpu()
{
    if (!m_diffDirty && !m_diffImage.isNull()) return;
    const exrv::Image& a = *m_layers[0].src;
    const exrv::Image& b = *m_layers[1].src;
    const auto& vs = m_layers[0].vs;
    const float gain = (float)std::exp2(vs.exposure);
    const double invG = 1.0 / qMax(0.01f, vs.gamma);
    QImage img(a.width, a.height, QImage::Format_RGB32);
    float pa[4], pb[4], d[4];
    for (int y = 0; y < a.height; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < a.width; ++x) {
            a.pixel(x, y, pa);
            if (x < b.width && y < b.height) b.pixel(x, y, pb);
            else pb[0] = pb[1] = pb[2] = pb[3] = 0.f;
            bool bad = false;
            for (int c = 0; c < 4; ++c) {
                d[c] = std::fabs(pa[c] - pb[c]);
                if (!std::isfinite(d[c])) bad = true;
            }
            if (bad) {
                line[x] = qRgb(255, 0, 255);
                continue;
            }
            float v[3];
            if (vs.channel == exrv::ChannelMode::RGB) {
                v[0] = d[0]; v[1] = d[1]; v[2] = d[2];
            } else {
                v[0] = v[1] = v[2] = channelValue(d, vs.channel);
            }
            int o[3];
            for (int c = 0; c < 3; ++c)
                o[c] = (int)std::lround(std::pow((double)qBound(0.f, v[c] * gain, 1.f), invG) * 255.0);
            line[x] = qRgb(o[0], o[1], o[2]);
        }
    }
    m_diffImage = img;
    m_diffDirty = false;
}

void ImageView::drawCpu()
{
    Layer& A = m_layers[0];
    const bool two = twoLayers();
    QPainter p(this);
    if (!m_glOk) p.fillRect(rect(), bgClearColor(m_bgMode));
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
    auto transformImg = [&](const QImage& img) -> QImage {
        QImage out = img;
        if (m_rot90 != 0) {
            QTransform tr;
            tr.rotate(m_rot90 * 90);
            out = out.transformed(tr);
        }
        if (m_flipH || m_flipV) out = out.mirrored(m_flipH, m_flipV);
        return out;
    };
    const QSize ds = imageSize();
    const QRectF target(m_offset, QSizeF(ds.width() * m_zoom, ds.height() * m_zoom));
    if (two && m_mode == CompareMode::Difference) {
        updateDiffCpu();
        p.drawImage(target, transformImg(m_diffImage));
        return;
    }
    if (two) {
        Layer& B = m_layers[1];
        const double split = wipeX();
        if (updateCpuImage(A)) {
            p.setClipRect(QRectF(0, 0, split, height()));
            p.drawImage(target, transformImg(A.cpuImage));
        }
        if (updateCpuImage(B)) {
            p.setClipRect(QRectF(split, 0, width() - split, height()));
            p.drawImage(target, transformImg(B.cpuImage));
        }
        return;
    }
    if (updateCpuImage(A)) p.drawImage(target, transformImg(A.cpuImage));
}

void ImageView::drawOverlay()
{
    QPainter p(this);
    const auto& src = m_layers[0].src;
    if (!m_glOk && (!src || src->empty())) p.fillRect(rect(), bgClearColor(m_bgMode));
    auto badge = [&](const QRectF& r, const QString& text) {
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() * 1.25);
        p.setFont(f);
        p.fillRect(r, QColor(0, 0, 0, 155));
        p.setPen(QColor(255, 255, 255));
        p.drawText(r, Qt::AlignCenter, text);
        p.setFont(font());
    };
    const bool two = twoLayers();
    if (two && m_mode == CompareMode::Wipe) {
        const double x = wipeX();
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
        p.setPen(QPen(QColor(255, 255, 255), 1));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
        p.setBrush(QColor(255, 255, 255));
        p.drawEllipse(QPointF(x, height() / 2.0), 6, 6);
        p.setBrush(Qt::NoBrush);
        badge(QRectF(x - 44, 8, 36, 26), "A");
        badge(QRectF(x + 8, 8, 36, 26), "B");
    } else if (two && m_mode == CompareMode::Difference) {
        badge(QRectF(width() - 104, 8, 96, 26), "|A − B|");
    } else if (!m_label.isEmpty()) {
        badge(QRectF(width() - 44, 8, 36, 26), m_label);
    }

    if (m_inspectMode == InspectMode::FalseColor)
        badge(QRectF(10, 8, 108, 24), QStringLiteral("FALSE COLOR"));
    else if (m_inspectMode == InspectMode::Clipping)
        badge(QRectF(10, 8, 128, 24), QStringLiteral("CLIP / NaN / NEG"));

    if (!src || src->empty()) {
        p.setPen(QColor(150, 150, 155));
        p.drawText(rect(), Qt::AlignCenter,
                   m_message.isEmpty() ? tr("Kéo thả file .exr vào đây hoặc nhấn Ctrl+O") : m_message);
        return;
    }

    const QSize ds = imageSize();
    QRectF target(m_offset, QSizeF(ds.width() * m_zoom, ds.height() * m_zoom));
    p.setPen(QPen(QColor(90, 90, 95), 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(target.adjusted(-0.5, -0.5, 0.5, 0.5));

    // Lưới pixel khi zoom >= 800%
    if (m_pixelGrid && m_zoom >= 8.0 && !m_playback) {
        QRectF vis = target.intersected(QRectF(rect()));
        if (!vis.isEmpty()) {
            int x0 = qMax(0, (int)qFloor((vis.left() - m_offset.x()) / m_zoom));
            int x1 = qMin(ds.width(), (int)qCeil((vis.right() - m_offset.x()) / m_zoom));
            int y0 = qMax(0, (int)qFloor((vis.top() - m_offset.y()) / m_zoom));
            int y1 = qMin(ds.height(), (int)qCeil((vis.bottom() - m_offset.y()) / m_zoom));
            p.setPen(QPen(QColor(255, 255, 255, m_zoom >= 16.0 ? 45 : 25), 1));
            for (int x = x0; x <= x1; ++x) {
                double sx = m_offset.x() + x * m_zoom;
                p.drawLine(QPointF(sx, vis.top()), QPointF(sx, vis.bottom()));
            }
            for (int y = y0; y <= y1; ++y) {
                double sy = m_offset.y() + y * m_zoom;
                p.drawLine(QPointF(vis.left(), sy), QPointF(vis.right(), sy));
            }
        }
    }

    if (src->hasOverscan() && m_rot90 == 0 && !m_flipH && !m_flipV) {
        QRectF disp(m_offset + QPointF(src->dispX, src->dispY) * m_zoom,
                    QSizeF(src->dispW * m_zoom, src->dispH * m_zoom));
        p.setPen(QPen(QColor(255, 200, 60), 1, Qt::SolidLine));
        p.drawRect(disp.adjusted(-0.5, -0.5, 0.5, 0.5));
        p.drawText(disp.topLeft() + QPointF(4, -4), tr("display window"));
    }
    if (m_selecting) {
        QRectF sr = QRectF(m_selStart, m_selEnd).normalized();
        p.setPen(QPen(QColor(232, 140, 40), 1.5, Qt::DashLine));
        p.setBrush(QColor(232, 140, 40, 28));
        p.drawRect(sr);
        p.setBrush(Qt::NoBrush);
    }
    if (two && m_mode == CompareMode::Difference) {
        p.setPen(QColor(200, 200, 205));
        p.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignBottom | Qt::AlignLeft,
                   tr("Khuếch đại ×%1 (exposure A) · tím = NaN/Inf").arg(std::exp2(m_layers[0].vs.exposure), 0, 'g', 3));
    }
    if (!m_message.isEmpty()) {
        p.setPen(QColor(255, 180, 80));
        p.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignTop | Qt::AlignLeft, m_message);
    }
}

// ---------------------------------------------------------------- Chuột
bool ImageView::nearWipe(QPointF p) const
{
    return twoLayers() && m_mode == CompareMode::Wipe && std::fabs(p.x() - wipeX()) <= 6.0;
}

void ImageView::wheelEvent(QWheelEvent* e)
{
    double steps = e->angleDelta().y() / 120.0;
    if (steps == 0) return;
    setZoom(m_zoom * qPow(1.25, steps), e->position());
    emitHover(e->position());
}

void ImageView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ShiftModifier) && !imageSize().isEmpty()) {
        m_selecting = true;
        m_selStart = m_selEnd = e->position();
        setCursor(Qt::CrossCursor);
        update();
        return;
    }
    if (e->button() == Qt::LeftButton && nearWipe(e->position())) {
        m_draggingWipe = true;
        return;
    }
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
        m_dragging = true;
        m_dragStart = e->position();
        m_offsetStart = m_offset;
        setCursor(Qt::ClosedHandCursor);
    }
}

void ImageView::mouseMoveEvent(QMouseEvent* e)
{
    if (m_selecting) {
        m_selEnd = e->position();
        update();
        return;
    }
    if (m_draggingWipe) {
        m_wipe = qBound(0.0, e->position().x() / qMax(1, width()), 1.0);
        update();
    } else if (m_dragging) {
        m_offset = m_offsetStart + (e->position() - m_dragStart);
        m_fitMode = false;
        emit viewChanged(m_zoom, m_offset, m_fitMode);
        update();
    } else if (nearWipe(e->position())) {
        setCursor(Qt::SplitHCursor);
    } else {
        unsetCursor();
    }
    emitHover(e->position());
}

void ImageView::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_selecting) {
        m_selecting = false;
        unsetCursor();
        update();
        const auto& src = m_layers[0].src;
        if (src && !src->empty()) {
            QPointF p0 = widgetToSourceImage(m_selStart);
            QPointF p1 = widgetToSourceImage(e->position());
            int x0 = qBound(0, (int)qFloor(qMin(p0.x(), p1.x())), src->width - 1);
            int y0 = qBound(0, (int)qFloor(qMin(p0.y(), p1.y())), src->height - 1);
            int x1 = qBound(0, (int)qFloor(qMax(p0.x(), p1.x())), src->width - 1);
            int y1 = qBound(0, (int)qFloor(qMax(p0.y(), p1.y())), src->height - 1);
            emit regionSelected(QRect(QPoint(x0, y0), QPoint(x1, y1)));
        }
        return;
    }
    m_dragging = false;
    m_draggingWipe = false;
    unsetCursor();
}

void ImageView::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (nearWipe(e->position())) {
        m_wipe = 0.5;
        update();
        return;
    }
    fit();
}

void ImageView::resizeEvent(QResizeEvent* e)
{
    QOpenGLWidget::resizeEvent(e);
    if (m_fitMode) fit();
}

void ImageView::leaveEvent(QEvent*) { emit hovered(-1, -1, false); }

void ImageView::emitHover(QPointF widgetPos)
{
    const auto& src = m_layers[0].src;
    if (!src || src->empty()) return;
    QPointF ip = widgetToSourceImage(widgetPos);
    int x = (int)qFloor(ip.x()), y = (int)qFloor(ip.y());
    emit hovered(x, y, x >= 0 && y >= 0 && x < src->width && y < src->height);
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
