#include "ExprCompiler.h"
#include "ExprAst.h"
#include "ExprUnits.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

using expr::Op;

namespace {

// meta datatype tag -> the VM's config type code. Anything else is not addressable
// by a program (a table, a string, another expression), and says so rather than
// guessing a width.
bool cfgTypeOf(const std::string& dt, uint8_t& out) {
    if (dt == "U08") { out = expr::CT_U8;  return true; }
    if (dt == "S08") { out = expr::CT_S8;  return true; }
    if (dt == "U16") { out = expr::CT_U16; return true; }
    if (dt == "S16") { out = expr::CT_S16; return true; }
    if (dt == "U32") { out = expr::CT_U32; return true; }
    if (dt == "S32") { out = expr::CT_S32; return true; }
    if (dt == "F32") { out = expr::CT_F32; return true; }
    return false;
}

// The compiler is a BACK END: the grammar, tokenizer and parser live in ExprAst (shared with the
// host evaluator), and this walks the resulting tree emitting instructions. Postfix falls out of
// the walk — emit both operands, then the operator — which is exactly what a stack machine eats.
struct Emitter {
    const MetaModel& meta;
    std::vector<uint8_t> out;
    std::string error;
    int errPos = -1;

    explicit Emitter(const MetaModel& m) : meta(m) {}

    bool fail(const std::string& msg, int pos) {
        if (error.empty()) { error = msg; errPos = pos; }
        return false;
    }
    void emit(uint8_t b) { out.push_back(b); }
    void emitU16(uint16_t v) { emit(v & 0xFF); emit(v >> 8); }
    void emitU24(uint32_t v) { emit(v & 0xFF); emit((v >> 8) & 0xFF); emit((v >> 16) & 0xFF); }
    void emitI32(int32_t v) {
        const uint32_t u = (uint32_t)v;
        for (int k = 0; k < 4; k++) emit((u >> (8 * k)) & 0xFF);
    }
    void emitF32(float v) { uint32_t u; std::memcpy(&u, &v, 4); emitI32((int32_t)u); }

    // Smallest encoding that represents the value exactly. The common gate compares against a round
    // number, so this is most of the size budget: `rpm > 2500` costs 3 bytes here rather than 5.
    void pushConst(double v) {
        if (v == 0.0) { emit(Op::OP_PUSH_ZERO); return; }
        if (v == 1.0) { emit(Op::OP_PUSH_ONE);  return; }
        const bool integral = (v == std::floor(v)) && std::abs(v) < 2147483647.0;
        if (integral && v >= -128 && v <= 127) { emit(Op::OP_PUSH_I8); emit((uint8_t)(int8_t)v); return; }
        if (integral && v >= -32768 && v <= 32767) {
            emit(Op::OP_PUSH_I16); emitU16((uint16_t)(int16_t)v); return;
        }
        // THE HUNDREDTHS FORM ONLY WHEN IT IS EXACT. OP_PUSH_F stores an i32 of hundredths, so it
        // cannot hold a scale of 0.001 — llround(0.1) is 0, and the program then multiplies by
        // NOTHING. That is how every 0.001-scaled setting in this ECU compiled: an output slot's
        // "uptime_s < [#…param_a] * 0.001" became "uptime_s < 0", false for ever, with no error
        // anywhere because a multiply by zero is a perfectly valid program. Checked by round-trip
        // rather than by testing the scale, because the question is exactly "does this survive the
        // encoding" and nothing else.
        const double hundredths = v * 100.0;
        const long long n = std::llround(hundredths);
        if (std::abs(hundredths - (double)n) < 1e-9 && n >= -2147483648LL && n <= 2147483647LL) {
            emit(Op::OP_PUSH_F);
            emitI32((int32_t)n);
            return;
        }
        emit(Op::OP_PUSH_F32);
        emitF32((float)v);
    }

    bool pushChannel(const std::string& name, int pos) {
        const auto& sm = meta.signalMap();
        const auto it = sm.find(name);
        if (it == sm.end()) return fail("unknown channel '" + name + "'", pos);
        emit(Op::OP_PUSH_SIG);
        emitU16((uint16_t)(it->second + 1));   // options_from:signals — stored selector is id + 1
        return true;
    }

