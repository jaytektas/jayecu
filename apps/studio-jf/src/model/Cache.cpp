#include <algorithm>
#include <limits>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>
#include "Cache.h"
#include "MathEvaluator.h"

#include "UnitManager.h"
#include "Perf.h"
#include <j/core/Log.h>
#include "MetaModel.h"

// ---------------------------------------------------------------------------------------------------
// Small helpers used inline below.
// ---------------------------------------------------------------------------------------------------
namespace {
// Relative fuzzy double compare.
bool fuzzyEq(double p1, double p2)
{
    return std::abs(p1 - p2) * 1000000000000. <= std::min(std::abs(p1), std::abs(p2));
}
// 0x-formatted absolute offset for the logs — config offsets are small, segment (learned) offsets sit
// high (e.g. 0x40000000), so hex makes the config-vs-segment split obvious at a glance.
std::string hx(int v)
{
    char b[12];
    std::snprintf(b, sizeof b, "0x%08x", static_cast<unsigned>(v));
    return b;
}
}  // namespace

Cache &Cache::instance()
{
    static Cache c;
    return c;
}

Cache::Cache() : undoStack_(new jf::JUndoStack())
{
    // Debounced-coalesced writes: each edit marks its byte range dirty and (re)arms a short debounce;
    // the dirty set flushes to the ECU once edits settle, or sooner under a sustained burst (the cap).
    // Single-shot, restartable; onTick fires on the main thread (dispatcher).
    flushTimer_.onTick.connect([this]() { flushWrites(); });
    flushCapTimer_.onTick.connect([this]() { flushWrites(); });
}

// ---- cache blocks ---------------------------------------------------------------------------------
// Config is block 0 (base 0, in configImage_); additional segments live in their own buffers but share
// the one absolute meta-path offset space. blockBytes() is the single seam every raw read/write goes
// through, so adding a block never touches the read/write/flush/undo logic — only what it resolves to.
const uint8_t *Cache::blockBytes(int offset, int size) const
{
    if (offset < 0 || size < 0)
        return nullptr;
    const uint64_t need = uint64_t(offset) + uint64_t(size);
    if (need <= configImage_.size())                       // config/tune block (base 0)
        return configImage_.empty() ? nullptr : configImage_.data() + offset;
    for (const Segment &s : segments_) {                   // a non-tune block (learned region, …)
        const uint64_t lo = s.base, hi = lo + s.bytes.size();
        if (uint64_t(offset) >= lo && need <= hi) {
            JLOGC("model.cache.block", jf::JLogLevel::Trace) << "route " << hx(offset) << "+" << size
                << " \xE2\x86\x92 segment @ " << hx(int(s.base)) << " (local " << (uint64_t(offset) - lo) << ")";
            return s.bytes.data() + (uint64_t(offset) - lo);
        }
    }
    JLOGC("model.cache.block", jf::JLogLevel::Debug) << "unmapped offset " << hx(offset) << "+" << size
        << " (config " << configImage_.size() << "B, " << segments_.size() << " segment(s)) \xE2\x86\x92 no-op";
    return nullptr;
}

uint8_t *Cache::blockBytes(int offset, int size)
{
    return const_cast<uint8_t *>(std::as_const(*this).blockBytes(offset, size));
}

void Cache::initSegments()
{
    // (Re)allocate the non-tune blocks from the meta (config block 0 stays configImage_). Zero until the
    // ECU read lands via applyConfigRange. Called on connect, so a stale block never survives a reconnect.
    segments_.clear();
    if (!meta_)
        return;
    for (const MetaModel::Segment &s : meta_->segments()) {
        if (s.base == 0 || s.size <= 0)                    // base 0 == the config/tune block
            continue;
        Segment seg;
        seg.base = s.base;
        seg.host = (s.kind == "host");        // PcVariables: local storage, never synced to the ECU
        seg.bytes.assign(size_t(s.size), 0);
        JLOGC("model.cache.block", jf::JLogLevel::Info) << "segment '" << s.id << "' (" << s.kind
            << ") base=" << hx(int(s.base)) << " size=" << s.size << "B allocated (zeroed)";
        segments_.push_back(std::move(seg));
    }
    // SEED THE HOST VARIABLES THAT DECLARE A DEFAULT. The block above is zeroed, and zero is not a
    // harmless starting point for every host variable: the bench test's count is "how many times to
    // fire", where 0 means cancel — so the first press of a Test button did nothing at all, on a
    // control the user had never been given a reason to touch. Only variables that say `default` move;
    // everything else keeps the zero it has always had.
    for (const MetaModel::PcVar &v : meta_->pcVars())
        if (v.hasDef) setConfigValue("pc." + v.name, v.defV);

    JLOGC("model.cache.block", jf::JLogLevel::Info) << segments_.size()
        << " non-tune cache block(s) ready; config block = " << configImage_.size() << "B";
}

void Cache::requestSegmentReads()
{
    for (const Segment &s : segments_) {                   // replies land in applyConfigRange (block-routed)
        if (s.host) continue;                              // host block: nothing on the ECU to read
        JLOGC("model.cache.block", jf::JLogLevel::Info) << "requesting segment read base=" << hx(int(s.base))
            << " size=" << s.bytes.size() << "B from the ECU";
        rangeReadRequested.emit(int(s.base), int(s.bytes.size()));
    }
}

void Cache::markDirty(int offset, int size)
{
    if (size <= 0)
        return;
    // A host block has no counterpart on the ECU, so queueing its bytes for a write would send an offset
    // the firmware cannot map. The edit still lands in the block (that is what makes the value live) and
    // still travels with the tune — it just never becomes a wire write.
    if (isHostOffset_(offset))
        return;
    const bool wasEmpty = dirty_.empty();
    dirty_.push_back({offset, offset + size});
    if (wasEmpty)
        flushCapTimer_.start(std::chrono::milliseconds(150), jf::JTimer::JMode::SingleShot);   // cap: not restarted, so a sustained burst still flushes ~150 ms
    flushTimer_.start(std::chrono::milliseconds(25), jf::JTimer::JMode::SingleShot);            // debounce: restarted on every edit
}

void Cache::flushWrites()
{
    flushTimer_.stop();
    flushCapTimer_.stop();
    if (dirty_.empty())
        return;
    std::sort(dirty_.begin(), dirty_.end());
    JLOGC("model.cache.flush", jf::JLogLevel::Debug) << "flush: " << dirty_.size() << " dirty range(s)";
    auto emitRange = [this](int a, int b) {                 // chunk a contiguous range into 1 KB frames
        JLOGC("model.cache.flush", jf::JLogLevel::Debug) << "  push " << hx(a) << "\xE2\x80\x93" << hx(b)
            << " (" << (b - a) << "B) \xE2\x86\x92 ECU";
        for (int i = a; i < b; i += 1024) {
            const int n = std::min(1024, b - i);
            if (const uint8_t *p = blockBytes(i, n))         // reads from the owning block (config or a segment)
                writeRequested.emit(i, std::vector<uint8_t>(p, p + n));
        }
    };
    int s = dirty_[0].first, e = dirty_[0].second;
    for (size_t k = 1; k < dirty_.size(); ++k) {
        if (dirty_[k].first <= e) e = std::max(e, dirty_[k].second);   // overlap/adjacent -> extend
        else { emitRange(s, e); s = dirty_[k].first; e = dirty_[k].second; }
    }
    emitRange(s, e);
    dirty_.clear();
}

// One tune edit, captured as a byte-range diff. undo()/redo() replay it into the cache and push the
// bytes to the ECU. The command already ran inline when it was created, so the first redo() (fired by
// JUndoStack::push) is skipped to avoid a duplicate write.
namespace {
class ConfigEditCommand : public jf::JUndoCommand {
public:
    ConfigEditCommand(Cache *c, int offset, std::vector<uint8_t> oldB, std::vector<uint8_t> newB, std::string text)
        : cache_(c), offset_(offset), old_(std::move(oldB)), new_(std::move(newB)), text_(std::move(text)) {}
    void undo() override { cache_->applyUndoBytes(offset_, old_); }
    void redo() override { if (skipFirst_) { skipFirst_ = false; return; } cache_->applyUndoBytes(offset_, new_); }
    std::string text() const override { return text_; }
private:
    Cache *cache_;
    int offset_;
    std::vector<uint8_t> old_, new_;
    std::string text_;
    bool skipFirst_ = true;
};
}  // namespace

void Cache::beginEdit()
{
    if (editDepth_++ == 0) {
        editSnapshot_ = configImage_;
        segSnapshot_.clear();                         // snapshot every block so a segment edit is diffable too
        for (const Segment &s : segments_) segSnapshot_.push_back(s.bytes);
        clamped_ = written_ = 0;   // fresh tally for this transaction (see clampedCount)
    }
}

void Cache::endEdit(const std::string &text)
{
    if (editDepth_ == 0 || --editDepth_ != 0)
        return;                                       // still inside an outer transaction
    // Diff each block against its beginEdit snapshot; one undo command + one dirty range per changed block.
    // Offsets are absolute, so undo replay (applyUndoBytes) and the flush stay block-agnostic. A single
    // edit touches one block, so this is one command as before — a cross-block compound edit is 2 steps.
    auto diffBlock = [&](int base, const std::vector<uint8_t> &snap, const std::vector<uint8_t> &cur) {
        if (snap.size() != cur.size() || snap == cur)
            return;                                   // nothing changed (or a resize we can't diff)
        int lo = 0, hi = int(cur.size());
        while (lo < hi && snap[lo] == cur[lo]) ++lo;
        while (hi > lo && snap[hi - 1] == cur[hi - 1]) --hi;
        if (lo >= hi)
            return;
        JLOGC("model.cache.edit", jf::JLogLevel::Debug) << "\"" << text << "\": "
            << (base ? "segment" : "config") << " diff " << hx(base + lo) << "+" << (hi - lo)
            << "B \xE2\x86\x92 undo + dirty";
        std::vector<uint8_t> before(snap.begin() + lo, snap.begin() + hi);
        std::vector<uint8_t> after (cur.begin()  + lo, cur.begin()  + hi);
        undoStack_->push(new ConfigEditCommand(this, base + lo, before, after, text));
        // …and file it under the page it was made on, for that page's own < > arrows. A new edit
        // discards the steps the arrows had walked back past, exactly as any redo tail is discarded.
        if (!editScope_.empty() && !replaying_) {
            ScopedHistory &h = scoped_[editScope_];
            h.steps.erase(h.steps.begin() + std::min(h.cursor, h.steps.size()), h.steps.end());
            h.steps.push_back(ScopedStep{ base + lo, std::move(before), std::move(after) });
            h.cursor = h.steps.size();
        }
        // Queue the changed span for a coalesced flush (debounced) instead of writing inline — rapid
        // sequential edits (a held '.', a slider drag) collapse into far fewer ECU frames.
        markDirty(base + lo, hi - lo);
    };
    diffBlock(0, editSnapshot_, configImage_);
    for (size_t k = 0; k < segments_.size() && k < segSnapshot_.size(); ++k)
        diffBlock(int(segments_[k].base), segSnapshot_[k], segments_[k].bytes);
}

void Cache::applyUndoBytes(int offset, const std::vector<uint8_t> &bytes)
{
    uint8_t *dst = blockBytes(offset, int(bytes.size()));   // config or a segment, by absolute offset
    if (!dst)
        return;
    std::copy(bytes.begin(), bytes.end(), dst);
    markDirty(offset, int(bytes.size()));             // coalesced flush (undo/redo)
    configLoaded.emit();                              // refresh every bound control (immediate)
}

// --- Scoped (per-page) history --------------------------------------------------------------------
// Walking a page's own steps is a BYTE APPLY, not a stack rewind: the step's `before` (or `after`) bytes
// go back into the image and out to the ECU. That keeps it correct however the global stack has been
// used in between, and makes an arrow press idempotent rather than order-sensitive.
bool Cache::canUndoScope(const std::string &scope) const
{
    const auto it = scoped_.find(scope);
    return it != scoped_.end() && it->second.cursor > 0;
}

bool Cache::canRedoScope(const std::string &scope) const
{
    const auto it = scoped_.find(scope);
    return it != scoped_.end() && it->second.cursor < it->second.steps.size();
}

void Cache::undoScope(const std::string &scope)
{
    const auto it = scoped_.find(scope);
    if (it == scoped_.end() || it->second.cursor == 0)
        return;
    const ScopedStep &st = it->second.steps[--it->second.cursor];
    replaying_ = true;
    applyUndoBytes(st.offset, st.before);
    replaying_ = false;
    JLOGC("model.cache.edit", jf::JLogLevel::Debug) << "undo on page \"" << scope << "\": "
        << hx(st.offset) << "+" << st.before.size() << "B (" << it->second.cursor << " left)";
}

void Cache::redoScope(const std::string &scope)
{
    const auto it = scoped_.find(scope);
    if (it == scoped_.end() || it->second.cursor >= it->second.steps.size())
        return;
    const ScopedStep &st = it->second.steps[it->second.cursor++];
    replaying_ = true;
    applyUndoBytes(st.offset, st.after);
    replaying_ = false;
}

long long Cache::msSinceTelemetry() const
{
    if (telemFrames_ == 0) return 1 << 30;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - telemAt_).count();
}

