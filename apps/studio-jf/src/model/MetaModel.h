#pragma once

#include <cstdint>
#include <set>
#include <map>
#include "BitRange.h"

#include <string>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include <j/config/Json.h>   // jf::JJson — the meta document is a JSON tree

#include "TableImage.h"   // the one resolved table descriptor every consumer uses

// MetaModel — the studio's view of a generated tuneit-meta.json: the device identity, the wire
// protocol (command codes + framing), and the telemetry descriptor used to decode 'A' frames.
// This is the contract the comms/cache layer is driven by.
class MetaModel {
public:
    // One named BIT GROUP of a packed field: "sensors.sensor[clt].diag_enable.raw_min" (1 bit, a flag)
    // or ".diag_severity.raw_min" (2 bits, a named level). The parent field is the storage byte; a group
    // is what the user actually edits, so each is addressable and carries its own kind/label/options.
    struct BitGroup {
        std::string name, label, help, kind;      // kind: "bool" (1 bit) or "enum" (wider)
        std::vector<std::string> options;
        int lo = 0, hi = 0;
        int width() const { return hi - lo + 1; }
    };

    struct TelemField {
        int         offset = 0;
        int         size = 0;
        std::string datatype;     // U08/S08/U16/S16/U32/S32/F32
        double      scale = 1.0;
        std::string units;
        std::string module;       // grouping (e.g. "Sensors")
        std::string label;
        std::string enumId;       // enum value-set id (when the channel is enum-valued), else ""
        // WORTH RECORDING, as the signal catalog declares it (`datalog:` in the schema). It is what a
        // recorder offers as "the standard set" — a choice the DEFINITION made, rather than a list the
        // studio would have to invent and then keep in step with the firmware by hand.
        bool        datalog = false;
        // HOW OFTEN THE CHANNEL CHANGES, in Hz — a FLOOR, not a promise. A sensor channel carries
        // its type's cadence (coolant 5, lambda 20, pressure 100, throttle 200) and a module channel
        // carries the control frame's 1000. A live consumer can claim a sensor faster (transient
        // enrichment holds throttle at 1 kHz), so the real rate is this or better, never worse.
        int         updateHz = 0;
        double      minV = 0.0;   // signal range — editor metadata ONLY (never clamps live values; it bounds
        double      maxV = 0.0;   // axis breakpoints that read this signal). min==max => unset.
        // Decimals this channel is read at. The meta derives it from the wire scale (digits ==
        // -log10(scale) for every signal), so it is the finest step the channel can express. -1 =
        // unstated. For a GENERIC sensor's channel this is the union wire's precision and describes no
        // real sensor — Cache::channelDomain() resolves the configured type instead.
        int         digits = -1;
        BitRange    bits;         // packed channel (a TS `bits` field in [OutputChannels])
        // Value names for an enum-valued channel, indexed by the value. A TS `bits` channel carries them
        // inline ("None", "TPS error", …) and they are what bitStringValue() looks up: a definition can
        // build a label out of one, so dropping them left nothing to build it from.
        std::vector<std::string> options;
        // Which SignalBus cell this channel occupies in the ECU — NOT the wire type above. A channel is
        // commonly a float on the bus and a scaled int on the wire; the pair is independent, and only this
        // one says whether the value is a real quantity or a mask/counter.
        bool        busFloat = true;
    };

    struct ConfigField {       // a config scalar, keyed by "module.field"
        int         offset = 0;
        int         size = 0;
        std::string datatype;
        double      scale = 1.0;
        double      minV = 0.0;
        double      maxV = 0.0;
        std::string units;
        std::string label;
        std::string help;          // hover tooltip (the field's description)
        int         count = 0;     // 1D array element count (live), else 0 for scalars
        int         digits = -1;   // display decimal places (meta "digits"); -1 = unspecified
        // HOW MANY CHARACTERS A TEXT FIELD ACTUALLY HOLDS, when the definition says so; 0 = unstated.
        // Not the same as `size`, and the difference is the whole question for a VIN: rusEFI declares
        //     vinNumber = string, ASCII, 3332, 17, 17
        // — seventeen bytes AND seventeen usable characters, because a VIN is seventeen characters and
        // there is no room left over for a terminator. A field that states no maximum keeps a byte back
        // for one, which is the safe reading when nothing says otherwise.
        int         maxChars = 0;
        // HOW MANY CHARACTERS OF THIS FIELD ARE USABLE — the one place that rule lives.
        //
        // It was written out three times (the editor's write, the tune file's load, and the check that
        // decides whether a value changed) and each said `size - 1`. That cost the VIN its seventeenth
        // character every time: the ECU held seventeen, the tune file stored seventeen, and loading it
        // put back sixteen — so "keep the ECU's tune" appeared not to stick and the studio asked the
        // same question on every connect, forever.
        int capacity() const {
            if (maxChars > 0) return std::min(maxChars, size);
            return size > 0 ? size - 1 : 0;      // no stated capacity: keep a byte for the terminator
        }
        std::string kind;          // "enum" / "bool" / "string" / "" — which control fits this field
        // A fixed-length ASCII blob (a VIN, an engine name, a script's label): its bytes are TEXT, not a
        // number. ONE predicate, so the dictionary, the tune writer and the tune reader cannot disagree
        // about what text is — they each used to re-test kind themselves.
        bool isText() const { return kind == "string"; }
        std::vector<std::string> options;   // enum option labels (kind == "enum")
        // Packed field: flags are stored several to a word (a TS `bits, U32, 1356, [0:0]`), so without the
        // range they all decode to the same number and writing one CLOBBERS its neighbours.
        BitRange    bits;
        std::vector<BitGroup> bitGroups;     // named groups packed into this scalar (schema `bits:`)
    };

    struct PickerSet {         // one interface-gated option list (board pins) for a `pickers` field
        std::vector<int>         ifaces;    // sibling-field values this set applies to
        std::vector<std::string> options;   // option labels
        std::vector<int>         values;    // the stored value per option (firmware pool index; not the position)
        // Does the option name real HARDWARE? A picker may offer a sentinel ("None" = 255, the value a
        // channel field holds when nothing is assigned), and a field resting on one holds no pin — so it
        // claims none. Without this every unassigned cylinder claimed "None" and the app greyed it out for
        // all the others, which reads as the option being taken.
        std::vector<uint8_t>     pins;      // parallel to options; 1 = a board pin, 0 = a sentinel
        // PERMANENTLY SPOKEN FOR. The board declares some pins `assignable: false` — the battery sense
        // on AV12, the knock inputs — because the HAL consumes them. They used to be blanked out of the
        // option list entirely, which hides them from every dropdown (right) and leaves nothing able to
        // NAME them (wrong): the battery's wiring row read "(none)" for a pin it can never leave. They
        // are options now, carrying their name, and this says never to offer one.
        std::vector<uint8_t>     fixed;     // parallel to options; 1 = real hardware, not assignable
        // May two elements of the array name the same pin through this field? For a cylinder's coil or
        // injector, yes, and it is the wiring: a distributor points every cylinder at one coil, wasted
        // spark points each companion pair at one, batch injection fires a bank off one driver. Two
        // SENSORS on one pin is still a clash, which is why this is declared and not assumed.
        bool                     shared = false;
    };

