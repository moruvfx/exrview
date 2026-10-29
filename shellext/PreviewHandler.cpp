// PreviewHandler — khung Preview (Alt+P) của Explorer: hiển thị ảnh EXR đã qua OCIO
// kèm dòng thông tin (kích thước, nén, layer, color space). Chạy trong prevhost.exe.
#include "Common.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <new>

namespace {
const wchar_t* kWndClass = L"ExrViewPreviewWnd";
}

class ExrPreviewHandler final : public IObjectWithSite,
                          public IPreviewHandler,
                          public IOleWindow,
                          public IInitializeWithStream,
                          public IPreviewHandlerVisuals {
public:
    ExrPreviewHandler() { DllAddRef(); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(ExrPreviewHandler, IObjectWithSite),
            QITABENT(ExrPreviewHandler, IOleWindow),
            QITABENT(ExrPreviewHandler, IInitializeWithStream),
            QITABENT(ExrPreviewHandler, IPreviewHandler),
            QITABENT(ExrPreviewHandler, IPreviewHandlerVisuals),
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
        if (!stream) return E_INVALIDARG;
        if (m_stream) m_stream->Release();
        m_stream = stream;
        m_stream->AddRef();
        return S_OK;
    }

    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown* site) override
    {
        if (m_site) { m_site->Release(); m_site = nullptr; }
        if (m_frame) { m_frame->Release(); m_frame = nullptr; }
        if (site) {
            m_site = site;
            m_site->AddRef();
            site->QueryInterface(IID_PPV_ARGS(&m_frame));
        }
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        return m_site ? m_site->QueryInterface(riid, ppv) : E_FAIL;
    }

    // IOleWindow
    IFACEMETHODIMP GetWindow(HWND* phwnd) override
    {
        if (!phwnd) return E_INVALIDARG;
        *phwnd = m_parent;
        return S_OK;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

    // IPreviewHandler
    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* prc) override
    {
        if (!hwnd || !prc) return E_INVALIDARG;
        m_parent = hwnd;
        m_rect = *prc;
        if (m_hwnd) {
            SetParent(m_hwnd, m_parent);
            moveWindow();
        }
        return S_OK;
    }
    IFACEMETHODIMP SetRect(const RECT* prc) override
    {
        if (!prc) return E_INVALIDARG;
        m_rect = *prc;
        moveWindow();
        return S_OK;
    }
    IFACEMETHODIMP DoPreview() override
    {
        if (m_hwnd || !m_stream) return E_FAIL;
        // Đủ nét cho khung preview cả trên màn hình 4K, vẫn nhanh khi thu nhỏ.
        renderExrStream(m_stream, 2048, m_result);
        registerClass();
        m_hwnd = CreateWindowExW(0, kWndClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, m_rect.left, m_rect.top,
                                 m_rect.right - m_rect.left, m_rect.bottom - m_rect.top, m_parent, nullptr, g_hInst,
                                 this);
        return m_hwnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IFACEMETHODIMP Unload() override
    {
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        if (m_stream) {
            m_stream->Release();
            m_stream = nullptr;
        }
        m_result = RenderResult();
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override
    {
        if (m_hwnd) ::SetFocus(m_hwnd);
        return S_OK;
    }
    IFACEMETHODIMP QueryFocus(HWND* phwnd) override
    {
        if (!phwnd) return E_INVALIDARG;
        *phwnd = ::GetFocus();
        return *phwnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override
    {
        return m_frame ? m_frame->TranslateAccelerator(msg) : S_FALSE;
    }

    // IPreviewHandlerVisuals — theo màu nền / chữ của Explorer (sáng hoặc tối).
    IFACEMETHODIMP SetBackgroundColor(COLORREF c) override
    {
        m_bg = c;
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, TRUE);
        return S_OK;
    }
    IFACEMETHODIMP SetFont(const LOGFONTW* plf) override
    {
        if (!plf) return E_INVALIDARG;
        if (m_font) DeleteObject(m_font);
        m_font = CreateFontIndirectW(plf);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, TRUE);
        return S_OK;
    }
    IFACEMETHODIMP SetTextColor(COLORREF c) override
    {
        m_fg = c;
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, TRUE);
        return S_OK;
    }

