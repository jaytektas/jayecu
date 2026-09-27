#include "ImageCache.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_SIMD          // portability over the last few percent: this decodes gauge art, not video
#include "../../third_party/stb_image.h"

const ImageCache::Entry& ImageCache::get(const std::string& path) {
    static const Entry kMiss{};
    if (path.empty()) return kMiss;

    auto it = m_map.find(path);
    if (it != m_map.end()) return it->second;      // hit OR a cached miss — both are answers

    Entry e{};
    if (m_hal) {
        int w = 0, h = 0, n = 0;
        // 4 = force RGBA, which is what uploadTexture takes; stb converts greyscale/palette/RGB for us.
        if (stbi_uc* px = stbi_load(path.c_str(), &w, &h, &n, 4)) {
            if (w > 0 && h > 0) {
                e.tex = m_hal->uploadTexture(px, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                e.w = w; e.h = h;
            }
            stbi_image_free(px);
        }
    }
    // Cached even on failure: a widget pointing at a missing file must not re-read the disk every frame.
    // If the hal was not set yet the miss would be permanent, so only remember it once there IS a hal.
    if (m_hal) return m_map.emplace(path, e).first->second;
    return kMiss;
}

void ImageCache::clear() {
    if (m_hal)
        for (auto& [path, e] : m_map)
            if (e.tex != jf::kNullTexture) m_hal->releaseTexture(e.tex);
    m_map.clear();
}
