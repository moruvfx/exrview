// ExrFile.cpp — đọc thông tin + pixel của file OpenEXR (single/multi-part, layer/AOV).
#include "exrcore.h"

#include <ImfBoxAttribute.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfCompressionAttribute.h>
#include <ImfDoubleAttribute.h>
#include <ImfFloatAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfInputPart.h>
#include <ImfIntAttribute.h>
#include <ImfLineOrderAttribute.h>
#include <ImfMatrixAttribute.h>
#include <ImfMultiPartInputFile.h>
#include <ImfPartType.h>
#include <ImfPreviewImageAttribute.h>
#include <ImfRationalAttribute.h>
#include <ImfRgbaFile.h>
#include <ImfStringAttribute.h>
#include <ImfStringVectorAttribute.h>
#include <ImfThreading.h>
#include <ImfTiledInputPart.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfVecAttribute.h>
#include <Iex.h>
#include <half.h> // imath_half_to_float (Imath ≥ 3.1)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <type_traits>

namespace exrv {

std::string toLower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool splitFrameNumber(const std::string& filename, std::string& prefix, long long& frame, int& padding,
                      std::string& suffix)
{
    size_t dot = filename.find_last_of('.');
    size_t end = (dot == std::string::npos) ? filename.size() : dot;
    // tìm cụm chữ số cuối cùng trước phần mở rộng
    size_t i = end;
    while (i > 0 && !std::isdigit((unsigned char)filename[i - 1])) --i;
    if (i == 0) return false;
    size_t j = i;
    while (j > 0 && std::isdigit((unsigned char)filename[j - 1])) --j;
    if (i - j > 15) return false;
    prefix = filename.substr(0, j);
    suffix = filename.substr(i);
    padding = (int)(i - j);
    frame = std::stoll(filename.substr(j, i - j));
    return true;
}

namespace {

// IStream đọc từ bộ nhớ, cho shell extension (dữ liệu đến từ IStream của Windows).
class MemIStream : public Imf::IStream {
public:
    MemIStream(const char* data, uint64_t size) : Imf::IStream("memory"), m_data(data), m_size(size) {}
    bool isMemoryMapped() const override { return true; }
    bool read(char c[], int n) override
    {
        if (m_pos + (uint64_t)n > m_size) throw IEX_NAMESPACE::InputExc("Unexpected end of file.");
        std::memcpy(c, m_data + m_pos, (size_t)n);
        m_pos += (uint64_t)n;
        return m_pos < m_size;
    }
    char* readMemoryMapped(int n) override
    {
        if (m_pos + (uint64_t)n > m_size) throw IEX_NAMESPACE::InputExc("Unexpected end of file.");
        char* p = const_cast<char*>(m_data + m_pos);
        m_pos += (uint64_t)n;
        return p;
    }
    uint64_t tellg() override { return m_pos; }
    void seekg(uint64_t pos) override { m_pos = pos; }
    void clear() override {}

private:
    const char* m_data;
    uint64_t m_size;
    uint64_t m_pos = 0;
};

void initThreads()
{
    static std::once_flag once;
    std::call_once(once, [] {
        unsigned n = std::thread::hardware_concurrency();
        Imf::setGlobalThreadCount((int)std::max(1u, std::min(n, 32u)));
    });
}

std::string compressionName(Imf::Compression c)
{
    switch (c) {
    case Imf::NO_COMPRESSION: return "None";
    case Imf::RLE_COMPRESSION: return "RLE";
    case Imf::ZIPS_COMPRESSION: return "ZIPS";
    case Imf::ZIP_COMPRESSION: return "ZIP";
    case Imf::PIZ_COMPRESSION: return "PIZ";
    case Imf::PXR24_COMPRESSION: return "PXR24";
    case Imf::B44_COMPRESSION: return "B44";
    case Imf::B44A_COMPRESSION: return "B44A";
    case Imf::DWAA_COMPRESSION: return "DWAA";
    case Imf::DWAB_COMPRESSION: return "DWAB";
    default: return "Compression #" + std::to_string((int)c);
    }
}

std::string attrToString(const Imf::Attribute& a)
{
    std::ostringstream o;
    o.precision(6);
    if (auto* s = dynamic_cast<const Imf::StringAttribute*>(&a)) return s->value();
    if (auto* v = dynamic_cast<const Imf::IntAttribute*>(&a)) { o << v->value(); return o.str(); }
    if (auto* v = dynamic_cast<const Imf::FloatAttribute*>(&a)) { o << v->value(); return o.str(); }
    if (auto* v = dynamic_cast<const Imf::DoubleAttribute*>(&a)) { o << v->value(); return o.str(); }
    if (auto* v = dynamic_cast<const Imf::V2iAttribute*>(&a)) { o << v->value().x << ", " << v->value().y; return o.str(); }
    if (auto* v = dynamic_cast<const Imf::V2fAttribute*>(&a)) { o << v->value().x << ", " << v->value().y; return o.str(); }
    if (auto* v = dynamic_cast<const Imf::V3fAttribute*>(&a)) {
        o << v->value().x << ", " << v->value().y << ", " << v->value().z;
        return o.str();
    }
    if (auto* v = dynamic_cast<const Imf::Box2iAttribute*>(&a)) {
        auto b = v->value();
        o << "(" << b.min.x << ", " << b.min.y << ") - (" << b.max.x << ", " << b.max.y << ")";
        return o.str();
    }
    if (auto* v = dynamic_cast<const Imf::CompressionAttribute*>(&a)) return compressionName(v->value());
    if (auto* v = dynamic_cast<const Imf::LineOrderAttribute*>(&a)) {
        switch (v->value()) {
        case Imf::INCREASING_Y: return "Increasing Y";
        case Imf::DECREASING_Y: return "Decreasing Y";
        default: return "Random Y";
        }
    }
    if (auto* v = dynamic_cast<const Imf::ChromaticitiesAttribute*>(&a)) {
        auto c = v->value();
        o << "R(" << c.red.x << ", " << c.red.y << ") G(" << c.green.x << ", " << c.green.y << ") B(" << c.blue.x
          << ", " << c.blue.y << ") W(" << c.white.x << ", " << c.white.y << ")";
        return o.str();
    }
    if (auto* v = dynamic_cast<const Imf::RationalAttribute*>(&a)) {
        o << v->value().n << "/" << v->value().d;
        return o.str();
    }
    if (auto* v = dynamic_cast<const Imf::StringVectorAttribute*>(&a)) {
        bool first = true;
        for (auto& s : v->value()) { o << (first ? "" : ", ") << s; first = false; }
        return o.str();
    }
    if (auto* v = dynamic_cast<const Imf::M44fAttribute*>(&a)) {
        for (int r = 0; r < 4; ++r) {
            o << (r ? " | " : "");
            for (int c = 0; c < 4; ++c) o << (c ? " " : "") << v->value()[r][c];
        }
        return o.str();
    }
    if (auto* v = dynamic_cast<const Imf::PreviewImageAttribute*>(&a)) {
        o << v->value().width() << " x " << v->value().height();
        return o.str();
    }
    if (auto* v = dynamic_cast<const Imf::TileDescriptionAttribute*>(&a)) {
        o << v->value().xSize << " x " << v->value().ySize;
        return o.str();
    }
    return "<" + std::string(a.typeName()) + ">";
}

std::string baseName(const std::string& ch)
{
    size_t p = ch.find_last_of('.');
    return p == std::string::npos ? ch : ch.substr(p + 1);
}
std::string prefixOf(const std::string& ch)
{
    size_t p = ch.find_last_of('.');
    return p == std::string::npos ? std::string() : ch.substr(0, p);
}

int channelOrder(const std::string& base)
{
    static const char* order[] = {"r", "red", "g", "green", "b", "blue", "a", "alpha", "x", "y", "z", "u", "v", "w"};
    std::string b = toLower(base);
    for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); ++i)
        if (b == order[i]) return i;
    return 100;
}

