#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "TableImage.h"

#include <j/core/Signal.h>
#include <j/core/Timer.h>
#include <j/core/UndoStack.h>

#include "BitRange.h"

#include "MetaModel.h"
#include "Units.h"   // MetaModel::Location — the currency of every read and write

// Cache — the value store between ECU comms and dashboard widgets: comms -> Cache -> widgets.
// The link decodes nothing; it hands raw telemetry frames here, and the Cache decodes them via the
// MetaModel descriptor into engineering-unit values keyed by channel name. Widgets poll their own
// timers and read value()/generation() (compare-generation, cheap) — no per-widget pushes.
class Cache {
public:
    static Cache &instance();

    // A NEW META CHANGES WHAT RESOLVES. The evaluator remembers which resolver owns a bare token (and
    // that nobody does), which is a property of the loaded definition — so a different one invalidates
    // it, exactly as it invalidates the meta's own path memo.
    void setMeta(const MetaModel *m);
    const MetaModel *meta() const { return meta_; }

    // Decode a raw 'A' telemetry frame into named values; bumps generation + emits frameUpdated.
    void ingestTelemetry(const std::vector<uint8_t> &frame);

    double value(const std::string &name) const {
        auto it = values_.find(name);
        if (it != values_.end()) return it->second;   // live telemetry value
        // Config-bound controls (a checkbox/gauge/field/radio pointed at a config field or array element like
        // "electronic_throttle.etb[0].enabled") aren't in the telemetry map — read them from the config image
        // so they reflect their real value AND so a run-mode toggle sees the current state. configValue()
        // returns 0 for non-config paths, so telemetry misses stay 0 as before.
        return configValue(name);
    }
    bool has(const std::string &name) const { return values_.count(name) != 0; }
    std::string unit(const std::string &name) const;
    std::string label(const std::string &name) const;   // meta caption for a binding ("" if none/unknown)
    std::string help(const std::string &name) const;    // meta help/description for a binding (hover tooltip)
    uint64_t generation() const { return generation_; }

    // --- LINK LIVENESS ------------------------------------------------------------------------------
    // Is telemetry actually arriving? That is a fact about the LINK, which the studio owns for every ECU
    // — not something a definition should have to volunteer. (It used to be exactly that: the native
    // firmware publishes a "1kHz Frame Count" channel and the native dashboard bound a gauge to it, so
    // connecting to anything that did not publish such a channel left no way to tell a working ECU from a
    // dead one.) Every protocol's frames land in ingestTelemetry, so counting them here covers all of them.
    uint64_t  telemetryFrames() const { return telemFrames_; }
    long long msSinceTelemetry() const;                 // 1<<30 when nothing has ever arrived

    // --- cache blocks -----------------------------------------------------------------------------
    // Config bindings resolve to an absolute device offset (see MetaModel). Config is block 0 (base 0,
    // the flashed tune, held in configImage_). Non-tune segments (the RAM-backed learned region (persisted to SD totems)) get
    // their own buffer here, addressed by the SAME offset space and read/written by meta path exactly like
    // config — but excluded from save / flash / layout_hash. initSegments() (re)allocates them from the
    // meta; requestSegmentReads() pulls their bytes (delivered back through applyConfigRange). Connect flow.
    void initSegments();
    void requestSegmentReads();

    // --- config (the local image of g_config, read once + edited; writes go to the ECU) ---
    void setConfigImage(const std::vector<uint8_t> &image);     // full image just read from the ECU
    bool hasConfig() const { return !configImage_.empty(); }
    const std::vector<uint8_t> &configImage() const { return configImage_; }
    bool isConfig(const std::string &path) const;          // is this binding a known config scalar?
    // THE LINK, as the model sees it — set where it opens and closes. With it closed every reading is the
    // last one received, so nothing here may treat a stale RPM as a turning engine.
    void setLinkOpen(bool open) { linkOpen_ = open; }
    bool linkOpen() const { return linkOpen_; }
    // Connected, and the engine is turning (RPM, or the trigger holding sync). A setting that applies only
    // at an engine stop cannot be changed then: the edit would sit in RAM doing nothing until the stop.
    bool engineTurning() const;
    // …so is THIS binding locked right now: an engine_stop setting while engineTurning().
    bool lockedWhileRunning(const std::string &path) const;
    double configMin(const std::string &path) const;
    double configMax(const std::string &path) const;
    int    digits(const std::string &path) const;          // meta display decimals for a scalar (default 2)
    double configValue(const std::string &path) const;

