#pragma once

// ImageCache — path → GPU texture, decoded once and kept.
//
// A widget's render() is handed a JPrimitiveBuffer, not the JGpuHal, so it cannot upload a texture at the
// point it wants to draw one. It also must not decode a PNG per frame. So both happen HERE: the app hands
// the cache its hal once, a widget asks for a path, and it gets back a handle plus the pixel size (which
// it needs to keep the image's aspect ratio).
//
// A failed load is cached as a MISS, deliberately. Without that, a widget naming a file that does not
// exist re-reads the disk every frame for ever — a typo in a path becoming a permanent I/O load with
// nothing on screen to explain it.

#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/GpuHal.h>

#include <string>
#include <unordered_map>

class ImageCache {
public:
    struct Entry { jf::TextureHandle tex = jf::kNullTexture; int w = 0, h = 0; };

    static ImageCache& instance() { static ImageCache c; return c; }

    // The app owns the hal and calls this once it has one. Until then every get() is a miss — a widget
    // asking early gets nothing and simply draws its fallback, rather than the cache guessing.
    void setHal(jf::JGpuHal* hal) { m_hal = hal; }

    // Decode + upload on first ask; the same handle thereafter. Returns a null-tex Entry on any failure
    // (no hal yet, unreadable file, unsupported format), which every caller treats as "draw the fallback".
    const Entry& get(const std::string& path);

    // Drop everything (a document close, a hal teardown). Textures are released through the hal that
    // made them, so this must run while that hal is still alive.
    void clear();

private:
    ImageCache() = default;
    jf::JGpuHal* m_hal = nullptr;
    std::unordered_map<std::string, Entry> m_map;
};