    struct ArrayField {        // one field of a struct-array element (e.g. a sensor's "source")
        std::string name;
        int         relOffset = 0;
        int         size = 0;      // bytes; only meaningful for blob fields (EXPR bytecode, ASCII text)
        std::string datatype;
        double      scale = 1.0;
        int         digits = -1;   // display precision (schema "digits"); -1 = unspecified
        std::string units, label, help;
        std::string subcat;        // dictionary-tree sub-category — fields sharing one nest under a node
        // WHICH value-set this enum is, by id ("sensor_type", "sensor_interface"). Needed because an
        // option ID is only unique WITHIN its set: `switch` is both a sensor TYPE and an INTERFACE,
        // and a rule about one silently applied to the other.
        std::string enumId;
        double      minV = 0.0, maxV = 0.0;
        // The field's own default(s), from the schema. Per ELEMENT when the schema gives each one its
        // own (a sensor's `source`: battery defaults to AV12, everything else to -1 = unassigned), else
        // a single value for the whole array. Empty = the meta stated none. This is what a stale value
        // is restored TO, so nothing has to hardcode a sentinel — see Cache::setConfigValue.
        std::vector<double> defaults;
        bool        hasDefault = false;
        std::string kind;          // "enum" / "" — control hint
        bool        typeScaled = false;   // stored at the scale of the sensor TYPE in this slot (see Cache)
        std::vector<std::string> options;   // enum option labels (index = value), when kind == "enum"
        // The same options BY ID ("analog_voltage"), index-aligned. Anything that RECOGNISES an option
        // matches on these: a label is for reading, and matching one makes a rename in the definition a
        // silent behaviour change.
        std::vector<std::string> optionIds;
        std::string pickerBy;      // sibling field whose value selects the option set (e.g. "interface")
        std::vector<PickerSet> pickerSets;   // interface-gated board-pin lists (sensor source); empty if none
        std::vector<BitGroup> bits;          // named bit groups packed into this field
    };

    struct ElemAxis {          // one axis of an element table (rel offsets within the element)
        int rel = 0, nRel = 0, srcRel = -1, enRel = -1, max = 0, min = 2;   // breakpoints / live-_n / src / en / size
        std::string datatype, units, label;
        // …and per-element raw units where the elements do not share one. A sensor cal's axis is ADC
        // counts for an analog input and HERTZ for a frequency one: the same struct, two meanings,
        // and an axis labelled "ADC" over breakpoints in hertz tells the reader the wrong thing about
        // the only number they are entering. Empty ⇒ every element uses `units` above.
        std::vector<std::string> elemUnits;
        double scale = 1.0;              // breakpoint display scale
        double vmin = 0.0, vmax = 0.0;   // breakpoint VALUE bounds (e.g. a % axis: 0..100)
        bool   hasBounds = false;        // false = unconstrained
    };
    // THE table: a typed cell grid addressed by 0..3 typed axes. A "curve" (display) is this wearing ONE axis;
    // a grid wears 2-3; a single cell wears none. `display` chooses the editor VIEW, not the type — there is
    // exactly one table type and one resolver. The two meta encodings (axis/value/points vs cell/axes) are
    // normalised into this ONE form at parse; nothing downstream sees a second kind.
    struct ElemTable {
        std::string name, display, label;
        int       cellRel = 0;  std::string cellType;  double cellScale = 1.0;   // the cell grid
        double    minV = 0.0, maxV = 0.0;   // schema cell value bounds (min==max ⇒ unset)
        ElemAxis  xAxis, yAxis, zAxis;
        bool      hasY = false, hasZ = false;
        // Type-scaled cells: the cell scale/units come from these per-array-element vectors (indexed by the
        // element's slot), driven by the sensor type occupying that slot; empty ⇒ use the single cellScale.
        bool                     typeScaled = false;
        std::vector<double>      cellElemScales;
        std::vector<std::string> cellElemUnits;
    };

    struct ElemSubField {      // one field of an element's nested sub-struct array (precond/cand)
        // `kind` is the control hint the meta carries for every other field — "signal", "enum", "bool".
        // It was not read here, so a nested sub-field could only be recognised as a picker by INFERENCE
        // from options_from, and an output candidate's `sig` (a plain signal selector) inferred nothing:
        // the page drew a dropdown with no options in it.
        std::string name, datatype, label, units, help, optionsFrom, kind;
        std::vector<std::string> options;
        int     relOffset = 0, size = 0;
        double  scale = 1.0, minV = 0.0, maxV = 0.0;
    };

    struct ElemArray {         // a fixed repeated sub-struct inside an element (precond[4], cand[4])
        std::string name;
        int     count = 0, relOffset = 0, stride = 0;
        std::vector<ElemSubField> fields;
    };

    // One sensor TYPE: what it reads in, how finely, over what domain. This — not a channel's telemetry
    // descriptor — is where a sensor's units/precision/range actually come from. For a CATALOGUED sensor
    // the two agree (codegen stamps the type onto the channel at build time); for a GENERIC input they
    // cannot, because its channel is wire-sized to the union of every type it could be given, so its
    // descriptor reads -720..3000 @ 0.01 and describes no sensor. Resolve the input's configured type
    // and ask this instead — see Cache::channelDomain().
    struct SensorType {
        std::string id;
        std::string units;
        int         digits = 0;        // decimals the type is read at
        // Engineering scale of a stored int16 — the firmware's SensorTypeDescriptor::val_scale. The
        // firmware decodes a cal curve at the scale of the type the input is SET TO, so an editor must
        // use this rather than any per-element scale baked in before the type was known.
        double      scale = 1.0;
        double      minV = 0.0, maxV = 0.0;
        bool        selectable = false;   // may a generic input be set to this type
    };

    // A field's default for one element: its own when the schema gave each element one, else the single
    // value, else `fallback` when the meta states none at all.
    static double fieldDefault(const ArrayField& f, int elemIndex, double fallback) {
        if (!f.hasDefault || f.defaults.empty()) return fallback;
        if (f.defaults.size() == 1) return f.defaults.front();
        return (elemIndex >= 0 && elemIndex < int(f.defaults.size())) ? f.defaults[elemIndex] : fallback;
    }