    // What a CHANNEL actually reads: its value domain, the decimals it is read at, and its units.
    //
    // Not the same question as "what does its telemetry descriptor say". A channel published by a
    // GENERIC sensor input (aux_1, abs_mode, rotary_trim_n) has a descriptor sized to the UNION of every
    // type that input could be given — -720..3000 @ 0.01 — because at build time nobody knows what it
    // will be. Read as a range or a precision that describes no sensor: it let an axis bin on an app_1
    // axis be typed as 30.777 and, had aux_1 been set to pressure, would have clamped a kPa axis to
    // -720. So resolve the input's CONFIGURED type from the tune and ask the type. A catalogued sensor
    // (clt, tps) needs no such step — codegen already stamped its type onto the descriptor — and a
    // channel no sensor publishes (rpm, advance) is the descriptor's own to answer.
    //
    // `known()` is false when nothing can say: an unconfigured generic input, or an unknown channel.
    // Callers must then leave the value alone rather than substitute a default — inventing a precision
    // is how a bin ends up quantised to a grid its signal never lands on.
    struct ChannelDomain {
        bool        hasRange = false;
        double      lo = 0.0, hi = 0.0;
        int         digits = -1;          // -1 = the channel states no precision
        std::string units;
        bool known() const { return hasRange || digits >= 0; }
    };
    ChannelDomain channelDomain(const std::string &channel) const;
    // The sensor TYPE an array element is read as — its catalog type, or for a generic input the type
    // the TUNE has it set to. Null when the element has no type at all (an unconfigured generic input).
    const MetaModel::SensorType *sensorTypeOf(const std::string &arrayKey, int index) const;
    // What the RAW channel behind an element's physical input reads — resolved the way the firmware
    // resolves it (interface picks the pool, source picks the pin), so a calibration's axis can ask the
    // channel it actually traces instead of inferring one from the board.
    ChannelDomain rawInputDomain(const std::string &arrayKey, int index) const;
    // "sensors.sensor[clt].source" — an element's field path, by catalog id where the array has one.
    static std::string elementFieldPath(const MetaModel::ConfigArray &ca, const std::string &arrayKey,
                                        int index, const std::string &field);
    // What a datatype can hold, raw. (rawBounds answers this for a resolved Location; this is the same
    // question when all you have is the type.)
    static void datatypeRange(const std::string &datatype, double &lo, double &hi);
    // Which option an element's enum field is set to, BY ID ("analog_voltage"), or "" if unavailable.
    std::string elementOption(const std::string &arrayKey, int index, const std::string &field) const;
    // The sensor type behind a `type_scaled` element table (a calibration), or null. Cheap enough to ask
    // per cell: it rejects a non-element path on a character, then walks the element once.
    const MetaModel::SensorType *typeScaledCells(const std::string &path) const;
    // The same question for a type-scaled element FIELD (a sensor's Reading Low/High threshold): stored
    // at the scale of the sensor type in that slot, so its scale, units and precision are the type's.
    const MetaModel::SensorType *typeScaledField(const std::string &path) const;

    // What an AXIS may hold — the one answer, whatever the axis is. An axis with a channel takes the
    // channel's domain; a sensor calibration's raw axis takes what its INTERFACE reads (ADC counts, Hz,
    // µs); anything else takes its own declared bounds, or failing that what its storage can represent.
    // Precision comes from the same place, so an integer axis is never offered six decimal places.
    ChannelDomain axisDomain(const TableImage &t, int axis) const;

    // HOW AN AXIS IS SHOWN. The bytes are always the axis's own quantity — a calibration's raw input is
    // ADC counts and stays counts, because that is what the firmware reads — but counts are a poor thing
    // to calibrate against by eye: a sender is specified in volts. So an axis can be VIEWED in any unit
    // of its own quantity (counts / mV / V for a raw analog input), with entry converted back on the way
    // in and the domain converted out for display.
    //
    // Precision follows the unit rather than the storage: one count is 0.8 mV, so millivolts want a
    // decimal and volts want four to keep two adjacent counts distinct. Rounding stays on the STORAGE
    // grid — a typed 3.3001 V lands on the count nearest it, which is the only value the axis can hold.
    struct AxisView {
        std::string from, to;          // the axis's own unit id, and the one to show it in
        int         digits = 0;        // decimals the display unit needs to separate two storage steps
        bool        hasRange = false;
        double      lo = 0.0, hi = 0.0;   // the domain, converted for display
        double toDisplay(double v) const;
        double toStorage(double v) const;
        std::string label() const;     // "V" / "mV" / "counts" — what to print beside the number
        // A stored value as TEXT. In the axis's own unit that is fmtBreak's floor rule — a stored value
        // can be finer than the declared digits and must not be hidden (98.5 on a whole-number axis is
        // not 99). Converted, the opposite holds: the grid underneath is counts and `digits` was derived
        // to separate two of them, so print exactly that many and the row reads evenly. Otherwise one
        // count lands on 0.333 and the next on 0.3333, and the strip looks broken.
        std::string text(double stored) const;
    };
    // `pref` is the chosen unit id, or "" / "Auto" / "Raw" for the axis's own.
    AxisView axisView(const TableImage &t, int axis, const std::string &pref) const;