    // A fifteenth copy of the path walk lived here — a config_ lookup with a resolveArrayField
    // fallback, which is to say the same two clauses the studio's Cache used to carry. The compiler
    // bakes offsets into bytecode the firmware executes with no lookup table of its own, so it of all
    // places wants the addresses everything else agrees on.
    bool resolveField(const std::string& path, int& off, std::string& dt,
                      double& sc, double& lo, double& hi) const {
        const MetaModel::Location L = meta.locate(path);
        if (!L.valid() || L.kind != MetaModel::Location::Kind::Scalar)
            return false;
        off = L.offset; dt = L.datatype; sc = L.scale; lo = L.minV; hi = L.maxV;
        return true;
    }

    // A config setting by meta path — a whole field, or one of its named bit groups. The offset and
    // bit range are baked HERE, from this layout's meta; the firmware has no table to look them up.
    bool pushConfig(const std::string& path, int pos) {
        if (const MetaModel::BitGroup* bg = meta.bitGroupOf(path)) {
            const size_t dot = path.rfind('.');
            const std::string owner = path.substr(0, dot);
            int off = 0; std::string dt; double sc = 1, lo = 0, hi = 0;
            uint8_t ct = 0;
            if (!resolveField(owner, off, dt, sc, lo, hi) || !cfgTypeOf(dt, ct))
                return fail("cannot address '" + owner + "'", pos);
            if (ct == expr::CT_F32) return fail("'" + owner + "' is a float — it has no bit groups", pos);
            const int width = bg->width();
            if (width < 1 || width > 8) return fail("bit group '" + path + "' is not 1..8 bits wide", pos);
            emit(Op::OP_PUSH_BCFG);
            emitU24((uint32_t)off);
            emit(ct);
            emit((uint8_t)((bg->lo & 0x1F) | (((width - 1) & 0x07) << 5)));
            return true;
        }
        int off = 0; std::string dt; double sc = 1, lo = 0, hi = 0;
        if (!resolveField(path, off, dt, sc, lo, hi))
            return fail("unknown setting '" + path + "'", pos);
        uint8_t ct = 0;
        if (!cfgTypeOf(dt, ct))
            return fail("'" + path + "' is not a value a program can read (" + dt + ")", pos);
        emit(Op::OP_PUSH_CFG);
        emitU24((uint32_t)off);
        emit(ct);
        // Config is stored SCALED while a bus channel is already in engineering units. Emit the
        // conversion so both sides of a comparison mean the same thing whatever they are; unlike
        // folding the scale into a literal it is still right when BOTH operands are settings.
        if (sc != 1.0 && sc != 0.0) { pushConst(sc); emit(Op::OP_MUL); }
        return true;
    }