void Cache::ingestTelemetry(const std::vector<uint8_t> &frame)
{
    if (!meta_ || !meta_->isValid())
        return;
    ++telemFrames_;                                     // liveness: counted for every protocol, always
    telemAt_ = std::chrono::steady_clock::now();
    const auto *p = reinterpret_cast<const unsigned char *>(frame.data());
    const int len = int(frame.size());

    const auto &telem = meta_->telemetry();
    // Whole-definition properties: read once, not per channel.
    const bool be  = meta_->bigEndian();
    const bool flt = meta_->floatBus();
    for (auto it = telem.begin(); it != telem.end(); ++it) {
        const MetaModel::TelemField &f = it->second;
        if (f.offset + f.size > len)
            continue;                               // frame shorter than descriptor — skip field
        // This firmware's channels come off a FLOAT SignalBus, so decoding in float (float raw * float
        // scale) carries exactly the precision the ECU sent — the table solver and any signal display then
        // agree with it. An IMPORTED definition is a different machine: rusEFI channels are scaled
        // integers, and forcing one through a float32 would silently drop everything past 2^24, so those
        // decode in double.
        // Packed channels ([OutputChannels] `bits`) extract their range from the decoded word FIRST; the
        // float decode below is unchanged for everything else, so precision is untouched.
        const double word = f.bits.extract(MetaModel::decodeRaw(f.datatype, p + f.offset, be), f.size);
        // Per CHANNEL: a float-cell value decodes in float so it carries exactly the precision the ECU
        // held; an integer cell (a mask, a counter) must not go through a float32 at all — dtc_indicators
        // is a bitmask, and rounding one lights the wrong lamps. The definition-level flag is the fallback
        // for a source with no float bus in the first place (an imported TunerStudio ini).
        values_[it->first] = (flt && f.busFloat)
            ? static_cast<double>(static_cast<float>(word) * static_cast<float>(f.scale))
            : word * f.scale;
    }
    JLOGC("model.cache.telem", jf::JLogLevel::Trace) << "telemetry frame " << len << "B \xE2\x86\x92 "
        << telem.size() << " channels decoded, gen=" << (generation_ + 1);
    // Command-state watch: a bench routine reports RUNNING for its whole run, then flips to OK/FAIL. On the
    // OK edge, re-read just the config region that routine wrote (spliced via applyConfigRange, so any in-flight
    // local edit still wins) — the ECU is authoritative about the cal, and the `op` tells us exactly what ran.
    // FAIL only notifies. Comparing the whole value (not just an edge) is dropped-frame-proof: the level
    // persists, so the next frame still carries the transition. value = (op << 2) | phase — see CommandState.h.
    auto cit = values_.find("command_state");
    if (cit != values_.end()) {
        const int cs = static_cast<int>(std::llround(cit->second));
        if (cs != lastCommandState_) {
            const bool seed = (lastCommandState_ < 0);   // first frame after connect: adopt a stale value, don't act
            lastCommandState_ = cs;
            const int op = cs >> 2, phase = cs & 0x3;
            if (!seed && (phase == 2 || phase == 3)) {      // 2 = OK, 3 = FAIL (terminal)
                if (phase == 2) rereadForOp(op);            // pull just the cal this routine wrote
                commandFinished.emit(op, phase == 2);
            }
        }
    }
    ++generation_;
    frameUpdated.emit();
}

void Cache::applyConfigRange(int offset, const std::vector<uint8_t> &bytes)
{
    if (offset < 0 || bytes.empty())
        return;
    uint8_t *dst = blockBytes(offset, int(bytes.size()));   // config or a segment (segment reads land here too)
    if (!dst)
        return;
    JLOGC("model.cache.block", jf::JLogLevel::Debug) << "splice " << bytes.size() << "B at " << hx(offset)
        << (uint64_t(offset) < configImage_.size() ? " (config)" : " (segment)")
        << (dirty_.empty() ? "" : " [dirty-wins merge]");
    bool changed = false;
    if (dirty_.empty()) {                                   // common case: nothing pending -> straight splice
        if (std::equal(bytes.begin(), bytes.end(), dst)) return;
        std::copy(bytes.begin(), bytes.end(), dst);
        changed = true;
    } else {
        // Clash rule: a byte the studio has queued (dirty_) is the user's in-flight intent — let it win;
        // apply the re-read only to bytes that aren't pending.
        for (int i = 0; i < int(bytes.size()); ++i) {
            const int o = offset + i;
            bool pending = false;
            for (const auto &r : dirty_)
                if (o >= r.first && o < r.second) { pending = true; break; }
            if (!pending && dst[i] != bytes[i]) { dst[i] = bytes[i]; changed = true; }
        }
    }
    if (changed)
        configLoaded.emit();                                // refresh bound controls (the relax field, …)
}

// A bench routine finished (command_state OK). Re-read ONLY the config region it wrote, resolved from the op
// — a handful of bytes instead of the whole 84 KB image. The op enum + what each routine writes is defined in
// firmware Comms/CommandState.h; keep these paths in step with it. Each range splices via applyConfigRange
// (dirty-wins). Unknown op falls back to a full read so a new routine can't silently leave the editor stale.
void Cache::rereadForOp(int op)
{
    if (!meta_) return;
    std::vector<std::string> paths;
    switch (op) {
        case 1: case 2:   // findlimits (etb 0/1): the two TPS sensors' cal + this etb's relax_pct
            paths = { "sensors.sensor", "electronic_throttle.etb[" + std::to_string(op - 1) + "]" };
            break;
        case 3: case 4:   // fillff (etb 0/1): this etb's ff_table
            paths = { "electronic_throttle.etb[" + std::to_string(op - 3) + "]" };
            break;
        case 5: case 6:   // autotune (etb 0/1): this etb's kp/ki/kd
            paths = { "electronic_throttle.etb[" + std::to_string(op - 5) + "]" };
            break;
        case 7:           // pedalcal: the pedal (APP) sensors' cal
            paths = { "sensors.sensor" };
            break;
        case 8:           // engine reconfigure: the stopped ECU recomputed cyl[].tdc_angle from cylinder_count /
            paths = { "engine.cyl" };   // firing_order (even-fire). Re-read the computed angles the studio greys.
            break;
        default:
            if (!configImage_.empty()) rangeReadRequested.emit(0, int(configImage_.size()));
            return;
    }
    for (const std::string &p : paths) {
        int off = 0, size = 0;
        if (meta_->resolveRegion(p, off, size) && size > 0)
            rangeReadRequested.emit(off, size);
    }
}

// The unit a binding is in. Telemetry first — a live channel is not a config location, and the two
// namespaces can share a name — then whatever the config path says.
//
// The config half used to be three lookups in a row (config_, arrayFieldUnits, resolveStrip), each
// covering one shape, so a table cell or a 1-D array element simply had no units and every readout of
// one was unlabelled.
std::string Cache::unit(const std::string &name) const
{
    if (!meta_)
        return {};
    if (meta_->telemetry().count(name))
        return meta_->telemetry().at(name).units;
    if (const MetaModel::SensorType *st = typeScaledCells(name); st && !st->units.empty())
        return st->units;                            // a cal reads in its sensor's units — see typeScaledCells
    if (const MetaModel::SensorType *st = typeScaledField(name); st && !st->units.empty())
        return st->units;                            // so does a threshold stored in them
    // A SENSOR'S RAW THRESHOLDS ARE IN ITS INPUT'S OWN UNITS. The field is declared "ADC" (counts, shown as
    // pin volts) because most inputs are voltage pins — but on a frequency input the raw value is Hz, on
    // a pulse input µs, on SENT the sensor's own counts, and the ECU compares the threshold with THAT. A
    // flex sensor's 150 Hz limit shown as "0.18 V" was a number nobody could set correctly.
    {
        static const std::string kRawMin = ".diag_raw_min", kRawMax = ".diag_raw_max";
        auto ends = [&](const std::string& suf) {
            return name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0;
        };
        if (name.rfind("sensors.sensor[", 0) == 0 && (ends(kRawMin) || ends(kRawMax))) {
            const std::string base = name.substr(0, name.size() - kRawMin.size());
            switch (static_cast<int>(configValue(base + ".interface"))) {
                case 3: return "Hz";                 // Frequency
                case 7: return "us";                 // Pulse Width
                case 6: return "";                   // SENT: the sensor's own counts
                default: break;                      // voltage pins: ADC counts, shown in the pin unit
            }
        }
    }
    return meta_->locate(name).units;
}

// See the header. Three cases, in the order the answer gets more specific:
//   * no sensor publishes this channel  -> the telemetry descriptor is the authority (rpm, advance)
//   * a CATALOGUED sensor publishes it  -> its type is settled at build time and codegen already
//                                          stamped it onto the descriptor; same answer, no tune read
//   * a GENERIC input publishes it      -> the descriptor is a union wire and says nothing true; the
//                                          answer is whatever type the TUNE has that input set to
Cache::ChannelDomain Cache::channelDomain(const std::string &channel) const
{
    ChannelDomain d;
    if (!meta_ || channel.empty())
        return d;

    const auto fromTelemetry = [&] {
        const auto it = meta_->telemetry().find(channel);
        if (it == meta_->telemetry().end())
            return;
        const MetaModel::TelemField &tf = it->second;
        if (tf.maxV > tf.minV) { d.hasRange = true; d.lo = tf.minV; d.hi = tf.maxV; }
        // THE SCALE ALREADY SAYS THE PRECISION when the descriptor does not say it outright. Almost no
        // telemetry channel carries an explicit `digits`, so this answered "unknown" for nearly all of
        // them — and every caller then fell back to a fixed default, which is how a raw pin read as
        // volts printed one decimal and collapsed 4096 counts into five readings. A channel stored at
        // 0.01 resolves to two places, one stored in whole counts to none. The same rule the breakpoint
        // path below already applies to an axis, so the two cannot disagree.
        d.digits = (tf.digits >= 0) ? tf.digits
                 : (tf.scale >= 1.0 || tf.scale <= 0.0) ? 0
                 : int(std::lround(-std::log10(tf.scale)));
        d.units  = tf.units;
    };
    const auto fromType = [&](const MetaModel::SensorType &st) {
        if (st.maxV > st.minV) { d.hasRange = true; d.lo = st.minV; d.hi = st.maxV; }
        d.digits = st.digits;
        d.units  = st.units;
    };

    std::string arrayKey; int idx = 0;
    if (!meta_->sensorForSignal(channel, arrayKey, idx)) { fromTelemetry(); return d; }

    const auto ait = meta_->configArrays().find(arrayKey);
    if (ait == meta_->configArrays().end()) { fromTelemetry(); return d; }
    const MetaModel::ConfigArray &ca = ait->second;

    // A type the catalog settled: the descriptor already carries it (and carries the per-sensor `gauge`
    // narrowing too, which the raw type does not), so it stays the better answer.
    if (idx < int(ca.elementTypes.size()) && !ca.elementTypes[idx].empty()) { fromTelemetry(); return d; }
    // No catalog type => a generic input, whose type is a tune byte.
    if (const MetaModel::SensorType *st = sensorTypeOf(arrayKey, idx))
        fromType(*st);
    return d;
}

// The type an element is READ AS. Two sources, in the order the firmware itself uses (effective_type):
// the catalog's own type if it has one, else the tune's `type` byte — which INDEXES the catalog, both
// being in SENSOR_TYPE_CATALOG order, so there is no name to match and nothing to drift apart.
const MetaModel::SensorType *Cache::sensorTypeOf(const std::string &arrayKey, int index) const
{
    if (!meta_ || index < 0)
        return nullptr;
    const auto it = meta_->configArrays().find(arrayKey);
    if (it == meta_->configArrays().end())
        return nullptr;
    const MetaModel::ConfigArray &ca = it->second;
    if (index < int(ca.elementTypes.size()) && !ca.elementTypes[index].empty())
        return meta_->sensorType(ca.elementTypes[index]);
    const std::string typePath = elementFieldPath(ca, arrayKey, index, "type");
    if (!meta_->locate(typePath).valid())
        return nullptr;                             // no type selector: nothing can be said
    const MetaModel::SensorType *st = meta_->sensorType(static_cast<int>(configValue(typePath)));
    // …and it has to be a type this input can actually BE. effective_type() accepts a stored type only
    // if the catalog marks it selectable — a generic input is read as an analog voltage through a cal
    // curve, so asking for switch, frequency or composition leaves it unconfigured in the firmware.
    // Describing it here as though the type had taken would have the studio state units and a range for
    // an input that publishes nothing.
    return (st && st->selectable) ? st : nullptr;
}

