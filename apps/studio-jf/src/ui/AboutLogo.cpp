#include "AboutLogo.h"

// The one decoder the studio already vendors, used the way ImageCache uses it.
#include "../../third_party/stb_image.h"

#include "../app/Resources.h"

namespace aboutlogo {

std::string path(const char* file) { return resources::path(file); }

Pixels load(const std::string& file) {
    Pixels out;
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load(file.c_str(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return out; }
    out.rgba.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    out.w = static_cast<uint32_t>(w);
    out.h = static_cast<uint32_t>(h);
    stbi_image_free(px);
    return out;
}

void attach(jf::JDialogRequest& req) {
    int w = 0, h = 0, n = 0;
    // 4 = force RGBA, which is the layout JDialogRequest::imageRgba is defined in.
    stbi_uc* px = stbi_load(path().c_str(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return; }
    req.imageRgba.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    req.imageWidth  = static_cast<uint32_t>(w);
    req.imageHeight = static_cast<uint32_t>(h);
    stbi_image_free(px);
}

}  // namespace aboutlogo
