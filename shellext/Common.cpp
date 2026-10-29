#include "Common.h"

#include "exrcore.h"

#include <algorithm>
#include <memory>
#include <mutex>

HINSTANCE g_hInst = nullptr;
std::atomic<long> g_dllRefs{0};

std::string toUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring regString(HKEY root, const wchar_t* subkey, const wchar_t* name)
{
    DWORD bytes = 0;
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0)
        return {};
    std::wstring v(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, &v[0], &bytes) != ERROR_SUCCESS)
        return {};
    v.resize(wcslen(v.c_str()));
    return v;
}

PreviewSettings readPreviewSettings()
{
    const wchar_t* key = L"Software\\ExrView\\Preview";
    PreviewSettings s;
    s.ocioConfig = toUtf8(regString(HKEY_CURRENT_USER, key, L"OcioConfig"));
    s.display = toUtf8(regString(HKEY_CURRENT_USER, key, L"Display"));
    s.view = toUtf8(regString(HKEY_CURRENT_USER, key, L"View"));
    s.defaultInput = toUtf8(regString(HKEY_CURRENT_USER, key, L"DefaultInput"));
    std::wstring e = regString(HKEY_CURRENT_USER, key, L"Exposure");
    std::wstring g = regString(HKEY_CURRENT_USER, key, L"Gamma");
    if (!e.empty()) s.exposure = (float)_wtof(e.c_str());
    if (!g.empty()) s.gamma = (float)_wtof(g.c_str());
    if (!(s.gamma > 0.f)) s.gamma = 1.f;
    return s;
}

// Một ColorPipeline dùng chung cho cả process (nạp config tốn thời gian).
static std::shared_ptr<exrv::ColorPipeline> getPipeline(const std::string& configPath)
{
    static std::mutex m;
    static std::shared_ptr<exrv::ColorPipeline> pipe;
    static std::string loadedPath;
    std::lock_guard<std::mutex> lock(m);
    if (!pipe || loadedPath != configPath) {
        auto p = std::make_shared<exrv::ColorPipeline>(); // config mặc định: $OCIO -> ACES dựng sẵn
        if (!configPath.empty()) {
            std::string err;
            if (!p->loadConfig(configPath, &err)) p->loadConfig("", &err);
        }
        pipe = p;
        loadedPath = configPath;
    }
    return pipe;
}

static bool readAll(IStream* s, std::vector<char>& data, std::wstring& name)
{
    STATSTG st = {};
    if (SUCCEEDED(s->Stat(&st, STATFLAG_DEFAULT))) {
        if (st.pwcsName) {
            name = st.pwcsName;
            CoTaskMemFree(st.pwcsName);
        }
    }
    const ULONGLONG kMax = 4ull << 30; // 4 GB
    ULONGLONG size = st.cbSize.QuadPart;
    if (size > kMax) return false;
    LARGE_INTEGER zero = {};
    s->Seek(zero, STREAM_SEEK_SET, nullptr);
    data.clear();
    if (size) data.reserve((size_t)size);
    char buf[1 << 16];
    for (;;) {
        ULONG got = 0;
        HRESULT hr = s->Read(buf, sizeof(buf), &got);
        if (got) data.insert(data.end(), buf, buf + got);
        if (FAILED(hr) || got == 0 || data.size() > kMax) break;
    }
    return !data.empty();
}

bool renderExrStream(IStream* stream, int maxSize, RenderResult& out)
{
    out = RenderResult();
    std::vector<char> data;
    std::wstring name;
    if (!stream || !readAll(stream, data, name)) {
        out.error = L"Không đọc được dữ liệu file";
        return false;
    }
    try {
        std::string err;
        auto file = exrv::ExrFile::openMemory(std::move(data), &err);
        if (!file) {
            out.error = L"Không mở được EXR: " + toWide(err);
            return false;
        }
        int part = 0, layer = 0;
        file->defaultLayer(part, layer);
        exrv::Image img;
        err.clear();
        if (!file->readLayer(part, layer, img, &err, maxSize)) {
            out.error = L"Không đọc được pixel: " + toWide(err);
            return false;
        }

        PreviewSettings ps = readPreviewSettings();
        auto pipe = getPipeline(ps.ocioConfig);

        exrv::ViewSettings vs;
        vs.input = pipe->guessInput(*file, part, toUtf8(name));
        if (vs.input.empty()) vs.input = pipe->canonicalColorSpace(ps.defaultInput);
        if (vs.input.empty()) vs.input = pipe->sceneLinear();
        auto displays = pipe->displays();
        vs.display = ps.display;
        if (std::find(displays.begin(), displays.end(), vs.display) == displays.end()) vs.display = pipe->defaultDisplay();
        auto views = pipe->views(vs.display);
        vs.view = ps.view;
        if (std::find(views.begin(), views.end(), vs.view) == views.end()) vs.view = pipe->defaultView(vs.display);
        vs.exposure = ps.exposure;
        vs.gamma = ps.gamma;

        if (!pipe->render(img, vs, out.bgra, &err)) {
            out.error = L"Lỗi OCIO: " + toWide(err);
            return false;
        }
        out.width = img.width;
        out.height = img.height;

        const auto& info = file->info();
        const auto& P = info.parts[part];
        size_t layers = 0;
        for (auto& p : info.parts) layers += p.layers.size();
        out.line1 = std::to_wstring(P.width) + L" × " + std::to_wstring(P.height) + L"  ·  " +
                    toWide(P.compression) + L"  ·  " + std::to_wstring(layers) + L" layer";
        if (info.parts.size() > 1) out.line1 += L"  ·  " + std::to_wstring(info.parts.size()) + L" part";
        out.line2 = toWide(vs.input) + L"  →  " + toWide(vs.display) + L" / " + toWide(vs.view);
        if (vs.exposure != 0.f) {
            wchar_t b[32];
            swprintf(b, 32, L"  (%+.1f EV)", vs.exposure);
            out.line2 += b;
        }
        return true;
    } catch (const std::exception& e) {
        out.error = L"Lỗi: " + toWide(e.what());
    } catch (...) {
        out.error = L"Lỗi không xác định";
    }
    return false;
}