std::vector<Layer> groupLayers(const std::vector<std::string>& channels)
{
    static const char* rgbaNames[] = {"R", "G", "B", "A", "Y", "RY", "BY"};
    std::vector<Layer> layers;
    std::map<std::string, size_t> idx;
    auto add = [&](const std::string& layerName, const std::string& ch) {
        auto it = idx.find(layerName);
        if (it == idx.end()) {
            idx[layerName] = layers.size();
            layers.push_back({layerName, {ch}});
        } else {
            layers[it->second].channels.push_back(ch);
        }
    };
    for (auto& ch : channels) {
        std::string pre = prefixOf(ch);
        if (pre.empty()) {
            bool isRgba = false;
            for (auto* n : rgbaNames) if (ch == n) isRgba = true;
            add(isRgba ? "RGBA" : ch, ch);
        } else {
            add(pre, ch);
        }
    }
    for (auto& l : layers)
        std::stable_sort(l.channels.begin(), l.channels.end(), [](const std::string& a, const std::string& b) {
            return channelOrder(baseName(a)) < channelOrder(baseName(b));
        });
    // RGBA lên đầu
    std::stable_partition(layers.begin(), layers.end(), [](const Layer& l) { return l.name == "RGBA"; });
    return layers;
}

// Chọn channel cho R,G,B,A từ danh sách channel của layer.
int mapChannels(const Layer& layer, const Imf::ChannelList& list, std::string out[4], bool& lumaChroma)
{
    lumaChroma = false;
    std::map<std::string, std::string> byBase;
    std::vector<std::string> usable;
    for (auto& ch : layer.channels) {
        const Imf::Channel* c = list.findChannel(ch);
        if (!c) continue;
        std::string b = toLower(baseName(ch));
        if (b == "ry" || b == "by") lumaChroma = true;
        if (c->xSampling != 1 || c->ySampling != 1) continue;
        byBase[b] = ch;
        usable.push_back(ch);
    }
    auto get = [&](std::initializer_list<const char*> names) -> std::string {
        for (auto* n : names) {
            auto it = byBase.find(n);
            if (it != byBase.end()) return it->second;
        }
        return {};
    };
    for (int i = 0; i < 4; ++i) out[i].clear();
    out[3] = get({"a", "alpha"});
    std::string r = get({"r", "red"}), g = get({"g", "green"}), b = get({"b", "blue"});
    if (!r.empty() || !g.empty() || !b.empty()) {
        out[0] = r; out[1] = g; out[2] = b;
    } else if (!get({"x"}).empty() && !get({"y"}).empty()) {
        out[0] = get({"x"}); out[1] = get({"y"}); out[2] = get({"z"});
    } else if (!get({"u"}).empty() && !get({"v"}).empty()) {
        out[0] = get({"u"}); out[1] = get({"v"});
    } else {
        int k = 0;
        for (auto& ch : usable) {
            if (ch == out[3]) continue;
            if (k < 3) out[k++] = ch;
        }
        if (k == 0 && !out[3].empty()) { out[0] = out[3]; out[3].clear(); } // layer chỉ có alpha
        if (k == 1) return 1;
    }
    int n = 0;
    for (int i = 0; i < 3; ++i) if (!out[i].empty()) ++n;
    return n;
}