    // A legal breakpoint for this axis: quantised to what its channel is read at, then held inside what
    // that channel can read. THE rule, in one place, because a bin can be made three ways — typed into
    // the axis dialog, generated by Linearise, or born from Insert — and the first two having it while
    // Insert did not is how an app_1 axis ended up holding 120.0 (append extrapolates last + (last-prev)
    // and walked past the ceiling) and 10.05 (a midpoint minting a hundredth on a tenth channel).
    // A channel that states nothing leaves the value untouched.
    double snapBin(const TableImage &t, int axis, double v) const;

    // Lay an axis out from `start` to `end` in steps of `inc`: the bin COUNT follows from the three,
    // which is the part no per-bin edit can do. Resizing goes through tiInsertBin/tiRemoveBin so the
    // cells under the axis are resampled exactly as they are for a hand insert — writing the count
    // directly would leave the grid reinterpreting cells that never moved. Wrap in beginEdit/endEdit
    // for one undo step. Returns false if the axis cannot be resized or the arguments are nonsense.
    bool tiBuildAxis(const TableImage &t, int axis, double start, double end, double inc);
    // "...diag_enable.raw_min" -> the storage of the field it is packed into, plus its bit range.
    bool   resolveBitGroup(const std::string &path, int &offset, int &size,
                           std::string &datatype, BitRange &bits) const;    // decode a scalar from the image (RAW — widget cooks)
    void setConfigValue(const std::string &path, double rawValue);  // write RAW (widget un-cooked) + request an ECU write
    // A picker gated `by` another field stores an index into the list that field selects. When the gate
    // moves, the index addresses a DIFFERENT list and is stale even though it is still in range — so it
    // is restored to the field's schema default. Called by setConfigValue, the one funnel every config
    // write goes through, so it holds however the gate is changed.
    void invalidateGatedSiblings(const std::string &path, double before, double after);
    double configScale(const std::string &path) const;   // the field's meta scale (raw<->eng); the widget SEEDS its Scale from this
    // The field's raw storage type ("F32", "S16", …). A caller converting display units back to raw needs
    // it to know whether "already there" means a float epsilon or half an integer count.
    std::string configDatatype(const std::string &path) const;
    void setReadOnly(bool r) { readOnly_ = r; }                     // when set, block all config/ECU writes (studio's Editing/design mode)
    bool readOnly() const { return readOnly_; }                     // …so a control can grey itself instead of failing the click

    // Structural table setup (axis breakpoints / bin count / source-channel selection) is layout work, not
    // live tuning, so it's allowed even while the tune is locked. Scope a Cache::WriteGuard around such
    // edits to let their writes through the readOnly_ gate. Nestable (counter). RAII: guard restores on scope exit.
    struct WriteGuard {
        Cache &c;
        explicit WriteGuard(Cache &cc) : c(cc) { ++c.forceWrites_; }
        ~WriteGuard() { --c.forceWrites_; }
        WriteGuard(const WriteGuard &) = delete;
        WriteGuard &operator=(const WriteGuard &) = delete;
    };

