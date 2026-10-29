// ColorPipeline.cpp — chuyển ảnh float sang màn hình qua OpenColorIO.
#include "exrcore.h"
#include "FallbackConfig.h"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

namespace OCIO = OCIO_NAMESPACE;

namespace exrv {

struct ColorPipeline::Impl {
    OCIO::ConstConfigRcPtr config;
    std::string name;
    std::string path;

    std::mutex mutex;
    std::string cacheKey;
    OCIO::ConstCPUProcessorRcPtr cpu;
    OCIO::DynamicPropertyDoubleRcPtr cpuExposure;

    OCIO::GpuShaderDescRcPtr gpuDesc;
    OCIO::DynamicPropertyDoubleRcPtr gpuExposure;

    // input -> (exposure động trong scene_linear) -> display/view
    OCIO::ConstProcessorRcPtr makeProcessor(const std::string& input, const std::string& display,
                                            const std::string& view)
    {
        auto dvt = OCIO::DisplayViewTransform::Create();
        dvt->setSrc(input.c_str());
        dvt->setDisplay(display.c_str());
        dvt->setView(view.c_str());
        auto vp = OCIO::LegacyViewingPipeline::Create();
        vp->setDisplayViewTransform(dvt);
        auto ec = OCIO::ExposureContrastTransform::Create();
        ec->setStyle(OCIO::EXPOSURE_CONTRAST_LINEAR);
        ec->setExposure(0.0);
        ec->setContrast(1.0);
        ec->setGamma(1.0);
        ec->setPivot(0.18);
        ec->makeExposureDynamic();
        vp->setLinearCC(ec);
        return vp->getProcessor(config, config->getCurrentContext());
    }
};

ColorPipeline::ColorPipeline() : d(new Impl)
{
    std::string err;
    loadConfig("", &err);
}
ColorPipeline::~ColorPipeline() = default;

bool ColorPipeline::loadConfig(const std::string& utf8Path, std::string* err)
{
    OCIO::ConstConfigRcPtr cfg;
    std::string name, path;
    try {
        if (!utf8Path.empty()) {
            cfg = OCIO::Config::CreateFromFile(utf8Path.c_str());
            path = utf8Path;
            name = cfg->getName() && *cfg->getName() ? cfg->getName() : utf8Path;
        } else {
            const char* env = std::getenv("OCIO");
            if (env && *env) {
                try {
                    cfg = OCIO::Config::CreateFromEnv();
                    path = env;
                    name = std::string("$OCIO: ") + env;
                } catch (const std::exception&) {
                    cfg.reset();
                }
            }
#if OCIO_VERSION_HEX >= 0x02020000
            // Config ACES dựng sẵn trong thư viện: studio-config có nhiều color space camera nhất.
            for (const char* uri : {"ocio://studio-config-latest", "ocio://default"}) {
                if (cfg) break;
                try {
                    cfg = OCIO::Config::CreateFromFile(uri);
                    name = cfg->getName();
                } catch (const std::exception&) {
                    cfg.reset();
                }
            }
#endif
            if (!cfg) {
                std::istringstream is(kFallbackConfig);
                cfg = OCIO::Config::CreateFromStream(is);
                name = "ExrView fallback (ACES 1.0)";
            }
        }
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
    std::lock_guard<std::mutex> lock(d->mutex);
    d->config = cfg;
    d->name = name;
    d->path = path;
    d->cacheKey.clear();
    d->cpu.reset();
    d->cpuExposure.reset();
    d->gpuDesc.reset();
    d->gpuExposure.reset();
    return true;
}

std::string ColorPipeline::configName() const { return d->name; }
std::string ColorPipeline::configPath() const { return d->path; }

std::vector<std::string> ColorPipeline::colorSpaces() const
{
    std::vector<std::string> out;
    if (!d->config) return out;
    int n = d->config->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE);
    for (int i = 0; i < n; ++i)
        out.push_back(d->config->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE, i));
    return out;
}

std::vector<std::string> ColorPipeline::displays() const
{
    std::vector<std::string> out;
    if (!d->config) return out;
    for (int i = 0; i < d->config->getNumDisplays(); ++i) out.push_back(d->config->getDisplay(i));
    return out;
}

std::vector<std::string> ColorPipeline::views(const std::string& display) const
{
    std::vector<std::string> out;
    if (!d->config || display.empty()) return out;
    int n = d->config->getNumViews(display.c_str());
    for (int i = 0; i < n; ++i) out.push_back(d->config->getView(display.c_str(), i));
    return out;
}

std::string ColorPipeline::defaultDisplay() const { return d->config ? d->config->getDefaultDisplay() : ""; }

