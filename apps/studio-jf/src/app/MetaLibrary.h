#pragma once

// MetaLibrary — where the studio finds the definition (the meta) for a layout hash: the meta library
// first, then every firmware kit it holds (shipped beside the studio, or downloaded). Shared by the
// window's connect and open paths and by `studio --headless`, so the two cannot find different metas.

#include <string>

namespace metalib {

// The path of a meta describing `layoutHash`, or "" when nothing here does.
std::string forLayoutHash(const std::string& layoutHash);

}  // namespace metalib