// Imf::IStream bọc InputStream (đọc thẳng từ IStream của Windows, không chép cả file).
class StreamAdapter : public Imf::IStream {
public:
    explicit StreamAdapter(InputStream* s) : Imf::IStream("stream"), m_s(s) {}
    bool read(char c[], int n) override
    {
        if (n < 0 || !m_s->read(c, (uint64_t)n)) throw IEX_NAMESPACE::InputExc("Unexpected end of file.");
        return m_s->tell() < m_s->size();
    }
    uint64_t tellg() override { return m_s->tell(); }
    void seekg(uint64_t pos) override
    {
        if (!m_s->seek(pos)) throw IEX_NAMESPACE::InputExc("Seek failed.");
    }
    void clear() override {}

private:
    InputStream* m_s;
};

// Kiểu pixel của bộ đệm đọc: float 32-bit hoặc half 16-bit (lưu dạng bit).
struct F32 {
    using T = float;
    static float get(float v) { return v; }
    static float one() { return 1.f; }
    static Imf::PixelType type() { return Imf::FLOAT; }
};
struct F16 {
    using T = uint16_t;
    static float get(uint16_t v) { return imath_half_to_float(v); }
    static uint16_t one() { return 0x3C00; } // 1.0
    static Imf::PixelType type() { return Imf::HALF; }
};