    bool walk(const expr_ast::Node& n) {
        using expr_ast::Kind;
        switch (n.kind) {
            case Kind::Number:
                pushConst(n.num);
                return true;

            case Kind::Ident:
                // A bare name is a bus channel if the catalog has one; otherwise try it as a config
                // path, so `electronic_throttle.etb[0].enabled` works unbracketed too.
                if (meta.signalMap().count(n.text)) return pushChannel(n.text, n.pos);
                if (n.text.find('.') != std::string::npos) return pushConfig(n.text, n.pos);
                return fail("unknown channel '" + n.text + "'", n.pos);

            case Kind::Sigil:
                if (n.sigil == '$') return pushChannel(n.text, n.pos);
                if (n.sigil == '#') return pushConfig(n.text, n.pos);
                // The studio-only address spaces. Refused by NAME rather than by parse failure —
                // the expression is valid, it just cannot run where it is being asked to run.
                return fail(std::string("[") + n.sigil + n.text + "] is studio state; the ECU "
                            "cannot read it, so it cannot be used in a firmware expression", n.pos);

            case Kind::Unary: {
                if (n.text == "-") {                      // negation = 0 - x, no opcode needed
                    emit(Op::OP_PUSH_ZERO);
                    if (!walk(*n.kids[0])) return false;
                    emit(Op::OP_SUB);
                    return true;
                }
                if (!walk(*n.kids[0])) return false;
                emit(Op::OP_NOT);
                return true;
            }

            case Kind::Binary: {
                if (n.text == "^")
                    return fail("^ is evaluated in the studio and has no ECU instruction", n.pos);
                if (!walk(*n.kids[0]) || !walk(*n.kids[1])) return false;
                const std::string& o = n.text;
                emit(o == ">"  ? Op::OP_GT : o == ">=" ? Op::OP_GE :
                     o == "<"  ? Op::OP_LT : o == "<=" ? Op::OP_LE :
                     o == "==" ? Op::OP_EQ : o == "!=" ? Op::OP_NE :
                     o == "&&" ? Op::OP_AND : o == "||" ? Op::OP_OR :
                     o == "+"  ? Op::OP_ADD : o == "-" ? Op::OP_SUB :
                     o == "*"  ? Op::OP_MUL : Op::OP_DIV);
                return true;
            }

            case Kind::Call: {
                const expr_ast::FnInfo* fn = expr_ast::functionInfo(n.text);
                if (!fn) return fail("unknown function '" + n.text + "'", n.pos);
                if (!fn->firmware)
                    return fail(n.text + "() is evaluated in the studio and has no ECU instruction",
                                n.pos);
                // age(channel) and interp(curve id, x) take a NAME / a literal, because the
                // selector is baked into the instruction rather than computed at run time.
                if (n.text == "age") {
                    const expr_ast::Node& a = *n.kids[0];
                    std::string ch;
                    if (a.kind == Kind::Ident) ch = a.text;
                    else if (a.kind == Kind::Sigil && a.sigil == '$') ch = a.text;
                    else return fail("age() takes a channel name", a.pos);
                    const auto it = meta.signalMap().find(ch);
                    if (it == meta.signalMap().end())
                        return fail("unknown channel '" + ch + "'", a.pos);
                    emit(Op::OP_AGE);
                    emitU16((uint16_t)(it->second + 1));
                    return true;
                }
                // A TABLE IS NAMED, not numbered. Both table instructions carry the id as an operand,
                // and an id is the table's place in the schema's own order — a number nobody can read
                // back and nothing keeps honest if the definition grows a table. The name is resolved
                // here, against the registry the firmware switches over, so a program that compiles
                // names a table this firmware actually has.
                if (n.text == "interp" || n.text == "table") {
                    const expr_ast::Node& id = *n.kids[0];
                    int tid = -1;
                    if (id.kind == Kind::Ident || (id.kind == Kind::Sigil && id.sigil == '$'))
                        tid = meta.tableId(id.text);
                    else if (id.kind == Kind::Number)
                        tid = static_cast<int>(id.num);      // a raw id still works, for a machine
                    if (tid < 0 || tid >= static_cast<int>(meta.tableRegistry().size()))
                        return fail(n.text + "() needs a table name this firmware has" +
                                    (id.kind == Kind::Ident ? " — '" + id.text + "' is not one" : ""),
                                    id.pos);
                    if (n.text == "interp") {
                        if (!walk(*n.kids[1])) return false;   // the x it is read at
                        emit(Op::OP_INTERP);
                    } else {
                        emit(Op::OP_TABLE);                    // at its own axes
                    }
                    emitU16((uint16_t)tid);
                    return true;
                }
                for (const auto& k : n.kids)
                    if (!walk(*k)) return false;
                emit(n.text == "min"    ? Op::OP_MIN :
                     n.text == "max"    ? Op::OP_MAX :
                     n.text == "abs"    ? Op::OP_ABS :
                     n.text == "clamp"  ? Op::OP_CLAMP :
                     n.text == "select" ? Op::OP_SELECT : Op::OP_BIT);
                return true;
            }
        }
        return fail("unsupported expression", n.pos);
    }
};

} // namespace

// ---------------------------------------------------------------------------

