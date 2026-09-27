#pragma once

#include <string>
#include <vector>
#include <map>

#include <j/core/Signal.h>

// Unit system + conversion. A Quantity (Pressure, Temperature, …) owns a set
// of Units, each defined as `base = (value - offset) / factor`. convert() routes any two units of
// the same quantity through their shared base. The quantity of a unit is discovered from its id, so
// a meta field that only carries a unit string ("kPa") can still be displayed in bar/psi.
//
// Per-quantity user preferences persist via JSettings; widgets listen on preferencesChanged and
// re-convert their stored ranges. The Cache hands raw engineering units (the meta scale); the
// display unit is whatever the widget/preferences select.
class UnitManager {
public:
    struct Unit {
        std::string id;
        std::string label;
        double factor = 1.0;   // multiplier to base unit
        double offset = 0.0;   // base = (value - offset) / factor
        // HOW THIS UNIT IS PRINTED, and the only place that answer lives. Precision belongs to the unit,
        // because it is the unit that decides whether a decimal carries information: the same pin is
        // 3.824 V and 3824 mV, and a format stamped on the widget cannot know which one it is being asked
        // for -- a stored "%.3f" is right in volts and comic in millivolts. Held as the FORMAT ITSELF, not
        // a decimal count, so the unit's default and a widget's override are the same kind of thing and
        // the rule is one line: the widget's if present, else the unit's. It also lets a unit say
        // something a digit count cannot ("%.0f" for RPM, a padded form for a fixed-width readout).
        std::string format;
    };

    struct Quantity {
        std::string id;
        std::string label;
        std::string baseUnit;
        std::vector<Unit> units;
    };

    enum class UnitSystem { Metric, Imperial };

    static UnitManager &instance();

    // Emitted when any preference changes (widgets re-convert their ranges).
    jf::JSignal<> preferencesChanged;

    double convert(double value, const std::string &fromUnit, const std::string &toUnit) const;

    // Configure the raw-analog quantity (ADC counts <-> mV <-> V) from the board's hardware meta. The
    // ECU is counts-native; this is how the CLIENT converts for display/entry. mvPerCount =
    // avFullscaleMv / fullScale. Default display unit is V (the user can switch to mV / counts). Idempotent.
    void configureAnalogRaw(int fullScale, int avFullscaleMv);

    std::string preferredUnit(const std::string &quantity) const;
    void setPreferredUnit(const std::string &quantity, const std::string &unitId);
    void setPreferences(const std::map<std::string, std::string> &prefs);
    void applySystemDefaults(UnitSystem system);
    std::map<std::string, std::string> getSystemDefaults(UnitSystem system) const;

    std::vector<std::string> quantities() const;
    Quantity getQuantity(const std::string &id) const {
        auto it = m_quantities.find(id);
        return it != m_quantities.end() ? it->second : Quantity{};
    }
    std::string findQuantityForUnit(const std::string &unitId) const;

    // The preferred unit to display a source unit in (its quantity's preference), or the source unit
    // itself when no preference / unknown quantity. The widget conversion target.
    std::string displayUnitFor(const std::string &sourceUnit) const;

    // The pretty label for a unit id ("C" -> "°C", "kPa" -> "kPa"); the id itself if unregistered.
    std::string unitLabel(const std::string &unitId) const;

    // The printf format a unit is read in ("%.3f" for volts); empty when the unit is not registered, so
    // a caller can fall back to whatever the channel itself declares. The one authority for display
    // precision. unitDigits() is the same answer as a decimal count, for spin boxes that need a step.
    std::string unitFormat(const std::string &unitId) const;
    int unitDigits(const std::string &unitId) const;

    void loadPreferences();
    void savePreferences();

private:
    UnitManager();
    void registerDefaults();

    std::map<std::string, Quantity> m_quantities;
    std::map<std::string, std::string> m_preferences;   // quantityId -> unitId
};