// Chuẩn hoá: 1 channel -> xám, thiếu alpha -> 1.
template <class Tr>
void normalize(typename Tr::T* p, size_t n, int nColor, const std::string ch[4], bool hasAlpha)
{
    if (nColor != 1 && hasAlpha) return;
    const int src = !ch[0].empty() ? 0 : (!ch[1].empty() ? 1 : 2);
    for (size_t i = 0; i < n; ++i, p += 4) {
        if (nColor == 1) p[0] = p[1] = p[2] = p[src];
        if (!hasAlpha) p[3] = Tr::one();
    }
}

// Box filter cộng dồn theo từng dòng nguồn: thu nhỏ ảnh lớn mà không cần giữ cả ảnh gốc trong RAM.
class BoxAccum {
public:
    BoxAccum(int sw, int sh, int tw, int th)
        : m_sw(sw), m_sh(sh), m_tw(tw), m_th(th), m_xmap(sw), m_col(tw, 0.f), m_row(th, 0.f), m_sum((size_t)tw * th * 4, 0.f)
    {
        for (int x = 0; x < sw; ++x) {
            m_xmap[x] = (int)((long long)x * tw / sw);
            m_col[m_xmap[x]] += 1.f;
        }
        for (int y = 0; y < sh; ++y) m_row[(size_t)((long long)y * th / sh)] += 1.f;
    }
    template <class Tr>
    void addRow(int sy, const typename Tr::T* row)
    {
        float* o = &m_sum[(size_t)((long long)sy * m_th / m_sh) * m_tw * 4];
        for (int sx = 0; sx < m_sw; ++sx, row += 4) {
            float* q = o + (size_t)m_xmap[sx] * 4;
            for (int c = 0; c < 4; ++c) {
                float v = Tr::get(row[c]);
                if (std::isfinite(v)) q[c] += v;
            }
        }
    }
    void finish(std::vector<float>& out)
    {
        out.swap(m_sum);
        for (int y = 0; y < m_th; ++y)
            for (int x = 0; x < m_tw; ++x) {
                float cnt = m_row[y] * m_col[x];
                float inv = cnt > 0.f ? 1.f / cnt : 0.f;
                float* p = &out[((size_t)y * m_tw + x) * 4];
                for (int c = 0; c < 4; ++c) p[c] *= inv;
            }
    }

private:
    int m_sw, m_sh, m_tw, m_th;
    std::vector<int> m_xmap;
    std::vector<float> m_col, m_row, m_sum;
};

// Đặt ảnh sw x sh vào ảnh đích ow x oh tại (ox, oy), phần ngoài = 0. Trùng khít thì chỉ chuyển bộ đệm.
template <class T>
void place(std::vector<T>& src, int sw, int sh, std::vector<T>& dst, int ow, int oh, int ox, int oy)
{
    if (ox == 0 && oy == 0 && sw == ow && sh == oh) {
        dst.swap(src);
        return;
    }
    dst.assign((size_t)ow * oh * 4, T(0));
    int x0 = std::max(0, -ox), x1 = std::min(sw, ow - ox); // [x0, x1) theo toạ độ nguồn
    if (x0 >= x1) return;
    for (int sy = 0; sy < sh; ++sy) {
        int ty = oy + sy;
        if (ty < 0 || ty >= oh) continue;
        std::memcpy(&dst[((size_t)ty * ow + (ox + x0)) * 4], &src[((size_t)sy * sw + x0) * 4],
                    sizeof(T) * 4 * (size_t)(x1 - x0));
    }
}

} // namespace

// ---------------------------------------------------------------- Image
float halfToFloat(uint16_t h) { return imath_half_to_float(h); }

void Image::pixel(int x, int y, float out[4]) const
{
    size_t i = ((size_t)y * width + x) * 4;
    for (int c = 0; c < 4; ++c) out[c] = isHalf() ? imath_half_to_float(rgbaHalf[i + c]) : rgba[i + c];
}

void Image::toFloat(std::vector<float>& out) const
{
    if (!isHalf()) {
        out = rgba;
        return;
    }
    out.resize(rgbaHalf.size());
    for (size_t i = 0; i < rgbaHalf.size(); ++i) out[i] = imath_half_to_float(rgbaHalf[i]);
}

