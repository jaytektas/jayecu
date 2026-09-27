// The CAN field picker: which generic CAN receive field a sensor reads, chosen BY NAME.
//
// A CAN sensor names its field with a FRAME (bus/ext/id as one key) and the field's START BIT. This
// checks the list the picker builds out of the tune's frame pool, which entries it will let you pick,
// and — the reason the reference changed at all — that a stored choice survives the pool being
// REPACKED and reads as broken when the field it names is gone.
//
//   cmake --build build --target can_field_picker_test && ./build/can_field_picker_test
#include "../src/surface/widgets/CanFieldWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Frame flags, as the firmware reads them.
enum : int { M_USED = 1, M_TX = 2 };
constexpr double kSigNone = 65535.0;

static void frame(Cache& C, int i, int flags, int bus, int id, int first, int n, const char* name) {
    const std::string p = "can.gc_frame[" + std::to_string(i) + "].";
    C.setConfigValue(p + "flags", flags);
    C.setConfigValue(p + "bus", bus);
    C.setConfigValue(p + "id", id);
    C.setConfigValue(p + "dlc", 8);
    C.setConfigValue(p + "first_field", first);
    C.setConfigValue(p + "field_count", n);
    C.setConfigString(p + "name", name);
}
static void field(Cache& C, int i, double sig, int bit, int width) {
    const std::string p = "can.gc_field[" + std::to_string(i) + "].";
    C.setConfigValue(p + "sig", sig);
    C.setConfigValue(p + "bit_off", bit);
    C.setConfigValue(p + "width", width);
    C.setConfigValue(p + "scale", 1.0);
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
#ifdef REAL_META
                            REAL_META,
#endif
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[can-field] (no meta found — skipped)"); return 0; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(image);

    const std::string slot = "sensors.sensor[lambda_1]";
    if (!meta.locate(slot + ".can_frame").valid() || !meta.locate(slot + ".can_bit").valid()) {
        std::puts("[can-field] (meta has no can_frame/can_bit on a sensor — skipped)");
        return 0;
    }

    std::puts("=== the CAN field picker ===");

    // THE SHIPPED DEFAULT IS UNSET, and unset cannot be 0 — 0 is a real channel (abs_mode), so a
    // field left alone would publish onto it and two of them would be two producers of one channel.
    ck(C.configValue("can.gc_field[0].sig") == kSigNone,
       "a field's channel defaults to UNSET, not to channel 0",
       std::to_string(C.configValue("can.gc_field[0].sig")));

    // One receive frame carrying two fields: one for a sensor to read, one the frame publishes itself.
    frame(C, 0, M_USED, /*bus=*/0, /*id=*/0x360, /*first=*/0, /*n=*/2, "Wideband");
    field(C, 0, kSigNone, /*bit=*/7,  16);        // no channel: a sensor reads it
    field(C, 1, 134.0,    /*bit=*/23, 16);        // names a channel: the frame writes it
    // …and a TRANSMIT frame, which can never feed a sensor: nothing fills a transmit field's value.
    frame(C, 1, M_USED | M_TX, /*bus=*/0, /*id=*/0x400, /*first=*/2, /*n=*/1, "Dash");
    field(C, 2, kSigNone, /*bit=*/7, 8);

    jf::JSceneGraph graph;
    PanelElement el;
    el.type = "canfield";
    el.props["signalName"] = "[#" + slot + ".can_frame]";
    CanFieldWidget w(graph);
    w.bind(&el, &C);

    ck(w.bitPath() == slot + ".can_bit",
       "the start bit is derived as the bound frame's sibling", w.bitPath());

    auto cs = w.choices();
    ck(cs.size() == 3, "\"(none)\" plus the two fields of the receive frame",
       std::to_string(cs.size()) + " entries");
    if (cs.size() != 3) { std::printf("\n%d failure(s)\n", fails); return fails ? 1 : 0; }

    ck(cs[0].bit == -1, "…\"(none)\" heads the list and is a real answer");
    // A FRAME IS NAMED BY ITS NAME AND FOUND BY ITS ID, and the label carries both: the name is what
    // a person recognises, the bus and id are what makes two frames called "Wideband" different.
    ck(has(cs[1].label, "Wideband") && has(cs[1].label, "CAN1 0x360") && has(cs[1].label, "bit 7 +16"),
       "a field reads as its frame's name, bus, id and start bit", cs[1].label);
    ck(cs[1].pickable, "…a field with no channel is one a sensor may read");
    ck(!cs[2].pickable, "…and one that already names a channel is listed, not offered");
    ck(has(cs[2].label, "\xE2\x86\x92"), "…with what it carries, so the reason is on screen", cs[2].label);

    // A TRANSMIT frame's field is not in the list at all: it is not a thing that could ever be read.
    for (const auto& c : cs) ck(!has(c.label, "Dash"), "a transmit frame's field is never offered");

    // WHAT THE TUNE IS ON. Nothing named yet -> "(none)".
    C.setConfigValue(slot + ".can_bit", -1);
    ck(w.currentChoice(cs) == 0, "a sensor naming no field sits on \"(none)\"");

    // Name the first field, and the picker finds it.
    C.setConfigValue(slot + ".can_frame", double(cs[1].frame));
    C.setConfigValue(slot + ".can_bit",   double(cs[1].bit));
    ck(w.currentChoice(cs) == 1, "…and naming a field selects it",
       std::to_string(w.currentChoice(cs)));

    // THE POOL IS REPACKED — the studio rewrites every frame's field run on any structural edit. The
    // field moves to another pool slot; its frame and its start bit do not, so the reference holds.
    // This is the whole reason the sensor stopped storing a pool index.
    frame(C, 2, M_USED, /*bus=*/0, /*id=*/0x360, /*first=*/8, /*n=*/2, "Wideband");
    frame(C, 0, 0, 0, 0, 0, 0, "");                    // the old slot is freed, as a repack frees it
    field(C, 8, kSigNone, 7,  16);
    field(C, 9, 134.0,    23, 16);
    cs = w.choices();
    ck(w.currentChoice(cs) >= 1 && cs[size_t(w.currentChoice(cs))].bit == 7,
       "the field moves through the pool and the choice still resolves",
       std::to_string(w.currentChoice(cs)));

    // …AND A REFERENCE THAT BREAKS SAYS SO. Move the field to other bits — the sensor is naming a
    // start bit the frame no longer has, which must not read as "never set up".
    field(C, 8, kSigNone, 31, 16);
    cs = w.choices();
    ck(w.currentChoice(cs) == -1, "a start bit the frame no longer has reads as missing, not as none",
       std::to_string(w.currentChoice(cs)));

    // Delete the frame outright and it is the same answer, for the same reason.
    frame(C, 2, 0, 0, 0, 0, 0, "");
    cs = w.choices();
    ck(cs.size() == 1 && w.currentChoice(cs) == -1,
       "…and so does a frame that has been deleted");

    std::printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