    // DRAWING THE REAL PAGE AGAINST A DIFFERENT TUNE.
    //
    // The difference report shows one page twice, once per tune, by rendering the page's own controls —
    // which is the only way the report speaks the same language as the studio does. But a control reads
    // its value from THIS cache, by singleton, from over two hundred places; threading a second image
    // through all of them is not a change, it is a rewrite of every widget.
    //
    // So the image is swapped underneath them for the length of one render pass and swapped back.
    // blockBytes(), arrayValues(), the string fields — every read path resolves against the borrowed
    // image without knowing there is one, and the two passes differ only in which vector was in place.
    //
    // WRITES ARE REFUSED WHILE IT IS OPEN. A write here would land in a borrowed buffer and then be
    // swapped away: an edit that silently did nothing, on a page that looks live. Nothing on a report
    // page is editable, and this is what MAKES that true rather than assumes it — readOnly_ is forced
    // and any WriteGuard in flight is suspended, both restored on the way out.
    //
    // The vector is borrowed, not copied: it must outlive the scope. No signals are emitted and the
    // generation does not move, because nothing about the studio's own state has changed.
    struct ImageScope {
        Cache &c;
        std::vector<uint8_t> &other;
        const bool wasReadOnly;
        const int  wasForce;
        ImageScope(Cache &cc, std::vector<uint8_t> &img)
            : c(cc), other(img), wasReadOnly(cc.readOnly_), wasForce(cc.forceWrites_) {
            std::swap(c.configImage_, other);
            c.readOnly_ = true;
            c.forceWrites_ = 0;
        }
        ~ImageScope() {
            std::swap(c.configImage_, other);
            c.readOnly_ = wasReadOnly;
            c.forceWrites_ = wasForce;
        }
        ImageScope(const ImageScope &) = delete;
        ImageScope &operator=(const ImageScope &) = delete;
    };

    // THE SAME, FOR A WHOLE OTHER FIRMWARE: its definition AND an image in its layout, for one render
    // pass. The firmware-update report draws the tuner's current pages against the firmware the ECU has
    // now, and the new firmware's pages against the one it is getting — two layouts, so swapping the image
    // alone would read the new firmware's bytes at the old firmware's offsets. Both are borrowed and
    // restored; writes are refused while it is open, exactly as ImageScope does. Expression token owners
    // belong to a definition, so they are dropped on the way in and again on the way out (as setMeta does).
    struct DefinitionScope {
        Cache &c;
        const MetaModel *wasMeta;
        ImageScope image;
        DefinitionScope(Cache &cc, const MetaModel &m, std::vector<uint8_t> &img);
        ~DefinitionScope();
        DefinitionScope(const DefinitionScope &) = delete;
        DefinitionScope &operator=(const DefinitionScope &) = delete;
    };

    // ASCII string config fields (e.g. "lua.source", the embedded Lua script). Read returns the
    // NUL-terminated text from the image; write splices the text + a NUL into the field (truncated
    // to fit), padding the tail with NULs, then batches the ECU write like any other edit.
    std::string configString(const std::string &path) const;
    void    setConfigString(const std::string &path, const std::string &text);

    // WHICH FIELD HOLDS THE LUA SCRIPT, asked of the connected definition rather than assumed.
    //
    // Both firmwares have one and they are not called the same thing: jayecu's is `lua.source` (~4 KB),
    // rusEFI's is `ts.luaScript` (48 KB), and both arrive in the meta as ordinary ASCII fields that
    // configString/setConfigString already handle. The editor was hardcoded to the jayecu name, so on
    // a rusEFI ECU it showed an empty box and its Apply button wrote to a path that does not resolve —
    // silently, because setConfigString returns early on one.
    //
    // First match wins. Every firmware the studio talks to has a script field, so there is no
    // "no script" state to design for — a definition carrying neither is a broken definition, and it
    // shows up as an empty editor the moment you look. A third firmware is one more entry and nothing
    // else.
    std::string scriptPath() const;
    // How many characters that field holds, from the definition (0 when there is no script field).
    size_t      scriptCapacity() const;

    // ---- table selectors -------------------------------------------------------------------------
    // The option list for a `table_ref` field (a scalar whose number is a table id), index-aligned with
    // the id. The meta ships the schema's LABELS; a generic table also carries a name the user typed,
    // and that name is the whole point of the pool — "Generic Table 3" says nothing, "Generic Table 3
    // — Fan Ramp" says which one to pick. Empty when the path is not a table selector.
    std::vector<std::string> tableOptionLabels(const std::string &path) const;

    // ---- blob fields (expression bytecode) -------------------------------------------------------
    // An expression field holds a compiled PROGRAM. It is read and written as bytes: decoding it as a
    // number would read byte 0 of the bytecode, which is an opcode, not a value. Works for a config
    // scalar and for an array element (the sensor precondition is the latter).
    std::vector<uint8_t> configBlob(const std::string &path) const;
    void    setConfigBlob(const std::string &path, const std::vector<uint8_t> &bytes);