// ---------------------------------------------------------------- ExrFile
struct ExrFile::Impl {
    std::string path;
    std::vector<char> data;
    std::unique_ptr<MemIStream> stream;
    std::unique_ptr<InputStream> src;
    std::unique_ptr<StreamAdapter> adapter;
    int threads = 0;
    std::unique_ptr<Imf::MultiPartInputFile> file; // khai báo sau cùng: huỷ trước stream
    FileInfo info;
};

ExrFile::ExrFile() : d(new Impl) {}
ExrFile::~ExrFile() = default;

static bool buildInfo(Imf::MultiPartInputFile& f, FileInfo& info)
{
    info.parts.clear();
    for (int p = 0; p < f.parts(); ++p) {
        const Imf::Header& h = f.header(p);
        Part part;
        if (h.hasName()) part.name = h.name();
        part.type = h.hasType() ? h.type() : (h.hasTileDescription() ? "tiledimage" : "scanlineimage");
        part.supported = part.type.find("deep") == std::string::npos;
        auto dw = h.displayWindow();
        auto da = h.dataWindow();
        part.dispX = dw.min.x;
        part.dispY = dw.min.y;
        part.width = dw.max.x - dw.min.x + 1;
        part.height = dw.max.y - dw.min.y + 1;
        part.dataX0 = da.min.x; part.dataY0 = da.min.y;
        part.dataX1 = da.max.x; part.dataY1 = da.max.y;
        part.compression = compressionName(h.compression());
        for (auto it = h.channels().begin(); it != h.channels().end(); ++it) part.channels.push_back(it.name());
        part.layers = groupLayers(part.channels);
        for (auto it = h.begin(); it != h.end(); ++it) {
            std::string n = it.name();
            if (n == "channels") continue;
            part.attributes.push_back({n, it.attribute().typeName(), attrToString(it.attribute())});
        }
        info.parts.push_back(std::move(part));
    }
    return true;
}

std::unique_ptr<ExrFile> ExrFile::openPath(const std::string& utf8Path, std::string* err)
{
    initThreads();
    std::unique_ptr<ExrFile> f(new ExrFile);
    try {
        f->d->path = utf8Path;
        f->d->threads = Imf::globalThreadCount();
        f->d->file.reset(new Imf::MultiPartInputFile(utf8Path.c_str(), f->d->threads));
        buildInfo(*f->d->file, f->d->info);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return nullptr;
    }
    return f;
}

std::unique_ptr<ExrFile> ExrFile::openMemory(std::vector<char> data, std::string* err)
{
    initThreads();
    std::unique_ptr<ExrFile> f(new ExrFile);
    try {
        f->d->data = std::move(data);
        f->d->threads = Imf::globalThreadCount();
        f->d->stream.reset(new MemIStream(f->d->data.data(), f->d->data.size()));
        f->d->file.reset(new Imf::MultiPartInputFile(*f->d->stream, f->d->threads));
        buildInfo(*f->d->file, f->d->info);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return nullptr;
    }
    return f;
}

std::unique_ptr<ExrFile> ExrFile::openStream(std::unique_ptr<InputStream> stream, std::string* err, int threads)
{
    if (!stream) {
        if (err) *err = "Stream rỗng";
        return nullptr;
    }
    initThreads();
    std::unique_ptr<ExrFile> f(new ExrFile);
    try {
        f->d->src = std::move(stream);
        f->d->threads = std::max(0, threads);
        f->d->adapter.reset(new StreamAdapter(f->d->src.get()));
        f->d->file.reset(new Imf::MultiPartInputFile(*f->d->adapter, f->d->threads));
        buildInfo(*f->d->file, f->d->info);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return nullptr;
    }
    return f;
}

const FileInfo& ExrFile::info() const { return d->info; }

void ExrFile::defaultLayer(int& part, int& layer) const
{
    part = 0; layer = 0;
    for (int p = 0; p < (int)d->info.parts.size(); ++p) {
        const Part& P = d->info.parts[p];
        if (!P.supported || P.layers.empty()) continue;
        for (int l = 0; l < (int)P.layers.size(); ++l)
            if (P.layers[l].name == "RGBA") { part = p; layer = l; return; }
        part = p; layer = 0;
        return;
    }
}