ExprCompiler::Result ExprCompiler::compile(const std::string& source, const MetaModel& meta,
                                           uint32_t cfgSize, uint16_t blockSize) {
    Result r;

    // ONE front end (ExprAst), shared with the host evaluator. A blank source is the always-armed
    // program, not an error — an empty gate is a meaning.
    std::string perr;
    int ppos = -1;
    const auto ast = expr_ast::parse(source, perr, ppos);
    if (!ast) {
        if (perr.empty()) { r.ok = true; r.code.push_back(expr::OP_END); return r; }
        r.error = perr;
        r.errPos = ppos;
        return r;
    }

    // UNITS, FOLDED BEFORE ANYTHING IS EMITTED. `age(trigger_teeth) < 1500ms` becomes a plain 1500
    // because age() answers in ms; `clt > 200F` becomes the coolant channel's own unit. Done here and
    // in MathEvaluator through the SAME helper, so the page that greys a row cannot disagree with the
    // firmware that runs it — see ExprUnits.h.
    {
        std::string uerr;
        int upos = -1;
        if (!expr_units::resolve(meta, *ast, uerr, upos)) {
            r.error = uerr;
            r.errPos = upos;
            return r;
        }
    }

    Emitter em(meta);
    if (!em.walk(*ast) || !em.error.empty()) {
        r.error  = em.error.empty() ? "invalid expression" : em.error;
        r.errPos = em.errPos;
        return r;
    }
    em.emit(expr::OP_END);

    if (em.out.size() > blockSize) {
        r.error = "expression is too long: " + std::to_string(em.out.size()) +
                  " bytes of " + std::to_string(blockSize);
        return r;
    }

    // Validate with the SAME validator the firmware runs. The studio must never hand an ECU a
    // program the ECU will reject — the tuner would see a gate that silently reverts to armed.
    // …and with the SAME registry size the firmware passes (EXPR_TABLE_COUNT). Zero was the honest
    // number when nothing could name a table; it is not any more, and left at zero it rejects every
    // program that reads one — which is a table feature that compiles to "please report this
    // expression" for the person trying to use it.
    const expr::Invalid v = expr::validate(em.out.data(), (uint16_t)em.out.size(), cfgSize,
                                           (uint16_t)meta.tableRegistry().size(),
                                           (uint16_t)meta.signalCount());
    if (v != expr::Invalid::None) {
        r.error = "the compiler produced a program the firmware would reject (code " +
                  std::to_string((int)v) + ") — please report this expression";
        return r;
    }

    r.ok = true;
    r.code = std::move(em.out);
    return r;
}

// ---------------------------------------------------------------------------
// Decompiler: bytecode -> source. RPN back to infix is mechanical — push operands as
// STRINGS and let each operator consume them, carrying a precedence so parentheses appear
// exactly where they change meaning and nowhere else.
// ---------------------------------------------------------------------------

