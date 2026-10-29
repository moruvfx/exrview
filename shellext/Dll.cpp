// Dll.cpp — điểm vào COM: class factory + đăng ký/gỡ đăng ký với Explorer.
//   regsvr32 ExrViewShell.dll      (chạy bằng Admin: cho mọi user; không Admin: chỉ user hiện tại)
//   regsvr32 /u ExrViewShell.dll
#include "Common.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <new>
#include <string>

HRESULT CreateExrThumbnailProvider(REFIID riid, void** ppv);
HRESULT CreateExrPreviewHandler(REFIID riid, void** ppv);

namespace {

// GUID chuẩn của Windows
const wchar_t* kThumbHandlerKey = L"{e357fccd-a995-4576-b01f-234630154e96}";   // IThumbnailProvider
const wchar_t* kPreviewHandlerKey = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}"; // IPreviewHandler
const wchar_t* kPrevhostAppId64 = L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";   // prevhost.exe (64-bit)
const wchar_t* kExtensions[] = {L".exr", L".sxr"};

typedef HRESULT (*CreateFn)(REFIID, void**);

class ClassFactory final : public IClassFactory {
public:
    explicit ClassFactory(CreateFn fn) : m_fn(fn) { DllAddRef(); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {QITABENT(ClassFactory, IClassFactory), {nullptr, 0}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        return m_fn(riid, ppv);
    }
    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) DllAddRef();
        else DllRelease();
        return S_OK;
    }

private:
    ~ClassFactory() { DllRelease(); }
    std::atomic<ULONG> m_ref{1};
    CreateFn m_fn;
};

// ---- Registry ----
LSTATUS setString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    return RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring getString(HKEY root, const std::wstring& key, const wchar_t* name)
{
    wchar_t buf[512];
    DWORD bytes = sizeof(buf);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return {};
    return buf;
}

std::wstring modulePath()
{
    wchar_t path[MAX_PATH * 4];
    GetModuleFileNameW(g_hInst, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    return path;
}

HRESULT registerAll(HKEY root)
{
    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring dll = modulePath();
    LSTATUS e = ERROR_SUCCESS;
    auto chk = [&](LSTATUS s) { if (s != ERROR_SUCCESS && e == ERROR_SUCCESS) e = s; };

    // Thumbnail provider
    std::wstring tk = classes + L"CLSID\\" EXRVIEW_THUMB_CLSID_STR;
    chk(setString(root, tk, nullptr, L"ExrView Thumbnail Provider"));
    chk(setString(root, tk + L"\\InprocServer32", nullptr, dll));
    chk(setString(root, tk + L"\\InprocServer32", L"ThreadingModel", L"Apartment"));

    // Preview handler (chạy trong prevhost.exe)
    std::wstring pk = classes + L"CLSID\\" EXRVIEW_PREVIEW_CLSID_STR;
    chk(setString(root, pk, nullptr, L"ExrView Preview Handler"));
    chk(setString(root, pk, L"DisplayName", L"ExrView Preview Handler"));
    chk(setString(root, pk, L"AppID", kPrevhostAppId64));
    chk(setString(root, pk + L"\\InprocServer32", nullptr, dll));
    chk(setString(root, pk + L"\\InprocServer32", L"ThreadingModel", L"Apartment"));
    chk(setString(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", EXRVIEW_PREVIEW_CLSID_STR,
                  L"ExrView Preview Handler"));

    for (const wchar_t* ext : kExtensions) {
        // Gắn vào cả phần mở rộng lẫn SystemFileAssociations để vẫn chạy khi .exr được mở bằng app khác.
        for (const std::wstring& base : {classes + ext, classes + L"SystemFileAssociations\\" + ext}) {
            chk(setString(root, base + L"\\ShellEx\\" + kThumbHandlerKey, nullptr, EXRVIEW_THUMB_CLSID_STR));
            chk(setString(root, base + L"\\ShellEx\\" + kPreviewHandlerKey, nullptr, EXRVIEW_PREVIEW_CLSID_STR));
        }
        std::wstring ek = classes + ext;
        if (getString(root, ek, L"PerceivedType").empty()) chk(setString(root, ek, L"PerceivedType", L"image"));
        if (getString(root, ek, L"Content Type").empty()) chk(setString(root, ek, L"Content Type", L"image/x-exr"));
    }
    return e == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(e);
}

void unregisterAll(HKEY root)
{
    const std::wstring classes = L"Software\\Classes\\";
    RegDeleteTreeW(root, (classes + L"CLSID\\" EXRVIEW_THUMB_CLSID_STR).c_str());
    RegDeleteTreeW(root, (classes + L"CLSID\\" EXRVIEW_PREVIEW_CLSID_STR).c_str());
    RegDeleteKeyValueW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", EXRVIEW_PREVIEW_CLSID_STR);
    for (const wchar_t* ext : kExtensions) {
        for (const std::wstring& base : {classes + ext, classes + L"SystemFileAssociations\\" + ext}) {
            // Chỉ xoá khi handler đang trỏ tới ExrView (không đụng vào app khác).
            std::wstring t = base + L"\\ShellEx\\" + kThumbHandlerKey;
            if (_wcsicmp(getString(root, t, nullptr).c_str(), EXRVIEW_THUMB_CLSID_STR) == 0) RegDeleteTreeW(root, t.c_str());
            std::wstring p = base + L"\\ShellEx\\" + kPreviewHandlerKey;
            if (_wcsicmp(getString(root, p, nullptr).c_str(), EXRVIEW_PREVIEW_CLSID_STR) == 0) RegDeleteTreeW(root, p.c_str());
        }
    }
}

bool isElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION el = {};
    DWORD size = 0;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &el, sizeof(el), &size)) r = el.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return r;
}

} // namespace

BOOL APIENTRY DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    CreateFn fn = nullptr;
    if (IsEqualCLSID(clsid, CLSID_ExrThumbnail)) fn = CreateExrThumbnailProvider;
    else if (IsEqualCLSID(clsid, CLSID_ExrPreview)) fn = CreateExrPreviewHandler;
    else return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ClassFactory(fn);
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_dllRefs.load() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer()
{
    // Admin -> HKLM (mọi user). Không Admin -> HKCU (chỉ user hiện tại).
    HRESULT hr = registerAll(isElevated() ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER);
    if (SUCCEEDED(hr)) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return hr;
}

STDAPI DllUnregisterServer()
{
    unregisterAll(HKEY_CURRENT_USER);
    if (isElevated()) unregisterAll(HKEY_LOCAL_MACHINE);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
