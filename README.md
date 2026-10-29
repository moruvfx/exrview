# ExrView

Trình xem ảnh **OpenEXR** cho Windows, quản lý màu bằng **OpenColorIO**, kèm **thumbnail và khung Preview trong Windows Explorer** — xem được ảnh EXR ngay trong thư mục mà không cần mở phần mềm nào.

## Tính năng

**Ứng dụng xem (ExrView.exe)**
- Đọc EXR single-part, multi-part, scanline/tiled, mọi kiểu nén (PIZ, ZIP, DWAA/DWAB, B44…), file luminance/chroma, file render dở dang.
- **Layer / AOV**: tự nhóm channel theo tiền tố (`diffuse.R/G/B`, `N.X/Y/Z`, `depth.Z`…), chọn từng layer, từng part.
- **Color space qua OCIO**: mặc định dùng config **ACES studio** dựng sẵn trong OCIO (ACEScg, ACES2065-1, ACEScct, Linear Rec.709/Rec.2020/P3, các log camera ARRI/Sony/RED/Panasonic…, display sRGB / Display P3 / Rec.1886 / Rec.2100-PQ…). Nạp được config `.ocio` riêng của studio (menu **Màu**), hoặc tự lấy theo biến môi trường `OCIO`.
- **Tự đoán input color space** từ header (`colorspace`, `oiio:ColorSpace`, chromaticities, cờ ACES container) hoặc tên file; layer dữ liệu (depth, normal, position…) tự chuyển sang Raw.
- **Exposure / Gamma** thay đổi tức thì — OCIO chạy trên **GPU** (OpenGL 3.3); máy không có GPU phù hợp tự chuyển sang CPU.
- **Pixel inspector**: giá trị float gốc R G B A và giá trị hiển thị dưới con trỏ.
- **Sequence**: mở 1 frame là tự nhận cả dãy `shot.1001.exr…`, play theo FPS, cache RAM có thể chỉnh (1–16 GB), prefetch frame kế tiếp.
- Bảng thông tin header (phím **I**), kéo-thả file, file gần đây.

**Windows Explorer (ExrViewShell.dll)**
- **Thumbnail** cho `.exr`/`.sxr` ở chế độ Medium/Large/Extra large icons.
- **Preview pane** (Alt+P): ảnh + kích thước, kiểu nén, số layer, color space đang dùng; tự theo giao diện sáng/tối của Explorer.
- Dùng chung lõi màu với viewer → màu trong Explorer khớp với viewer. Menu **Explorer → Dùng cài đặt màu hiện tại cho thumbnail/preview** để chọn display/view/exposure cho Explorer.

## Phím tắt

| Phím | Chức năng |
|---|---|
| Ctrl+O | Mở file |
| F / 1 / 2 | Vừa khung / 100% / 200% |
| Lăn chuột · kéo chuột · double-click | Zoom quanh con trỏ · pan · vừa khung |
| C R G B A L | Kênh RGB / R / G / B / Alpha / Luminance |
| [ ] · 0 | Exposure −½ / +½ stop · reset exposure & gamma |
| ← → · Space · Home/End | Frame trước/sau · play/pause · frame đầu/cuối |
| I · F5 | Bảng thông tin file · đọc lại file |

## Cách có file cài đặt

### Cách 1 — GitHub Actions (dễ nhất, không cần cài gì)
1. Tạo repository mới trên GitHub, đẩy toàn bộ thư mục này lên.
2. Vào tab **Actions** → workflow **Build Windows** tự chạy (hoặc bấm *Run workflow*).
3. Khoảng 20–40 phút lần đầu (vcpkg build OpenEXR + OCIO, các lần sau có cache). Tải artifact **ExrView-windows**: có `ExrView-Setup-1.0.0.exe` và bản portable `.zip`.
4. Đẩy tag `v1.0.0` để workflow tự tạo Release kèm file cài đặt.

### Cách 2 — Build trên máy
Cần: Visual Studio 2022 (workload *Desktop development with C++*), CMake ≥ 3.21, Qt 6 bản *MSVC 2022 64-bit*, vcpkg, Inno Setup 6 (tuỳ chọn).

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
.\build.ps1 -QtDir C:\Qt\6.8.0\msvc2022_64 -Vcpkg C:\vcpkg
```
Kết quả: `dist\` (bản chạy ngay) và `installer\Output\ExrView-Setup-1.0.0.exe`.

## Cài đặt & sử dụng
1. Chạy `ExrView-Setup-x.y.z.exe` (cần quyền Admin để đăng ký preview cho Explorer).
2. Mở thư mục có file `.exr`, chọn **View → Large icons** để thấy thumbnail, **Alt+P** để bật Preview pane.
3. Double-click file `.exr` để mở bằng ExrView (nếu Windows hỏi app mặc định, chọn ExrView).

Dùng bản portable: giải nén, mở PowerShell **Admin** trong thư mục đó và chạy `regsvr32 ExrViewShell.dll` (không có Admin thì chỉ đăng ký cho user hiện tại). Gỡ: `regsvr32 /u ExrViewShell.dll`.

## Xử lý sự cố
- **Không thấy thumbnail**: File Explorer → *Options → View* → bỏ chọn *Always show icons, never thumbnails*. Thumbnail cũ được Windows cache: chạy **Disk Cleanup** → chọn *Thumbnails* để tạo lại (cũng cần làm sau khi đổi cài đặt màu cho Explorer).
- **Preview pane trống**: kiểm tra *Options → View → Show preview handlers in preview pane* đã bật.
- **App khác giành thumbnail .exr** (một số plugin/codec): cài lại ExrView hoặc chạy lại `regsvr32 ExrViewShell.dll` để đăng ký lại.
- **Viewer báo "CPU" ở thanh trạng thái**: card đồ hoạ/driver không có OpenGL 3.3 (vd. Remote Desktop, máy ảo) — vẫn xem được, chỉ chậm hơn khi kéo exposure.
- **Màu trong Explorer khác viewer**: trong viewer chọn display/view/exposure mong muốn rồi bấm *Explorer → Dùng cài đặt màu hiện tại…*.

## Cấu trúc mã nguồn
```
core/        Lõi C++ dùng chung: đọc EXR (OpenEXR) + pipeline màu (OCIO, CPU & sinh shader GPU)
viewer/      Ứng dụng Qt 6: ImageView (OpenGL, OCIO trên GPU), FrameLoader (đọc nền + cache), MainWindow
shellext/    DLL COM cho Explorer: IThumbnailProvider + IPreviewHandler, tự đăng ký (regsvr32)
tools/       exrinfo — công cụ dòng lệnh in thông tin EXR / xuất ảnh đã qua OCIO (để kiểm thử)
installer/   Script Inno Setup
```
Lõi được build tĩnh vào cả viewer và DLL, nên DLL không phụ thuộc Qt; VC++ runtime được kèm cạnh DLL.

## Giấy phép thư viện
OpenEXR và OpenColorIO: BSD-3-Clause. Qt 6: LGPL v3 (liên kết động, các DLL Qt nằm cạnh ExrView.exe).