// What an axis may hold. Three sources, most specific first — see the header.
Cache::ChannelDomain Cache::axisDomain(const TableImage &t, int axis) const
{
    ChannelDomain d;
    if (!meta_ || axis < 0 || axis >= int(t.axes.size()))
        return d;
    const TableImage::Axis &a = t.axes[axis];

    // 1. An axis with a CHANNEL is the channel's.
    if (const int sid = tiSrc(t, axis); sid >= 0)
        return channelDomain(meta_->signalName(sid));

    // What the breakpoint STORAGE can hold, and how finely it can say it. An integer array at scale 1
    // counts in whole numbers — offering six decimal places on it (which "unknown" did) is nonsense.
    const bool isFloat = !a.breakType.empty() && (a.breakType[0] == 'F' || a.breakType[0] == 'f');
    const double sc = a.breakScale != 0.0 ? a.breakScale : 1.0;
    if (!isFloat) {
        double lo = 0.0, hi = 0.0;
        datatypeRange(a.breakType, lo, hi);
        d.hasRange = true; d.lo = lo * sc; d.hi = hi * sc;
        d.digits = (sc >= 1.0) ? 0 : int(std::lround(-std::log10(sc)));
    }
    d.units = a.units;

    // 2. A sensor CALIBRATION's axis is not channel-less at all — it reads the pin's raw counts, and
    //    HardwareInput publishes exactly those on the bus every frame (hw_av3: ADC, 0..4095, whole
    //    numbers, all of it already in telemetry). It simply is not named by an axis `src` selector:
    //    the sensor's own interface + source name it, the same join primary_hw_sig() makes in firmware.
    //    So resolve the channel and ask it, rather than inferring a domain from the board's ADC width
    //    and a guess about what each interface counts in — which also gets Hz and µs right for free.
    if (t.cellTypeScaled && t.elemIndex >= 0 && !t.elemArray.empty()) {
        if (const ChannelDomain raw = rawInputDomain(t.elemArray, t.elemIndex); raw.known())
            return raw;
    }

    // 3. The schema's own bounds, when it declares any, narrow whatever we have.
    if (a.hasBounds && a.vmax > a.vmin) {
        d.lo = d.hasRange ? std::max(d.lo, a.vmin) : a.vmin;
        d.hi = d.hasRange ? std::min(d.hi, a.vmax) : a.vmax;
        d.hasRange = d.hi > d.lo;
    }
    return d;
}

double Cache::AxisView::toDisplay(double v) const {
    return (from.empty() || to.empty() || from == to) ? v : UnitManager::instance().convert(v, from, to);
}
double Cache::AxisView::toStorage(double v) const {
    return (from.empty() || to.empty() || from == to) ? v : UnitManager::instance().convert(v, to, from);
}
std::string Cache::AxisView::text(double stored) const {
    const double v = toDisplay(stored);
    if (to.empty()) return fmtBreak(v, digits);       // the axis's own unit: show what is really there
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", std::clamp(digits, 0, 9), v);
    return b;
}

std::string Cache::AxisView::label() const {
    const std::string l = UnitManager::instance().unitLabel(to.empty() ? from : to);
    return l.empty() ? (to.empty() ? from : to) : l;
}

Cache::AxisView Cache::axisView(const TableImage &t, int axis, const std::string &pref) const
{
    AxisView v;
    const ChannelDomain d = axisDomain(t, axis);
    v.from = d.units;
    v.digits = std::max(0, d.digits);
    v.hasRange = d.hasRange; v.lo = d.lo; v.hi = d.hi;
    if (v.from.empty()) return v;
    UnitManager &um = UnitManager::instance();
    // AN AXIS FALLS BACK THE WAY A VALUE DOES. "Raw" means storage units and stays that way, but empty
    // or "Auto" is not a refusal to convert — it is "no page-level opinion", and the answer to that is
    // the user's per-quantity preference, exactly as CanvasWidget::displayUnitOf resolves it for a
    // reading. Returning storage units here instead put the two halves of a sensor page in different
    // units by default: the raw readout showed volts (AnalogRaw prefers V) while the calibration curve
    // it is meant to be read against showed ADC counts, so the live number could not be located on the
    // very table it was there to help fill in.
    std::string want = pref;
    if (want.empty() || want == "Auto") want = um.displayUnitFor(v.from);
    if (want.empty() || want == "Raw" || want == v.from) return v;
    const std::string pref_ = want;
    const std::string q = um.findQuantityForUnit(v.from);
    if (q.empty() || q != um.findQuantityForUnit(pref_))
        return v;
    v.to = pref_;

    // What ONE STORAGE STEP is worth in the display unit decides the precision: showing volts to two
    // places would print eight adjacent counts as the same number, and typing one back would jump the
    // breakpoint. The step is the axis's own precision (a count, or a tenth of a unit) converted.
    const double step = std::pow(10.0, -v.digits);
    const double shown = std::fabs(um.convert(step, v.from, v.to) - um.convert(0.0, v.from, v.to));
    v.digits = (shown >= 1.0 || shown <= 0.0) ? 0
             : std::min(6, static_cast<int>(std::ceil(-std::log10(shown))));
    if (v.hasRange) {
        double lo = v.toDisplay(v.lo), hi = v.toDisplay(v.hi);
        if (lo > hi) std::swap(lo, hi);
        v.lo = lo; v.hi = hi;
    }
    return v;
}

double Cache::snapBin(const TableImage &t, int axis, double v) const
{
    const ChannelDomain d = axisDomain(t, axis);
    v = snapBreak(v, d.digits);                     // precision first...
    return d.hasRange ? std::clamp(v, d.lo, d.hi) : v;   // ...so the domain still has the last word
}

bool Cache::tiBuildAxis(const TableImage &t, int axis, double start, double end, double inc)
{
    if (!t.valid || axis < 0 || axis >= int(t.axes.size()) || inc <= 0.0 || end <= start)
        return false;
    const TableImage::Axis &a = t.axes[axis];
    if (a.nBase < 0)
        return false;                                  // fixed grid: the count is not ours to change

    // BRING THE REQUEST INSIDE THE AXIS FIRST. Generating the ramp and clamping each bin as it is
    // written looks equivalent and is not: every value past the ceiling lands ON the ceiling, so a span
    // that overshoots produces a run of identical breakpoints. locate() takes the LAST bin equal to the
    // value, so those duplicates are not merely ugly — every cell behind them becomes unreachable, which
    // is a table quietly losing rows. Clamping the ENDS instead keeps the ramp a ramp.
    const ChannelDomain d = axisDomain(t, axis);
    if (d.hasRange) { start = std::clamp(start, d.lo, d.hi); end = std::clamp(end, d.lo, d.hi); }
    // A step finer than the axis can express duplicates bins just as surely: at a tenth, an increment of
    // 0.01 snaps every tenth bin onto its neighbour. One step of the channel's own precision is the
    // smallest increment that can produce distinct breakpoints.
    if (d.digits >= 0) inc = std::max(inc, std::pow(10.0, -d.digits));
    if (end <= start)
        return false;                                  // nothing left to lay out once both are in range

    // How many bins the three numbers ask for, held to what the schema allows. The last bin lands ON
    // `end` when the step divides the span; otherwise the run stops before overshooting it.
    const int want = int(std::floor((end - start) / inc + 1e-9)) + 1;
    const int cap  = a.nMax > 0 ? a.nMax : want;
    const int n    = std::clamp(want, std::max(1, a.nMin), cap);

    // Resize first, one bin at a time, so each step resamples the cells the way a hand insert does.
    for (int have = tiLiveN(t, axis); have > n; --have) tiRemoveBin(t, axis, have - 1);
    for (int have = tiLiveN(t, axis); have < n; ++have) tiInsertBin(t, axis, have);

    std::vector<double> bins(n);
    for (int i = 0; i < n; ++i) bins[i] = snapBin(t, axis, start + inc * i);
    tiWriteBins(t, axis, bins);
    return true;
}

std::string Cache::label(const std::string &name) const
{
    return meta_ ? meta_->labelFor(name) : std::string();
}

std::string Cache::help(const std::string &name) const
{
    return meta_ ? meta_->helpFor(name) : std::string();
}

void Cache::setConfigImage(const std::vector<uint8_t> &image)
{
    JLOGC("model.cache", jf::JLogLevel::Info) << "config image loaded (" << image.size() << " B)";
    // A load that lands the SAME bytes is not a new document — it is a re-read confirming what we hold.
    // Clearing history on one of those is how a burn, which re-reads to be safe, quietly emptied the undo
    // arrows on the dialog the user had just been working in.
    const bool changed = configImage_ != image;
    configImage_ = image;
    if (changed) {
        if (undoStack_) undoStack_->clear();   // a genuinely new image is a new baseline; edit history resets
        scoped_.clear();                       // …the per-page arrows with it
    }
    lastCommandState_ = -1;                // re-seed the command-state watch: a stale terminal from before this
                                           // connect must not trigger a spurious re-read on the first frame
    configLoaded.emit();
}


// Does this path name a config VALUE? A named bit group counts — the widget write gate
// (writableConfigPath) asks here, so without it a checkbox bound to one silently did nothing. A run or
// a table names a group of values, not one, so neither is writable through a scalar binding.
bool Cache::engineTurning() const
{
    if (!linkOpen_) return false;
    return (has("rpm") && value("rpm") > 0.0) || (has("sync_level") && value("sync_level") >= 1.0);
}

bool Cache::lockedWhileRunning(const std::string &path) const
{
    return meta_ && !path.empty() && meta_->appliesAt(path) == "engine_stop" && engineTurning();
}

bool Cache::isConfig(const std::string &path) const
{
    if (!meta_)
        return false;
    return meta_->locate(path).kind == MetaModel::Location::Kind::Scalar;
}

// The declared bounds of a path. A bit group's range is its OWN width, not the parent byte's — locate()
// already says so, because that is a property of the location and not of whoever is asking.
double Cache::configMin(const std::string &path) const
{
    return meta_ ? meta_->locate(path).minV : 0.0;
}

double Cache::configMax(const std::string &path) const
{
    return meta_ ? meta_->locate(path).maxV : 0.0;
}

// Display precision. The meta states it per field where it matters; otherwise it follows the SCALE
// (codegen's own rule: 1.0 -> 0, 0.1 -> 1, 0.01 -> 2), so a 0.1-degree field shows one decimal instead
// of a flat two. Both halves used to walk the path themselves, and the scale-derived half only ran for
// struct-array fields — a 1-D array or a table cell fell through to 2 whatever its scale said.
int Cache::digits(const std::string &path) const
{
    if (!meta_)
        return 2;
    if (const MetaModel::SensorType *st = typeScaledField(path); st && st->digits >= 0)
        return st->digits;              // a threshold in its sensor's units reads at the type's precision
    const MetaModel::Location L = meta_->locate(path);
    if (!L.valid())
        return 2;                       // historical default for anything still unresolved
    if (L.digits >= 0)
        return L.digits;
    int d = 0;
    for (double s = L.scale; s > 0.0 && s < 0.9999 && d < 6; s *= 10.0)
        ++d;
    return d;
}

// Solve a table to its value at the current operating point — a faithful port of the firmware's
// tbl::table_eval + interp (firmware/Engine/TableEngine.h + TableEval.h, the source of truth). Per axis: read
// its source signal, bracket it in the (engineering) breakpoints, linear-blend per axis (1D→2D→3D fall out of
// one trilinear expression), clamp past the ends, collapse an off/degenerate axis (n<2) to index 0. Cells come
// through tiCell (already scaled to engineering), matching the firmware's cell * scale.
Eng Cache::solveTable(const std::string &path) const
{
    const TableImage t = resolveTable(path);
    if (!t.valid || t.axes.empty()) return Eng{};
    const int nax = static_cast<int>(t.axes.size());

    int   n[3] = { 1, 1, 1 };                 // live bins per axis (1 = off/absent → collapses to index 0)
    double v[3] = { 0.0, 0.0, 0.0 };          // each axis's source-signal value
    std::vector<double> bp[3];                // each axis's breakpoints, in engineering units
    for (int a = 0; a < 3 && a < nax; ++a) {
        n[a]  = tiLiveN(t, a);
        bp[a] = tiBins(t, a);
        const int sid = tiSrc(t, a);
        if (sid >= 0 && meta_) { const std::string nm = meta_->signalName(sid); if (!nm.empty()) v[a] = value(nm); }
    }

    // ---- Everything below runs in FLOAT to match the firmware bit-for-bit (it interpolates in float). ----
    // locate(): lower index i + fraction f in [0,1], clamped at the ends; n<2 → i=0, f=0 (mirrors locate_t).
    auto locate = [](const std::vector<double> &b, int nn, float x, int &i, float &f) {
        i = 0; f = 0.0f;
        if (nn < 2 || static_cast<int>(b.size()) < 2) return;
        const int last = std::min(nn, static_cast<int>(b.size()));
        for (int k = 1; k < last; ++k) { if (static_cast<float>(b[k]) <= x) i = k; else break; }
        if (i > last - 2) i = last - 2;
        const float x0 = static_cast<float>(b[i]), x1 = static_cast<float>(b[i + 1]);
        f = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0f;
        if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
    };

    int xi = 0, yi = 0, zi = 0; float xf = 0.0f, yf = 0.0f, zf = 0.0f;
    locate(bp[0], n[0], static_cast<float>(v[0]), xi, xf);
    const bool y2 = n[1] >= 2; if (y2) locate(bp[1], n[1], static_cast<float>(v[1]), yi, yf);
    const bool z2 = n[2] >= 2; if (z2) locate(bp[2], n[2], static_cast<float>(v[2]), zi, zf);

    // cell() = float(raw) * float(scale), exactly as interp()'s cell lambda (NOT tiCell's double raw*scale).
    // Indices clamped to the live grid — safe, since a collapsed axis has fraction 0 so the far cell contributes 0.
    const int dx = std::max(1, n[0]), dy = std::max(1, n[1]), dz = std::max(1, n[2]);
    const float sc = static_cast<float>(t.cellScale);
    auto cell = [&](int x, int y, int z) -> float {
        const int off = tiCellOffset(t, std::clamp(x, 0, dx - 1), std::clamp(y, 0, dy - 1), std::clamp(z, 0, dz - 1));
        return static_cast<float>(readAt(off, t.cellType, 1.0)) * sc;
    };
    auto lerp = [](float a, float b, float tt) { return a + (b - a) * tt; };

    const int x1 = xi + 1, y1 = y2 ? yi + 1 : yi, z1 = z2 ? zi + 1 : zi;
    const float z0 = lerp(lerp(cell(xi, yi, zi), cell(x1, yi, zi), xf),
                          lerp(cell(xi, y1, zi), cell(x1, y1, zi), xf), yf);
    const float zh = lerp(lerp(cell(xi, yi, z1), cell(x1, yi, z1), xf),
                          lerp(cell(xi, y1, z1), cell(x1, y1, z1), xf), yf);
    return Eng{ static_cast<double>(lerp(z0, zh, zf)) };   // float widened to double — value preserved
}