    struct ConfigArray {       // an array-of-structs (sensors, outputs, …) keyed by "module.array"
        std::string module, name, label;
        int     baseOffset = 0, count = 0, stride = 0;
        std::vector<ArrayField> fields;
        std::vector<std::string> elementLabels;   // per-element names (e.g. catalog sensor names); empty = use the index
        std::vector<std::string> elementIds;      // per-element stable ids (e.g. "clt"); empty = index-only
        std::vector<std::string> elementSignals;  // per-element PRIMARY channel (provides[0], default id) — the "$*" of a template
        // Per-element sensor TYPE, settled by the catalog at build time. Empty = the element has no type
        // of its own (a `generic` input): it takes one from the tune's own `type` byte, and it is the
        // ONLY kind of row where that byte is read. Every element carries the byte — they share one
        // struct — so without this there is nothing to say where writing it means anything.
        std::vector<std::string> elementTypes;
        // WHICH INTERFACES each element may be read through (option ids of the `sensor_interface`
        // set), from the catalogue's own declaration. Empty for an element = unconstrained. The
        // firmware has always carried this as SensorDescriptor::interface_mask; nothing published it,
        // so every picker offered the whole enum and a sensor could be set to an interface that reads
        // a pin it is not wired to — a coolant-flow switch on SENT, a switch on Pulse Width.
        std::vector<std::vector<std::string>> elementInterfaces;
        // WHICH OPTIONS each element's enum FIELD may take, as option indices — for an array whose
        // elements are different hardware. outputs.output[i] IS pin i, and an IGN pin cannot be an
        // injector: field name -> per element -> allowed indices. Absent = unconstrained.
        std::unordered_map<std::string, std::vector<std::vector<int>>> elementOptions;
        std::vector<ElemTable> tables;   // element-local tables (e.g. the sensor calibration curve)
        std::vector<ElemArray> arrays;   // element-local repeated sub-structs (precond/cand)
    };

    // If "module.array[index].table" names an element table, return it (+ its array + index); else null.
    const ElemTable *elementTable(const std::string &path, const ConfigArray *&arrOut, int &index) const;
    // The same for a plain element FIELD: "module.array[key].field" -> the field (+ its array + index).
    const ArrayField *elementField(const std::string &path, const ConfigArray *&arrOut, int &index) const;

    // THE table path. Any table — a singular module map OR one instance of an in-array table
    // ("module.array[i].table") — resolves to the same offset descriptor (TableImage). Cache, TuneFile,
    // and the editor all go through this, so there is one table structure and no second form to forget.
    TableImage  resolveTable(const std::string &path) const;
    // Every table INSTANCE in the layout: each module map once, plus each in-array (std) table once per
    // array element. Iterating this + resolveTable() touches every table exactly once.
    std::vector<std::string> tableInstancePaths() const;

    struct Axis {              // one axis of a table — drives Axis Setup (signal / size / enable)
        std::string array;     // "module.<axis>" config array of breakpoints (headers)
        std::string channel;   // default live channel name (for the trace cursor)
        std::string nScalar;   // "module.<axis>_n" live bin-count scalar ("" if a fixed grid)
        std::string srcScalar; // "module.<table>_<ax>_src" channel selector (SignalId), or ""
        std::string enScalar;  // "module.<table>_<ax>_en" enable toggle (optional axis), or ""
        int     nMax = 0;      // maximum bins
        bool    optional = false;
    };

    // Help text for ANY bindable path (scalar / array-element field / table / element table); "" if none.
    std::string helpFor(const std::string &path) const;

    struct ConfigTable {       // a config 2D/3D map (a stack of z planes), keyed by "module.table"
        int         offset = 0;
        std::string datatype;  // cell type
        int         cellSize = 0;
        double      scale = 1.0;
        int         digits = -1;   // cell display precision (schema "digits"); -1 = unspecified
        int         cols = 0;      // DEFAULT live x bins (the schema's `cols`)
        int         rows = 0;      // default live y bins
        int         depth = 1;     // default live z bins (planes); 1 = a plain 2D table
        // The ALLOCATION — how wide the storage actually is. The cells are addressed at this stride
        // whatever <axis>_n says, so it is what every offset is computed from; cols/rows/depth above are
        // only the schema's starting size. They were being used as the allocation ("max x bins" said the
        // comment) and they are not: ve_table is 22x23x2 live inside 32x32x4.
        int         colsMax = 0, rowsMax = 0, depthMax = 1;
        double      minV = 0.0;
        double      maxV = 0.0;
        std::string units;
        std::string label;
        std::string help;          // hover tooltip
        // The base table(s) this one's values can be ROLLED INTO — a learned correction names the map it
        // corrects (schema `apply_to`). Empty for an ordinary table. See Surface::tableApplyToBase.
        std::vector<std::string> applyTo;
        bool applyAdd = false;       // …folded in by ADDING (a duty offset), not multiplying (a percentage)
        std::string xAxis;         // "module.<rpm_axis>" config array for column headers (or "")
        std::string yAxis;         // row headers
        std::string zAxis;         // depth (plane) breakpoints, when 3D (or "")
        std::string xChannel;      // live telemetry channel driving the x axis (for the trace cursor)
        std::string yChannel;
        std::string zChannel;      // live channel driving the z (depth) axis
        std::vector<Axis> axes;    // x[, y[, z]] — full per-axis info for the Axis Setup dialog
        // HEADINGS THAT ARE NAMES, NOT NUMBERS. Some axes have no quantity to print: the VVT trim's
        // rows are the four cams, the bank trim's columns are banks 1 and 2. Empty for an ordinary
        // axis, whose breakpoints ARE the heading.
        std::vector<std::string> rowLabels, colLabels;
    };

    struct LuaApiEntry {
        std::string name;
        std::string sig;
        std::string doc;
    };

    // Bytes per datatype tag.
    static int dataSize(const std::string &datatype);

    bool loadFile(const std::string &path);
    // THE OLDEST STUDIO A META WORKS WITH. A meta states min_studio (firmware/min_studio.txt, via
    // codegen); set this studio's own version once at startup and loadFile refuses a meta that needs a
    // newer one — rather than reading a format it does not know. Then needsStudio() says which version.
    // Unset (tests, tools): no check.
    static void setStudioVersion(const std::string &v) { studioVersion_ = v; }
    // THE HIGHEST META FORMAT THIS STUDIO READS (meta.meta_format; tools/meta_format.py in the firmware
    // tree). A newer one is refused like a too-new min_studio, whatever min_studio says: the format is
    // checked by the build, so it cannot be forgotten the way a hand-raised version can. Raise this in
    // the same change that teaches the studio the new structure.
    static constexpr int kMetaFormat = 1;
    const std::string &needsStudio() const { return needsStudio_; }

