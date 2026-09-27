// AN IMAGE IS DECODED ONCE, AND A BAD PATH COSTS NOTHING.
//
// A widget's render() is handed a primitive buffer, not the hal, so it can neither upload a texture at
// the point it wants to draw nor afford to decode a file per frame. The cache does both once.
//
// The failure path matters as much as the success one: a widget naming a file that does not exist is a
// typo, and re-reading the disk every frame for ever is what a typo must NOT cost. A miss is remembered.
//
//   cmake --build build --target image_cache_test && ./build/image_cache_test

#include "../src/model/ImageCache.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[image] %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

// A hal that counts uploads instead of making textures — the cache's contract is about how OFTEN it
// uploads, which is exactly what a real GPU would make hardest to observe.
struct CountingHal : jf::JGpuHal {
    int uploads = 0, releases = 0;
    // The rest of the hal is a rendering back end this test has no use for; stubbed so the counting
    // half can exist on its own.
    bool initialize() override { return true; }
    void resizeSurface(jf::GpuSurfaceId, uint32_t, uint32_t) override {}
    jf::JGpuFrameContext beginFrame(jf::GpuSurfaceId) override { return {}; }
    bool uploadFontAtlas(const uint8_t*, uint32_t, uint32_t) override { return true; }
    void drawPrimitives(const jf::JPrimitiveBuffer&) override {}
    void submitAndPresentFrame(const jf::JGpuFrameContext&) override {}
    void waitIdle() override {}
    jf::JGpuApiType getBackendType() const noexcept override { return jf::JGpuApiType{}; }
    jf::GpuSurfaceId createSurface(const jf::JNativeWindowHandle&, uint32_t, uint32_t) override { return 0; }
    void destroySurface(jf::GpuSurfaceId) override {}
    jf::TextureHandle uploadTexture(const uint8_t*, uint32_t w, uint32_t h) override {
        ++uploads; lastW = w; lastH = h; return static_cast<jf::TextureHandle>(uploads);
    }
    void releaseTexture(jf::TextureHandle) override { ++releases; }
    uint32_t lastW = 0, lastH = 0;
};

// Smallest thing stb will decode without a codec: a 2x1 uncompressed 32-bit TGA.
static std::string writeTga(const char* path) {
    std::vector<uint8_t> f(18, 0);
    f[2] = 2;                     // uncompressed true-colour
    f[12] = 2; f[13] = 0;         // width  = 2
    f[14] = 1; f[15] = 0;         // height = 1
    f[16] = 32; f[17] = 0x20;     // 32 bpp, top-left origin
    const uint8_t px[8] = { 0,0,255,255,  0,255,0,255 };   // BGRA: red, green
    f.insert(f.end(), px, px + 8);
    if (FILE* fp = std::fopen(path, "wb")) { std::fwrite(f.data(), 1, f.size(), fp); std::fclose(fp); }
    return path;
}

int main() {
    CountingHal hal;
    ImageCache& c = ImageCache::instance();

    // Before the app hands over a hal, every ask is a miss — and is NOT remembered, or the miss would
    // outlive the reason for it and the image would never appear once the hal arrived.
    const std::string img = writeTga("/tmp/_jf_needle_test.tga");
    check(c.get(img).tex == jf::kNullTexture, "no hal yet -> a miss");

    c.setHal(&hal);
    const ImageCache::Entry& a = c.get(img);
    check(a.tex != jf::kNullTexture, "with a hal the image loads");
    check(a.w == 2 && a.h == 1,      "and reports its pixel size (the aspect the widget needs)");
    check(hal.uploads == 1,          "one upload");

    const ImageCache::Entry& b = c.get(img);
    check(b.tex == a.tex,   "the same path returns the same texture");
    check(hal.uploads == 1, "... without decoding or uploading again");

    // A path that does not exist: one attempt, then remembered as a miss.
    for (int i = 0; i < 5; ++i) check(c.get("/tmp/_jf_no_such_image.png").tex == jf::kNullTexture,
                                      i == 0 ? "a missing file is a miss" : "... still a miss, every time");
    check(hal.uploads == 1, "a missing file never uploads");

    c.clear();
    check(hal.releases == 1, "clear() releases what it made");
    std::remove(img.c_str());

    std::printf("[image] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
