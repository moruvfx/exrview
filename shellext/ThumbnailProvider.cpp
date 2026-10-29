// ThumbnailProvider — ảnh thu nhỏ của file .exr trong Explorer (icon Medium/Large/Extra large).
#include "Common.h"

#include <shlwapi.h>
#include <thumbcache.h>

#include <cstring>
#include <new>

class ExrThumbnailProvider final : public IInitializeWithStream, public IThumbnailProvider {
public:
    ExrThumbnailProvider() { DllAddRef(); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(ExrThumbnailProvider, IInitializeWithStream),
            QITABENT(ExrThumbnailProvider, IThumbnailProvider),
            {nullptr, 0},
        };
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override
    {
        if (m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        if (!stream) return E_INVALIDARG;
        m_stream = stream;
        m_stream->AddRef();
        return S_OK;
    }

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) override
    {
        if (!phbmp || !alpha) return E_POINTER;
        *phbmp = nullptr;
        *alpha = WTSAT_RGB;
        if (!m_stream) return E_UNEXPECTED;

        RenderResult r;
        if (!renderExrStream(m_stream, (int)cx, r) || r.width <= 0) return E_FAIL;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = r.width;
        bmi.bmiHeader.biHeight = -r.height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) return E_OUTOFMEMORY;
        std::memcpy(bits, r.bgra.data(), (size_t)r.width * r.height * 4);
        *phbmp = bmp;
        return S_OK;
    }

private:
    ~ExrThumbnailProvider()
    {
        if (m_stream) m_stream->Release();
        DllRelease();
    }
    std::atomic<ULONG> m_ref{1};
    IStream* m_stream = nullptr;
};

HRESULT CreateExrThumbnailProvider(REFIID riid, void** ppv)
{
    auto* p = new (std::nothrow) ExrThumbnailProvider();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}