namespace {

struct Frag {
    std::string text;
    int prec = 100;      // higher binds tighter; 100 = atom
};

// The SAME rungs as expr_ast (C precedence): equality is LOOSER than the relational operators,
// and unary binds tightest. A decompiler with its own table is how round trips start meaning
// something different from what went in.
int precOf(uint8_t op) {
    switch (op) {
        case expr::OP_OR:  return 1;
        case expr::OP_AND: return 2;
        case expr::OP_EQ: case expr::OP_NE: return 3;
        case expr::OP_GT: case expr::OP_GE: case expr::OP_LT: case expr::OP_LE: return 4;
        case expr::OP_ADD: case expr::OP_SUB: return 5;
        case expr::OP_MUL: case expr::OP_DIV: return 6;
        case expr::OP_NOT: return 8;
        default: return 100;
    }
}

std::string wrap(const Frag& f, int need) {
    return f.prec < need ? "(" + f.text + ")" : f.text;
}

std::string trimNum(double v) {
    char buf[32];
    if (v == std::floor(v) && std::abs(v) < 1e15) snprintf(buf, sizeof(buf), "%lld", (long long)v);
    else {
        snprintf(buf, sizeof(buf), "%.2f", v);
        std::string s(buf);
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
    return buf;
}

} // namespace

std::string ExprCompiler::decompile(const uint8_t* code, uint16_t len, const MetaModel& meta) {
    if (!code || expr::is_empty(code, len)) return {};

    std::vector<Frag> st;
    uint16_t i = 0;
    auto pop = [&](Frag& f) -> bool {
        if (st.empty()) return false;
        f = st.back(); st.pop_back(); return true;
    };

    while (i < len) {
        const uint8_t op = code[i++];
        if (op == expr::OP_END) break;
        if ((uint16_t)(i + expr::operand_bytes_(op)) > len) return "<truncated program>";

        switch (op) {
            case expr::OP_PUSH_SIG: case expr::OP_AGE: {
                uint16_t sel; std::memcpy(&sel, code + i, 2); i += 2;
                std::string nm = sel ? meta.signalName(sel - 1) : std::string("none");
                if (nm.empty()) nm = "channel#" + std::to_string(sel - 1);
                st.push_back({op == expr::OP_AGE ? ("age(" + nm + ")") : nm, 100});
                break;
            }
            case expr::OP_PUSH_CFG: case expr::OP_PUSH_BCFG: {
                const uint32_t off = (uint32_t)code[i] | ((uint32_t)code[i+1] << 8) |
                                     ((uint32_t)code[i+2] << 16);
                const bool packed = (op == expr::OP_PUSH_BCFG);
                uint8_t bits = packed ? code[i+4] : 0;
                i += packed ? 5 : 4;
                double scale = 1.0;
                std::string path = "[#" + configPathAt(meta, (int)off, &scale) + "]";
                if (packed) {
                    const uint8_t lo = bits & 0x1F, w = (uint8_t)((bits >> 5) & 0x07) + 1;
                    path += ".bits[" + std::to_string(lo) + ":" +
                            std::to_string(lo + w - 1) + "]";
                } else if (scale != 1.0 && scale != 0.0) {
                    // Swallow the scale conversion the compiler emits after every scaled setting
                    // (see pushConfig). Rendering it as source would be a SILENT WRONG ANSWER:
                    // recompiling `[#x] * 0.1` applies the field's scale a second time, so the
                    // gate that comes back out is not the gate that went in.
                    const uint16_t save = i;
                    double k = 0; bool haveK = false;
                    if (i < len) {
                        const uint8_t nop = code[i];
                        const uint16_t nb = expr::operand_bytes_(nop);
                        if ((uint16_t)(i + 1 + nb) <= len) {
                            if (nop == expr::OP_PUSH_F) {
                                int32_t v; std::memcpy(&v, code + i + 1, 4);
                                k = v / 100.0; haveK = true; i += 5;
                            } else if (nop == expr::OP_PUSH_F32) {
                                float v; std::memcpy(&v, code + i + 1, 4);
                                k = v; haveK = true; i += 5;
                            } else if (nop == expr::OP_PUSH_I8) {
                                k = (int8_t)code[i + 1]; haveK = true; i += 2;
                            } else if (nop == expr::OP_PUSH_I16) {
                                int16_t v; std::memcpy(&v, code + i + 1, 2);
                                k = v; haveK = true; i += 3;
                            } else if (nop == expr::OP_PUSH_ONE) {
                                k = 1.0; haveK = true; i += 1;
                            }
                        }
                    }
                    const bool matched = haveK && std::abs(k - scale) < 1e-9 &&
                                         i < len && code[i] == expr::OP_MUL;
                    if (matched) i++;                       // consume the MUL as well
                    else         i = save;                  // not our pattern — leave it alone
                }
                st.push_back({path, 100});
                break;
            }
            case expr::OP_PUSH_F: {
                int32_t v; std::memcpy(&v, code + i, 4); i += 4;
                st.push_back({trimNum(v / 100.0), 100});
                break;
            }
            case expr::OP_PUSH_F32: {
                float v; std::memcpy(&v, code + i, 4); i += 4;
                st.push_back({trimNum(v), 100});
                break;
            }
            case expr::OP_PUSH_I16: {
                int16_t v; std::memcpy(&v, code + i, 2); i += 2;
                st.push_back({std::to_string((int)v), 100});
                break;
            }
            case expr::OP_PUSH_I8: {
                const int8_t v = (int8_t)code[i]; i += 1;
                st.push_back({std::to_string((int)v), 100});
                break;
            }
            case expr::OP_PUSH_ZERO: st.push_back({"0", 100}); break;
            case expr::OP_PUSH_ONE:  st.push_back({"1", 100}); break;
            case expr::OP_INTERP: {
                uint16_t id; std::memcpy(&id, code + i, 2); i += 2;
                Frag x; if (!pop(x)) return "<malformed program>";
                st.push_back({"interp(" + std::to_string(id) + ", " + x.text + ")", 100});
                break;
            }
            case expr::OP_NOT: {
                Frag a; if (!pop(a)) return "<malformed program>";
                // Unary binds tighter than comparison (rung 8), so the operand keeps its
                // parentheses unless it is an atom — `not rpm > 4000` would otherwise decompile
                // to something that re-parses as `(not rpm) > 4000`.
                st.push_back({"not " + wrap(a, 8), 8});
                break;
            }
            case expr::OP_ABS: {
                Frag a; if (!pop(a)) return "<malformed program>";
                st.push_back({"abs(" + a.text + ")", 100});
                break;
            }
            case expr::OP_CLAMP: case expr::OP_SELECT: {
                Frag c, a, b;
                if (!pop(b) || !pop(a) || !pop(c)) return "<malformed program>";
                const char* fn = (op == expr::OP_CLAMP) ? "clamp" : "select";
                st.push_back({std::string(fn) + "(" + c.text + ", " + a.text + ", " + b.text + ")", 100});
                break;
            }
            default: {
                Frag a, b;
                if (!pop(b) || !pop(a)) return "<malformed program>";
                const char* sym = nullptr;
                switch (op) {
                    case expr::OP_GT: sym = " > ";  break;
                    case expr::OP_GE: sym = " >= "; break;
                    case expr::OP_LT: sym = " < ";  break;
                    case expr::OP_LE: sym = " <= "; break;
                    case expr::OP_EQ: sym = " == "; break;
                    case expr::OP_NE: sym = " != "; break;
                    case expr::OP_AND: sym = " and "; break;
                    case expr::OP_OR:  sym = " or ";  break;
                    case expr::OP_ADD: sym = " + ";  break;
                    case expr::OP_SUB: sym = " - ";  break;
                    case expr::OP_MUL: sym = " * ";  break;
                    case expr::OP_DIV: sym = " / ";  break;
                    case expr::OP_MIN: case expr::OP_MAX: {
                        const char* fn = (op == expr::OP_MIN) ? "min" : "max";
                        st.push_back({std::string(fn) + "(" + a.text + ", " + b.text + ")", 100});
                        break;
                    }
                    case expr::OP_BIT:
                        st.push_back({"bit(" + a.text + ", " + b.text + ")", 100});
                        break;
                    default: return "<unknown opcode>";
                }
                if (!sym) break;
                const int pr = precOf(op);
                // Left-assoc: the right operand needs parens at EQUAL precedence too, or
                // `a - (b - c)` decompiles as `a - b - c`, which is a different number.
                st.push_back({wrap(a, pr) + sym + wrap(b, pr + 1), pr});
                break;
            }
        }
    }
    if (st.size() != 1) return "<malformed program>";
    return st.front().text;
}

// Reverse-lookup a baked config offset to its meta path (and its display scale, which the
// decompiler needs to recognise the conversion the compiler emitted). Linear over the
// descriptor, which is fine: decompiling happens when a tune is opened, not per frame.
std::string ExprCompiler::configPathAt(const MetaModel& meta, int offset, double* scaleOut) {
    if (scaleOut) *scaleOut = 1.0;
    for (const auto& [path, f] : meta.config())
        if (f.offset == offset) {
            if (scaleOut) *scaleOut = f.scale;
            return path;
        }
    // Array elements: walk each array's elements/fields for a matching absolute offset.
    //
    // BY COUNT, NOT BY elementIds.size(). Only arrays whose elements are NAMED carry ids — sensors have
    // "clt", outputs have nothing — so iterating the id list visited zero elements for every array with
    // plain numeric elements, and every reference into one decompiled as a raw byte address. An output's
    // wizard parameter came back as "config@119348" rather than "outputs.output[0].param_a", which is
    // an expression the reader cannot check and cannot edit. The struct's own comments already say the
    // rule — "empty = use the index" — and MetaModel applies it elsewhere; this did not.
    for (const auto& [aname, arr] : meta.configArrays()) {
        for (int e = 0; e < arr.count; e++) {
            const std::string key = (e < (int)arr.elementIds.size() && !arr.elementIds[e].empty())
                                        ? arr.elementIds[e] : std::to_string(e);
            const int ebase = arr.baseOffset + e * arr.stride;
            for (const auto& f : arr.fields) {
                if (ebase + f.relOffset == offset) {
                    if (scaleOut) *scaleOut = f.scale;
                    return aname + "[" + key + "]." + f.name;
                }
            }
            // ...and the repeated sub-structs INSIDE an element (precond[4], cand[4]), which were not
            // walked at all: a condition referring to one of them had the same raw-address problem.
            for (const auto& sa : arr.arrays) {
                for (int i = 0; i < sa.count; i++) {
                    const int sbase = ebase + sa.relOffset + i * sa.stride;
                    for (const auto& sf : sa.fields) {
                        if (sbase + sf.relOffset == offset) {
                            if (scaleOut) *scaleOut = sf.scale;
                            return aname + "[" + key + "]." + sa.name
                                 + "[" + std::to_string(i) + "]." + sf.name;
                        }
                    }
                }
            }
        }
    }
    return "config@" + std::to_string(offset);
}