    // One cell of an already-resolved table, by caller-computed offset. The widget layer owns
    // display->storage mapping (transpose, planes, slice views), so it supplies the offset — but the
    // bounds come from the descriptor, so a cell can never be addressed without its limits.
    MetaModel::Location cellLocAt(const TableImage &t, int offset) const;

    // ---- THE two accessors -------------------------------------------------------------------------
    // A Location (MetaModel::locate) says where a value lives and what it may hold; these are the only
    // things that read or write one. Both speak RAW — the unit the bytes are in and the unit the
    // schema's min/max/default are authored in — so nothing in here multiplies by scale. Engineering and
    // display units are the widget layer's business (dispV/srcV), converted once at the boundary where a
    // human is involved, rather than at each of a dozen call sites that each had to remember.
    // RAW, and the type says so: these are the storage doors. Anything that has already been
    // multiplied by a field scale is an Eng and will not fit through them. See model/Units.h.
    Raw    readRaw(const MetaModel::Location &L) const;
    // `notifyPath`, when given, is the path configValueChanged is emitted for. A write that nothing is
    // told about is a write dependent controls do not see — a condition on the field, a row count, a
    // sibling whose Max depends on it. The path is not derivable from a Location (several paths can name
    // the same bytes), so the caller that has one says so, and there is still exactly one write.
    void   writeRaw(const MetaModel::Location &L, Raw raw, const std::string &notifyPath = {});

    // What a location may hold, in raw units: its DECLARED bounds, or — when it declares none — the
    // range its datatype can actually store.
    //
    // "Undeclared" used to mean "unenforced", encoded as min == max, and 196 config fields were in that
    // state. It now means the type's own range, so every field is bounded and clamping IS saturation:
    // encodeRaw casts without saturating, so an out-of-range value used to WRAP (999999 into an S16
    // landing at -13617, silently writing a value nobody asked for). There is no unbounded case left to
    // forget, and no second opinion in a different unit to disagree with.
    static void rawBounds(const MetaModel::Location &L, double &lo, double &hi);


    // How many values the CURRENT (or most recently closed) edit transaction clamped, and how many it
    // wrote. A bulk op reads these after endEdit() to report "pasted 256 cells, 12 clamped to +-60" —
    // silent clamping during a paste would otherwise hide a mismatch between what you pasted and what
    // landed. Reset at the start of each outermost transaction.
    int clampedCount() const { return clamped_; }
    int writtenCount() const { return written_; }

    // Generic byte access at an absolute config-image offset, in engineering units (= raw*scale).
    // Used by the element-table cell editor, which computes offsets from the table descriptor.
    // bitLo/bitHi select a bit RANGE within the word at `offset` (-1 = the whole word). A packed read
    // extracts and shifts; a packed write is read-modify-write, so the bits either side are preserved.
    double readAt(int offset, const std::string &datatype, double scale,
                  BitRange bits = {}) const;
    void   writeAt(int offset, const std::string &datatype, double scale, double engValue,
                   BitRange bits = {});

    // Config tables (the z=0 plane). Cells are row-major at live strides: index = row*cols + col.
    bool isTable(const std::string &path) const;
    // The most appropriate dashboard control for a binding: "table" / "enum" / "toggle" / "config"
    // (input box) / "value" (telemetry readout). Drives drag-drop widget creation.
    std::string controlFor(const std::string &path) const;
    // The REGISTERED WIDGET TYPE for a binding — controlFor's classification mapped to a name the widget
    // registry actually knows. The two are not the same vocabulary ("config" is a classification; the widget
    // is "configedit"), and using a classification as a type yields a null instance that renders NOTHING.
    // One rule, shared by a dictionary drop and by an import, so neither can drift from the registry.
    std::string widgetTypeFor(const std::string &path) const;
    int liveCols(const std::string &path) const;          // live x bins (>=1; collapses to 1 when X is off)
    int liveRows(const std::string &path) const;          // live y bins (>=1; collapses to 1 when Y is off)
    int liveDepth(const std::string &path) const;         // live z planes (>=1; 1 for a plain 2D table)
    // Cell read/write at display plane z (0 = the only plane of a 2D table). Storage is row-major per
    // plane at the live stride: index = z*rows*cols + row*cols + col.
    double tableCell(const std::string &path, int col, int row, int z = 0) const;
    void setTableCell(const std::string &path, int col, int row, double engValue, int z = 0);
    std::vector<double> axisValues(const std::string &arrayPath) const;   // breakpoint values for headers
    void restoreTableDefaults(const std::string &path);   // reset a table (config OR curve) to the default tune

