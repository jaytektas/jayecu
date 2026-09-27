#pragma once

#include <algorithm>
#include <string>
#include <vector>

// A sigil provider: one address space of named values, addressed in expressions by a leading sigil
// character. Ported from omnidyno's ISigilResolver (Qt-free). studio-jf registers three:
//   #  config (dictionary) values      — [#electronic_throttle.etb[0].min_tps_pct]
//   $  telemetry (firmware bus) values — [$rpm]
//   @  laid-out widget state           — [@ve_table_1.rowcount]   (Phase 2)
// Resolvers read LIVE state (Cache / widget registry), so there is no per-call data frame.
class ISigilResolver {
public:
    virtual char        sigil() const = 0;                          // the addressing character (#, $, @)
    virtual double      resolveSigil(const std::string& name) const = 0;   // name WITHOUT the sigil char
    virtual std::vector<std::string> available() const = 0;         // every name this resolver can resolve

    // Does this resolver own `name`? A MEMBERSHIP test, not a listing: resolving a bare identifier used to
    // ask every resolver for available() and search the result, so each identifier in each expression built
    // a vector of every config path and every telemetry channel — thousands of strings, per token, per
    // frame. Override with a lookup; the default keeps a resolver that has not been updated working.
    virtual bool provides(const std::string& name) const {
        const auto av = available();
        return std::find(av.begin(), av.end(), name) != av.end();
    }

    // Optional metadata about a name (for pickers / formatting). Default: unknown.
    virtual std::string getUnit(const std::string&)       const { return {}; }
    virtual std::string getQuantity(const std::string&)   const { return {}; }
    virtual std::string getLabel(const std::string&)      const { return {}; }
    virtual std::string getCategory(const std::string&)   const { return {}; }
    virtual std::string getVisibility(const std::string&) const { return {}; }
    virtual std::string getFormula(const std::string&)    const { return {}; }

    virtual ~ISigilResolver() = default;
};
