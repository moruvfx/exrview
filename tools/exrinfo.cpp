// exrinfo — công cụ dòng lệnh: in thông tin EXR và (tuỳ chọn) xuất ảnh đã qua OCIO ra PPM.
//   exrinfo file.exr [out.ppm] [--display D] [--view V] [--input CS] [--exposure E] [--max N] [--config path]
#include "exrcore.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: exrinfo file.exr [out.ppm] [--display D] [--view V] [--input CS] [--exposure E] "
                     "[--max N] [--config path] [--memory] [--list]\n";
        return 2;
    }
    std::string path = argv[1], out, config;
    exrv::ViewSettings vs;
    int maxSize = 0;
    bool memory = false, list = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--display") vs.display = next();
        else if (a == "--view") vs.view = next();
        else if (a == "--input") vs.input = next();
        else if (a == "--exposure") vs.exposure = std::stof(next());
        else if (a == "--max") maxSize = std::stoi(next());
        else if (a == "--config") config = next();
        else if (a == "--memory") memory = true;
        else if (a == "--list") list = true;
        else out = a;
    }

    std::string err;
    std::unique_ptr<exrv::ExrFile> f;
    if (memory) {
        std::ifstream is(path, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
        f = exrv::ExrFile::openMemory(std::move(data), &err);
    } else {
        f = exrv::ExrFile::openPath(path, &err);
    }
    if (!f) { std::cerr << "open failed: " << err << "\n"; return 1; }

    for (size_t p = 0; p < f->info().parts.size(); ++p) {
        auto& P = f->info().parts[p];
        std::cout << "part " << p << " '" << P.name << "' " << P.type << " " << P.width << "x" << P.height
                  << " data(" << P.dataX0 << "," << P.dataY0 << ")-(" << P.dataX1 << "," << P.dataY1 << ") "
                  << P.compression << "\n";
        for (auto& L : P.layers) {
            std::cout << "  layer " << L.name << ":";
            for (auto& c : L.channels) std::cout << " " << c;
            std::cout << "\n";
        }
        for (auto& a : P.attributes) std::cout << "  @" << a.name << " (" << a.type << ") = " << a.value << "\n";
    }

    exrv::ColorPipeline cp;
    if (!config.empty() && !cp.loadConfig(config, &err)) { std::cerr << "config: " << err << "\n"; return 1; }
    std::cout << "config: " << cp.configName() << "\n";
    if (list) {
        std::cout << "colorspaces:";
        for (auto& c : cp.colorSpaces()) std::cout << " [" << c << "]";
        std::cout << "\ndisplays:\n";
        for (auto& dsp : cp.displays()) {
            std::cout << "  " << dsp << ":";
            for (auto& v : cp.views(dsp)) std::cout << " [" << v << "]";
            std::cout << "\n";
        }
    }

    int part, layer;
    f->defaultLayer(part, layer);
    std::string guess = cp.guessInput(*f, part, path);
    std::cout << "guessed input: " << (guess.empty() ? "(none)" : guess) << "\n";
    if (out.empty()) return 0;

    exrv::Image img;
    err.clear();
    auto t0 = std::chrono::steady_clock::now();
    if (!f->readLayer(part, layer, img, &err, maxSize)) { std::cerr << "read failed: " << err << "\n"; return 1; }
    if (!err.empty()) std::cerr << "warning: " << err << "\n";
    if (vs.input.empty()) vs.input = guess.empty() ? cp.sceneLinear() : guess;
    if (vs.display.empty()) vs.display = cp.defaultDisplay();
    if (vs.view.empty()) vs.view = cp.defaultView(vs.display);
    std::cout << "render " << img.width << "x" << img.height << " " << vs.input << " -> " << vs.display << " / "
              << vs.view << "\n";
    auto t1 = std::chrono::steady_clock::now();
    std::vector<uint8_t> bgra;
    if (!cp.render(img, vs, bgra, &err)) { std::cerr << "render failed: " << err << "\n"; return 1; }
    auto t2 = std::chrono::steady_clock::now();
    if (!cp.render(img, vs, bgra, &err)) return 1; // lần 2: processor đã cache
    auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::cout << "time: read " << ms(t0, t1) << " ms, render " << ms(t1, t2) << " ms, re-render " << ms(t2, t3) << " ms\n";
    std::ofstream os(out, std::ios::binary);
    os << "P6\n" << img.width << " " << img.height << "\n255\n";
    for (size_t i = 0; i < (size_t)img.width * img.height; ++i) {
        char rgb[3] = {(char)bgra[i * 4 + 2], (char)bgra[i * 4 + 1], (char)bgra[i * 4 + 0]};
        os.write(rgb, 3);
    }
    return 0;
}
