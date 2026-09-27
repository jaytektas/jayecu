// PanelLibrary — the page store's serialisation, deliberately compiled rather than inline. See the
// declarations in PanelLibrary.h for why: these walk every page in the document, and inline they were
// compiled into main.cpp, which is built at a lower optimisation level because its own compile time is
// the cost of every UI change. A 7.5 MB document parsed by unoptimised code is a seven-second wait
// between the window appearing and the interface arriving.

#include "PanelLibrary.h"

jf::JJson PanelLibrary::toJson() const {
    jf::JJson o = jf::JJson::object();
    for (const auto& [k, v] : panels_) o[k] = v->toJson();
    return o;
}

void PanelLibrary::load(const jf::JJson& o) {
    panels_.clear();
    if (!o.isObject()) return;
    for (const auto& [k, v] : o.obj()) {
        auto m = std::make_unique<PanelModel>();
        m->load(v);
        panels_.emplace(k, std::move(m));
    }
}

jf::JJson PanelLibrary::parseDocument(const std::string& path) { return jf::JJson::parseFile(path); }