    // Connect-time baseline (the restore point): the ECU's config image as it was when connected.
    // Restore copies regions from it back into the working tune (and pushes them to the ECU).
    void setBaseline(const std::vector<uint8_t> &image) { baseline_ = image; }
    const std::vector<uint8_t> &baseline() const { return baseline_; }
    bool hasBaseline() const { return !baseline_.empty(); }
    // Restore ONE binding's region(s) from the baseline: a table (cells+axes+counts), an element table
    // (a curve), or a scalar/array field. No-op if there's no baseline or the path is unknown.
    void restoreFromBaseline(const std::string &path);
    // The shared engine behind both restores — copy a binding's region(s) from `source` (defaults or the
    // connect-time baseline) into the working tune. No-op if `source` is empty or the path is unknown.
    void restoreBindingFrom(const std::string &path, const std::vector<uint8_t> &source);

    // --- Undo/redo for tune edits -------------------------------------------------------------------
    // The stack of config-image edits (Edit ▸ Undo/Redo in run mode). Every mutation is captured as a
    // byte-range diff; undo/redo replays it into the cache AND pushes the reverted bytes to the ECU.
    jf::JUndoStack *undoStack() const { return undoStack_; }
    // Group a compound edit (a fill, interpolate, insert-bin, …) into ONE undo step. Nestable; the leaf
    // mutations auto-wrap, so a single edit is atomic and a wrapped batch coalesces into one command.
    void beginEdit();
    void endEdit(const std::string &text);
    // Apply a byte region to the image + push it to the ECU (used by undo/redo). Public for the command.
    void applyUndoBytes(int offset, const std::vector<uint8_t> &bytes);

    // --- Scoped edit history (the < > arrows on an imported dialog) ---------------------------------
    // A dialog's arrows step through THAT DIALOG's changes. A single global stack would have them undo
    // whatever the user last touched anywhere — a table cell on another page, a field they can't see —
    // which is not undo, it is a surprise. So every edit is also filed under the page it was made on
    // (setEditScope, called when the navigation switches pages) and the arrows walk that file.
    //
    // Independent of the global Ctrl+Z stack by design: replaying a scoped step is a byte apply, so the
    // two histories can disagree about ORDER without either producing wrong bytes.
    void setEditScope(const std::string &scope) { editScope_ = scope; }
    const std::string &editScope() const { return editScope_; }
    bool canUndoScope(const std::string &scope) const;
    bool canRedoScope(const std::string &scope) const;
    void undoScope(const std::string &scope);
    void redoScope(const std::string &scope);
    // Resize a table's live grid: re-strides the cells and rewrites the axis breakpoints (hold-edge),
    // then updates the _n bin-count scalars. Keeps the data coherent at the new stride.
    void writeAxisValues(const std::string &arrayPath, int count, const std::vector<double> &vals);
    // Copy every cell of one Z plane onto another (3D tables) — seed a new flex plane from an existing
    // one, then tweak. No-op for equal/out-of-range planes. Wrapped as one undo step.
    void copyTablePlane(const std::string &path, int fromZ, int toZ);
    // Fill the intermediate Z planes by a per-cell linear blend between the first and last plane (e.g.
    // set E0 + E100, interpolate the rest). No-op for tables with fewer than 3 planes. One undo step.
    void interpolateTablePlanes(const std::string &path);

    // Insert / remove a single breakpoint (bin) on an axis at storage index `at` (axis 0 = x/cols,
    // axis 1 = y/rows), interpolating the new breakpoint + cells (like the curve editor's point insert).
    // Respects the axis nMax (insert) and a floor of 2 bins (remove). Returns false if it can't.
    bool insertTableBin(const std::string &path, int axis, int at);
    bool removeTableBin(const std::string &path, int axis, int at);

