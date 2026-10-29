// exrcore.h — lõi đọc EXR + quản lý màu OCIO, dùng chung cho viewer và shell extension.
// Header này cố ý không lộ kiểu của OpenEXR/OCIO ra ngoài (pimpl) để shell extension
// chỉ phụ thuộc vào STL.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace exrv {

struct Attribute {
    std::string name;
    std::string type;
    std::string value;
};

// Một "layer" = nhóm channel cùng tiền tố (vd: "diffuse.R/G/B" -> layer "diffuse").
struct Layer {
    std::string name;                  // "RGBA" cho layer mặc định
    std::vector<std::string> channels; // tên channel đầy đủ trong file
};

struct Part {
    std::string name;     // tên part (multi-part) hoặc rỗng
    std::string type;     // scanlineimage / tiledimage / deepscanline / deeptile
    bool supported = true;
    int width = 0, height = 0;                 // display window
    int dispX = 0, dispY = 0;                  // gốc display window
    int dataX0 = 0, dataY0 = 0, dataX1 = -1, dataY1 = -1; // data window (bao gồm)
    std::string compression;
    std::vector<std::string> channels;
    std::vector<Layer> layers;
    std::vector<Attribute> attributes;
};

struct FileInfo {
    std::vector<Part> parts;
};

// Ảnh float RGBA (theo display window), alpha = 1 nếu không có.
struct Image {
    int width = 0, height = 0;
    int sourceChannels = 0; // số channel thật sự đọc được (1..4)
    bool hasAlpha = false;
    std::vector<float> rgba;
    bool empty() const { return width <= 0 || height <= 0; }
    size_t bytes() const { return rgba.size() * sizeof(float); }
};

class ExrFile {
public:
    // Đường dẫn UTF-8.
    static std::unique_ptr<ExrFile> openPath(const std::string& utf8Path, std::string* err);
    // Dữ liệu trong bộ nhớ (dùng cho shell extension đọc từ IStream).
    static std::unique_ptr<ExrFile> openMemory(std::vector<char> data, std::string* err);
    ~ExrFile();

    const FileInfo& info() const;

    // Đọc một layer. maxSize > 0: thu nhỏ (box filter) để vừa trong maxSize x maxSize.
    bool readLayer(int part, int layer, Image& out, std::string* err, int maxSize = 0) const;

    // Layer mặc định nên hiển thị (RGBA của part đầu tiên nếu có).
    void defaultLayer(int& part, int& layer) const;

    // Gợi ý color space ghi trong header (vd attribute "colorspace", "oiio:ColorSpace").
    std::string colorspaceAttribute(int part) const;
    // Chromaticities: rx ry gx gy bx by wx wy. Trả false nếu không có.
    bool chromaticities(int part, float out[8]) const;

private:
    ExrFile();
    struct Impl;
    std::unique_ptr<Impl> d;
};

enum class ChannelMode { RGB = 0, Red, Green, Blue, Alpha, Luminance };

struct ViewSettings {
    std::string input;    // color space đầu vào
    std::string display;
    std::string view;
    float exposure = 0.f; // stops
    float gamma = 1.f;
    ChannelMode channel = ChannelMode::RGB;
};

// ---- GPU (OpenGL) ----
// LUT mà shader OCIO cần. depth > 0: LUT 3D (width = height = depth = cạnh).
struct GpuLut {
    std::string textureName;
    std::string samplerName;
    int width = 0, height = 0, depth = 0;
    int channels = 3;   // 1 (RED) hoặc 3 (RGB)
    bool linear = true; // nội suy tuyến tính hay nearest
    std::vector<float> values;
};

struct GpuUniformValue {
    std::string name;
    enum Type { Double, Bool, Float3, VectorFloat, VectorInt } type = Double;
    std::vector<float> f; // Double/Bool/Float3/VectorFloat
    std::vector<int> i;   // VectorInt
};

struct GpuShader {
    std::string text; // GLSL 1.30 (dùng được với #version 330 core): uniform/sampler + vec4 OCIODisplay(vec4)
    std::vector<GpuLut> luts;
};

class ColorPipeline {
public:
    ColorPipeline();
    ~ColorPipeline();

    // path rỗng => tự chọn: biến môi trường OCIO -> config ACES dựng sẵn (OCIO >= 2.2)
    // -> config tối giản nhúng sẵn.
    bool loadConfig(const std::string& utf8Path, std::string* err);
    std::string configName() const;
    std::string configPath() const; // rỗng nếu là config dựng sẵn

    std::vector<std::string> colorSpaces() const;
    std::vector<std::string> displays() const;
    std::vector<std::string> views(const std::string& display) const;
    std::string defaultDisplay() const;
    std::string defaultView(const std::string& display) const;
    bool hasColorSpace(const std::string& name) const;
    std::string canonicalColorSpace(const std::string& nameOrAlias) const; // "" nếu không có
    std::string sceneLinear() const;

    // Đoán color space đầu vào từ header + tên file. "" nếu không có manh mối.
    std::string guessInput(const ExrFile& file, int part, const std::string& filename) const;
    // Bản không cần ExrFile còn mở: attr = colorspaceAttribute(), chroma = nullptr nếu không có.
    std::string guessInput(const std::string& attr, const float* chroma, const std::string& filename) const;

    // Render sang BGRA 8-bit (thứ tự byte B,G,R,A — dùng trực tiếp cho QImage
    // Format_RGB32/ARGB32 và DIB của Windows). Alpha đầu ra luôn 255.
    bool render(const Image& in, const ViewSettings& vs, std::vector<uint8_t>& bgra, std::string* err);

    // Sinh shader GPU cho input -> display/view, exposure là uniform động
    // (đổi exposure không cần build lại shader). Gamma/kênh do viewer tự làm sau/trước OCIODisplay.
    bool buildGpuShader(const std::string& input, const std::string& display, const std::string& view,
                        GpuShader& out, std::string* err);
    // Giá trị uniform hiện tại của shader vừa build, với exposure (stops) cho trước.
    std::vector<GpuUniformValue> gpuUniforms(float exposure);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Tiện ích
std::string toLower(std::string s);
// Phân tích tên file sequence: "shot_v01.0042.exr" -> prefix "shot_v01.", số 42, padding 4, suffix ".exr"
bool splitFrameNumber(const std::string& filename, std::string& prefix, long long& frame, int& padding,
                      std::string& suffix);

} // namespace exrv