    // ---- PcVariables: host-side values declared PER PROJECT ------------------------------------------
    // "Virtual" configuration: named values the ECU knows nothing about, used to drive widget conditions,
    // expressions and imported TunerStudio dialogs. They follow the same separation as everything else —
    // the META declares the path, the TUNE carries the value, the DASHBOARD holds the control — so they
    // are ordinary config fields living in a `kind: "host"` segment, addressed as `pc.<name>` and read or
    // written by exactly the code every other field uses.
    //
    // They arrive as an OVERLAY rather than in the meta file itself, because a generated meta cannot be
    // hand-edited: the definition is regenerated from the schema, while these belong to the project. The
    // overlay is applied after loadFile and re-applied whenever the set changes.
    // NOTE ON SCALE. A config field needs one because the ECU stores a raw integer and the meta says what
    // it means. A hand-authored PcVariable has no such split: the studio owns the storage, an F32 holds the
    // value directly, and any display scaling belongs to the WIDGET, which seeds its own Scale property.
    // The field below exists for IMPORTED declarations, where a TunerStudio ini names a storage type and a
    // scale of its own (a U08 x 0.1, say) and fidelity means honouring them. Nothing hand-authored should
    // ever need to set it.
    struct PcVar {
        std::string name;                 // path is "pc." + name
        std::string datatype = "F32";     // storage in the host block
        double      scale = 1.0;          // imported declarations only — see the note above
        double      minV = 0.0, maxV = 0.0;
        std::string units, label, help;
        std::string kind;                 // "" / "bool" / "enum"
        std::vector<std::string> options; // enum labels
        // A host variable starts at ZERO unless it says otherwise, and zero is not always a harmless
        // starting point: the bench test's count is "how many times to fire", where 0 means cancel —
        // so an undeclared default made the first press of Test do nothing at all.
        bool        hasDef = false;
        double      defV   = 0.0;
    };
    // Declare the host segment and one config field per variable. Offsets are assigned in declaration
    // order; a tune is keyed by PATH, so reordering or inserting a variable never moves anyone's value.
    //
    // TWO SOURCES, kept apart. The DEFINITION declares the ones its pages need (the meta's "pcVars":
    // the firmware schema's pc_vars, or an imported ini's [PcVariables]); the PROJECT may add its own
    // (the pcvars.json side-car, edited in the dictionary). applyPcVars sets the PROJECT's set — a name
    // the definition already declares is ignored, so a definition variable cannot be renamed, removed
    // or saved into the side-car. pcVars() is the live union, definition first.
    void applyPcVars(const std::vector<PcVar> &vars);
    const std::vector<PcVar> &pcVars() const { return pcVars_; }
    const std::vector<PcVar> &projectPcVars() const { return projectPcVars_; }
    bool isDefinitionPcVar(const std::string &name) const {
        for (const PcVar &v : definitionPcVars_) if (v.name == name) return true;
        return false;
    }
    static constexpr uint32_t kPcSegmentBase = 0x50000000u;   // above the learned region (0x40000000)
    bool isValid() const { return valid_; }
    std::string path() const { return path_; }

    // Identity
    std::string product() const   { return meta_["product"].str(); }
    std::string board() const     { return meta_["board"].str(); }
    // The signature an imported TunerStudio definition declares; empty for a native meta.
    std::string tsSignature() const { return tsSignature_; }
    std::string fwVersion() const { return meta_["fw_version"].str(); }
    std::string layoutHash() const{ return meta_["layout_hash"].str(); }
    // What kind of controller this definition describes: "ecu" (absent = an engine ECU) or "tcu" (J8HP).
    // Engine-only instruments and status (trigger, knock, cycle, rpm/clt) are offered for an ECU only.
    std::string deviceClass() const { const std::string c = meta_["device_class"].str(); return c.empty() ? "ecu" : c; }
    bool hasTelemetry(const std::string &ch) const { return telem_.count(ch) != 0; }
    std::string minStudio() const { return meta_["min_studio"].str(); }
    int configSize() const    { return meta_["config_size"].number<int>(); }
    int telemetrySize() const { return meta_["telemetry_size"].number<int>(); }
    // Max config read/write data chunk (bytes) the firmware assembles in one frame — the single source
    // for the comms chunk size so the host never over-chunks past the firmware's frame buffer.
    int blockSize() const { return protocol_["framing"]["block_size"].number<int>(); }

    // Lua API for autocomplete
    const std::vector<LuaApiEntry> &luaFunctions() const { return luaFunctions_; }
    const std::vector<LuaApiEntry> &luaCallbacks() const { return luaCallbacks_; }

    // The default tune as a packed EcuConfig image (offline baseline; empty if the meta omits it).
    const std::vector<uint8_t> &defaultImage() const { return defaultImage_; }

    // Bus signals: name <-> SignalId index (a channel selector scalar stores the index).
    const std::map<std::string, int> &signalMap() const { return signals_; }   // not signals(): Qt macro

    // EVERY TABLE AN ID CAN NAME, in id order — the firmware's table_registry.h as data. Two customers:
    // the compiler (table(<name>) bakes the id into the instruction) and the expression builder (which
    // offers the names). Both must agree with the VM, and there is one list for all three.
    struct TableRef { std::string name, label; };
    const std::vector<TableRef> &tableRegistry() const { return tableRegistry_; }
    int tableId(const std::string &name) const {         // -1 when this firmware has no such table
        for (size_t i = 0; i < tableRegistry_.size(); ++i)
            if (tableRegistry_[i].name == name) return static_cast<int>(i);
        return -1;
    }

    // THE SIZE OF THE CATALOG, which is not the number of NAMED signals. Ids are append-only and
    // permanent (definition/signal_ids.lock), so the map is sparse: 337 names with ids running to 388,
    // because an id is never reused after its signal goes. The firmware's SIG_COUNT is that upper
    // bound, and it is what a program's selector must be checked against — bounding on the map's SIZE
    // rejects every channel added since the catalog last had no gaps, which is every recent one.
    int signalCount() const {
        int hi = 0;
        for (const auto &[idx, _name] : signalByIndex_) hi = std::max(hi, idx + 1);
        return hi;
    }
    std::string signalName(int index) const {
        auto it = signalByIndex_.find(index);
        return it != signalByIndex_.end() ? it->second : std::string();
    }

    // Protocol: command code for a named command ("telemetry" -> 'A'), 0 if absent.
    char cmd(const std::string &name) const;

    // Telemetry descriptor
    const std::unordered_map<std::string, TelemField> &telemetry() const { return telem_; }
    // The value range declared for a signal/channel (from its telemetry descriptor). Returns false when
    // the channel is unknown or its range is unset (min==max) — callers then leave the value unclamped.
    //
    // For a channel published by a GENERIC sensor input this is the union WIRE, not a sensor's range —
    // ask Cache::channelDomain() instead, which resolves the input's configured type first.
    bool signalRange(const std::string &channel, double &lo, double &hi) const;