std::string ColorPipeline::defaultView(const std::string& display) const
{
    return d->config && !display.empty() ? d->config->getDefaultView(display.c_str()) : "";
}

std::string ColorPipeline::canonicalColorSpace(const std::string& n) const
{
    if (!d->config || n.empty()) return {};
    try {
        auto cs = d->config->getColorSpace(n.c_str()); // hỗ trợ alias và role
        if (cs) return cs->getName();
    } catch (const std::exception&) {
    }
    return {};
}

bool ColorPipeline::hasColorSpace(const std::string& n) const { return !canonicalColorSpace(n).empty(); }

std::string ColorPipeline::sceneLinear() const
{
    std::string s = canonicalColorSpace(OCIO::ROLE_SCENE_LINEAR);
    if (s.empty()) s = canonicalColorSpace(OCIO::ROLE_DEFAULT);
    if (s.empty()) {
        auto all = colorSpaces();
        if (!all.empty()) s = all.front();
    }
    return s;
}

namespace {
struct Gamut {
    float c[8];
    std::vector<const char*> names;
};
} // namespace

std::string ColorPipeline::guessInput(const ExrFile& file, int part, const std::string& filename) const
{
    float ch[8];
    bool hasChroma = file.chromaticities(part, ch);
    return guessInput(file.colorspaceAttribute(part), hasChroma ? ch : nullptr, filename);
}

std::string ColorPipeline::guessInput(const std::string& attr, const float* chroma, const std::string& filename) const
{
    auto firstOf = [&](std::initializer_list<const char*> names) -> std::string {
        for (auto* n : names) {
            std::string c = canonicalColorSpace(n);
            if (!c.empty()) return c;
        }
        return {};
    };

    // 1) Attribute color space trong header.
    if (!attr.empty()) {
        std::string c = canonicalColorSpace(attr);
        if (!c.empty()) return c;
        std::string a = toLower(attr);
        if (a.find("acescg") != std::string::npos || a == "ap1") return firstOf({"ACEScg", "ACES - ACEScg"});
        if (a.find("aces2065") != std::string::npos || a == "aces" || a == "ap0")
            return firstOf({"ACES2065-1", "ACES - ACES2065-1"});
        if (a.find("2020") != std::string::npos) return firstOf({"Linear Rec.2020", "lin_rec2020"});
        if (a == "linear" || a == "lin_srgb" || a == "lin_rec709" || a.find("linear") != std::string::npos)
            return firstOf({"Linear Rec.709 (sRGB)", "lin_rec709", "Utility - Linear - sRGB", "scene_linear"});
        if (a == "srgb") return firstOf({"sRGB Encoded Rec.709 (sRGB)", "srgb_tx", "Utility - sRGB - Texture"});
    }

    // 2) Chromaticities.
    if (chroma) {
        const float* ch = chroma;
        static const Gamut gamuts[] = {
            {{0.7347f, 0.2653f, 0.f, 1.f, 0.0001f, -0.077f, 0.32168f, 0.33767f}, {"ACES2065-1", "ACES - ACES2065-1"}},
            {{0.713f, 0.293f, 0.165f, 0.830f, 0.128f, 0.044f, 0.32168f, 0.33767f}, {"ACEScg", "ACES - ACEScg"}},
            {{0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f},
             {"Linear Rec.709 (sRGB)", "lin_rec709", "Utility - Linear - sRGB"}},
            {{0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f},
             {"Linear Rec.2020", "lin_rec2020", "Utility - Linear - Rec.2020"}},
            {{0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f},
             {"Linear P3-D65", "lin_p3d65", "Utility - Linear - P3-D65"}},
        };
        for (auto& g : gamuts) {
            bool ok = true;
            for (int i = 0; i < 8; ++i) ok = ok && std::fabs(g.c[i] - ch[i]) < 0.005f;
            if (!ok) continue;
            for (auto* n : g.names) {
                std::string c = canonicalColorSpace(n);
                if (!c.empty()) return c;
            }
        }
    }

    // 3) Tên color space nằm trong tên file (vd "beauty_ACEScg.0001.exr").
    if (d->config && !filename.empty()) {
        try {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
            // Hàm deprecated nhưng đúng ý: tìm tên color space nằm trong chuỗi
            // (getColorSpaceFromFilepath() thì luôn trả về rule mặc định).
            const char* found = d->config->parseColorSpaceFromString(filename.c_str());
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
            if (found && std::strlen(found) >= 5) {
                auto cs = d->config->getColorSpace(found);
                std::string defName = canonicalColorSpace(OCIO::ROLE_DEFAULT);
                if (cs && !cs->isData() && defName != cs->getName()) return cs->getName();
            }
        } catch (const std::exception&) {
        }
    }
    return {};
}