std::string ExrFile::colorspaceAttribute(int part) const
{
    if (part < 0 || part >= d->file->parts()) return {};
    const Imf::Header& h = d->file->header(part);
    for (auto it = h.begin(); it != h.end(); ++it) {
        std::string n = toLower(it.name());
        bool match = n == "colorspace" || n == "color_space" || n == "colourspace" || n == "oiio:colorspace" ||
                     n == "ocio:colorspace" || (n.size() > 10 && n.compare(n.size() - 10, 10, "colorspace") == 0);
        if (!match) continue;
        if (auto* s = dynamic_cast<const Imf::StringAttribute*>(&it.attribute())) return s->value();
    }
    if (auto* flag = h.findTypedAttribute<Imf::IntAttribute>("acesImageContainerFlag"))
        if (flag->value() == 1) return "ACES2065-1";
    return {};
}

bool ExrFile::chromaticities(int part, float out[8]) const
{
    if (part < 0 || part >= d->file->parts()) return false;
    auto* c = d->file->header(part).findTypedAttribute<Imf::ChromaticitiesAttribute>("chromaticities");
    if (!c) return false;
    auto v = c->value();
    float t[8] = {v.red.x, v.red.y, v.green.x, v.green.y, v.blue.x, v.blue.y, v.white.x, v.white.y};
    std::memcpy(out, t, sizeof(t));
    return true;
}

bool ExrFile::readLayer(int part, int layerIdx, Image& out, std::string* err, int maxSize) const
{
    ReadOptions opt;
    opt.maxSize = maxSize;
    return readLayer(part, layerIdx, out, err, opt);
}