    // The sensor type catalog, IN CATALOG ORDER — position is the value a `type` byte holds, the same
    // index the firmware's SENSOR_TYPE_CATALOG uses. Empty on a meta that predates it.
    const std::vector<SensorType> &sensorTypes() const { return sensorTypes_; }
    const SensorType *sensorType(int index) const {
        return (index >= 0 && index < int(sensorTypes_.size())) ? &sensorTypes_[index] : nullptr;
    }
    const SensorType *sensorType(const std::string &id) const {
        for (const SensorType &t : sensorTypes_) if (t.id == id) return &t;
        return nullptr;
    }
    // Which sensor element PUBLISHES this channel, if any: its array ("sensors.sensor") and index.
    // Built from the arrays' elementSignals at load, so it costs a hash lookup rather than a scan.
    bool sensorForSignal(const std::string &channel, std::string &arrayKey, int &index) const;

    // Config descriptor. Scalars keyed by "module.field"; tables keyed by "module.table".
    const std::unordered_map<std::string, ConfigField> &config() const { return config_; }
    const std::unordered_map<std::string, ConfigTable> &configTables() const { return tables_; }
    const std::unordered_map<std::string, ConfigArray> &configArrays() const { return structArrays_; }   // sensors, outputs, …

    // Cache blocks. Config bindings carry an absolute device offset; a segment is one contiguous range of
    // that offset space with its own backing store in the Cache. `config` (base 0) is the flashed tune;
    // other segments (e.g. the RAM-backed `learned` region, persisted to SD totems) are read/written by meta path exactly like
    // config, but are NOT part of the tune. Empty if the meta predates the segments descriptor.
    struct Segment { std::string id, kind; uint32_t base = 0; int size = 0; bool writable = true; };
    const std::vector<Segment> &segments() const { return segments_; }

    // THE VE AUTOTUNER'S CONTRACT, from whichever definition is loaded. Our schema declares it under
    // `autotune:`; a TunerStudio ini declares the same facts in its own [VeAnalyze] section and the
    // importer converts them — so the panel reads one shape and neither ECU needs a special case.
    //
    // `egoChannels` is the half nothing could be inferred: the corrections the ECU is ALREADY applying,
    // as multipliers. Folded back in, a mixture held on target by closed loop still reads as the wrong
    // table it is. Empty (or all reading 1.0) means the measurement stands on its own.
    struct AutotuneFilter { std::string name, channel; bool above = false; double value = 0.0; };
    // HOW AN EGO CHANNEL IS READ, because "the correction the ECU is applying" is published in three
    // different shapes and they are not interchangeable. Our own firmware publishes a MULTIPLIER
    // (fuel_corr_stft, units 'x', 1.0 = none). rusEFI publishes Gego as a percentage where 100 is none
    // — its own ini says so: "VE Analyze requires a 100-based correction while the user-facing STFT
    // channel is 0-based". And a plain trim percentage is 0 = none.
    //
    // Units cannot settle this: both of the percentage forms declare "%", and reading a 100-based one
    // as a multiplier does not misjudge the correction by a little, it multiplies the whole proposal by
    // a hundred. So the definition states the basis and this converts it. The default is Multiplier,
    // which is what every existing declaration means.
    enum class EgoBasis { Multiplier, Percent100, Percent0 };
    struct AutotuneEgo {
        std::string channel;
        EgoBasis    basis = EgoBasis::Multiplier;
        // The channel's reading as the engine wants it: a multiplier where 1.0 is no correction.
        double asMultiplier(double v) const {
            switch (basis) {
                case EgoBasis::Percent100: return v / 100.0;
                case EgoBasis::Percent0:   return 1.0 + v / 100.0;
                case EgoBasis::Multiplier: break;
            }
            return v;
        }
    };
    static EgoBasis egoBasisFromName(const std::string &s) {
        if (s == "percent_100") return EgoBasis::Percent100;
        if (s == "percent_0")   return EgoBasis::Percent0;
        return EgoBasis::Multiplier;
    }
    struct Autotune {
        std::string table;            // the map being tuned ("fuel_calculator.ve_table")
        std::string targetTable;      // …and the map holding its target, where the target is a map
        std::string targetChannel;    // …or the live channel commanding it, where the ECU publishes one
        std::string lambdaChannel;
        std::string delayTable;       // the ECU's own transport-delay map, when it has one
        std::vector<AutotuneEgo>     egoChannels;
        std::vector<AutotuneFilter>  filters;
        bool valid() const { return !table.empty() && !lambdaChannel.empty(); }
    };
    // A table the studio learns from a channel that MEASURES its answer (schema `value_autotune`):
    // Predicted MAP is the manifold pressure a steady throttle and speed settle to. A record counts once
    // every `steady` channel has held within its span for `settleMs`. See autotune::Engine::setValueMode.
    struct ValueAutotuneSteady { std::string channel; double span = 0.0; };
    struct ValueAutotune {
        std::string name, table, valueChannel;
        double      settleMs = 500.0;
        std::vector<ValueAutotuneSteady> steady;
        std::vector<AutotuneFilter>      filters;
    };
    // How many characters a text field holds, as the definition states it; 0 when it does not say.
    // See ConfigField::maxChars — the VIN is the case that makes the distinction matter.
    int textCapacity(const std::string &path) const {
        const auto it = config_.find(path);
        return it == config_.end() ? 0 : it->second.maxChars;
    }

