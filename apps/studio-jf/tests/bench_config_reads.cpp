// What one frame of CONFIG READS costs — the answer to "why has the studio gone sluggish".
//
// Every widget resolves several paths per frame: its value, its enable condition, its visibility, its
// unit and its decimals. A page of two hundred controls is therefore hundreds of reads at 30-60 frames
// a second, and anything added to Cache::configValue() is paid that often. This measures it against the
// REAL meta, so the number is the one the studio actually pays.
//
// Recorded here so a regression has something to be compared against. Measured on the jaytek_v1 meta:
//
//   3.36 ms/frame   isExpressionField() asked at the FRONT of configValue (it re-parses the path and
//                   scans an array's element ids to answer) — 20% of a 60 Hz frame, on reads alone
//   2.63 ms/frame   …asked of the resolved field instead (L.datatype == "EXPR")
//   1.15 ms/frame   …with locate() memoised: what a path resolves to depends only on the meta
//   0.51 ms/frame   …and configValue no longer asking isTable() first, which repeated locate's walk
//
//   build:  cmake --build build --target bench_config_reads
//   run:    ./build/bench_config_reads "<meta path>"
#include "model/Cache.h"
#include "model/MetaModel.h"
#include <chrono>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    MetaModel meta;
    if (!meta.loadFile(argv[1])) { std::printf("meta load failed\n"); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&meta);
    c.setConfigImage(meta.defaultImage());

    const std::vector<std::string> paths = {
        "engine.cylinder_count", "fuel_calculator.stage1_stoich_x10", "idle.enabled",
        "sensors.sensor[clt].enabled", "sensors.sensor[clt].source", "sensors.sensor[tps].scale_x1000",
        "outputs.output[7].function", "outputs.output[7].cyl_mask", "outputs.output[7].min_on_ms",
        "trigger.streams[0].slots", "engine.cyl[3].bank", "electronic_throttle.etb[0].tps_a_src",
    };
    // 400 reads is a modest page; a table page with a live cell trace does far more.
    constexpr int kReads = 400, kFrames = 60;
    auto t0 = std::chrono::steady_clock::now();
    double sink = 0;
    for (int f = 0; f < kFrames; ++f)
        for (int i = 0; i < kReads; ++i) sink += c.configValue(paths[i % paths.size()]);
    auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("%d reads x %d frames: %.1f ms total, %.2f ms/frame  (16.7 ms is one frame at 60 Hz)\n",
                kReads, kFrames, ms, ms / kFrames);
    // …and the parts, so the cost lands somewhere specific rather than on "config reads".
    auto timeIt = [&](const char* what, auto&& fn) {
        auto a = std::chrono::steady_clock::now();
        for (int f = 0; f < kFrames; ++f)
            for (int i = 0; i < kReads; ++i) fn(paths[i % paths.size()]);
        auto b = std::chrono::steady_clock::now();
        std::printf("  %-22s %.2f ms/frame\n", what,
                    std::chrono::duration<double, std::milli>(b - a).count() / kFrames);
    };
    // A MISS is the case the Diagnostics tab lives in: value() falls back to configValue() for any
    // name, so 380 telemetry channels resolve nothing, 30 times a second.
    const std::vector<std::string> misses = { "rpm", "clt", "map", "tps", "lambda_1", "knock_level",
                                              "vvt_angle_1", "inj_pw", "advance", "battery" };
    {
        auto a = std::chrono::steady_clock::now();
        for (int f = 0; f < kFrames; ++f)
            for (int i = 0; i < kReads; ++i) sink += c.configValue(misses[i % misses.size()]);
        auto b = std::chrono::steady_clock::now();
        std::printf("  %-22s %.2f ms/frame\n", "configValue() MISS",
                    std::chrono::duration<double, std::milli>(b - a).count() / kFrames);
    }
    timeIt("locate()",      [&](const std::string& p) { sink += meta.locate(p).offset; });
    timeIt("isTable()",     [&](const std::string& p) { sink += c.isTable(p) ? 1 : 0; });
    timeIt("configValue()", [&](const std::string& p) { sink += c.configValue(p); });
    return sink == 1e9 ? 1 : 0;
}
