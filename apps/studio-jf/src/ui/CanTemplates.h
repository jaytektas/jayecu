#pragma once
//
// CanTemplates — the published protocols, as data the studio loads rather than tables the ECU carries.
//
// A dash's broadcast set or a vehicle interface is a long list of frames that somebody transcribed
// once from a document. Compiling that into the firmware made every correction a firmware release,
// and made the ECU carry forty frames for protocols this car does not speak. So it ships as JSON
// beside the meta, is loaded into the tune's generic CAN pool, and stays editable afterwards —
// which is the whole point: a frame that turns out to be wrong is a tune edit on the car.
//
// Codegen validates these against the signal catalog at build time, so a template naming a channel
// that no longer exists stops the build rather than loading here as a field bound to nothing.
//
#include <j/config/Json.h>

#include "../model/StudioPaths.h"
#include "../app/Resources.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace CanTemplates {

struct Field {
    // Empty means the field carries NO channel: a sensor consumes it, and the sensor publishes the
    // reading with its own calibration and diagnostics rather than the field writing it underneath.
    std::string sig;
    int      bitOff = 0, width = 8, flags = 0, policy = 0, ttlMs = 500;
    uint32_t sentinel = 0;
    double   scale = 1.0, offset = 0.0;
};
struct Frame {
    int  id = 0, dlc = 8, periodMs = 0;
    bool ext = false;
    std::string name;
    std::vector<Field> fields;
};
struct Template {
    std::string id, name, note;
    // The sensor this template is for, when it is a CAN sensor rather than a protocol — so the
    // editor can say what to point at the field instead of leaving the note to carry it alone.
    std::string consumer;
    bool transmit = true;
    int  bitrate = 0;
    std::vector<Frame> frames;
};

// Where they come from. The ones SHIPPED beside the studio (the installer and the AppImage carry
// can_templates/), and the data folder's (`make studio-meta` fills it on a development machine, and a
// user may add their own). One of the same file name in the data folder wins, so a template can be
// replaced without touching the program.
inline std::string dir() { return StudioPaths::dataDir("can_templates"); }
inline std::string shippedDir() { return resources::exeDir() + "can_templates"; }

inline std::vector<Template> load() {
    std::vector<Template> out;
    std::error_code ec;
    std::map<std::string, std::filesystem::path> byName;
    for (const std::string& d : { shippedDir(), dir() }) {
        const std::filesystem::path root(d);
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& e : std::filesystem::directory_iterator(root, ec))
            if (e.is_regular_file() && e.path().extension() == ".json") byName[e.path().filename().string()] = e.path();
    }
    std::vector<std::filesystem::path> files;
    for (const auto& [n, p] : byName) files.push_back(p);

    for (const auto& p : files) {
        const jf::JJson j = jf::JJson::parseFile(p.string());
        if (j.isNull()) continue;
        Template t;
        t.id       = j["id"].str();
        t.name     = j["name"].str();
        t.note     = j["note"].str();
        t.bitrate  = j["bitrate"].template number<int>();
        t.transmit = j["direction"].str() != "receive";
        t.consumer = j["consumer"].str();
        if (t.id.empty()) t.id = p.stem().string();
        if (t.name.empty()) t.name = t.id;
        for (const jf::JJson& fj : j["frames"].arr()) {
            Frame f;
            f.id       = fj["id"].template number<int>();
            f.dlc      = fj["dlc"].template number<int>();
            f.periodMs = fj["period_ms"].template number<int>();
            f.ext      = fj["ext"].boolean();
            f.name     = fj["name"].str();
            for (const jf::JJson& sj : fj["fields"].arr()) {
                Field s;
                s.sig      = sj["sig"].str();          // empty = a sensor consumes this field
                s.ttlMs    = sj["ttl_ms"].number(500);
                s.sentinel = static_cast<uint32_t>(sj["sentinel"].number(0.0));
                s.bitOff = sj["bit_off"].template number<int>();
                s.width  = sj["width"].template number<int>();
                s.flags  = sj["flags"].template number<int>();
                s.scale  = sj["scale"].number(1.0);
                s.offset = sj["offset"].number(0.0);
                s.policy = sj["policy"].template number<int>();
                f.fields.push_back(s);
            }
            t.frames.push_back(f);
        }
        if (!t.frames.empty()) out.push_back(std::move(t));
    }
    return out;
}

// Loaded once — the set only changes when the studio is reinstalled.
inline const std::vector<Template>& available() {
    static const std::vector<Template> kAll = load();
    return kAll;
}

}  // namespace CanTemplates