    // --- unified table access: TableImage = the studio mirror of the firmware's TableDesc -------------
    // resolveTable() turns a table path (module OR per-element) into one offset-keyed descriptor; every
    // ti* op then runs on that one struct, so a module table and etb[i].ff_table go through identical
    // code (cf. firmware desc(cfg) vs desc(cfg,i) feeding one tbl::interp). Cells use the LIVE stride.
    TableImage resolveTable(const std::string &path) const;
    // Solve a bound table to its live value at the current operating point — a faithful port of the firmware's
    // tbl::table_eval + interp (the source of truth). Feeds a table widget's "Current Value" and any @table ref.
    // ENGINEERING, and the type says so. It interpolates cooked cells against breakpoints in
    // engineering units because that is what the firmware does; configValue() has to uncook() the
    // answer to keep its own RAW contract, and now it cannot forget to. See model/Units.h.
    Eng solveTable(const std::string &path) const;
    int  tiLiveN(const TableImage &t, int axis) const;            // LIVE bins on an axis (the search bound; always >= 1)
    int  tiAllocN(const TableImage &t, int axis) const;           // PHYSICAL bins — the stride cells are laid out at
    std::vector<double> tiBins(const TableImage &t, int axis) const;  // the breakpoint values
    void tiWriteBins(const TableImage &t, int axis, const std::vector<double> &vals);
    int  tiSrc(const TableImage &t, int axis) const;              // channel SignalId, or -1 if no source
    // WHICH LIVE CHANNEL AN AXIS TRACES, and the one rule for asking. A native table names it by
    // SignalId through the in-image selector; an IMPORTED one has no selector and no signal table at
    // all — the ini states the channel by name (veTable's axes are RPMValue / veTableYAxis) and that
    // lands in the axis's defaultSig. Asking only for the id leaves every imported table traceless.
    // Empty when the axis names nothing.
    std::string axisChannel(const TableImage &t, int axis) const;
    void tiSetSrc(const TableImage &t, int axis, int sigId);
    bool tiEnabled(const TableImage &t, int axis) const;          // optional-axis enable (true if no toggle)
    void tiSetEnabled(const TableImage &t, int axis, bool on);
    // THE cell address rule, for every reader and writer: cells are laid out at the ALLOCATION's stride
    // (tiAllocN), so <axis>_n bounds the search and never the layout. Anything computing this itself is a
    // second copy of a layout rule, which is exactly how a table drew differently from how it read.
    int  tiCellOffset(const TableImage &t, int c, int r, int z) const;
    double tiCell(const TableImage &t, int c, int r, int z) const;
    void tiSetCell(const TableImage &t, int c, int r, int z, double engVal);
    // The value tiInsertBin WOULD give a bin inserted at `at`: the midpoint of its neighbours, or a
    // continuation of the end step when appending. Exposed so a caller can OFFER it before committing
    // to the insert — the editor asks what the new bin should read, and asking after inserting means
    // the table has already changed and Cancel leaves a bin behind.
    double tiInsertSeed(const TableImage &t, int axis, int at) const;
    bool tiInsertBin(const TableImage &t, int axis, int at);      // one reflow, both table flavours
    bool tiRemoveBin(const TableImage &t, int axis, int at);
    void announceReload();                                        // emit configLoaded() (refresh views)

    // --- command-state refresh (mirror FIRMWARE-authored config writes) -------------------------------
    // A bench routine (findlimits/fillff/autotune/pedalcal) writes cal on the ECU and reports its progress
    // in the `command_state` telemetry channel: RUNNING for the whole routine, then OK/FAIL. On the OK edge
    // we re-read the whole config as one range through the splice path (applyConfigRange, dirty-wins) so the
    // editor matches the ECU — no region prediction, no completion-window guess. See ingestTelemetry.
    void applyConfigRange(int offset, const std::vector<uint8_t> &bytes);   // splice a re-read range into the cache

    // Writes are debounced+coalesced: an edit marks its byte range dirty and the dirty set is flushed to
    // the ECU shortly after edits settle (or sooner under a sustained burst). flushWrites() forces an
    // immediate flush — call it before disconnect / quit / burn so nothing pending is lost.
    void flushWrites();
    bool hasPendingWrites() const { return !dirty_.empty(); }   // edits queued but not yet flushed

    // Send a CLI command to the ECU (e.g. a command button). Routed to the link like config writes.
    void sendCli(const std::string &cmd) { if (!cmd.empty()) cliRequested.emit(cmd); }

