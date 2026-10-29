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
#include <ImfTileDescriptionAttribute.h>
#include <ImfVecAttribute.h>
#include <Iex.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

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

void downsample(Image& img, int maxSize)
{
    if (maxSize <= 0 || (img.width <= maxSize && img.height <= maxSize)) return;
    double s = (double)maxSize / std::max(img.width, img.height);
    int nw = std::max(1, (int)std::lround(img.width * s));
    int nh = std::max(1, (int)std::lround(img.height * s));
    std::vector<float> out((size_t)nw * nh * 4);
    std::vector<int> x0(nw + 1);
    for (int x = 0; x <= nw; ++x) x0[x] = (int)((long long)x * img.width / nw);
    for (int y = 0; y < nh; ++y) {
        int ya = (int)((long long)y * img.height / nh), yb = std::max(ya + 1, (int)((long long)(y + 1) * img.height / nh));
        for (int x = 0; x < nw; ++x) {
            int xa = x0[x], xb = std::max(xa + 1, x0[x + 1]);
            double acc[4] = {0, 0, 0, 0};
            for (int sy = ya; sy < yb; ++sy) {
                const float* p = &img.rgba[((size_t)sy * img.width + xa) * 4];
                for (int sx = xa; sx < xb; ++sx, p += 4) {
                    for (int c = 0; c < 4; ++c) acc[c] += std::isfinite(p[c]) ? p[c] : 0.0;
                }
            }
            double inv = 1.0 / ((double)(yb - ya) * (xb - xa));
            float* o = &out[((size_t)y * nw + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = (float)(acc[c] * inv);
        }
    }
    img.width = nw;
    img.height = nh;
    img.rgba.swap(out);
}

} // namespace

struct ExrFile::Impl {
    std::string path;
    std::vector<char> data;
    std::unique_ptr<MemIStream> stream;
    std::unique_ptr<Imf::MultiPartInputFile> file;
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
        f->d->file.reset(new Imf::MultiPartInputFile(utf8Path.c_str(), Imf::globalThreadCount()));
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
        f->d->stream.reset(new MemIStream(f->d->data.data(), f->d->data.size()));
        f->d->file.reset(new Imf::MultiPartInputFile(*f->d->stream, Imf::globalThreadCount()));
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

    int dw = P.dataX1 - P.dataX0 + 1, dh = P.dataY1 - P.dataY0 + 1;
    if (dw <= 0 || dh <= 0 || P.width <= 0 || P.height <= 0) { if (err) *err = "Data window rỗng"; return false; }
    std::vector<float> buf;
    std::string warn;

    try {
        if (lumaChroma && d->info.parts.size() == 1) {
            // File luminance/chroma (Y, RY, BY): để RgbaInputFile tự chuyển sang RGB.
            std::unique_ptr<Imf::RgbaInputFile> rf;
            std::unique_ptr<MemIStream> ms;
            if (!d->data.empty()) {
                ms.reset(new MemIStream(d->data.data(), d->data.size()));
                rf.reset(new Imf::RgbaInputFile(*ms, Imf::globalThreadCount()));
            } else {
                rf.reset(new Imf::RgbaInputFile(d->path.c_str(), Imf::globalThreadCount()));
            }
            std::vector<Imf::Rgba> px((size_t)dw * dh);
            rf->setFrameBuffer(px.data() - P.dataX0 - (ptrdiff_t)P.dataY0 * dw, 1, dw);
            try { rf->readPixels(P.dataY0, P.dataY1); } catch (const std::exception& e) { warn = e.what(); }
            buf.resize((size_t)dw * dh * 4);
            for (size_t i = 0; i < px.size(); ++i) {
                buf[i * 4 + 0] = px[i].r; buf[i * 4 + 1] = px[i].g;
                buf[i * 4 + 2] = px[i].b; buf[i * 4 + 3] = px[i].a;
            }
            nColor = 3;
            ch[3] = "A";
        } else {
            buf.assign((size_t)dw * dh * 4, 0.f);
            Imf::FrameBuffer fb;
            Imath::Box2i box(Imath::V2i(P.dataX0, P.dataY0), Imath::V2i(P.dataX1, P.dataY1));
            for (int i = 0; i < 4; ++i) {
                if (ch[i].empty()) continue;
                fb.insert(ch[i], Imf::Slice::Make(Imf::FLOAT, &buf[i], box, sizeof(float) * 4, sizeof(float) * 4 * dw));
            }
            Imf::InputPart in(*d->file, part);
            in.setFrameBuffer(fb);
            // File render dở dang vẫn hiển thị được phần đã có.
            try { in.readPixels(P.dataY0, P.dataY1); } catch (const std::exception& e) { warn = e.what(); }
        }
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }

    // Chuẩn hoá: 1 channel -> xám, thiếu alpha -> 1.
    bool hasAlpha = !ch[3].empty();
    size_t n = (size_t)dw * dh;
    for (size_t i = 0; i < n; ++i) {
        float* p = &buf[i * 4];
        if (nColor == 1) {
            float v = !ch[0].empty() ? p[0] : (!ch[1].empty() ? p[1] : p[2]);
            p[0] = p[1] = p[2] = v;
        }
        if (!hasAlpha) p[3] = 1.f;
    }

    // Đặt data window vào display window.
    out.width = P.width;
    out.height = P.height;
    out.sourceChannels = nColor + (hasAlpha ? 1 : 0);
    out.hasAlpha = hasAlpha;
    if (P.dataX0 == P.dispX && P.dataY0 == P.dispY && dw == P.width && dh == P.height) {
        out.rgba.swap(buf);
    } else {
        out.rgba.assign((size_t)P.width * P.height * 4, 0.f);
        int x0 = std::max(P.dataX0, P.dispX), x1 = std::min(P.dataX1, P.dispX + P.width - 1);
        int y0 = std::max(P.dataY0, P.dispY), y1 = std::min(P.dataY1, P.dispY + P.height - 1);
        for (int y = y0; y <= y1 && x0 <= x1; ++y) {
            const float* src = &buf[(((size_t)(y - P.dataY0)) * dw + (x0 - P.dataX0)) * 4];
            float* dst = &out.rgba[(((size_t)(y - P.dispY)) * P.width + (x0 - P.dispX)) * 4];
            std::memcpy(dst, src, sizeof(float) * 4 * (size_t)(x1 - x0 + 1));
        }
    }
    downsample(out, maxSize);
    if (!warn.empty() && err) *err = "Đọc chưa trọn file: " + warn;
    return true;
}

} // namespace exrv