    const Autotune &autotune() const { return autotune_; }
    // WHEN A SETTING TAKES EFFECT, if not at once: "engine_stop" (copied into the firmware's working copy
    // at the next engine stop — Trigger, Engine) or "reboot"; "" for everything else. `path` is any binding
    // into it — "engine.cylinder_count", "trigger.streams[2].enabled", a table — resolved by its module and
    // top-level name, which is what the schema marks.
    std::string appliesAt(std::string path) const {
        if (path.size() >= 4 && path[0] == '[' && path[1] == '#' && path.back() == ']')
            path = path.substr(2, path.size() - 3);          // the builder's writable-path form
        const size_t dot = path.find('.');
        if (dot == std::string::npos) return {};
        const size_t end = path.find_first_of(".[", dot + 1);
        const std::string top = path.substr(0, end == std::string::npos ? std::string::npos : end);
        // An element's own field first ("outputs.output[3].function" -> "outputs.output.function").
        if (end != std::string::npos && path[end] == '[') {
            const size_t close = path.find(']', end), fdot = close == std::string::npos ? close : path.find('.', close);
            if (fdot != std::string::npos) {
                const size_t fend = path.find_first_of(".[", fdot + 1);
                const auto f = applies_.find(top + "." + path.substr(fdot + 1, fend == std::string::npos
                                                                              ? std::string::npos : fend - fdot - 1));
                if (f != applies_.end()) return f->second;
            }
        }
        const auto it = applies_.find(top);
        return it == applies_.end() ? std::string() : it->second;
    }
    const std::vector<ValueAutotune> &valueAutotunes() const { return valueAutotunes_; }
    // Resolve a whole-region binding to a byte span: "module.array" (every element) or
    // "module.array[index]" (one element, e.g. electronic_throttle.etb[0] — its relax_pct + ff_table +
    // all fields). Used by the gen-watch scoped refresh to re-read the regions a command rewrites.
    bool resolveRegion(const std::string &path, int &offset, int &size) const;
    // The control "kind" of a field (config scalar OR array element): "signal" (a *_src/*_sig channel
    // selector), "bool" (a 0/1 toggle), "enum", or "" — so the studio picks the right control uniformly,
    // whether the field is a top-level scalar or an array-of-structs element.
    std::string fieldKind(const std::string &path) const;
    // Nested sub-struct field lookups ("sensors.sensor[2].precond[0].op") — see MetaModel.cpp.
    const ElemSubField *subFieldOf(const std::string &path) const;
    std::string subFieldKind(const std::string &path) const;
    // A named bit group addressed as "<field path>.<group>" — the unit a user edits, as opposed to the
    // storage byte it is packed into. Null when the path is not one.
    const BitGroup *bitGroupOf(const std::string &path) const;
    // A channel selector. "sensor" is the SAME control restricted to channels a sensor actually
    // produces — everything that renders or routes a picker treats the two alike, and only the option
    // LIST differs (see isSensorField / sensorChannels).
    bool isSignalField(const std::string &path) const {
        std::string k = fieldKind(path);
        if (k.empty()) k = subFieldKind(path);   // a precondition's Signal is a picker like any other
        return k == "signal" || k == "sensor";
    }
    // A TABLE selector: the number is a table id (the expression VM's registry order), so the control
    // is a picker over table NAMES and never a spin box over an index nobody can read back. Distinct
    // from a path that IS a table — that is the grid editor's business; this is a scalar naming one.
    bool isTableField(const std::string &path) const {
        std::string k = fieldKind(path);
        if (k.empty()) k = subFieldKind(path);
        return k == "table_ref";
    }
    // Restricted variant: the field must name a channel SOMETHING ACQUIRES. Pointing a calibrating
    // input at an arbitrary bus signal is how autocal/pedalcal come to write an unrelated sensor's
    // calibration — they resolve their target by primary_channel.
    bool isSensorField(const std::string &path) const { return fieldKind(path) == "sensor"; }
    // A compiled EXPRESSION field (firmware/Signal/ExprIsa.h bytecode): a program, not a number, so
    // it binds to the expression editor and never to a spin box. Works for a top-level scalar and an
    // array element alike, since the sensor precondition is the latter.
    bool isExpressionField(const std::string &path) const { return fieldKind(path) == "expression"; }
    // Byte span of a blob field (expression bytecode / ASCII text), scalar or array element.
    bool resolveBlob(const std::string &path, int &offset, int &size) const;

    // THE LOCATE MEMO. locate() is the single hottest call in the studio — every config read, bound, unit,
    // decimal and range goes through it — and answering costs a path parse, a couple of map lookups and,
    // for an array element, a linear scan of the element ids and then of the element's fields. Nothing it
    // computes depends on the config VALUES, only on the meta, so the answer for a path never changes
    // while a meta is loaded. Memoised, it is a hash lookup; unmemoised it was ~5 microseconds a call,
    // which a page doing hundreds of reads per frame pays every frame.
    void clearLocateCache() const { locCache_.clear(); }
    // The channels a sensor publishes: every element of the sensors catalog, by its primary channel id.
    std::set<std::string> sensorChannels() const {
        std::set<std::string> out;
        const auto it = structArrays_.find("sensors.sensor");
        if (it == structArrays_.end()) return out;
        for (const std::string &id : it->second.elementIds) out.insert(id);
        return out;
    }
    // The telemetry channel a template's "$*" resolves to: an element's PRIMARY signal (provides[0],
    // captured per-element in element_signals), addressed by the same config key the "[*]" star uses. When
    // a sensor's id and signal differ (boost_pressure -> boost_kpa), one element-context value still drives
    // BOTH the config binding (#…sensor[*]…) and the telemetry binding ($*). Falls back to the key itself
    // when unmapped — for every sensor whose id already equals its signal, which is most of them.
    std::string primarySignalForKey(const std::string &key) const {
        for (const auto &[name, ca] : structArrays_) {
            const size_t n = ca.elementIds.size() < ca.elementSignals.size()
                                 ? ca.elementIds.size() : ca.elementSignals.size();
            for (size_t i = 0; i < n; ++i)
                if (ca.elementIds[i] == key && !ca.elementSignals[i].empty())
                    return ca.elementSignals[i];
        }
        return key;
    }
    // Every config signal-selector field (kind "signal") — a config field whose value is a SignalId,
    // i.e. "this module input points at channel X". Drives the indirect-source picker: a display widget
    // can FOLLOW one of these to show the live bus value of whatever channel it currently selects. Each
    // carries the resolve path plus a group/leaf split for a tidy grouped tree (group = module or array,
    // leaf = the field, with the element name for array-of-structs members like output[3].source).
    struct SignalSelector { std::string path, group, leaf; };
    std::vector<SignalSelector> signalSelectorFields() const;
    // The human caption for a binding (config scalar "module.field", telemetry channel, 1D array, or
    // array-of-structs field "module.array[key].field"); "" if unknown. Used to auto-label bound fields.
    std::string labelFor(const std::string &path) const;