// A named bit GROUP path ("...diag_enable.raw_min") resolved onto its parent field's storage: the byte the
// group is packed into, plus the range within it. The group is what a control binds to; the byte is only
// where it lives. Returns false for any path that is not one.
// A named bit group's storage: the word it is packed into, and which bits of it are the value. Both
// come from its Location, because that is what a bit group's location IS.
bool Cache::resolveBitGroup(const std::string &path, int &offset, int &size,
                            std::string &datatype, BitRange &bits) const
{
    if (!meta_) return false;
    const MetaModel::Location L = meta_->locate(path);
    if (!L.valid() || !L.bits.packed()) return false;
    offset = L.offset; datatype = L.datatype; size = MetaModel::dataSize(L.datatype); bits = L.bits;
    return true;
}

// Read a config path. RAW out — the Cache no longer cooks; the widget applies the scale and the units.
// The exact mirror of setConfigValue, which is the point: a value written through one and read through
// the other cannot disagree about where it lives or which bits of it are the value.
//
// It used to be a fourth copy of the walk: a bit-group branch that decoded and extracted by hand, a
// config_ lookup, a resolveArrayField fallback, and then a SECOND config_ lookup at the end to find the
// bit range again — a path resolved three times to read one number. A table path is the one thing it
// does that a Location cannot: a table names a grid, and reading a grid as a single value means
// interpolating it at the live channel values, which is a table operation and not an address.
double Cache::configValue(const std::string &path) const
{
    PERF_COUNT("cache config reads");
    if (!meta_)
        return 0.0;
    // ONE RESOLUTION, then decide. isTable() was asked first and answers by parsing the path and
    // scanning an array's element tables — the same work locate() does, done twice per read. locate()
    // already classifies a table, and it is memoised, so this is now a hash lookup and a switch.
    const MetaModel::Location L = meta_->locate(path);
    // A table reads as its live interpolated value — in ENGINEERING units, because that is the only way
    // to interpolate it the way the firmware does. This function promises RAW, so the answer is uncooked
    // on the way out, and the types make that a step you have to write rather than one you can forget.
    // Getting it wrong showed every table readout in the studio a tenth of the cell it sat under.
    if (L.kind == MetaModel::Location::Kind::Table)
        return uncook(solveTable(path), configScale(path)).v;
    // An EXPRESSION field is a program, not a number: decodeRaw has no case for EXPR, so every slot —
    // written or empty — read back as 0. Report PRESENCE instead: 1 when the slot holds a program, 0
    // when it is empty. That is the only question about a program a condition can usefully ask, and it
    // is the one the pages need: a monitor's ACTION is meaningless until its condition has been written,
    // so it gates on "[#...condition] > 0".
    //
    // ASKED OF THE RESOLVED FIELD, not of the path. isExpressionField() re-parses the path and scans an
    // array's element ids and fields to answer — perfectly fine once, and a disaster in front of EVERY
    // config read, which is what this was. locate() has already done that work, and its datatype is the
    // same answer for free: 3.4 ms of a 16.7 ms frame went into asking the expensive way.
    if (L.valid() && L.datatype == "EXPR") {
        const std::vector<uint8_t> prog = configBlob(path);
        return std::any_of(prog.begin(), prog.end(), [](uint8_t b) { return b != 0; }) ? 1.0 : 0.0;
    }
    if (!L.valid()) {
        // Debug, not Warn: value() falls back to configValue() for ANY name (telemetry names read while
        // disconnected land here every frame), so a miss is normal, not an error to flag.
        JLOGC("model.cache", jf::JLogLevel::Debug) << "unresolved config path: " << path;
        return 0.0;
    }
    return readRaw(L).v;
}

// The field's meta scale (raw<->eng), config or array element. The widget SEEDS its Scale property from this
// (the Cache itself no longer multiplies by it).
// A bit group is stored in its parent field, so it reports THAT type — which is what locate() gives,
// since the group's location IS the parent's word plus a bit range.
std::string Cache::configDatatype(const std::string &path) const
{
    return meta_ ? meta_->locate(path).datatype : std::string{};
}

// The raw->engineering scale of any path. Ask the grammar; it is the same walk.
//
// This used to know about config scalars and struct-array fields and nothing else — so for a 1-D ARRAY
// it silently returned 1.0. Everything built on it inherited that: scaleOf(), and therefore dispV() and
// srcV(), did no scaling for a 1-D array, and Array1DWidget compensated by reading with the array's
// scale itself. Two wrongs that cancelled, until you looked at a scaled one (short_pw_adder_table,
// scale 0.001) or moved either half. locate() covers every shape, including bit groups, which carry
// scale 1.0 because a bit range has nothing to cook.
// A calibration's cells are readings in the sensor's TYPE, and for a generic input that type is a tune
// byte — so the scale and units the meta ships per element are a build-time guess (0.01, decided before
// the input had a type). Everything that displays or writes a cal has to resolve it the same way or the
// halves disagree: the bounds said 200 % while the display multiplied by 0.01 and showed 20.
// "sensors.sensor[clt].source" — an element's field path. The catalog id when the array has one, else
// the index, which is the form every resolver accepts. Written once: three copies of this conditional
// had grown, and a fourth was about to.
std::string Cache::elementFieldPath(const MetaModel::ConfigArray &ca, const std::string &arrayKey,
                                    int index, const std::string &field)
{
    const std::string key = (index >= 0 && index < int(ca.elementIds.size()) && !ca.elementIds[index].empty())
                          ? ca.elementIds[index] : std::to_string(index);
    return arrayKey + "[" + key + "]." + field;
}

// The ID of the option an element's enum field is set to ("analog_voltage"), or "" if the field, the
// element or the id list is absent. Ids rather than labels: a label is for reading, and matching one is
// how a rename in the definition silently changes behaviour here.
// The raw channel an element's physical input publishes, resolved as the firmware resolves it:
// interface says WHICH pool, source says which pin in it. Unknown when the element owns no external
// pin (on-board, CAN) or is unassigned — in which case the axis falls back to what its storage holds.
Cache::ChannelDomain Cache::rawInputDomain(const std::string &arrayKey, int index) const
{
    if (!meta_)
        return {};
    const std::string iface = elementOption(arrayKey, index, "interface");
    const std::vector<std::string> *pool = iface.empty() ? nullptr : meta_->hwPoolSignals(iface);
    if (!pool || pool->empty())
        return {};
    const auto it = meta_->configArrays().find(arrayKey);
    if (it == meta_->configArrays().end())
        return {};
    const MetaModel::ConfigArray &ca = it->second;
    const int src = static_cast<int>(configValue(elementFieldPath(ca, arrayKey, index, "source")));
    // An UNASSIGNED pin (-1, which every sensor is in the default tune) still calibrates against the
    // same quantity: every channel in a pool carries one descriptor — all 20 analog pins are ADC
    // 0..4095, all 8 digital-frequency pins are Hz — so the first stands for the pool. Saying "unknown"
    // here instead would offer a 0..65535 axis on a 12-bit input until a pin happened to be chosen.
    const int idx = (src >= 0 && src < int(pool->size())) ? src : 0;
    return channelDomain((*pool)[idx]);
}

std::string Cache::elementOption(const std::string &arrayKey, int index, const std::string &field) const
{
    if (!meta_ || index < 0)
        return {};
    const auto it = meta_->configArrays().find(arrayKey);
    if (it == meta_->configArrays().end())
        return {};
    const MetaModel::ConfigArray &ca = it->second;
    const MetaModel::ArrayField *af = nullptr;
    for (const auto &f : ca.fields) if (f.name == field) { af = &f; break; }
    if (!af || af->optionIds.empty())
        return {};
    const int sel = static_cast<int>(configValue(elementFieldPath(ca, arrayKey, index, field)));
    return (sel >= 0 && sel < int(af->optionIds.size())) ? af->optionIds[sel] : std::string();
}

const MetaModel::SensorType *Cache::typeScaledCells(const std::string &path) const
{
    if (!meta_ || path.find('[') == std::string::npos)
        return nullptr;                              // not an element path: nothing to resolve
    const MetaModel::ConfigArray *arr = nullptr; int idx = 0;
    const MetaModel::ElemTable *et = meta_->elementTable(path, arr, idx);
    if (!et || !et->typeScaled || !arr)
        return nullptr;
    return sensorTypeOf(arr->module.empty() ? arr->name : (arr->module + "." + arr->name), idx);
}

const MetaModel::SensorType *Cache::typeScaledField(const std::string &path) const
{
    if (!meta_ || path.find('[') == std::string::npos)
        return nullptr;
    const MetaModel::ConfigArray *arr = nullptr; int idx = 0;
    const MetaModel::ArrayField *af = meta_->elementField(path, arr, idx);
    if (!af || !af->typeScaled || !arr)
        return nullptr;
    return sensorTypeOf(arr->module.empty() ? arr->name : (arr->module + "." + arr->name), idx);
}

double Cache::configScale(const std::string &path) const
{
    if (!meta_) return 1.0;
    if (const MetaModel::SensorType *st = typeScaledCells(path); st && st->scale > 0.0)
        return st->scale;                            // the type's, not the per-element guess
    // …and the same for a field stored in its sensor's units. The firmware multiplies these by the
    // type's val_scale (PipelineBuilder: diag_op_min * vscale), so the studio must divide by the same.
    if (const MetaModel::SensorType *st = typeScaledField(path); st && st->scale > 0.0)
        return st->scale;
    const MetaModel::Location L = meta_->locate(path);
    return L.valid() ? L.scale : 1.0;
}

// Write a config path. RAW in — the unit the bytes are in, and the unit the schema's min/max/default
// are authored in; the widget layer un-cooked it (srcV) before it got here.
//
// This was the thirteenth resolver: it re-implemented the path walk inline (a bit-group branch, a
// config_ lookup, a resolveArrayField fallback), and having resolved the field it discarded the bounds
// it had just looked up — `double mn, mx;` existed only to satisfy a signature. So every scalar widget
// in the studio wrote through the one door that did not clamp, and out-of-range values WRAPPED rather
// than saturating. It is the same operation as every other write now, and it clamps because there is no
// way to reach the bytes that does not.
// WHICH OPTION SET A GATED PICKER IS ON, as an index into the field's sets (-1 = none applies). A
// picker declared `by: <sibling>` offers a different list per value of that sibling, and the stored
// number is an index into the list it was picked from -- so the set identity IS the value's meaning.
static int pickerSetIndexFor(const MetaModel::ArrayField &f, int gate)
{
    for (size_t i = 0; i < f.pickerSets.size(); ++i)
        for (int ifc : f.pickerSets[i].ifaces)
            if (ifc == gate) return static_cast<int>(i);
    return -1;
}

void Cache::setConfigValue(const std::string &path, double raw)
{
    if (!meta_)
        return;
    const MetaModel::Location L = meta_->locate(path);
    if (!L.valid() || !blockBytes(L.offset, MetaModel::dataSize(L.datatype)))
        return;                                    // unknown path, or unmapped (bad offset / no block)
    JLOGC("model.cache.edit", jf::JLogLevel::Debug) << "set " << path << " = " << raw
        << " @ " << hx(L.offset) << " (" << L.datatype << ")"
        << (L.bits.packed() ? " [" + std::to_string(L.bits.lo) + ":" + std::to_string(L.bits.hi) + "]" : "");
    const double before = configValue(path);
    writeRaw(L, Raw{raw}, path);
    invalidateGatedSiblings(path, before, raw);
}

