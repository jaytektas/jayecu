#pragma once

#include "ISigilResolver.h"
#include "Cache.h"
#include "MetaModel.h"

#include <functional>

// The two channel sigil providers, backed by the live Cache + MetaModel. (The @ widget-state
// resolver lives with the surface, since it reads laid-out controls' runtime state.)

// $ — telemetry (firmware signal-bus) values. Cache::value() already falls back to config for
// config-backed names, so $ resolves live telemetry first and static config as a courtesy.
class TelemetrySigilResolver : public ISigilResolver {
public:
    char   sigil() const override { return '$'; }
    double resolveSigil(const std::string& name) const override { return Cache::instance().value(name); }
    bool   provides(const std::string& name) const override {
        if (Cache::instance().has(name)) return true;                    // a live value by that name
        const MetaModel* m = Cache::instance().meta();
        return m && m->telemetry().count(name) != 0;
    }
    std::vector<std::string> available() const override {
        std::vector<std::string> out;
        if (const MetaModel* m = Cache::instance().meta())
            for (const auto& [name, tf] : m->telemetry()) { (void)tf; out.push_back(name); }
        return out;
    }
    std::string getUnit(const std::string& n)  const override { return Cache::instance().unit(n); }
    std::string getLabel(const std::string& n) const override { return Cache::instance().label(n); }
};

// @ — laid-out widget state (a table's rowcount/selectedRow, a combo's index, an enum's value…).
// Resolution of "<name>.<prop>" is DELEGATED to a lookup the app installs (it searches the surfaces
// for the named element and calls its descriptor's sigilProp getter), so this resolver stays free of
// any surface/model dependency. `lister` enumerates "<name>.<prop>" for every placed named widget.
class WidgetSigilResolver : public ISigilResolver {
public:
    std::function<double(const std::string& name, const std::string& prop)> lookup;
    std::function<std::vector<std::string>()>                               lister;
    std::function<bool(const std::string& name)>                            owns;   // cheap membership

    char   sigil() const override { return '@'; }
    // Widget state is addressed as "<name>.<prop>" and enumerating it means walking every placed widget, so
    // membership is answered from the SHAPE of the token: anything without a dot cannot be one of ours.
    bool   provides(const std::string& nameDotProp) const override {
        const size_t dot = nameDotProp.rfind('.');
        if (dot == std::string::npos || !lookup) return false;
        // Ask the INDEX, not the tree. Answering "maybe, it has a dot" sent every unresolved config-looking
        // token through a full widget walk — 3.7ms each, hundreds of times a frame.
        return owns ? owns(nameDotProp.substr(0, dot)) : true;
    }
    double resolveSigil(const std::string& nameDotProp) const override {
        const auto dot = nameDotProp.rfind('.');
        if (dot == std::string::npos || !lookup) return 0.0;
        return lookup(nameDotProp.substr(0, dot), nameDotProp.substr(dot + 1));
    }
    std::vector<std::string> available() const override { return lister ? lister() : std::vector<std::string>{}; }
};

// % — the STUDIO's own state, as channels. A status strip is not made only of the definition's
// indicators: some lamps ("Data Logging", "Protocol Error") report on the tool rather than on the
// controller. Nothing in a definition can describe those, so they need a source of their own.
class AppStateSigilResolver : public ISigilResolver {
public:
    inline static bool linkError = false;   // last controller exchange failed (framing, CRC, timeout)
    // A DATALOG is being recorded (DatalogRecorder) — the engine's behaviour captured for the person
    // tuning it. Not the studio's own JLOGC/genesis.log diagnostics, which are never channels.
    inline static bool logging   = false;
    inline static bool connected = false;   // a link is open

    char   sigil() const override { return '%'; }
    bool   provides(const std::string& name) const override {
        return name == "linkError" || name == "logging" || name == "connected";
    }
    double resolveSigil(const std::string& name) const override {
        if (name == "linkError") return linkError ? 1.0 : 0.0;
        if (name == "logging")   return logging   ? 1.0 : 0.0;
        if (name == "connected") return connected ? 1.0 : 0.0;
        return 0.0;
    }
    std::vector<std::string> available() const override {
        return { "linkError", "logging", "connected" };
    }
};

// # — config (dictionary) scalars, decoded from the config image in engineering units.
class ConfigSigilResolver : public ISigilResolver {
public:
    char   sigil() const override { return '#'; }
    double resolveSigil(const std::string& path) const override { return Cache::instance().configValue(path); }
    bool   provides(const std::string& path) const override {
        // Ask the SAME question the value lookup answers. This listed only the flat scalar and table maps,
        // so an ARRAY-ELEMENT path ("trigger.streams[0].primitive") was not claimed by anybody — and an
        // unclaimed bare token falls through to a "first non-zero answer" scan, which cannot distinguish a
        // legitimate 0 from "no answer". The result was a condition that worked for every value of a field
        // except zero, silently, in an expression the editor itself had written.
        return Cache::instance().isConfig(path);
    }
    std::vector<std::string> available() const override {
        std::vector<std::string> out;
        if (const MetaModel* m = Cache::instance().meta()) {
            for (const auto& [path, cf] : m->config())       { (void)cf; out.push_back(path); }
            for (const auto& [path, ct] : m->configTables()) { (void)ct; out.push_back(path); }
        }
        return out;
    }
    std::string getUnit(const std::string& n)  const override { return Cache::instance().unit(n); }
    std::string getLabel(const std::string& n) const override { return Cache::instance().label(n); }
};