    // Enum option labels for a config path (top-level scalar OR struct-array field); empty if not enum.
    std::vector<std::string> enumOptions(const std::string &path) const;
    // An enum's value-set IDS, index = value. Labels are for people; ids are what the rest of the meta
    // joins on (hw_pool_signals is keyed by interface id).
    const std::vector<std::string> &enumIds(const std::string &setId) const {
        static const std::vector<std::string> kNone;
        const auto it = enumIds_.find(setId);
        return it == enumIds_.end() ? kNone : it->second;
    }
    // Those options BY ID, index-aligned (empty when the definition ships none for this field).
    std::vector<std::string> enumOptionIds(const std::string &path) const;
    // The enum SET a path's options come from ("sensor_type"), or "" — an option id means nothing
    // without it, because ids are unique only within their set.
    std::string enumSetId(const std::string &path) const;
    // The option ids an ENUM FIELD OF AN ARRAY ELEMENT may take, when the catalogue constrains them
    // (today: a sensor's `interface`). Empty = no constraint, offer the whole set. Keyed off the
    // field's own enum set id, so it costs nothing on every other enum and cannot be fooled by a
    // renamed label — the same rule the sensor-type filter learned the hard way.
    std::vector<std::string> allowedOptionIds(const std::string &path) const;
    // The option INDICES an element's enum field may take (ConfigArray::elementOptions); empty = any.
    std::vector<int> allowedOptionIndices(const std::string &path) const;
    // Enum option labels for a CHANNEL (telemetry/signal name OR a config path); empty if not enum.
    // For an interface-gated `pickers` field (sensor source), return its picker (sibling field + sets);
    // false if the path isn't such a field. The caller resolves the active set from the sibling's value.
    bool fieldPicker(const std::string &path, std::string &byField, std::vector<PickerSet> &sets) const;
    // Is `value` a pin this field may never be moved off? The board declares some pins
    // `assignable: false` because the HAL consumes them — the battery sense on AV12 — and a control
    // that offers to CLEAR one offers to break it: nothing can put it back, because nothing may pick
    // it. Asked of the whole field rather than of one set: a fixed pin is fixed in every set that
    // lists it.
    bool pickerValueFixed(const std::string &path, int value) const {
        std::string by;
        std::vector<PickerSet> sets;
        if (!fieldPicker(path, by, sets)) return false;
        for (const PickerSet &ps : sets)
            for (size_t i = 0; i < ps.values.size(); ++i)
                if (ps.values[i] == value)
                    return i < ps.fixed.size() && ps.fixed[i] != 0;
        return false;
    }
    const std::unordered_map<std::string, ConfigField> &arrays1d() const { return arrays1d_; }   // 1D axis/curve arrays

    // A STRIP — N scalars, `stride` bytes apart, from `offset`. Two different shapes in the meta are the
    // same thing to anything that draws or edits a run of values: a 1-D array ("ignition.ign_rpm_axis")
    // and a nested sub-array inside an array element ("trigger.streams[0].cell[].v"). The empty brackets
    // name the repeated dimension — the run itself rather than one member of it.
    //
    // Without this a strip widget could only ever show a 1-D array, so a per-element run (a stream's
    // cells, an output's candidates) had to be laid out as one widget per index — which puts the shape
    // of the data into the PAGE, where the layout engine then has to be argued with about hidden slots.
    // ---- ONE path grammar -------------------------------------------------------------------------
    //
    // A config path is a dotted name in which any segment may carry a subscript: "[3]", "[clt]" (an
    // element's catalog id) or "[]" (the whole run). Every addressable thing in the ECU is one of those,
    // and locate() is the only thing that reads them.
    //
    // It replaces thirteen functions that each answered the same question for one shape — resolveField,
    // resolveElement, resolveCell, resolveArrayField, resolveStrip, resolveBlob, resolveRegion,
    // resolveTable, resolveBitGroup, subFieldOf, bitGroupOf, cellRefAt, and setConfigValue's own inline
    // copy. There were thirteen because the grammar grew a term at a time and each term got a function
    // rather than a clause. That is also why bounds enforcement was broken: with thirteen ways to obtain
    // a location, twelve forgot to bring the limits. A Location cannot be built without them.
    struct Location {
        enum class Kind : uint8_t { None, Scalar, Run, Table };
        Kind        kind     = Kind::None;
        int         offset   = -1;
        std::string datatype;
        double      scale    = 1.0;
        BitRange    bits;                       // packed field: write must preserve its neighbours
        double      minV     = 0.0, maxV = 0.0; // declared bounds (raw units, as `default` is)
        int         count    = 0;               // Run: elements; Table: cells at max allocation
        int         stride   = 0;               // Run: byte step between elements
        // BYTES, for a field whose datatype does not say how many. A number's width follows from its
        // datatype; a blob's does not — an ASCII name owns a fixed NUL-padded run and is unwritable
        // without knowing how long it is. 0 for everything else.
        int         size     = 0;
        // What the meta says ABOUT the field, carried by the same walk that found it. Six accessors —
        // configMin, configMax, digits, configDatatype, configScale, unit — each repeated the walk to
        // fetch one of these, so a path was resolved half a dozen times to draw one row, and each copy
        // was free to disagree about the shapes it handled (configScale did not know 1-D arrays existed).
        // One walk, one answer, and asking for a different property cannot land you on a different field.
        std::string units, label, help;
        int         digits   = -1;              // meta display precision; -1 = unspecified (derive from scale)
        // What the VALUE means, not just how to draw it: "signal" makes this field's number an index into
        // the signal catalog, "enum" an index into `options`. The tune stores both by NAME, and it reaches
        // array element fields through locate() — so the kind has to travel with the location or the tune
        // writer cannot tell a selector from an ordinary number. (`kind` above is the LOCATION's kind —
        // scalar/run/table — so the field's own kind needs its own name.)
        std::string valueKind;
        std::vector<std::string> options;
        // A board-pin selector: its number is an index into the BOARD's pool, so it ages exactly like a
        // SignalId — change the pool and a sensor quietly reads a different physical pin. It carries no
        // `kind`, only these gated option lists, so the tune names it by the option's label.
        std::vector<PickerSet> pickerSets;
        bool valid() const { return kind != Kind::None && offset >= 0 && !datatype.empty(); }
    };
    Location locate(const std::string &path) const;

    // Read the values of a 1D config array/axis ("module.<axis>") from an image; empty if absent.
    std::vector<double> arrayValues(const std::string &path, const std::vector<uint8_t> &image) const;

    // True when the definition declares BIG-endian storage (TS inis say so among their field
    // declarations). Everything the firmware here produces is little-endian; an imported definition need
    // not be, and decoding one the wrong way round yields plausible-looking rubbish rather than an error.
    bool bigEndian() const { return bigEndian_; }

    // True when the channels in this definition are produced by a FLOAT signal bus — which is what this
    // firmware has, so telemetry is decoded in float to carry exactly the precision the ECU sent. An
    // imported TunerStudio definition is NOT such a bus: rusEFI channels are scaled integers, and pushing
    // a 32-bit count through a float32 would quietly lose everything past 2^24 that the source had.
    bool floatBus() const { return floatBus_; }

    // Decode one field value (engineering units = raw * scale applied by caller).
    static double decodeRaw(const std::string &datatype, const unsigned char *p, bool be = false);
    // Encode a raw (pre-scale) value little-endian into p, per datatype.
    static void encodeRaw(const std::string &datatype, unsigned char *p, double raw, bool be = false);

    // Scan a library directory for the meta whose meta.layout_hash matches; "" if none.

    // Navigation tree from the meta (list of {name, children?, target_path?} nodes).
    // Used to seed a default dashboard.gui on first connect to an ECU.
    const jf::JJson &navigationTree() const { return navTree_; }