private:
    ~ExrPreviewHandler()
    {
        Unload();
        SetSite(nullptr);
        if (m_font) DeleteObject(m_font);
        DllRelease();
    }

    void moveWindow()
    {
        if (m_hwnd)
            SetWindowPos(m_hwnd, nullptr, m_rect.left, m_rect.top, m_rect.right - m_rect.left,
                         m_rect.bottom - m_rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    static void registerClass()
    {
        static bool done = false;
        if (done) return;
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);
        done = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<ExrPreviewHandler*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (self) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                self->paint(hwnd, hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            break;
        case WM_PRINTCLIENT: // chụp cửa sổ (PrintWindow, DWM...)
            if (self) {
                self->paint(hwnd, (HDC)wp);
                return 0;
            }
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void paint(HWND hwnd, HDC hdc)
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;

        // Vẽ vào bộ đệm để không nhấp nháy khi kéo khung.
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP buf = CreateCompatibleBitmap(hdc, std::max(1, W), std::max(1, H));
        HGDIOBJ oldBmp = SelectObject(mem, buf);
        HBRUSH bg = CreateSolidBrush(m_bg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        if (!m_font) {
            // Explorer thường gọi SetFont; nếu không, dùng font hệ thống (Segoe UI — có đủ dấu tiếng Việt).
            NONCLIENTMETRICSW ncm = {sizeof(ncm)};
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
                m_font = CreateFontIndirectW(&ncm.lfMessageFont);
        }
        HGDIOBJ oldFont = SelectObject(mem, m_font ? m_font : GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW tm;
        GetTextMetricsW(mem, &tm);
        int lineH = tm.tmHeight + 4;
        int margin = 12;
        SetBkMode(mem, TRANSPARENT);
        ::SetTextColor(mem, m_fg);

        if (m_result.width > 0) {
            int textArea = lineH * 2 + margin;
            int availW = W - margin * 2, availH = H - margin * 2 - textArea;
            if (availW > 0 && availH > 0) {
                double s = std::min((double)availW / m_result.width, (double)availH / m_result.height);
                if (s > 1.0) s = std::min(s, 4.0); // phóng tối đa 4x
                int dw = std::max(1, (int)(m_result.width * s)), dh = std::max(1, (int)(m_result.height * s));
                int dx = margin + (availW - dw) / 2, dy = margin + (availH - dh) / 2;
                BITMAPINFO bmi = {};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = m_result.width;
                bmi.bmiHeader.biHeight = -m_result.height;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(mem, s < 1.0 ? HALFTONE : COLORONCOLOR);
                SetBrushOrgEx(mem, 0, 0, nullptr);
                StretchDIBits(mem, dx, dy, dw, dh, 0, 0, m_result.width, m_result.height, m_result.bgra.data(), &bmi,
                              DIB_RGB_COLORS, SRCCOPY);
                RECT t1 = {margin, dy + dh + margin, W - margin, dy + dh + margin + lineH};
                RECT t2 = {margin, t1.bottom, W - margin, t1.bottom + lineH};
                DrawTextW(mem, m_result.line1.c_str(), -1, &t1, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                COLORREF dim = RGB((GetRValue(m_fg) + GetRValue(m_bg)) / 2, (GetGValue(m_fg) + GetGValue(m_bg)) / 2,
                                   (GetBValue(m_fg) + GetBValue(m_bg)) / 2);
                ::SetTextColor(mem, dim);
                DrawTextW(mem, m_result.line2.c_str(), -1, &t2, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        } else {
            std::wstring msg = m_result.error.empty() ? L"Không xem trước được file này" : m_result.error;
            RECT t = {margin, margin, W - margin, H - margin};
            DrawTextW(mem, msg.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        }

        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(buf);
        DeleteDC(mem);
    }

    std::atomic<ULONG> m_ref{1};
    IStream* m_stream = nullptr;
    IUnknown* m_site = nullptr;
    IPreviewHandlerFrame* m_frame = nullptr;
    HWND m_parent = nullptr;
    HWND m_hwnd = nullptr;
    RECT m_rect = {};
    COLORREF m_bg = RGB(255, 255, 255);
    COLORREF m_fg = RGB(0, 0, 0);
    HFONT m_font = nullptr;
    RenderResult m_result;
};

HRESULT CreateExrPreviewHandler(REFIID riid, void** ppv)
{
    auto* p = new (std::nothrow) ExrPreviewHandler();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}