bool ColorPipeline::render(const Image& in, const ViewSettings& vs, std::vector<uint8_t>& bgra, std::string* err)
{
    if (in.empty()) return false;
    const size_t npx = (size_t)in.width * in.height;
    bgra.resize(npx * 4);

    // Kênh Alpha: hiển thị thẳng dạng xám, không qua OCIO.
    if (vs.channel == ChannelMode::Alpha) {
        for (size_t i = 0; i < npx; ++i) {
            float a = std::min(1.f, std::max(0.f, in.rgba[i * 4 + 3]));
            uint8_t v = (uint8_t)(a * 255.f + 0.5f);
            bgra[i * 4 + 0] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = v;
            bgra[i * 4 + 3] = 255;
        }
        return true;
    }

    // Giữ khoá suốt quá trình render: exposure là thuộc tính động của processor dùng chung.
    std::lock_guard<std::mutex> lock(d->mutex);
    std::string key = vs.input + '\x1f' + vs.display + '\x1f' + vs.view;
    if (key != d->cacheKey || !d->cpu) {
        if (!d->config) {
            if (err) *err = "Chưa nạp được OCIO config";
            return false;
        }
        try {
            auto proc = d->makeProcessor(vs.input, vs.display, vs.view);
            d->cpu = proc->getDefaultCPUProcessor();
            d->cpuExposure.reset();
            try {
                auto prop = d->cpu->getDynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE);
                d->cpuExposure = OCIO::DynamicPropertyValue::AsDouble(prop);
            } catch (const std::exception&) {
                // Processor bị tối giản (vd input = Raw) — không còn exposure động.
            }
            d->cacheKey = key;
        } catch (const std::exception& e) {
            d->cpu.reset();
            d->cacheKey.clear();
            if (err) *err = e.what();
            return false;
        }
    }
    OCIO::ConstCPUProcessorRcPtr cpu = d->cpu;
    if (d->cpuExposure) d->cpuExposure->setValue((double)vs.exposure);

    // Bộ đệm làm việc (áp dụng chế độ channel trước khi qua OCIO).
    std::vector<float> work(in.rgba);
    if (vs.channel != ChannelMode::RGB) {
        for (size_t i = 0; i < npx; ++i) {
            float* p = &work[i * 4];
            float v;
            switch (vs.channel) {
            case ChannelMode::Red: v = p[0]; break;
            case ChannelMode::Green: v = p[1]; break;
            case ChannelMode::Blue: v = p[2]; break;
            default: v = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; break;
            }
            p[0] = p[1] = p[2] = v;
        }
    }

    // Bảng tra gamma + lượng tử hoá 8-bit.
    const int LUT = 4096;
    uint8_t lut[LUT + 1];
    double invG = 1.0 / std::max(0.01f, vs.gamma);
    for (int i = 0; i <= LUT; ++i) lut[i] = (uint8_t)std::lround(std::pow((double)i / LUT, invG) * 255.0);

    unsigned nThreads = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    if (npx < 65536) nThreads = 1;
    int rowsPer = (in.height + (int)nThreads - 1) / (int)nThreads;
    std::vector<std::thread> pool;
    std::string threadErr;
    std::mutex errMutex;
    for (unsigned t = 0; t < nThreads; ++t) {
        int y0 = (int)t * rowsPer, y1 = std::min(in.height, y0 + rowsPer);
        if (y0 >= y1) break;
        pool.emplace_back([&, y0, y1] {
            float* base = &work[(size_t)y0 * in.width * 4];
            try {
                OCIO::PackedImageDesc desc(base, in.width, y1 - y0, 4);
                cpu->apply(desc);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> l(errMutex);
                threadErr = e.what();
                return;
            }
            size_t end = (size_t)y1 * in.width;
            for (size_t i = (size_t)y0 * in.width; i < end; ++i) {
                const float* p = &work[i * 4];
                uint8_t* o = &bgra[i * 4];
                for (int c = 0; c < 3; ++c) {
                    float v = p[c];
                    int idx = v > 0.f ? (v >= 1.f ? LUT : (int)(v * LUT + 0.5f)) : 0; // NaN -> 0
                    o[2 - c] = lut[idx];
                }
                o[3] = 255;
            }
        });
    }
    for (auto& th : pool) th.join();
    if (!threadErr.empty()) {
        if (err) *err = threadErr;
        return false;
    }
    return true;
}