bool ExrFile::readLayer(int part, int layerIdx, Image& out, std::string* err, const ReadOptions& opt) const
{
    out = Image();
    if (part < 0 || part >= (int)d->info.parts.size()) { if (err) *err = "Part không hợp lệ"; return false; }
    const Part& P = d->info.parts[part];
    if (!P.supported) { if (err) *err = "Chưa hỗ trợ deep EXR (" + P.type + ")"; return false; }
    if (layerIdx < 0 || layerIdx >= (int)P.layers.size()) { if (err) *err = "Layer không hợp lệ"; return false; }
    const Layer& L = P.layers[layerIdx];
    const Imf::Header& h = d->file->header(part);

    std::string ch[4];
    bool lumaChroma = false;
    int nColor = mapChannels(L, h.channels(), ch, lumaChroma);
    // File luminance/chroma (Y, RY, BY): để RgbaInputFile tự chuyển sang RGB (luôn trả về half).
    const bool useLuma = lumaChroma && d->info.parts.size() == 1;
    if (useLuma) {
        nColor = 3;
        ch[3] = "A";
    }
    const bool hasAlpha = !ch[3].empty();

    const int dw = P.dataX1 - P.dataX0 + 1, dh = P.dataY1 - P.dataY0 + 1;
    if (dw <= 0 || dh <= 0 || P.width <= 0 || P.height <= 0) { if (err) *err = "Data window rỗng"; return false; }

    // Vùng ảnh đầu ra: display window, hoặc display ∪ data window khi bật overscan.
    int wx0 = P.dispX, wy0 = P.dispY, wx1 = P.dispX + P.width - 1, wy1 = P.dispY + P.height - 1;
    if (opt.overscan) {
        wx0 = std::min(wx0, P.dataX0); wy0 = std::min(wy0, P.dataY0);
        wx1 = std::max(wx1, P.dataX1); wy1 = std::max(wy1, P.dataY1);
    }
    const int ww = wx1 - wx0 + 1, wh = wy1 - wy0 + 1;
    double s = 1.0;
    if (opt.maxSize > 0 && std::max(ww, wh) > opt.maxSize) s = (double)opt.maxSize / std::max(ww, wh);
    auto sc = [s](int v) { return (int)std::lround(v * s); };
    const int ow = std::max(1, sc(ww)), oh = std::max(1, sc(wh));
    const int tw = std::max(1, sc(dw)), th = std::max(1, sc(dh)); // data window sau khi thu nhỏ
    const int ox = sc(P.dataX0 - wx0), oy = sc(P.dataY0 - wy0);
    const bool resample = tw != dw || th != dh;

    // Mọi channel cần đọc đều HALF -> giữ half (không mất dữ liệu, nửa RAM).
    bool allHalf = true;
    if (!useLuma)
        for (int i = 0; i < 4; ++i) {
            if (ch[i].empty()) continue;
            const Imf::Channel* c = h.channels().findChannel(ch[i]);
            if (!c || c->type != Imf::HALF) allHalf = false;
        }

    out.width = ow;
    out.height = oh;
    out.sourceChannels = nColor + (hasAlpha ? 1 : 0);
    out.hasAlpha = hasAlpha;
    out.dispX = sc(P.dispX - wx0);
    out.dispY = sc(P.dispY - wy0);
    out.dispW = std::max(1, sc(P.width));
    out.dispH = std::max(1, sc(P.height));

    std::string warn;
    auto keepWarn = [&warn](const std::exception& e) { if (warn.empty()) warn = e.what(); };
    auto addSlices = [&](Imf::FrameBuffer& fb, Imf::PixelType type, char* base, size_t px, int x0, int y0, int x1, int y1) {
        Imath::Box2i box(Imath::V2i(x0, y0), Imath::V2i(x1, y1));
        for (int i = 0; i < 4; ++i) {
            if (ch[i].empty()) continue;
            fb.insert(ch[i], Imf::Slice::Make(type, base + i * px, box, px * 4, px * 4 * (size_t)(x1 - x0 + 1)));
        }
    };

    // Đọc nguyên data window (độ phân giải gốc) vào buf.
    auto readFull = [&](auto tr, auto& buf) {
        using Tr = decltype(tr);
        using T = typename Tr::T;
        buf.assign((size_t)dw * dh * 4, T(0));
        if (useLuma) {
            std::unique_ptr<Imf::RgbaInputFile> rf;
            std::unique_ptr<MemIStream> ms;
            std::unique_ptr<StreamAdapter> sa;
            if (d->src) {
                sa.reset(new StreamAdapter(d->src.get()));
                rf.reset(new Imf::RgbaInputFile(*sa, d->threads));
            } else if (!d->data.empty()) {
                ms.reset(new MemIStream(d->data.data(), d->data.size()));
                rf.reset(new Imf::RgbaInputFile(*ms, d->threads));
            } else {
                rf.reset(new Imf::RgbaInputFile(d->path.c_str(), d->threads));
            }
            std::vector<Imf::Rgba> px((size_t)dw * dh);
            rf->setFrameBuffer(px.data() - P.dataX0 - (ptrdiff_t)P.dataY0 * dw, 1, dw);
            try { rf->readPixels(P.dataY0, P.dataY1); } catch (const std::exception& e) { keepWarn(e); }
            static_assert(sizeof(Imf::Rgba) == 4 * sizeof(uint16_t), "Rgba phải là 4 half");
            std::vector<uint16_t> bits((size_t)dw * dh * 4);
            std::memcpy(bits.data(), px.data(), bits.size() * sizeof(uint16_t));
            if constexpr (std::is_same<T, uint16_t>::value) buf.swap(bits);
            else for (size_t i = 0; i < bits.size(); ++i) buf[i] = imath_half_to_float(bits[i]);
        } else {
            Imf::FrameBuffer fb;
            addSlices(fb, Tr::type(), reinterpret_cast<char*>(buf.data()), sizeof(T), P.dataX0, P.dataY0, P.dataX1, P.dataY1);
            Imf::InputPart in(*d->file, part);
            in.setFrameBuffer(fb);
            // File render dở dang vẫn hiển thị được phần đã có.
            try { in.readPixels(P.dataY0, P.dataY1); } catch (const std::exception& e) { keepWarn(e); }
        }
        normalize<Tr>(buf.data(), (size_t)dw * dh, nColor, ch, hasAlpha);
    };

    try {
        if (!resample) {
            if (allHalf && opt.allowHalf) {
                std::vector<uint16_t> buf;
                readFull(F16(), buf);
                place(buf, dw, dh, out.rgbaHalf, ow, oh, ox, oy);
            } else {
                std::vector<float> buf;
                readFull(F32(), buf);
                place(buf, dw, dh, out.rgba, ow, oh, ox, oy);
            }
        } else {
            std::vector<float> small;
            bool done = false;

            // Tiled có mipmap/ripmap: đọc level nhỏ nhất vẫn ≥ kích thước cần.
            if (!useLuma && P.type == "tiledimage") {
                Imf::TiledInputPart tp(*d->file, part);
                int lx = 0, ly = 0;
                if (tp.levelMode() == Imf::MIPMAP_LEVELS) {
                    for (int l = tp.numLevels() - 1; l > 0; --l)
                        if (tp.levelWidth(l) >= tw && tp.levelHeight(l) >= th) { lx = ly = l; break; }
                } else if (tp.levelMode() == Imf::RIPMAP_LEVELS) {
                    for (int l = tp.numXLevels() - 1; l > 0; --l)
                        if (tp.levelWidth(l) >= tw) { lx = l; break; }
                    for (int l = tp.numYLevels() - 1; l > 0; --l)
                        if (tp.levelHeight(l) >= th) { ly = l; break; }
                }
                if (lx > 0 || ly > 0) {
                    Imath::Box2i lb = tp.dataWindowForLevel(lx, ly);
                    const int lw = lb.max.x - lb.min.x + 1, lh = lb.max.y - lb.min.y + 1;
                    std::vector<float> buf((size_t)lw * lh * 4, 0.f);
                    Imf::FrameBuffer fb;
                    addSlices(fb, Imf::FLOAT, reinterpret_cast<char*>(buf.data()), sizeof(float), lb.min.x, lb.min.y,
                              lb.max.x, lb.max.y);
                    tp.setFrameBuffer(fb);
                    try {
                        tp.readTiles(0, tp.numXTiles(lx) - 1, 0, tp.numYTiles(ly) - 1, lx, ly);
                    } catch (const std::exception& e) { keepWarn(e); }
                    normalize<F32>(buf.data(), (size_t)lw * lh, nColor, ch, hasAlpha);
                    BoxAccum acc(lw, lh, tw, th);
                    for (int y = 0; y < lh; ++y) acc.addRow<F32>(y, &buf[(size_t)y * lw * 4]);
                    acc.finish(small);
                    done = true;
                }
            }

            if (!done && useLuma) {
                std::vector<float> buf;
                readFull(F32(), buf);
                BoxAccum acc(dw, dh, tw, th);
                for (int y = 0; y < dh; ++y) acc.addRow<F32>(y, &buf[(size_t)y * dw * 4]);
                acc.finish(small);
            } else if (!done) {
                // Đọc theo dải 256 dòng (≥ số dòng/chunk lớn nhất, DWAB) và cộng dồn ngay:
                // RAM chỉ cần 1 dải thay vì cả ảnh gốc.
                const int strip = 256;
                std::vector<float> buf((size_t)dw * strip * 4);
                BoxAccum acc(dw, dh, tw, th);
                Imf::InputPart in(*d->file, part);
                for (int y0 = P.dataY0; y0 <= P.dataY1; y0 += strip) {
                    const int y1 = std::min(P.dataY1, y0 + strip - 1), rows = y1 - y0 + 1;
                    std::fill(buf.begin(), buf.begin() + (size_t)dw * rows * 4, 0.f);
                    Imf::FrameBuffer fb;
                    addSlices(fb, Imf::FLOAT, reinterpret_cast<char*>(buf.data()), sizeof(float), P.dataX0, y0, P.dataX1, y1);
                    in.setFrameBuffer(fb);
                    try { in.readPixels(y0, y1); } catch (const std::exception& e) { keepWarn(e); }
                    normalize<F32>(buf.data(), (size_t)dw * rows, nColor, ch, hasAlpha);
                    for (int r = 0; r < rows; ++r) acc.addRow<F32>(y0 - P.dataY0 + r, &buf[(size_t)r * dw * 4]);
                }
                acc.finish(small);
            }
            place(small, tw, th, out.rgba, ow, oh, ox, oy);
        }
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }

    if (!warn.empty() && err) *err = "Đọc chưa trọn file: " + warn;
    return true;
}

} // namespace exrv
