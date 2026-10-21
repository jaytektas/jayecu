#include "Headless.h"

#include "AiCommands.h"
#include "MetaLibrary.h"
#include "StudioVersion.h"
#include "comms/DeviceScanner.h"
#include "comms/EcuLink.h"
#include "model/Cache.h"
#include "model/Ecu.h"
#include "model/MetaModel.h"
#include "model/TuneFile.h"

#include <j/config/Json.h>
#include <j/core/JAiBus.h>
#include <j/core/Log.h>
#include <j/core/MainThreadDispatcher.h>
#include <j/io/SerialPort.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace headless {

void useParentConsole() {
#if defined(_WIN32)
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;   // redirected
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
}

namespace {

std::atomic<bool> g_stop{false};

// One line of JSON on stdout: what a supervising script waits for. Flushed, because a pipe buffers.
void say(const jf::JJson& o) {
    std::printf("%s\n", o.dump().c_str());
    std::fflush(stdout);
}
void sayError(const std::string& why) {
    jf::JJson o = jf::JJson::object();
    o["event"] = jf::JJson(std::string("error"));
    o["error"] = jf::JJson(why);
    say(o);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The library entry `want` names: its uid exactly, else the one entry whose car or label contains it.
bool findEcu(const std::string& want, Ecu::Summary& out, std::string& why) {
    const std::vector<Ecu::Summary> all = Ecu::list();
    for (const Ecu::Summary& e : all) if (e.uid == want) { out = e; return true; }
    std::vector<Ecu::Summary> hits;
    const std::string w = lower(want);
    for (const Ecu::Summary& e : all) {
        const Ecu::Vehicle& v = e.vehicle;
        const std::string hay = lower(v.name + " " + v.make + " " + v.model + " " + e.label + " " + e.uid);
        if (hay.find(w) != std::string::npos) hits.push_back(e);
    }
    if (hits.size() == 1) { out = hits.front(); return true; }
    why = hits.empty() ? "no ECU in the library matches '" + want + "'"
                       : "'" + want + "' matches " + std::to_string(hits.size()) + " ECUs; give its uid";
    return false;
}

// identity = "jayecu <board> <version> <build> <layout_hash> <device_uid> t<telemetry_size>"
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

// The serial ports worth asking: the ones that say they are a jayecu, else every USB serial port.
std::vector<std::string> candidatePorts() {
    std::vector<std::string> all, ours;
    for (const auto& info : jf::JSerialPort::availablePorts()) {
        if (!DeviceScanner::isUsbSerial(info)) continue;
        all.push_back(info.port);
        if (info.hasVidPid && info.vendorId == 0x0483 && info.productId == 0x5740) ours.push_back(info.port);
    }
    return ours.empty() ? all : ours;
}

}  // namespace

int run(const Args& args) {
    useParentConsole();
    MetaModel::setStudioVersion(STUDIO_VERSION);
    std::signal(SIGINT,  [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    jf::JAiBus& bus = jf::JAiBus::instance();
    bus.enable();
    if (!bus.enabled()) { sayError("could not create the AI bus"); return 1; }

    static MetaModel meta;          // static: Cache holds a pointer to it for the life of the process
    static EcuLink   link;
    Cache& c = Cache::instance();

    // State the commands report.
    struct State {
        std::string port, uid, board, fw, build, layout, tune;
        Ecu::Summary entry;
        bool connected = false;
    };
    static State st;

    auto loadMeta = [&](const std::string& path) -> std::string {
        if (path.empty()) return "no definition (meta) for this layout here: open the ECU once in the studio "
                                 "window, or pass --meta <file>";
        if (!meta.loadFile(path)) {
            if (!meta.needsStudio().empty()) return "that definition needs studio " + meta.needsStudio();
            return "cannot read the definition " + path;
        }
        c.setMeta(&meta);
        return {};
    };

    // --- what to open --------------------------------------------------------------------------
    if (!args.ecu.empty()) {
        std::string why;
        if (!findEcu(args.ecu, st.entry, why)) { sayError(why); return 2; }
        st.uid = st.entry.uid;
        st.board = st.entry.board;
    }

    if (!args.connect) {
        if (!args.meta.empty() && args.ecu.empty()) {
            // A definition's own defaults: something to read and edit with no ECU and no library.
            if (const std::string why = loadMeta(args.meta); !why.empty()) { sayError(why); return 2; }
            c.setConfigImage(meta.defaultImage());
            st.tune = "defaults";
        } else if (!args.ecu.empty()) {
            std::unique_ptr<Ecu> e(Ecu::open(st.uid));
            if (!e) { sayError("cannot open the library entry " + st.uid); return 2; }
            st.tune = !args.tune.empty() ? args.tune : st.entry.currentTune;
            const std::vector<uint8_t> raw = st.tune.empty() ? std::vector<uint8_t>{} : e->loadTune(st.tune);
            if (raw.empty()) { sayError("the ECU has no tune named '" + st.tune + "'"); return 2; }
            std::string hash;
            if (TuneFile::isDictFormat(raw))
                if (auto j = jf::JJson::tryParse(std::string(raw.begin(), raw.end())); j && j->isObject())
                    hash = (*j)["layout_hash"].str();
            const std::string metaPath = !args.meta.empty() ? args.meta : metalib::forLayoutHash(hash);
            if (const std::string why = loadMeta(metaPath); !why.empty()) { sayError(why); return 2; }
            if (TuneFile::isDictFormat(raw)) {
                MigrationReport rep;
                c.setConfigImage(TuneFile::deserialise(raw, meta, rep, nullptr));
            } else {
                c.setConfigImage(raw);
            }
            st.layout = meta.layoutHash();
        } else {
            sayError("say what to open: --ecu <uid|name>, --connect [port], or --meta <file>");
            return 2;
        }
    }

    // --- the link ------------------------------------------------------------------------------
    link.telemetryFrame.connect([](const std::vector<uint8_t>& f) { Cache::instance().ingestTelemetry(f); });
    c.writeRequested.connect([](int off, const std::vector<uint8_t>& b) { if (link.isOpen()) link.writeConfig(off, b); });
    c.cliRequested.connect([](const std::string& cmd) { if (link.isOpen()) link.sendCli(cmd); });
    c.rangeReadRequested.connect([](int off, int size) { if (link.isOpen()) link.readConfigRange(off, size); });
    link.openedChanged.connect([](bool open) {
        Cache::instance().setLinkOpen(open);
        if (!open && st.connected) {
            st.connected = false;
            jf::JJson o = jf::JJson::object();
            o["event"] = jf::JJson(std::string("disconnected"));
            say(o);
        }
    });
    link.errorOccurred.connect([](const std::string& m) { sayError("link: " + m); });
    link.identityReceived.connect([&](const std::string& sig) {
        const std::vector<std::string> p = words(sig);
        st.board  = p.size() > 1 ? p[1] : std::string();
        st.fw     = p.size() > 2 ? p[2] : std::string();
        st.build  = p.size() > 3 ? p[3] : std::string();
        st.layout = p.size() > 4 ? p[4] : std::string();
        st.uid    = p.size() > 5 ? p[5] : std::string();
        const std::string metaPath = !args.meta.empty() ? args.meta : metalib::forLayoutHash(st.layout);
        if (const std::string why = loadMeta(metaPath); !why.empty()) { sayError(why); link.close(); return; }
        link.setConfigSize(meta.configSize());
        link.setBlockSize(meta.blockSize());
        link.readConfigImage();
    });
    link.configImageReady.connect([&](const std::vector<uint8_t>& img) {
        c.setConfigImage(img);
        c.setBaseline(img);
        st.connected = true;
        if (Ecu* e = Ecu::openOrCreate(st.uid, st.board)) {
            e->setLastPort(st.port);
            st.tune = !args.tune.empty() ? args.tune : e->currentTune();
            delete e;
        }
        st.entry = Ecu::summary(st.uid);
        jf::JJson o = jf::JJson::object();
        o["event"] = jf::JJson(std::string("connected"));
        o["port"] = jf::JJson(st.port);
        o["board"] = jf::JJson(st.board);
        o["firmware"] = jf::JJson(st.fw);
        o["ecu_id"] = jf::JJson(st.uid);
        say(o);
    });

    if (args.connect) {
        st.port = args.port;
        if (st.port.empty()) {
            const std::vector<std::string> cands = candidatePorts();
            if (cands.empty()) { sayError("no serial ports found"); return 3; }
            if (cands.size() == 1 && st.uid.empty()) {
                st.port = cands.front();
            } else {
                // Several, or a particular ECU wanted: ask each port who it is.
                const auto devs = DeviceScanner::scan(cands, 1500);
                std::vector<DeviceScanner::Device> hits;
                for (const auto& d : devs) if (st.uid.empty() || d.uid == st.uid) hits.push_back(d);
                if (hits.size() == 1) {
                    st.port = hits.front().port;
                } else {
                    jf::JJson o = jf::JJson::object();
                    o["event"] = jf::JJson(std::string("error"));
                    o["error"] = jf::JJson(std::string(hits.empty() ? "no matching ECU answered"
                                                                    : "several ECUs answered: pass --ecu <uid> or a port"));
                    jf::JJson list = jf::JJson::array();
                    for (const auto& d : devs) {
                        jf::JJson e = jf::JJson::object();
                        e["port"] = jf::JJson(d.port); e["board"] = jf::JJson(d.board);
                        e["ecu_id"] = jf::JJson(d.uid); e["firmware"] = jf::JJson(d.fwVersion);
                        list.push(e);
                    }
                    o["ecus"] = list;
                    say(o);
                    return 3;
                }
            }
        }
        link.open(st.port);
        if (!link.isOpen()) return 3;   // errorOccurred has said why
    }

    // --- the commands --------------------------------------------------------------------------
    aicmd::Host host;
    host.status = [] {
        jf::JJson o = jf::JJson::object();
        o["mode"] = jf::JJson(std::string("headless"));
        o["studio"] = jf::JJson(std::string(STUDIO_VERSION));
        o["connected"] = jf::JJson(st.connected && link.isOpen());
        if (!st.port.empty()) o["port"] = jf::JJson(st.port);
        jf::JJson ecu = jf::JJson::object();
        if (!st.uid.empty())    ecu["id"] = jf::JJson(st.uid);
        if (!st.board.empty())  ecu["board"] = jf::JJson(st.board);
        if (!st.fw.empty())     ecu["firmware"] = jf::JJson(st.fw);
        if (!st.build.empty())  ecu["build"] = jf::JJson(st.build);
        if (!st.entry.vehicle.name.empty())  ecu["name"] = jf::JJson(st.entry.vehicle.name);
        if (!st.entry.vehicle.make.empty())  ecu["make"] = jf::JJson(st.entry.vehicle.make);
        if (!st.entry.vehicle.model.empty()) ecu["model"] = jf::JJson(st.entry.vehicle.model);
        o["ecu"] = ecu;
        if (!st.tune.empty()) o["tune"] = jf::JJson(st.tune);
        return o;
    };
    host.burn = [](std::string& why) {
        if (!link.isOpen()) { why = "not connected to an ECU"; return false; }
        Cache::instance().flushWrites();
        link.burn();
        return true;
    };
    host.save = [](std::string& why) {
        if (st.uid.empty()) { why = "no ECU to save into (a --meta session has no project)"; return false; }
        std::unique_ptr<Ecu> e(Ecu::openOrCreate(st.uid, st.board));
        if (!e) { why = "cannot open the ECU's folder"; return false; }
        const std::string name = st.tune.empty() ? std::string("headless") : st.tune;
        e->saveTune(name, TuneFile::serialise(Cache::instance().configImage(), meta, nullptr));
        e->setCurrentTune(name);
        st.tune = name;
        return true;
    };
    host.page = [](const std::string&, std::string& why) { why = "there are no pages without a window"; return false; };
    bus.onAction = [&host](uint32_t, const std::string& action) -> int {
        if (action == "quit") { g_stop = true; jf::JAiBus::instance().reply("{\"ok\":true}"); return 1; }
        std::string reply;
        if (!aicmd::handle(action, host, reply)) {
            jf::JAiBus::instance().reply("{\"ok\":false,\"error\":\"unknown command: try help\"}");
            return 2;
        }
        jf::JAiBus::instance().reply(reply);
        return reply.rfind("{\"ok\":true", 0) == 0 ? 1 : 2;   // 2: answered, and the answer is an error
    };

    {
        jf::JJson o = jf::JJson::object();
        o["event"] = jf::JJson(std::string("ready"));
        o["bus"] = jf::JJson(std::string(jf::kAiBusDefaultName));
        o["studio"] = jf::JJson(std::string(STUDIO_VERSION));
        o["connecting"] = jf::JJson(args.connect);
        if (!st.tune.empty()) o["tune"] = jf::JJson(st.tune);
        say(o);
    }
    JLOGC("headless", jf::JLogLevel::Info) << "headless studio serving the AI bus";

    // The whole event loop: timers and the serial reader post their work here, the bus is serviced
    // between. 5 ms is the wait for work, not a poll interval — posted work wakes it at once.
    const std::vector<jf::JWidget*> noWidgets;
    while (!g_stop) {
        jf::JMainThreadDispatcher::instance().drainFor(5);
        bus.tick(noWidgets, nullptr);
    }
    if (link.isOpen()) {
        c.flushWrites();
        for (int i = 0; i < 400 && link.busy(); ++i) jf::JMainThreadDispatcher::instance().drainFor(5);
        link.close(false);
    }
    return 0;
}

}  // namespace headless