// A GATE MOVED, SO WHAT IT GATES IS STALE. A picker field declared `by: <sibling>` stores an index into
// the option set that sibling selects; change the sibling and the same number addresses a different
// list. It is not a range error -- the number usually stays perfectly valid, which is exactly why this
// was silent: sensor `source` 0 is AV1 under Analog Voltage and DIG1 under Frequency, so switching the
// interface quietly re-pointed the sensor at a different physical pin. The test is therefore whether the
// APPLICABLE SET CHANGED, never whether the value is still in range.
//
// Restores the field's own schema default (per element where the schema gives each one its own) rather
// than a hardcoded sentinel, and only ever touches siblings of the field just written.
void Cache::invalidateGatedSiblings(const std::string &path, double before, double after)
{
    if (!meta_ || before == after) return;
    const auto lb = path.find('['), rb = path.find(']'), dot = path.rfind('.');
    if (lb == std::string::npos || rb == std::string::npos || dot == std::string::npos || dot < rb)
        return;
    const std::string arrKey = path.substr(0, lb);
    const std::string elem   = path.substr(lb + 1, rb - lb - 1);
    const std::string fld    = path.substr(dot + 1);

    const auto &arrays = meta_->configArrays();
    const auto it = arrays.find(arrKey);
    if (it == arrays.end()) return;
    const MetaModel::ConfigArray &ca = it->second;

    int elemIndex = -1;                            // for a per-element default; ids first, else the subscript
    for (size_t i = 0; i < ca.elementIds.size(); ++i)
        if (ca.elementIds[i] == elem) { elemIndex = static_cast<int>(i); break; }
    if (elemIndex < 0) { try { elemIndex = std::stoi(elem); } catch (...) { elemIndex = -1; } }

    for (const MetaModel::ArrayField &f : ca.fields) {
        if (f.pickerBy != fld || f.pickerSets.empty()) continue;
        const int wasSet = pickerSetIndexFor(f, static_cast<int>(std::lround(before)));
        const int nowSet = pickerSetIndexFor(f, static_cast<int>(std::lround(after)));
        if (wasSet == nowSet) continue;            // same list -> the stored index still means what it did
        const std::string dep = arrKey + "[" + elem + "]." + f.name;
        const double def = MetaModel::fieldDefault(f, elemIndex, f.minV);
        if (configValue(dep) == def) continue;     // already at its default: nothing to clear
        const MetaModel::Location DL = meta_->locate(dep);
        if (!DL.valid() || !blockBytes(DL.offset, MetaModel::dataSize(DL.datatype))) continue;
        JLOGC("model.cache.edit", jf::JLogLevel::Info)
            << "gate " << path << " " << before << " -> " << after
            << " changed which list " << dep << " indexes (set " << wasSet << " -> " << nowSet
            << "); its value is stale, restoring default " << def;
        writeRaw(DL, Raw{def}, dep);
    }
}

Cache::DefinitionScope::DefinitionScope(Cache &cc, const MetaModel &m, std::vector<uint8_t> &img)
    : c(cc), wasMeta(cc.meta_), image(cc, img)
{
    c.meta_ = &m;
    MathEvaluator::instance().invalidateOwners();
}

Cache::DefinitionScope::~DefinitionScope()
{
    c.meta_ = wasMeta;
    MathEvaluator::instance().invalidateOwners();
}

void Cache::setMeta(const MetaModel *m)
{
    meta_ = m;
    // See the declaration: token ownership belongs to the definition, not to the values.
    MathEvaluator::instance().invalidateOwners();
}

std::vector<uint8_t> Cache::configBlob(const std::string &path) const
{
    int off = 0, size = 0;
    if (!meta_ || !meta_->resolveBlob(path, off, size)) return {};
    if (off < 0 || off + size > int(configImage_.size())) return {};
    return std::vector<uint8_t>(configImage_.begin() + off, configImage_.begin() + off + size);
}

void Cache::setConfigBlob(const std::string &path, const std::vector<uint8_t> &bytes)
{
    int off = 0, size = 0;
    if (!meta_ || !meta_->resolveBlob(path, off, size)) return;
    if (off < 0 || off + size > int(configImage_.size())) return;
    // Zero-fill the tail: a shorter program must not leave the previous one's bytes behind it,
    // where they would be read as instructions after the new OP_END if anything ever mis-parsed.
    std::vector<uint8_t> field(size_t(size), 0);
    if (!bytes.empty())
        std::memcpy(field.data(), bytes.data(), std::min(bytes.size(), size_t(size)));
    if (std::memcmp(configImage_.data() + off, field.data(), size_t(size)) == 0)
        return;                                       // identical → no write, no dirty edit
    beginEdit();
    std::memcpy(configImage_.data() + off, field.data(), size_t(size));
    endEdit("Edit " + path);                          // batches the chunked write to the ECU
}

// TEXT IS A BLOB, ADDRESSED THE SAME WAY EVERY OTHER FIELD IS. Both of these used to look the path up
// in the flat config map, which holds DECLARED fields only — so a string inside a struct array was
// invisible to them: outputs.output[3].name resolves through resolveBlob (as an expression program
// does) and appears nowhere in that map. The Output Setup page's Name box therefore showed nothing and
// swallowed every keystroke, because the write had no field to land in and said so by doing nothing.
std::string Cache::configString(const std::string &path) const
{
    int off = 0, size = 0;
    if (!meta_ || !meta_->resolveBlob(path, off, size)) return {};
    if (off < 0 || off + size > int(configImage_.size())) return {};
    const char *p = reinterpret_cast<const char *>(configImage_.data()) + off;
    int n = 0;
    while (n < size && p[n] != '\0') ++n;
    return std::string(p, n);
}

// The script field's name, per firmware. Ordered: the native one first, so a jayecu ECU is never
// ambiguous, and a definition that somehow carried both would use its own.
static const char *const kScriptPaths[] = { "lua.source", "ts.luaScript" };

std::string Cache::scriptPath() const
{
    if (!meta_) return {};
    int off = 0, size = 0;
    for (const char *p : kScriptPaths)
        if (meta_->resolveBlob(p, off, size) && size > 1) return p;
    return {};
}

size_t Cache::scriptCapacity() const
{
    const std::string path = scriptPath();
    if (path.empty() || !meta_) return 0;
    // capacity() is the ONE place that rule lives (MetaModel.h) — a second `size - 1` here is exactly
    // the duplication that cost the VIN its seventeenth character.
    const auto cf = meta_->config().find(path);
    return cf != meta_->config().end() ? size_t(cf->second.capacity()) : 0;
}

void Cache::setConfigString(const std::string &path, const std::string &text)
{
    int off = 0, size = 0;
    if (!meta_ || !meta_->resolveBlob(path, off, size)) return;
    if (off < 0 || off + size > int(configImage_.size())) return;
    std::vector<uint8_t> field(size_t(size), 0);      // NUL-padded field
    // HOW MANY CHARACTERS FIT — the definition's answer where it gives one, else one short of the field.
    //
    // The field is zero-filled above, so anything shorter than it is terminated by its own padding: a
    // terminator only costs a byte when the text fills the field exactly. That is precisely the VIN —
    // seventeen characters in seventeen bytes, declared "17, 17" — and reserving a byte regardless made
    // the last character of every VIN impossible to enter, while the same rule cost the 32-byte name
    // fields a character they were never going to miss.
    //
    // So a stated capacity is honoured, and a field that states none keeps a byte back, which is the
    // safe reading when nothing says otherwise.
    const auto cf = meta_->config().find(path);
    const size_t cap = cf != meta_->config().end() ? size_t(cf->second.capacity()) : size_t(size - 1);
    const size_t bodyLen = std::min<size_t>(text.size(), cap);
    std::copy(text.begin(), text.begin() + bodyLen, field.begin());
    if (memcmp(configImage_.data() + off, field.data(), size_t(size)) == 0)
        return;                                       // identical → no write
    beginEdit();
    memcpy(configImage_.data() + off, field.data(), size_t(size));
    endEdit("Edit " + path);                          // batches the chunked write to the ECU
}

// The definition's declared byte order. A whole-meta property, so this is a lookup rather than state.
bool Cache::be_() const { return meta_ && meta_->bigEndian(); }

// Does this offset fall in a host (PcVariable) block?
bool Cache::isHostOffset_(int offset) const
{
    for (const Segment &s : segments_)
        if (s.host && uint64_t(offset) >= s.base && uint64_t(offset) < s.base + s.bytes.size())
            return true;
    return false;
}

double Cache::readAt(int offset, const std::string &datatype, double scale, BitRange bits) const
{
    const int size = MetaModel::dataSize(datatype);
    const uint8_t *p = blockBytes(offset, size);   // config or a segment, by absolute offset
    if (!p)
        return 0.0;
    // Packed: the value is a bit RANGE of the word, not the word. Several flags share one word, so
    // decoding the whole thing would give every one of them the same number.
    return bits.extract(MetaModel::decodeRaw(datatype, p, be_()), size) * scale;
}

// The single point at which bytes enter a cache block. Gate, resolve the owning block, encode — once.
void Cache::encodeAt_(int offset, const std::string &datatype, double scale, double engValue,
                      BitRange bits)
{
    if (readOnly_ && forceWrites_ == 0) return;   // Locked mode: view-only — but a WriteGuard lets structural table setup through
    const int size = MetaModel::dataSize(datatype);
    uint8_t *p = blockBytes(offset, size);        // config image (base 0) or a segment (learned region)
    if (!p)
        return;
    const double raw = (scale == 0.0) ? engValue : engValue / scale;
    if (!bits.packed()) {
        MetaModel::encodeRaw(datatype, p, raw, be_());
        return;
    }
    // Packed: READ-MODIFY-WRITE. Writing the word outright would zero every other flag sharing it — a
    // one-bit edit silently clearing its neighbours is the kind of corruption nobody attributes to the
    // widget they were actually using.
    MetaModel::encodeRaw(datatype, p,
                         bits.insert(MetaModel::decodeRaw(datatype, p, be_()), std::llround(raw), size),
                         be_());
}

void Cache::writeAt(int offset, const std::string &datatype, double scale, double engValue, BitRange bits)
{
    beginEdit();
    encodeAt_(offset, datatype, scale, engValue, bits);
    endEdit("Edit cells");   // endEdit() batches the write to the ECU
}

// ---- Resolved writes ------------------------------------------------------------------------------
// resolveX() answers "where does this live and what may it hold"; write() is the only thing that puts a
// value there. Bounds travel WITH the location, so there is no way to obtain a writable ref and not have
// its limits — the clamp cannot be forgotten at a call site the way it was before.


MetaModel::Location Cache::cellLocAt(const TableImage &t, int offset) const
{
    MetaModel::Location L;
    if (!t.valid || t.cellSize <= 0)
        return L;                       // invalid() -> readRaw/writeRaw are no-ops
    L.kind = MetaModel::Location::Kind::Scalar;
    L.offset = offset; L.datatype = t.cellType; L.scale = t.cellScale;
    L.minV = t.cellMinV; L.maxV = t.cellMaxV;
    return L;
}


// What a DATATYPE can hold, in raw units — rawBounds' second half, reachable without inventing a
// Location to ask through. Two callers were building a dummy one field by field to get at it.
void Cache::datatypeRange(const std::string &datatype, double &lo, double &hi)
{
    MetaModel::Location probe;
    probe.kind = MetaModel::Location::Kind::Scalar;
    probe.offset = 0;
    probe.datatype = datatype;
    rawBounds(probe, lo, hi);
}

// What a location may hold, in RAW units. Declared bounds if it has them; otherwise the range its
// datatype can store — see the declaration for why there is no third answer.
void Cache::rawBounds(const MetaModel::Location &L, double &lo, double &hi)
{
    if (L.maxV > L.minV) { lo = L.minV; hi = L.maxV; return; }
    // A packed bit range holds what its width holds, regardless of the word's type.
    if (L.bits.packed()) {
        lo = 0.0;
        hi = static_cast<double>((1ull << (L.bits.hi - L.bits.lo + 1)) - 1ull);
        return;
    }
    const std::string &d = L.datatype;
    const int bytes = MetaModel::dataSize(d);
    if (d.empty() || d[0] == 'F' || d[0] == 'f' || bytes <= 0) {   // float: no integer range to clamp to
        lo = -std::numeric_limits<double>::infinity();
        hi =  std::numeric_limits<double>::infinity();
        return;
    }
    const double span = std::ldexp(1.0, bytes * 8);
    const bool   sign = (d[0] == 'S' || d[0] == 's');
    lo = sign ? -span / 2.0 : 0.0;
    hi = sign ?  span / 2.0 - 1.0 : span - 1.0;
}

Raw Cache::readRaw(const MetaModel::Location &L) const
{
    if (!L.valid())
        return Raw{};
    return Raw{ readAt(L.offset, L.datatype, 1.0, L.bits) };   // scale 1: the Cache speaks raw
}

void Cache::writeRaw(const MetaModel::Location &L, Raw rawv, const std::string &notifyPath)
{
    if (!L.valid())
        return;
    const double raw = rawv.v;
    double lo = 0.0, hi = 0.0;
    rawBounds(L, lo, hi);
    const double v = (lo <= hi) ? std::clamp(raw, lo, hi) : raw;
    // Own a transaction even when the caller opened none, so a lone write still gets an undo entry and a
    // markDirty (without which the edit never reaches the ECU). Nesting is refcounted, so a bulk caller's
    // outer beginEdit/endEdit still coalesces the run into one step. The tally is bumped AFTER
    // beginEdit(): an outermost beginEdit() resets it, which would discard the count this call just made.
    beginEdit();
    if (v != raw) ++clamped_;
    ++written_;
    const double before = notifyPath.empty() ? 0.0 : readRaw(L).v;
    encodeAt_(L.offset, L.datatype, 1.0, v, L.bits);
    if (!notifyPath.empty()) {
        configValueChanged.emit(notifyPath);
        const double after = readRaw(L).v;
        if (after != before) configEdited.emit(notifyPath, before, after);
    }
    endEdit(notifyPath.empty() ? "Edit" : "Edit " + notifyPath);
}