    // CLI command catalog auto-derived by codegen from the firmware registry (meta "commands"): each
    // {name, help, args:[{label,type,min,max,units}]}. The dictionary lists these (draggable) and a
    // CommandButton is built per command — no per-command studio code. Empty array if absent.
    const jf::JJson &commands() const { return commands_; }   // top-level array, parsed alongside navTree_
    // WHICH CHANNELS a recording carries, as the firmware declares them (schema datalog_templates).
    // Held here rather than re-parsed from the file: the meta is ~1.5 MB and this is asked whenever a
    // preferences page opens.
    const jf::JJson &datalogTemplates() const { return datalogTemplates_; }

    // Board hardware self-description from the meta ({board, adc:{full_scale,vref_mv}, av/at:{fullscale_mv}}).
    // The client converts ECU-native ADC counts <-> mV/V from this (the firmware never converts).
    const jf::JJson &hardware() const { return hardware_; }
    // Pin pool index -> the RAW channel HardwareInput publishes for it, per interface id: the meta's
    // copy of the firmware's HW_POOL_SIG / HW_DIG_*_SIG, and the join primary_hw_sig() makes. A
    // sensor's `source` indexes one of these, which is how "AV3" becomes hw_av3 — a channel that
    // already states ADC, 0..4095 in telemetry, so nothing downstream has to infer it from the board.
    const std::vector<std::string> *hwPoolSignals(const std::string &ifaceId) const {
        const auto it = hwPool_.find(ifaceId);
        return it == hwPool_.end() ? nullptr : &it->second;
    }

    // Outside-world wiring for a board resource, from the board's connector map: its connector terminal
    // ("CN3-26") and wire-colour code ("G/W"). Keyed by resource/signal name (the same name the pin
    // pickers show — "AV2", "LS3", "IGN1"). True if the resource is in the connector map.
    bool wiringTrim(const std::string &resource, std::string &pin, std::string &color) const {
        auto it = wiring_.find(resource);
        if (it == wiring_.end()) return false;
        pin = it->second.first; color = it->second.second; return true;
    }

    // The physical connectors: id ("CN3") -> shell {colour ("blue"), part, description}. The studio
    // draws the connector diagram and colour-codes a terminal's shell from this.
    struct Connector { std::string color, part, desc; };
    const std::unordered_map<std::string, Connector> &connectors() const { return connectors_; }

    // DTC hover text: exact per-code phrase (sensor faults + module signal-validity codes), falling
    // back to the OBD subsystem category label, else empty. `code` is the hex-BCD form (0x0122 == P0122).
    std::string dtcDescription(uint16_t code) const {
        auto it = dtcDesc_.find(code);
        if (it != dtcDesc_.end()) return it->second;
        for (const DtcRange &r : dtcCats_)
            if (code >= r.lo && code <= r.hi) return r.label;
        return {};
    }

private:
    // Resolve an element key — a numeric index OR a catalog id (e.g. "clt") — to (array, index);
    // null if the array/key is unknown. Lets binding paths use either form.
    const ConfigArray *elementOf(const std::string &arrayKey, const std::string &elemKey, int &index) const;

    // See clearLocateCache(). Mutable because locate() is const and this is a memo, not model state.
    mutable std::unordered_map<std::string, Location> locCache_;
    // The resolver itself. locate() is the memo in front of it.
    Location locateUncached(const std::string &path) const;

    bool valid_ = false;
    bool bigEndian_ = false;   // declared by the definition (TS `endianness = big`)
    std::string tsSignature_;  // an imported definition's own signature; empty for a native meta
    inline static std::string studioVersion_;   // see setStudioVersion
    std::string needsStudio_;                  // why the last loadFile refused, if it did
    std::vector<PcVar> pcVars_;            // the live set: definition + project (see applyPcVars)
    std::vector<PcVar> definitionPcVars_;  // declared by the meta
    std::vector<PcVar> projectPcVars_;     // added by the project (the side-car)
    std::vector<PcVar> pendingPcVars_;     // declared by the file, applied once config_ is built
    void rebuildPcVars();
    bool floatBus_  = true;    // this firmware's channels come off a float SignalBus; imports say otherwise
    std::string path_;
    jf::JJson datalogTemplates_;
    jf::JJson meta_;
    jf::JJson hardware_;   // board self-description (adc/av/at scaling) — one meta = one board
    std::unordered_map<std::string, std::vector<std::string>> hwPool_;   // interface id -> pool channels
    jf::JJson protocol_;
    jf::JJson navTree_;
    jf::JJson commands_;
    std::unordered_map<std::string, TelemField> telem_;
    std::unordered_map<std::string, std::vector<std::string>> enums_;   // enum id -> ordered option labels (index = value)
    std::unordered_map<std::string, std::vector<std::string>> enumIds_; // enum id -> ordered value IDS (index = value)
    std::unordered_map<std::string, ConfigField> config_;
    std::unordered_map<std::string, ConfigTable> tables_;
    std::unordered_map<std::string, ConfigField> arrays1d_;   // 1D axis/curve arrays, for header values
    std::unordered_map<std::string, ConfigArray> structArrays_;   // arrays-of-structs (sensors, outputs, …)
    std::vector<uint8_t> defaultImage_;             // packed EcuConfig default tune (offline baseline)
    std::vector<Segment> segments_;
    Autotune             autotune_;                 // cache blocks (config + nvram/volatile), by device offset
    std::vector<ValueAutotune> valueAutotunes_;
    std::map<std::string, std::string> applies_;   // "module.name" -> engine_stop | reboot
    std::vector<SensorType> sensorTypes_;   // catalog order — index == the stored `type` byte
    // channel name -> {array key, element index} of the sensor that publishes it.
    std::unordered_map<std::string, std::pair<std::string, int>> signalOwner_;
    std::map<std::string, int> signals_;            // signal name -> SignalId index
    std::vector<TableRef>      tableRegistry_;      // id -> table (meta "table_registry", id order)
    std::unordered_map<int, std::string> signalByIndex_;   // SignalId index -> signal name
    std::unordered_map<std::string, std::pair<std::string, std::string>> wiring_;   // resource -> {conn pin, colour}
    std::unordered_map<std::string, Connector> connectors_;   // connector id -> shell {colour, part, desc}
    std::vector<LuaApiEntry> luaFunctions_;
    std::vector<LuaApiEntry> luaCallbacks_;

    struct DtcRange { uint16_t lo, hi; std::string label; };
    std::unordered_map<uint16_t, std::string> dtcDesc_;   // exact P-code -> phrase
    std::vector<DtcRange>                      dtcCats_;   // OBD subsystem ranges -> category label (fallback)
};
