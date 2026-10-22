#pragma once

// MetaLibrary — where the studio finds the definition (the meta) for a layout hash: the meta library
// first, then every firmware kit it holds (shipped beside the studio, or downloaded). Shared by the
// window's connect and open paths and by `studio --headless`, so the two cannot find different metas.

#include <string>

class MetaModel;

namespace metalib {

// The path of a meta describing `layoutHash`, or "" when nothing here does.
std::string forLayoutHash(const std::string& layoutHash);

// The meta for `layoutHash`, loaded once and kept for the life of the process; null when nothing
// here describes it. What a tune load hands TuneFile::deserialise as the layout the tune was saved
// on, so a value whose scale changed between firmwares is converted rather than copied.
const MetaModel* loadedFor(const std::string& layoutHash);

}  // namespace metalib