bool Cache::isTable(const std::string &path) const
{
    if (!meta_)
        return false;
    if (meta_->configTables().count(path))
        return true;
    // A per-element table too ("module.array[i].table", e.g. electronic_throttle.etb[0].ff_table) — the
    // unified resolveTable()/ti* path handles both, so the editor must accept it as a table.
    const MetaModel::ConfigArray *arr = nullptr;
    int idx = 0;
    const MetaModel::ElemTable *et = meta_->elementTable(path, arr, idx);
    return et && arr;   // any element table (curve or grid) is the one table type
}

std::string Cache::widgetTypeFor(const std::string &path) const
{
    // A RUN of values -> the dedicated 1D array editor (controlFor doesn't classify these). Covers both
    // shapes: a 1-D breakpoint array, and a per-element sub-array addressed as a whole
    // ("trigger.streams[0].cell[].v"). Without the second, dragging a stream's cells out of the
    // dictionary dropped a spin box over cell 0 rather than the strip the path actually names.
    if (meta_ && meta_->locate(path).kind == MetaModel::Location::Kind::Run) return "array1d";
    // An EXPRESSION field is a compiled program: it binds to the expression editor, and never to a
    // spin box over byte 0 of its bytecode. Checked before everything else because the field is an
    // array element (a sensor's precondition) and would otherwise fall through to "configedit".
    if (meta_ && meta_->isExpressionField(path)) return "expression";
    // A STRING field is text, not a number: a spin box cannot show a VIN.
    if (meta_) {
        const auto sf = meta_->config().find(path);
        if (sf != meta_->config().end() && sf->second.isText()) return "text";
    }
    const std::string kind = controlFor(path);
    if (kind == "table")  return "table";
    if (kind == "curve")  return "curve";
    // A STATIC option list is a dropdown. The enum picker stays for the two cases a fixed list cannot
    // express: a signal selector (hundreds of channels, needs the searchable dialog) and an interface-gated
    // pin picker (its options depend on a sibling field's value). Everything else — "Four Stroke / Two
    // Stroke", "Rising / Falling" — is a combo box, which brings keyboard select, type-ahead and the wheel
    // for free, and is what a list of choices looks like everywhere else in the app.
    if (kind == "enum") {
        std::string by; std::vector<MetaModel::PickerSet> sets;
        // A table selector has 167 options and grows with the schema. A combo box is the right control
        // for "Rising / Falling"; it is the wrong one for a list you have to search, which is exactly
        // why the signal selector uses the picker dialog — and a table id is the same kind of question.
        const bool dynamic = meta_ && (meta_->isSignalField(path) || meta_->isTableField(path)
                                       || meta_->fieldPicker(path, by, sets));
        return dynamic ? "enum" : "combobox";
    }
    if (kind == "toggle") return "checkbox";    // 0/1 flag
    if (kind == "config") return "configedit";  // editable config scalar
    return "field";                             // "value" (telemetry readout) / unknown
}

std::vector<std::string> Cache::tableOptionLabels(const std::string &path) const
{
    if (!meta_ || !meta_->isTableField(path)) return {};
    std::vector<std::string> opts = meta_->enumOptions(path);
    const std::vector<std::string> ids = meta_->enumOptionIds(path);
    // A GENERIC table's name is config, not meta: it is what the tuner called it, and it is the only
    // thing that tells Generic Table 3 from Generic Table 4. Read it live so a rename shows up in the
    // picker the moment it is typed, rather than at the next meta push.
    static const std::string kGeneric = "generic_tables_table_";
    for (size_t i = 0; i < opts.size() && i < ids.size(); ++i) {
        if (ids[i].compare(0, kGeneric.size(), kGeneric) != 0) continue;
        const std::string n = configString("generic_tables.name_" + ids[i].substr(kGeneric.size()));
        if (!n.empty()) opts[i] += "  \xE2\x80\x94 " + n;      // "Generic Table 3 — Fan Ramp"
    }
    return opts;
}

std::string Cache::controlFor(const std::string &path) const
{
    if (!meta_)
        return "value";
    if (meta_->configTables().count(path))
        return "table";
    const MetaModel::ConfigArray *ca = nullptr;
    int elemIdx;
    if (const MetaModel::ElemTable *et = meta_->elementTable(path, ca, elemIdx))  // "sensors.sensor[i].cal"
        return et->display == "curve" ? "curve" : "table";
    const std::string fk = meta_->fieldKind(path);     // explicit control kind (config scalar OR array elem)
    // A channel selector — "signal" (any bus channel) or "sensor" (restricted to channels a sensor
    // actually produces). Both are pickers. Matching the literal "signal" here meant a sensor-kind field
    // fell all the way through to "config" and was dropped as a SPIN BOX: ETB's TPS A Signal offered you
    // a number to type instead of the sensor list, and typing a raw channel id is exactly what the
    // restricted picker exists to prevent. isSignalField() already covers both.
    // A named BIT GROUP is the unit the user edits — the byte around it is just storage. One bit is a
    // flag (checkbox); a wider group is a named level (list). Without this, dragging a sensor's
    // Diagnostics out gave a spin box over the whole mask, where typing a number rewrote every check.
    if (const MetaModel::BitGroup *bg = meta_->bitGroupOf(path))
        return bg->width() == 1 ? "toggle" : "enum";
    if (meta_->isSignalField(path)) return "enum";
    if (meta_->isTableField(path))  return "enum";   // a table id: picked from a list, never typed
    if (fk == "bool")   return "toggle";   // 0/1 flag (array elems too)
    auto isBoolRange = [](const std::string &dt, double mn, double mx) {
        return (dt.empty() || dt.front() != 'F') && fuzzyEq(mn + 1, 1) && fuzzyEq(mx, 1.0);   // int 0..1
    };
    if (meta_->config().count(path)) {
        const MetaModel::ConfigField &f = meta_->config().at(path);
        if (f.kind == "enum" && !f.options.empty())
            return "enum";
        if (f.kind == "bool" || isBoolRange(f.datatype, f.minV, f.maxV))
            return "toggle";
        return "config";
    }
    if (const MetaModel::Location L = meta_->locate(path);
        L.kind == MetaModel::Location::Kind::Scalar) {                    // sensors.sensor[i].field
        std::string by; std::vector<MetaModel::PickerSet> sets;
        if (!meta_->enumOptions(path).empty() || meta_->fieldPicker(path, by, sets))
            return "enum";
        return isBoolRange(L.datatype, L.minV, L.maxV) ? "toggle" : "config";
    }
    return "value";   // telemetry / unknown
}


// The live shape of a table, by path. Each is the one descriptor plus the one question — where these
// used to branch on whether the path named a module table or a per-element one, re-deriving the module
// case inline and delegating only the element case. TableImage exists precisely so that split does not:
// "there is no module-path / element-path split", as its own comment has said all along.
int Cache::liveCols(const std::string &path) const
{
    return tiLiveN(resolveTable(path), 0);
}

int Cache::liveRows(const std::string &path) const
{
    return tiLiveN(resolveTable(path), 1);
}

int Cache::liveDepth(const std::string &path) const
{
    return tiLiveN(resolveTable(path), 2);     // a 2D table (no z axis) collapses to a single plane
}

double Cache::tableCell(const std::string &path, int col, int row, int z) const
{
    if (!meta_)
        return 0.0;
    if (!meta_->configTables().count(path)) {                    // per-element table -> unified path
        const TableImage t = resolveTable(path);
        return t.valid ? tiCell(t, col, row, z) : 0.0;
    }
    const MetaModel::ConfigTable &t = meta_->configTables().at(path);
    // PHYSICAL stride: a table is a fixed grid and <axis>_n only says how much of it is in play.
    const int plane = t.rowsMax * t.colsMax;                                // cells per z plane
    const int off = t.offset + (z * plane + row * t.colsMax + col) * t.cellSize;
    const uint8_t *p = blockBytes(off, t.cellSize);   // config or a segment (a learned-region table)
    if (!p)
        return 0.0;
    return MetaModel::decodeRaw(t.datatype, p, meta_->bigEndian()) * t.scale;
}

void Cache::setTableCell(const std::string &path, int col, int row, double engValue, int z)
{
    if (!meta_)
        return;
    if (!meta_->configTables().count(path)) {                    // per-element table -> unified path
        const TableImage t = resolveTable(path);
        if (!t.valid)
            return;
        beginEdit();
        tiSetCell(t, col, row, z, engValue);
        endEdit("Edit cell");
        return;
    }
    const MetaModel::ConfigTable &t = meta_->configTables().at(path);
    // PHYSICAL stride: a table is a fixed grid and <axis>_n only says how much of it is in play.
    const int plane = t.rowsMax * t.colsMax;                                // cells per z plane
    const int off = t.offset + (z * plane + row * t.colsMax + col) * t.cellSize;
    JLOGC("model.cache.edit", jf::JLogLevel::Debug) << "cell " << path << "[c" << col << ",r" << row
        << (z ? ",z" + std::to_string(z) : "") << "] = " << engValue << " @ " << hx(off);
    beginEdit();
    encodeAt_(off, t.datatype, t.scale, engValue);
    endEdit("Edit cell");   // endEdit() batches the write to the ECU (one frame for a multi-cell edit)
}

void Cache::writeAxisValues(const std::string &arrayPath, int count, const std::vector<double> &vals)
{
    if (!meta_ || !meta_->arrays1d().count(arrayPath))
        return;
    const MetaModel::ConfigField &f = meta_->arrays1d().at(arrayPath);
    const int esz = MetaModel::dataSize(f.datatype);
    for (int i = 0; i < count; ++i) {
        const double v = vals.empty() ? 0.0 : vals[std::min(i, int(vals.size()) - 1)];   // hold edge
        const int off = f.offset + i * esz;
        if (off + esz > int(configImage_.size()))
            break;
        encodeAt_(off, f.datatype, f.scale, v);
    }
    // No write here: writeAxisValues only runs inside a resize/insert/delete transaction, whose
    // endEdit() pushes the whole changed region to the ECU in one batch.
}

// A full 3D snapshot of a table's live cells (eng units), indexed [z][row][col]. Taken before any
// stride change so the cells can be re-laid at the new size without reading the half-rewritten image.
static std::vector<std::vector<std::vector<double>>> snapshotCells(const Cache &c, const std::string &path,
                                                       int depth, int rows, int cols)
{
    std::vector<std::vector<std::vector<double>>> cells(depth, std::vector<std::vector<double>>(rows, std::vector<double>(cols)));
    for (int z = 0; z < depth; ++z)
        for (int r = 0; r < rows; ++r)
            for (int col = 0; col < cols; ++col)
                cells[z][r][col] = c.tableCell(path, col, r, z);
    return cells;
}


bool Cache::insertTableBin(const std::string &path, int axis, int at)
{
    if (!meta_)
        return false;
    if (!meta_->configTables().count(path))                  // per-element table -> self-contained ti* reflow
        return tiInsertBin(resolveTable(path), axis, at);
    const MetaModel::ConfigTable t = meta_->configTables().at(path);
    if (axis < 0 || axis >= int(t.axes.size()) || t.axes[axis].nScalar.empty())
        return false;                                     // only resizable axes (have a live _n scalar)
    const int oldCols = liveCols(path), oldRows = liveRows(path), oldDepth = liveDepth(path);
    const int oldN = (axis == 0) ? oldCols : (axis == 1) ? oldRows : oldDepth;
    if (t.axes[axis].nMax > 0 && oldN >= t.axes[axis].nMax)
        return false;                                     // already at the axis maximum
    at = std::clamp(at, 0, oldN);                          // 0..oldN (oldN = append at the end)
    beginEdit();

    const auto cells = snapshotCells(*this, path, oldDepth, oldRows, oldCols);   // all planes

    // Value at a new index along the inserted axis: copy the shifted neighbour everywhere, and at the
    // inserted slot `at` average the two neighbour slices (edge-copy at a boundary). Works for any axis
    // by pointing `f` along x / y / z, so one rule covers column / row / plane inserts.
    auto interpAt = [oldN, at](int newIdx, const std::function<double(int)> &f) -> double {
        if (newIdx < at)    return f(newIdx);
        if (newIdx > at)    return f(newIdx - 1);
        if (at == 0)        return f(0);
        if (at >= oldN)     return f(oldN - 1);
        return (f(at - 1) + f(at)) * 0.5;
    };

    // The inserted breakpoint extrapolates at the edges (keep the axis ascending).
    std::vector<double> av = axisValues(t.axes[axis].array);   av.resize(oldN);
    std::vector<double> nav(oldN + 1);
    for (int i = 0; i < oldN + 1; ++i) {
        if (i < at)            nav[i] = av[i];
        else if (i > at)       nav[i] = av[i - 1];
        else if (at == 0)      nav[i] = av.empty() ? 0.0 : av[0] - (oldN > 1 ? av[1] - av[0] : 1.0);
        else if (at >= oldN)   nav[i] = av.back() + (oldN > 1 ? av[oldN - 1] - av[oldN - 2] : 1.0);
        else                   nav[i] = (av[at - 1] + av[at]) * 0.5;
    }

    const int newCols = oldCols + (axis == 0 ? 1 : 0);
    const int newRows = oldRows + (axis == 1 ? 1 : 0);
    const int newDepth = oldDepth + (axis == 2 ? 1 : 0);
    setConfigValue(t.axes[axis].nScalar, oldN + 1);        // new stride before writing cells
    for (int z = 0; z < newDepth; ++z)
        for (int r = 0; r < newRows; ++r)
            for (int c = 0; c < newCols; ++c) {
                double v;
                if (axis == 0)      v = interpAt(c, [&](int s) { return cells[z][r][s]; });
                else if (axis == 1) v = interpAt(r, [&](int s) { return cells[z][s][c]; });
                else                v = interpAt(z, [&](int s) { return cells[s][r][c]; });
                setTableCell(path, c, r, v, z);
            }
    writeAxisValues(t.axes[axis].array, oldN + 1, nav);
    endEdit("Insert bin");
    tableEdited.emit();
    return true;
}

