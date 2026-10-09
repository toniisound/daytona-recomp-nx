// ROM-free check: a Raster split into scanline lanes (Raster::set_parallel)
// draws exactly the same layer as the serial render, with real threads.
#include "runtime/raster.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

namespace {
std::mt19937 rng(0x5eed1a7e);
uint32_t random32() { return rng(); }
int pick(int max) { return int(random32() % unsigned(max)); }

struct Memory {
    std::vector<uint8_t> pal, xlat, luma;
    std::vector<uint32_t> tex0, tex1;
    Memory() : pal(0x4000), xlat(0xc000), luma(0x20000), tex0(0x80000), tex1(0x80000) {
        for (auto *bytes : {&pal, &xlat, &luma}) for (auto &b : *bytes) b = uint8_t(random32());
        for (auto *sheet : {&tex0, &tex1}) for (auto &w : *sheet) w = random32();
    }
    rt::VideoMem view() const { return {pal.data(), xlat.data(), luma.data(), tex0.data(), tex1.data()}; }
};

rt::GeoPoly polygon(int vertices) {
    rt::GeoPoly p;
    p.num_vertices = uint8_t(vertices);
    p.window = uint8_t(pick(4));
    p.z = uint16_t(pick(16));
    p.luma = uint8_t(random32());
    p.texlod = pick(4);
    p.viewport[0] = pick(40); p.viewport[1] = pick(40); p.viewport[2] = 495 - pick(40); p.viewport[3] = 384 - pick(40);
    p.texheader[0] = uint16_t(random32());
    p.texheader[1] = uint16_t(random32());
    p.texheader[2] = uint16_t(random32());
    p.texheader[3] = uint16_t(random32());
    const float cx = float(pick(540) - 22), cy = float(pick(440) - 28), rx = float(pick(170) + 1), ry = float(pick(150) + 1);
    for (int i = 0; i < vertices; ++i) {
        const double angle = 6.2831853071795864769 * double(i) / double(vertices);
        const float z = float(pick(160) + 10) / 10.f;
        const float x = cx + rx * float(std::cos(angle)), y = cy + ry * float(std::sin(angle));
        p.v[i].x = x * z; p.v[i].y = (384.f - y) * z; p.v[i].p[0] = z;
        p.v[i].p[1] = float(pick(8200) - 64) * 8.f; p.v[i].p[2] = float(pick(8200) - 64) * 8.f;
    }
    return p;
}

void threaded(int count, const std::function<void(int)> &job) {
    std::vector<std::thread> threads;
    for (int k = 1; k < count; ++k) threads.emplace_back(job, k);
    job(0);
    for (auto &t : threads) t.join();
}
} // namespace

int main() {
    Memory mem;
    const rt::VideoMem view = mem.view();
    rt::Raster serial;
    std::vector<rt::Raster> lanes(3);
    for (int k = 0; k < 3; ++k) lanes[size_t(k)].set_parallel(threaded, k + 2); // 2, 3 and 4 lanes
    size_t drawn = 0;
    for (int frame = 0; frame < 300; ++frame) {
        const int margin = frame % 3 == 0 ? 0 : 48;
        serial.set_wide_margin(margin);
        for (auto &r : lanes) r.set_wide_margin(margin);
        std::vector<rt::GeoPoly> polys;
        const int n = pick(120) + 1;
        for (int i = 0; i < n; ++i) polys.push_back(polygon(3 + pick(6)));
        const int windows = pick(4);
        serial.render(polys, windows, view, 0, 0, 0, 495 + 2 * margin, 0, 383);
        for (size_t i = 0; i < size_t(serial.stride()) * 512; ++i) drawn += serial.pixels()[i] != 0;
        for (size_t k = 0; k < lanes.size(); ++k) {
            lanes[k].render(polys, windows, view, 0, 0, 0, 495 + 2 * margin, 0, 383);
            const size_t size = size_t(serial.stride()) * 512;
            for (size_t i = 0; i < size; ++i)
                if (serial.pixels()[i] != lanes[k].pixels()[i]) {
                    std::fprintf(stderr, "FAIL: frame %d, %zu lanes, pixel %zu: %08x != %08x\n", frame, k + 2, i,
                                 lanes[k].pixels()[i], serial.pixels()[i]);
                    return 1;
                }
        }
    }
    if (drawn == 0) { std::fputs("FAIL: no pixel was ever drawn\n", stderr); return 1; }
    std::printf("raster lanes: 300 random frames identical with 2, 3 and 4 lanes (%zu pixels drawn)\n", drawn);
    return 0;
}
