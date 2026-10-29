// Common.h — phần dùng chung của shell extension: GUID, đếm tham chiếu DLL, đọc cài đặt,
// đọc IStream và render EXR -> BGRA 8-bit qua lõi exrcore.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// {2222F408-4C02-4EF2-8893-520179C9792B}
static const CLSID CLSID_ExrThumbnail = {0x2222f408, 0x4c02, 0x4ef2, {0x88, 0x93, 0x52, 0x01, 0x79, 0xc9, 0x79, 0x2b}};
// {BA6AEF84-B730-460B-8AE3-ABDD08DB62E4}
static const CLSID CLSID_ExrPreview = {0xba6aef84, 0xb730, 0x460b, {0x8a, 0xe3, 0xab, 0xdd, 0x08, 0xdb, 0x62, 0xe4}};

#define EXRVIEW_THUMB_CLSID_STR L"{2222F408-4C02-4EF2-8893-520179C9792B}"
#define EXRVIEW_PREVIEW_CLSID_STR L"{BA6AEF84-B730-460B-8AE3-ABDD08DB62E4}"

extern HINSTANCE g_hInst;
extern std::atomic<long> g_dllRefs;
inline void DllAddRef() { ++g_dllRefs; }
inline void DllRelease() { --g_dllRefs; }

std::string toUtf8(const std::wstring& w);
std::wstring toWide(const std::string& s);

// Cài đặt do viewer ghi vào HKCU\Software\ExrView\Preview (menu Explorer).
struct PreviewSettings {
    std::string ocioConfig;
    std::string display;
    std::string view;
    std::string defaultInput;
    float exposure = 0.f;
    float gamma = 1.f;
};
PreviewSettings readPreviewSettings();

struct RenderResult {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra; // top-down, B G R A
    std::wstring line1;        // "3840 × 2160 · PIZ · 12 layers"
    std::wstring line2;        // "ACEScg → sRGB - Display / ACES 1.0 - SDR Video"
    std::wstring error;
};

// Đọc toàn bộ IStream (giới hạn kích thước) rồi render layer mặc định, cạnh dài nhất <= maxSize.
bool renderExrStream(IStream* stream, int maxSize, RenderResult& out);