bool Cache::removeTableBin(const std::string &path, int axis, int at)
{
    if (!meta_)
        return false;
    if (!meta_->configTables().count(path))                  // per-element table -> self-contained ti* reflow
        return tiRemoveBin(resolveTable(path), axis, at);
    const MetaModel::ConfigTable t = meta_->configTables().at(path);
    if (axis < 0 || axis >= int(t.axes.size()) || t.axes[axis].nScalar.empty())
        return false;
    const int oldCols = liveCols(path), oldRows = liveRows(path), oldDepth = liveDepth(path);
    const int oldN = (axis == 0) ? oldCols : (axis == 1) ? oldRows : oldDepth;
    if (oldN <= 1)
        return false;                                      // keep at least 1 bin (a length-1 axis)
    at = std::clamp(at, 0, oldN - 1);
    beginEdit();

    const auto cells = snapshotCells(*this, path, oldDepth, oldRows, oldCols);   // all planes
    const auto srcIdx = [at](int newIdx) { return newIdx < at ? newIdx : newIdx + 1; };  // skip `at`

    std::vector<double> av = axisValues(t.axes[axis].array);   av.resize(oldN);
    std::vector<double> nav;
    for (int i = 0; i < oldN; ++i) if (i != at) nav.push_back(av[i]);

    const int newCols = oldCols - (axis == 0 ? 1 : 0);
    const int newRows = oldRows - (axis == 1 ? 1 : 0);
    const int newDepth = oldDepth - (axis == 2 ? 1 : 0);
    setConfigValue(t.axes[axis].nScalar, oldN - 1);
    for (int z = 0; z < newDepth; ++z)
        for (int r = 0; r < newRows; ++r)
            for (int c = 0; c < newCols; ++c) {
                const double v = (axis == 0) ? cells[z][r][srcIdx(c)]
                               : (axis == 1) ? cells[z][srcIdx(r)][c]
                                             : cells[srcIdx(z)][r][c];
                setTableCell(path, c, r, v, z);
            }
    writeAxisValues(t.axes[axis].array, oldN - 1, nav);
    endEdit("Delete bin");
    configLoaded.emit();
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Unified table access (TableImage). resolveTable() resolves a module table OR a per-element table to
// one offset-keyed descriptor; the ti* ops then run on that one struct. No module/element branching
// past resolveTable — the firmware already proved one resolver covers both (TableEngine.h tbl::interp).
// ---------------------------------------------------------------------------------------------------

TableImage Cache::resolveTable(const std::string &path) const
{
    PERF_COUNT("table resolves");
    // One path: delegate to the canonical meta resolver...
    if (!meta_)
        return TableImage{};
    TableImage ti = meta_->resolveTable(path);
    // ...and then answer the one question the meta cannot. A sensor calibration's CELLS are readings in
    // that sensor's TYPE — its units, its precision, its domain — and for a generic input that type is a
    // tune byte, which is Cache's to read, not the meta's. Filling it here means every consumer of a
    // TableImage (the grid, the curve, entry clamping, paste, transforms) gets the same answer without
    // any of them knowing what a sensor is.
    if (ti.valid && ti.cellTypeScaled && ti.elemIndex >= 0) {
        if (const MetaModel::SensorType *st = sensorTypeOf(ti.elemArray, ti.elemIndex)) {
            ti.cellDigits = st->digits;
            ti.cellUnits  = st->units;
            // THE SCALE COMES FROM THE TYPE TOO, not from the per-element list the meta ships. That list
            // is fixed at build time, and a generic input has no type then — it is guessed at 0.01. The
            // firmware has no such problem: decode_curve reads the cal at SENSOR_TYPE_CATALOG[effective
            // _type].val_scale, i.e. the type the tune has the input set to. Taking the same number here
            // is what makes the studio and the ECU agree about what a cal byte means; without it, an
            // aux set to pressure was edited at 0.01 and read back by the firmware at 0.1 — every cal
            // point ten times off, and a domain that could not reach past 327 kPa of the type's 3000.
            if (st->scale > 0.0) ti.cellScale = st->scale;
            if (st->maxV > st->minV && ti.cellScale != 0.0) {
                // Bounds are stored RAW (as `default` is), so divide by the cell scale. Never widen past
                // what the storage can hold: encodeRaw casts without saturating, so a bound outside the
                // datatype would let a write wrap instead of clamping.
                double lo = st->minV / ti.cellScale, hi = st->maxV / ti.cellScale;
                if (lo > hi) std::swap(lo, hi);
                double dlo = 0.0, dhi = 0.0;
                datatypeRange(ti.cellType, dlo, dhi);
                lo = std::max(lo, dlo); hi = std::min(hi, dhi);
                if (hi > lo) { ti.cellMinV = lo; ti.cellMaxV = hi; }
            }
        }
    }
    return ti;
}

void Cache::announceReload()
{
    tableEdited.emit();   // in-place view refresh — NOT configLoaded (that rebuilds widgets, killing popups)
}

// How many bins the CELLS are laid out at on this axis — a stride, so always >= 1. A disabled axis
// still occupies one slice (the firmware reads cell[0] along it), which is why this cannot double as
// the on/off question: see tiEnabled.
int Cache::tiLiveN(const TableImage &t, int axis) const
{
    if (axis < 0 || axis >= int(t.axes.size())) return 1;
    const TableImage::Axis &a = t.axes[axis];
    if (a.enBase >= 0 && readAt(a.enBase, "U08", 1.0) == 0.0)
        return 1;                                              // disabled axis collapses to one bin
    if (a.nBase < 0) return std::max(1, a.nMax);               // fixed grid
    const int cap = a.nMax > 0 ? a.nMax : 256;
    return std::clamp(int(readAt(a.nBase, "U08", 1.0)), 1, cap);
}

// How wide an axis's STORAGE is — the stride, whatever the live count currently says. A fixed-grid axis
// states it as nMax; a resizable one allocates nMax bins and lives inside them.
int Cache::tiAllocN(const TableImage &t, int axis) const
{
    if (axis < 0 || axis >= int(t.axes.size())) return 1;
    const TableImage::Axis &a = t.axes[axis];
    return a.nMax > 0 ? a.nMax : std::max(1, tiLiveN(t, axis));
}

std::vector<double> Cache::tiBins(const TableImage &t, int axis) const
{
    std::vector<double> v;
    if (axis < 0 || axis >= int(t.axes.size())) return v;
    const TableImage::Axis &a = t.axes[axis];
    const int n = tiLiveN(t, axis);
    for (int k = 0; k < n; ++k)
        v.push_back(readAt(a.breaksBase + k * a.breakSize, a.breakType, a.breakScale));
    return v;
}

void Cache::tiWriteBins(const TableImage &t, int axis, const std::vector<double> &vals)
{
    if (axis < 0 || axis >= int(t.axes.size())) return;
    const TableImage::Axis &a = t.axes[axis];
    for (int k = 0; k < int(vals.size()); ++k)
        writeAt(a.breaksBase + k * a.breakSize, a.breakType, a.breakScale, vals[k]);
}

int Cache::tiSrc(const TableImage &t, int axis) const
{
    if (axis < 0 || axis >= int(t.axes.size()) || t.axes[axis].srcBase < 0) return -1;
    return int(readAt(t.axes[axis].srcBase, "S16", 1.0));   // channel selector is int16 (-1 = no source)
}

std::string Cache::axisChannel(const TableImage &t, int axis) const
{
    if (axis < 0 || axis >= int(t.axes.size())) return {};
    std::string nm;
    if (meta_) {
        const int sid = tiSrc(t, axis);
        if (sid >= 0) nm = meta_->signalName(sid);
    }
    if (nm.empty()) nm = t.axes[axis].defaultSig;   // an imported table names its channel outright
    return nm;
}

void Cache::tiSetSrc(const TableImage &t, int axis, int sigId)
{
    if (axis < 0 || axis >= int(t.axes.size()) || t.axes[axis].srcBase < 0) return;
    writeAt(t.axes[axis].srcBase, "S16", 1.0, sigId);       // channel selector is int16 (-1 = no source)
    // The bins are NOT touched. Changing the channel changes what they MEAN, not what they are, and
    // the tuner's numbers are not the studio's to rewrite on a menu pick.
    //
    // The reference ECU makes this visible: its axis holds a raw count and each type supplies a scale
    // and a floor (0.1/-273 C, 0.1/-101.3 kPa, 0.001/0 V), so switching channel only re-reads the same
    // count — 0..10000 rpm reads as -273..727 C — and switching back returns 0..10000 exactly. Those
    // offsets are absolute zero and full vacuum: the bottom of an unsigned encoding, not a conversion.
    // There is nothing to reproduce, because our breakpoints are already engineering values in F32 at
    // scale 1.0 — no raw underneath, so preserving the raw IS preserving the number, and the round
    // trip is exact for free.
    //
    // What was here before — stretching the axis's span onto the new channel's domain — made every bin
    // legal at the cost of being a one-way door: rpm -> pedal -> rpm could not give the axis back once
    // its undo was spent. Legality is enforced where a value is AUTHORED (typed, Insert, Linearise). A
    // bin left illegal by a channel change is FLAGGED in the dialog instead: visible, fixable, and
    // costing nothing if the channel was picked by mistake.
}

// Is an OPTIONAL axis switched on? Absent axes are off; non-optional ones are on — nothing to switch.
// Deliberately NOT tiLiveN: that is a STRIDE and is always >= 1, because a disabled axis still occupies
// one slice (the firmware reads cell[0] along it).
//
// axisActive(path, i) asked this a second way, through axisEffLen's return value, where 0 meant "off"
// and any other number meant "this many bins". It was wrong for every per-element table — that branch
// went through tiLiveN, which cannot say "off" — and it had no callers at all, so it is gone rather
// than fixed.
bool Cache::tiEnabled(const TableImage &t, int axis) const
{
    if (axis < 0 || axis >= int(t.axes.size())) return false;
    const TableImage::Axis &a = t.axes[axis];
    if (a.enBase < 0) return true;                              // non-optional axis is always on
    return readAt(a.enBase, "U08", 1.0) != 0.0;
}

void Cache::tiSetEnabled(const TableImage &t, int axis, bool on)
{
    if (axis < 0 || axis >= int(t.axes.size()) || t.axes[axis].enBase < 0) return;
    writeAt(t.axes[axis].enBase, "U08", 1.0, on ? 1 : 0);
}

int Cache::tiCellOffset(const TableImage &t, int c, int r, int z) const
{
    // The ALLOCATION is the stride, matching the firmware (tbl::interp takes each axis's alloc). The
    // live <axis>_n bounds the SEARCH, not the layout, so changing a size moves no cell.
    const int xa = std::max(1, tiAllocN(t, 0));
    const int ya = t.axes.size() > 1 ? std::max(1, tiAllocN(t, 1)) : 1;
    return t.cellBase + (z * xa * ya + r * xa + c) * t.cellSize;
}

double Cache::tiCell(const TableImage &t, int c, int r, int z) const
{
    return readAt(tiCellOffset(t, c, r, z), t.cellType, t.cellScale);
}

void Cache::tiSetCell(const TableImage &t, int c, int r, int z, double engVal)
{
    writeAt(tiCellOffset(t, c, r, z), t.cellType, t.cellScale, engVal);
}

// Insert a bin on `axis` at storage index `at`: snapshot the cells at the OLD live stride, bump the
// live-n (changing the stride), then re-lay every cell + the breakpoint array. One rule (interpAt
// pointed along x/y/z) covers a column / row / plane insert, for module and element tables alike.
bool Cache::tiInsertBin(const TableImage &t, int axis, int at)
{
    if (!t.valid || axis < 0 || axis >= int(t.axes.size())) return false;
    const TableImage::Axis &a = t.axes[axis];
    if (a.nBase < 0) return false;                             // fixed (non-resizable) axis
    const int xn = tiLiveN(t, 0), yn = t.axes.size() > 1 ? tiLiveN(t, 1) : 1,
              zn = t.axes.size() > 2 ? tiLiveN(t, 2) : 1;
    const int oldN = (axis == 0) ? xn : (axis == 1) ? yn : zn;
    if (a.nMax > 0 && oldN >= a.nMax) return false;
    at = std::clamp(at, 0, oldN);
    beginEdit();

    std::vector<double> snap(xn * yn * zn);
    for (int z = 0; z < zn; ++z)
        for (int r = 0; r < yn; ++r)
            for (int c = 0; c < xn; ++c)
                snap[z * xn * yn + r * xn + c] = tiCell(t, c, r, z);
    auto cell = [&](int c, int r, int z) { return snap[z * xn * yn + r * xn + c]; };
    auto interpAt = [oldN, at](int newIdx, const std::function<double(int)> &f) -> double {
        if (newIdx < at)  return f(newIdx);
        if (newIdx > at)  return f(newIdx - 1);
        if (at == 0)      return f(0);
        if (at >= oldN)   return f(oldN - 1);
        return (f(at - 1) + f(at)) * 0.5;
    };

    std::vector<double> av = tiBins(t, axis); av.resize(oldN);
    std::vector<double> nav(oldN + 1);
    for (int i = 0; i < oldN + 1; ++i) {
        if (i < at)       nav[i] = av[i];
        else if (i > at)  nav[i] = av[i - 1];
        else              nav[i] = tiInsertSeed(t, axis, at);   // ONE rule, shared with the editor's prompt
    }

    const int nxn = xn + (axis == 0 ? 1 : 0), nyn = yn + (axis == 1 ? 1 : 0),
              nzn = zn + (axis == 2 ? 1 : 0);
    writeAt(a.nBase, "U08", 1.0, oldN + 1);                    // new stride BEFORE writing cells
    for (int z = 0; z < nzn; ++z)
        for (int r = 0; r < nyn; ++r)
            for (int c = 0; c < nxn; ++c) {
                double v;
                if (axis == 0)      v = interpAt(c, [&](int s) { return cell(s, r, z); });
                else if (axis == 1) v = interpAt(r, [&](int s) { return cell(c, s, z); });
                else                v = interpAt(z, [&](int s) { return cell(c, r, s); });
                tiSetCell(t, c, r, z, v);
            }
    tiWriteBins(t, axis, nav);   // nav[at] came from tiInsertSeed, which already snapped it
    endEdit("Insert bin");
    configLoaded.emit();
    return true;
}

double Cache::tiInsertSeed(const TableImage &t, int axis, int at) const
{
    if (!t.valid || axis < 0 || axis >= int(t.axes.size()))
        return 0.0;
    const int n = tiLiveN(t, axis);
    std::vector<double> av = tiBins(t, axis);
    av.resize(n);
    double seed;
    if (av.empty())        seed = 0.0;
    else if (at <= 0)      seed = av.front() - (n > 1 ? av[1] - av[0] : 1.0);          // before the start
    else if (at >= n)      seed = av.back()  + (n > 1 ? av[n - 1] - av[n - 2] : 1.0);  // past the end
    else                   seed = (av[at - 1] + av[at]) * 0.5;                          // between two bins
    // A seed is a value like any other: it belongs on the channel's grid and inside its range, or
    // appending off the end of a pedal axis proposes 110 on a scale that stops at 100.
    return snapBin(t, axis, seed);
}

bool Cache::tiRemoveBin(const TableImage &t, int axis, int at)
{
    if (!t.valid || axis < 0 || axis >= int(t.axes.size())) return false;
    const TableImage::Axis &a = t.axes[axis];
    if (a.nBase < 0) return false;
    const int xn = tiLiveN(t, 0), yn = t.axes.size() > 1 ? tiLiveN(t, 1) : 1,
              zn = t.axes.size() > 2 ? tiLiveN(t, 2) : 1;
    const int oldN = (axis == 0) ? xn : (axis == 1) ? yn : zn;
    if (oldN <= std::max(1, a.nMin)) return false;             // schema floor: can't shrink below nMin
    at = std::clamp(at, 0, oldN - 1);
    beginEdit();

    std::vector<double> snap(xn * yn * zn);
    for (int z = 0; z < zn; ++z)
        for (int r = 0; r < yn; ++r)
            for (int c = 0; c < xn; ++c)
                snap[z * xn * yn + r * xn + c] = tiCell(t, c, r, z);
    auto cell = [&](int c, int r, int z) { return snap[z * xn * yn + r * xn + c]; };
    auto dropAt = [at](int newIdx, const std::function<double(int)> &f) {
        return f(newIdx < at ? newIdx : newIdx + 1);
    };

    std::vector<double> av = tiBins(t, axis); av.resize(oldN);
    std::vector<double> nav;
    for (int i = 0; i < oldN; ++i) if (i != at) nav.push_back(av[i]);

    const int nxn = xn - (axis == 0 ? 1 : 0), nyn = yn - (axis == 1 ? 1 : 0),
              nzn = zn - (axis == 2 ? 1 : 0);
    writeAt(a.nBase, "U08", 1.0, oldN - 1);
    for (int z = 0; z < nzn; ++z)
        for (int r = 0; r < nyn; ++r)
            for (int c = 0; c < nxn; ++c) {
                double v;
                if (axis == 0)      v = dropAt(c, [&](int s) { return cell(s, r, z); });
                else if (axis == 1) v = dropAt(r, [&](int s) { return cell(c, s, z); });
                else                v = dropAt(z, [&](int s) { return cell(c, r, s); });
                tiSetCell(t, c, r, z, v);
            }
    tiWriteBins(t, axis, nav);
    endEdit("Remove bin");
    tableEdited.emit();
    return true;
}

void Cache::copyTablePlane(const std::string &path, int fromZ, int toZ)
{
    if (!meta_ || !meta_->configTables().count(path))
        return;
    const int depth = liveDepth(path);
    if (fromZ < 0 || toZ < 0 || fromZ >= depth || toZ >= depth || fromZ == toZ)
        return;
    const int cols = liveCols(path), rows = liveRows(path);
    beginEdit();
    for (int r = 0; r < rows; ++r)                     // different planes, so read-then-write can't alias
        for (int c = 0; c < cols; ++c)
            setTableCell(path, c, r, tableCell(path, c, r, fromZ), toZ);
    endEdit("Copy plane");
    configLoaded.emit();
}

void Cache::interpolateTablePlanes(const std::string &path)
{
    if (!meta_ || !meta_->configTables().count(path))
        return;
    const int depth = liveDepth(path);
    if (depth < 3)
        return;                                        // nothing between the first and last plane
    const int cols = liveCols(path), rows = liveRows(path);
    beginEdit();
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const double a = tableCell(path, c, r, 0), b = tableCell(path, c, r, depth - 1);
            for (int z = 1; z < depth - 1; ++z)
                setTableCell(path, c, r, a + (b - a) * double(z) / (depth - 1), z);
        }
    endEdit("Interpolate planes");
    configLoaded.emit();
}