bool ColorPipeline::buildGpuShader(const std::string& input, const std::string& display, const std::string& view,
                                   GpuShader& out, std::string* err)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    out = GpuShader();
    d->gpuDesc.reset();
    d->gpuExposure.reset();
    if (!d->config) {
        if (err) *err = "Chưa nạp được OCIO config";
        return false;
    }
    try {
        auto proc = d->makeProcessor(input, display, view);
        auto gpu = proc->getDefaultGPUProcessor();
        auto desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_3);
        desc->setFunctionName("OCIODisplay");
        desc->setResourcePrefix("ocio_");
#if OCIO_VERSION_HEX >= 0x02030000
        desc->setAllowTexture1D(false); // mọi LUT 1D đều thành texture 2D cao 1 pixel
#endif
        gpu->extractGpuShaderInfo(desc);
        out.text = desc->getShaderText();

        for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
            const char *tex = nullptr, *sampler = nullptr;
            unsigned edge = 0;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
            desc->get3DTexture(i, tex, sampler, edge, interp);
            const float* values = nullptr;
            desc->get3DTextureValues(i, values);
            GpuLut lut;
            lut.textureName = tex;
            lut.samplerName = sampler;
            lut.width = lut.height = lut.depth = (int)edge;
            lut.channels = 3;
            lut.linear = interp != OCIO::INTERP_NEAREST;
            lut.values.assign(values, values + (size_t)edge * edge * edge * 3);
            out.luts.push_back(std::move(lut));
        }
        for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
            const char *tex = nullptr, *sampler = nullptr;
            unsigned w = 0, h = 0;
            OCIO::GpuShaderDesc::TextureType channel = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
#if OCIO_VERSION_HEX >= 0x02030000
            OCIO::GpuShaderDesc::TextureDimensions dims = OCIO::GpuShaderDesc::TEXTURE_2D;
            desc->getTexture(i, tex, sampler, w, h, channel, dims, interp);
#else
            desc->getTexture(i, tex, sampler, w, h, channel, interp);
#endif
            const float* values = nullptr;
            desc->getTextureValues(i, values);
            GpuLut lut;
            lut.textureName = tex;
            lut.samplerName = sampler;
            lut.width = (int)w;
            lut.height = (int)std::max(1u, h);
            lut.channels = channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL ? 1 : 3;
            lut.linear = interp != OCIO::INTERP_NEAREST;
            lut.values.assign(values, values + (size_t)lut.width * lut.height * lut.channels);
            out.luts.push_back(std::move(lut));
        }
        if (desc->hasDynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE)) {
            auto prop = desc->getDynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE);
            d->gpuExposure = OCIO::DynamicPropertyValue::AsDouble(prop);
        }
        d->gpuDesc = desc;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
    return true;
}

std::vector<GpuUniformValue> ColorPipeline::gpuUniforms(float exposure)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    std::vector<GpuUniformValue> out;
    if (!d->gpuDesc) return out;
    if (d->gpuExposure) d->gpuExposure->setValue((double)exposure);
    for (unsigned i = 0; i < d->gpuDesc->getNumUniforms(); ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        const char* name = d->gpuDesc->getUniform(i, data);
        GpuUniformValue u;
        u.name = name ? name : "";
        switch (data.m_type) {
        case OCIO::UNIFORM_DOUBLE:
            u.type = GpuUniformValue::Double;
            if (data.m_getDouble) u.f.push_back((float)data.m_getDouble());
            break;
        case OCIO::UNIFORM_BOOL:
            u.type = GpuUniformValue::Bool;
            if (data.m_getBool) u.f.push_back(data.m_getBool() ? 1.f : 0.f);
            break;
        case OCIO::UNIFORM_FLOAT3:
            u.type = GpuUniformValue::Float3;
            if (data.m_getFloat3) {
                auto v = data.m_getFloat3();
                u.f.assign(v.begin(), v.end());
            }
            break;
        case OCIO::UNIFORM_VECTOR_FLOAT:
            u.type = GpuUniformValue::VectorFloat;
            if (data.m_vectorFloat.m_getSize && data.m_vectorFloat.m_getVector) {
                int n = data.m_vectorFloat.m_getSize();
                const float* v = data.m_vectorFloat.m_getVector();
                if (v && n > 0) u.f.assign(v, v + n);
            }
            break;
        case OCIO::UNIFORM_VECTOR_INT:
            u.type = GpuUniformValue::VectorInt;
            if (data.m_vectorInt.m_getSize && data.m_vectorInt.m_getVector) {
                int n = data.m_vectorInt.m_getSize();
                const int* v = data.m_vectorInt.m_getVector();
                if (v && n > 0) u.i.assign(v, v + n);
            }
            break;
        default:
            continue;
        }
        out.push_back(std::move(u));
    }
    return out;
}

} // namespace exrv