    // Signals.
    jf::JSignal<> frameUpdated;
    jf::JSignal<> configLoaded;
    jf::JSignal<> tableEdited;    // a value/axis edit (NOT a structural reload): views refresh IN PLACE, no rebuild
    jf::JSignal<std::string> configValueChanged;
    // A NAMED EDIT that changed a value: (path, raw before, raw after). Emitted inside the edit's own
    // transaction, so whatever a listener writes in answer is part of the same undo step — the coil and
    // injector rows a cylinder-count edit lays out (EngineOutputLayout) are undone with the edit.
    jf::JSignal<std::string, double, double> configEdited;
    // A config edit to push to the ECU (EcuLink turns this into a 'w'): absolute offset + bytes.
    jf::JSignal<int, std::vector<uint8_t>> writeRequested;
    jf::JSignal<std::string> cliRequested;   // a CLI command to run on the ECU (EcuLink sends it via 'E')
    jf::JSignal<int, int> rangeReadRequested;   // re-read this config byte range (EcuLink)
    jf::JSignal<int, bool> commandFinished;     // a bench routine ended: (cmdstate op, ok) — drives a status toast

private:
    bool linkOpen_ = false;
    Cache();
    // Restore a table's cell region (z=0 plane) from an arbitrary source image (default or baseline).
    void restoreTableFrom(const std::string &path, const std::vector<uint8_t> &source);
    void markDirty(int offset, int size);   // queue a byte range + (re)arm the flush timers
    void rereadForOp(int op);               // command_state OK: re-read just the config region that op wrote

    int lastCommandState_ = -1;             // last-seen command_state telemetry (edge-detect RUNNING -> OK/FAIL)

    // THE writer. Every path that mutates the config image funnels through here — setConfigValue,
    // setTableCell, writeAt and the ti* family alike — so the read-only gate and the encode exist once.
    // Before this, three separate copies of "raw = eng/scale; encodeRaw(...)" existed and only one of
    // them honoured readOnly_, so a config scalar could be written while the tune was locked.
    // Does NOT begin/end an edit transaction: callers own that (they batch many cells into one undo step).
    void encodeAt_(int offset, const std::string &datatype, double scale, double engValue,
                   BitRange bits = {});
    bool be_() const;   // the loaded definition's declared byte order (imported inis may be big-endian)
    bool isHostOffset_(int offset) const;   // in a host (PcVariable) block — never written to the ECU

    int clamped_ = 0, written_ = 0;   // per-transaction write/clamp tally (see clampedCount)

    std::vector<std::pair<int, int>> dirty_;  // pending [start,end) write ranges (merged at flush)
    jf::JTimer flushTimer_;          // debounce: fires ~25 ms after the last edit
    jf::JTimer flushCapTimer_;       // cap: fires ~150 ms after the first pending edit (sustained)
    const MetaModel *meta_ = nullptr;
    bool             readOnly_ = false;        // Locked (view-only) mode — blocks writeAt
    int              forceWrites_ = 0;         // >0: a WriteGuard permits structural writes through readOnly_
    std::vector<uint8_t> baseline_;            // connect-time restore point
    // Per-page edit history: the steps made on a page, and how far back the arrows have walked.
    struct ScopedStep { int offset; std::vector<uint8_t> before, after; };
    struct ScopedHistory { std::vector<ScopedStep> steps; size_t cursor = 0; };
    std::map<std::string, ScopedHistory> scoped_;
    std::string editScope_;
    bool replaying_ = false;   // inside a scoped undo/redo: do not re-record it as a new step

    jf::JUndoStack *undoStack_ = nullptr;
    std::vector<uint8_t> editSnapshot_;        // image at the start of the current edit transaction
    int editDepth_ = 0;              // transaction nesting depth (0 = not editing)
    std::unordered_map<std::string, double> values_;
    uint64_t generation_ = 0;
    uint64_t telemFrames_ = 0;                          // telemetry frames ingested this session
    std::chrono::steady_clock::time_point telemAt_{};   // …and when the last one landed
    std::vector<uint8_t> configImage_;               // cache block 0: the flashed tune (base 0)

    // Non-tune cache blocks (nvram/volatile, e.g. the learned region), addressed by absolute offset.
    // `host` marks a block the ECU knows nothing about: a PcVariable segment. It is addressed by meta path
    // exactly like config — same resolve, same read/write, same widgets — but it is never read FROM the
    // ECU and never flushed TO it, because there is no storage over there to sync with. It is still part of
    // the tune, since its contents are values (meta declares the paths, the tune carries the values).
    struct Segment { uint32_t base = 0; bool host = false; std::vector<uint8_t> bytes; };
    std::vector<Segment> segments_;
    std::vector<std::vector<uint8_t>> segSnapshot_;  // per-segment snapshot at beginEdit (parallel to segments_)
    // Map an absolute offset to the storage of the block that fully contains [offset, offset+size): the
    // config image (base 0) or a segment. nullptr if no single block covers it (a bad/unmapped offset —
    // callers then no-op instead of indexing out of range). One seam makes every byte access block-aware.
    uint8_t       *blockBytes(int offset, int size);
    const uint8_t *blockBytes(int offset, int size) const;
};