std::vector<double> Cache::axisValues(const std::string &arrayPath) const
{
    if (!meta_ || arrayPath.empty())
        return {};
    return meta_->arrayValues(arrayPath, configImage_);
}

void Cache::restoreTableFrom(const std::string &path, const std::vector<uint8_t> &source)
{
    if (!meta_ || !meta_->configTables().count(path))
        return;
    const MetaModel::ConfigTable &t = meta_->configTables().at(path);
    const int sz = t.colsMax * t.rowsMax * t.depthMax * t.cellSize;   // the whole allocation, every plane
    if (int(source.size()) < t.offset + sz || int(configImage_.size()) < t.offset + sz)
        return;

    // A TABLE IS ITS CELLS *AND* ITS AXES. This restored the cell region and nothing else, so the
    // breakpoints and the live bin counts — which live in their own regions of the image, nowhere near
    // the cells — kept whatever they had become. Restore after resizing or re-laying an axis and the
    // baseline's cells came back underneath TODAY's grid: right numbers, wrong stride, wrong
    // breakpoints. It reads as a corrupted restore, because that is exactly what it is.
    //
    // The element-table branch of restoreBindingFrom has always restored the lot ("cell grid + each
    // present axis (breakpoints + live-count + optional src/en)"). A config table gets the same set,
    // for the same reason: a restore that leaves half the table's meaning behind is not a restore.
    auto region = [&](int off, int len) {
        if (off < 0 || len <= 0) return;
        if (int(source.size()) < off + len || int(configImage_.size()) < off + len) return;
        std::copy(source.begin() + off, source.begin() + off + len, configImage_.begin() + off);
    };
    auto scalarRegion = [&](const std::string &p) {
        if (p.empty() || !meta_) return;
        const MetaModel::Location L = meta_->locate(p);
        if (L.kind == MetaModel::Location::Kind::Scalar)
            region(L.offset, MetaModel::dataSize(L.datatype));
    };

    beginEdit();
    std::copy(source.begin() + t.offset, source.begin() + t.offset + sz, configImage_.begin() + t.offset);
    for (const MetaModel::Axis &a : t.axes) {
        // The breakpoints, however the meta happens to carry them: a shared axis is a 1D array, while a
        // table's own axis is registered as a TABLE that is an axis (ve_table_x_axis carries axis/axis_of).
        // Asking arrays1d alone restored the count and left the breakpoints where they were — the half-
        // restored grid this whole change exists to stop.
        if (!a.array.empty()) {
            if (meta_->arrays1d().count(a.array)) {
                // count x element size, NOT `size`: the 1D-array parse fills `count` (the full
                // allocation) and leaves `size` at zero, so asking for size copied nothing at all and
                // the breakpoints stayed exactly where they were.
                const MetaModel::ConfigField &f = meta_->arrays1d().at(a.array);
                region(f.offset, f.count * MetaModel::dataSize(f.datatype));
            } else if (meta_->configTables().count(a.array)) {
                // A table's OWN axis is registered as a table that is an axis (it carries axis/axis_of),
                // so it is in configTables rather than arrays1d and neither arrays1d nor resolveBlob —
                // which answers for scalars and element fields — can see it. This is the case that
                // matters most: it is every named table's x/y/z.
                const MetaModel::ConfigTable &at = meta_->configTables().at(a.array);
                region(at.offset, at.colsMax * at.rowsMax * at.depthMax * at.cellSize);
            } else {
                int off = 0, len = 0;
                if (meta_->resolveBlob(a.array, off, len)) region(off, len);
            }
        }
        scalarRegion(a.nScalar);                           // how many of them are live
        scalarRegion(a.srcScalar);                         // …what channel indexes them
        scalarRegion(a.enScalar);                          // …and whether the axis is in play at all
    }
    configLoaded.emit();                                  // editors reload from the restored image
    endEdit("Restore table");   // endEdit() batches the write to the ECU
}

void Cache::restoreTableDefaults(const std::string &path)
{
    // Defaults and Connect-Point restore the SAME regions — only the source image differs. Going through
    // restoreBindingFrom() (not the config-table-only restoreTableFrom) is what makes Restore Defaults
    // actually work on an element table / curve: restoreTableFrom no-ops on those, so it silently did
    // nothing on an ETB feed-forward or a sensor calibration curve while Connect-Point restored them.
    restoreBindingFrom(path, meta_ ? meta_->defaultImage() : std::vector<uint8_t>());
}

void Cache::restoreFromBaseline(const std::string &path)
{
    restoreBindingFrom(path, baseline_);   // the connect-time image
}

void Cache::restoreBindingFrom(const std::string &path, const std::vector<uint8_t> &source)
{
    if (!meta_ || source.empty() || path.empty())
        return;
    // Full config table: restore the whole cell region from the source.
    if (meta_->configTables().count(path)) {
        restoreTableFrom(path, source);
        return;
    }
    // Element table (a curve): restore the element's local table region (axis + value cells + count).
    const MetaModel::ConfigArray *arr = nullptr;
    int elemIdx = 0;
    if (const MetaModel::ElemTable *et = meta_->elementTable(path, arr, elemIdx)) {
        // ONE table: restore the cell grid + each present axis (breakpoints + live-count + optional src/en),
        // whether it wears one axis (curve) or two/three (grid).
        const int base = arr->baseOffset + elemIdx * arr->stride;
        auto dsz = [](const std::string &dt){ return (dt == "U08" || dt == "S08") ? 1
                                                : (dt == "U32" || dt == "S32" || dt == "F32") ? 4 : 2; };
        const int ymax = et->hasY ? et->yAxis.max : 1;
        const int zmax = et->hasZ ? et->zAxis.max : 1;
        int lo = base + et->cellRel, hi = base + et->cellRel + et->xAxis.max * ymax * zmax * dsz(et->cellType);
        auto ext = [&](int rel, int n, int sz){ if (rel < 0) return; lo = std::min(lo, base + rel); hi = std::max(hi, base + rel + n * sz); };
        auto axisSpan = [&](const MetaModel::ElemAxis &ax){ ext(ax.rel, ax.max, dsz(ax.datatype)); ext(ax.nRel, 1, 1); ext(ax.srcRel, 1, 1); ext(ax.enRel, 1, 1); };
        axisSpan(et->xAxis);
        if (et->hasY) axisSpan(et->yAxis);
        if (et->hasZ) axisSpan(et->zAxis);
        const int sz = hi - lo;
        if (sz > 0 && lo >= 0 && int(source.size()) >= hi && int(configImage_.size()) >= hi) {
            beginEdit();
            std::copy(source.begin() + lo, source.begin() + lo + sz, configImage_.begin() + lo);
            configLoaded.emit();
            endEdit("Restore table");
        }
        return;
    }
    // Any other value: copy back the bytes it occupies.
    const MetaModel::Location L = meta_->locate(path);
    if (L.kind != MetaModel::Location::Kind::Scalar)
        return;
    const int offset = L.offset, size = MetaModel::dataSize(L.datatype);
    if (size <= 0 || offset < 0 || int(source.size()) < offset + size || int(configImage_.size()) < offset + size)
        return;
    beginEdit();
    std::copy(source.begin() + offset, source.begin() + offset + size, configImage_.begin() + offset);
    configValueChanged.emit(path);
    endEdit("Restore " + path);   // endEdit() batches the write to the ECU
}
