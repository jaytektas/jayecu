#include "UnitManager.h"

#include <j/config/Settings.h>

UnitManager &UnitManager::instance()
{
    static UnitManager inst;
    return inst;
}

UnitManager::UnitManager()
{
    registerDefaults();
    loadPreferences();
}

void UnitManager::registerDefaults()
{
    // Torque
    Quantity torque;
    torque.id = "Torque";
    torque.label = "Torque";
    torque.baseUnit = "Nm";
    torque.units.push_back(Unit{"Nm", "Nm", 1.0, 0.0, "%.1f"});
    torque.units.push_back(Unit{"lb-ft", "lb-ft", 0.73756, 0.0, "%.1f"});
    torque.units.push_back(Unit{"kg-m", "kg-m", 0.10197, 0.0, "%.2f"});
    m_quantities[torque.id] = torque;

    // Power
    Quantity power;
    power.id = "Power";
    power.label = "Power";
    power.baseUnit = "kW";
    power.units.push_back(Unit{"W", "W", 1000.0, 0.0, "%.0f"});
    power.units.push_back(Unit{"kW", "kW", 1.0, 0.0, "%.1f"});
    power.units.push_back(Unit{"hp", "hp", 1.34102, 0.0, "%.1f"});
    power.units.push_back(Unit{"ps", "ps", 1.35962, 0.0, "%.1f"});
    m_quantities[power.id] = power;

    // Linear Speed
    Quantity speed;
    speed.id = "Speed";
    speed.label = "Linear Speed";
    speed.baseUnit = "m/s";
    speed.units.push_back(Unit{"m/s", "m/s", 1.0, 0.0, "%.1f"});
    speed.units.push_back(Unit{"km/h", "km/h", 3.6, 0.0, "%.1f"});
    speed.units.push_back(Unit{"mph", "mph", 2.23694, 0.0, "%.1f"});
    m_quantities[speed.id] = speed;

    // Angle. ONE unit, and that is the point: an angle has no conversion anybody tunes in — nobody asks
    // for ignition advance in radians — but a unit belongs to a QUANTITY or unitLabel() has nowhere to
    // look, so "deg" printed as "deg" where a temperature printed as "°C". The quantity exists to give
    // the symbol a home. (Add rad here if a use ever appears; the preference machinery already handles
    // a quantity with choices.)
    Quantity angle;
    angle.id = "Angle";
    angle.label = "Angle";
    angle.baseUnit = "deg";
    angle.units.push_back(Unit{"deg", "\xC2\xB0", 1.0, 0.0, "%.1f"});
    m_quantities[angle.id] = angle;

    // Angular Speed
    Quantity angular;
    angular.id = "AngularSpeed";
    angular.label = "Angular Speed";
    angular.baseUnit = "RPM";
    angular.units.push_back(Unit{"RPM", "RPM", 1.0, 0.0, "%.0f"});
    angular.units.push_back(Unit{"rad/s", "rad/s", 0.10471975511965977, 0.0, "%.1f"});   // 2*pi / 60
    angular.units.push_back(Unit{"deg/s", "deg/s", 6.0, 0.0, "%.0f"});                   // 360 / 60
    angular.units.push_back(Unit{"RPS", "RPS", 0.016666666666666666, 0.0, "%.2f"});      // 1 / 60
    m_quantities[angular.id] = angular;

    // Frequency
    Quantity freq;
    freq.id = "Frequency";
    freq.label = "Frequency";
    freq.baseUnit = "Hz";
    freq.units.push_back(Unit{"Hz", "Hz", 1.0, 0.0, "%.1f"});
    freq.units.push_back(Unit{"kHz", "kHz", 0.001, 0.0, "%.2f"});
    freq.units.push_back(Unit{"MHz", "MHz", 0.000001, 0.0, "%.3f"});
    m_quantities[freq.id] = freq;

    // Time. The ECU is full of it and none of it was convertible: dwell is microseconds, injector pulse
    // width milliseconds, run time seconds, and each was stuck in whatever unit the schema happened to
    // declare — a dwell of 3503 could not be read as 3.5 ms, and a band written as "200ms" had no
    // quantity to belong to. Base is the second, so a threshold converts between all of them.
    Quantity time;
    time.id = "Time";
    time.label = "Time";
    time.baseUnit = "s";
    time.units.push_back(Unit{"s", "s", 1.0, 0.0, "%.2f"});
    time.units.push_back(Unit{"ms", "ms", 1000.0, 0.0, "%.1f"});
    time.units.push_back(Unit{"us", "\xC2\xB5s", 1000000.0, 0.0, "%.0f"});
    time.units.push_back(Unit{"min", "min", 1.0 / 60.0, 0.0, "%.2f"});
    m_quantities[time.id] = time;

    // Pressure
    Quantity pressure;
    pressure.id = "Pressure";
    pressure.label = "Pressure";
    pressure.baseUnit = "kPa";
    pressure.units.push_back(Unit{"Pa", "Pa", 1000.0, 0.0, "%.0f"});
    pressure.units.push_back(Unit{"kPa", "kPa", 1.0, 0.0, "%.1f"});
    pressure.units.push_back(Unit{"bar", "bar", 0.01, 0.0, "%.2f"});
    pressure.units.push_back(Unit{"psi", "psi", 0.145038, 0.0, "%.1f"});
    m_quantities[pressure.id] = pressure;

    // Temperature
    Quantity temp;
    temp.id = "Temperature";
    temp.label = "Temperature";
    temp.baseUnit = "C";
    temp.units.push_back(Unit{"C", "°C", 1.0, 0.0, "%.1f"});
    temp.units.push_back(Unit{"F", "°F", 1.8, 32.0, "%.1f"});
    temp.units.push_back(Unit{"K", "K", 1.0, 273.15, "%.1f"});
    m_quantities[temp.id] = temp;

    // Pulse Density — a speed pickup's calibration. It is a COUNT PER DISTANCE, so it belongs to the
    // unit system exactly as the speed it produces does: the same wheel is 2450 pulses/km and 3943
    // pulses/mile, and a driver working in miles should type the number they measured in miles. The
    // stored byte is always per kilometre; this is what turns it into the other one and back.
    Quantity pulses;
    pulses.id = "PulseDensity";
    pulses.label = "Pulse Density";
    pulses.baseUnit = "pulses/km";
    pulses.units.push_back(Unit{"pulses/km", "pulses/km", 1.0, 0.0, "%.0f"});
    pulses.units.push_back(Unit{"pulses/mi", "pulses/mi", 1.609344, 0.0, "%.0f"});
    m_quantities[pulses.id] = pulses;

    // RPM per unit speed — a gear's ratio, measured rather than calculated (it includes the tyres
    // actually fitted). It is an RPM divided by a SPEED, so it converts with the speed underneath it:
    // the same gear is 60 RPM per km/h and 96.6 RPM per mph. Sharing the pulse-density factor is not a
    // coincidence — both are "per unit distance" with the same distance in the denominator.
    Quantity rpmPerSpeed;
    rpmPerSpeed.id = "RpmPerSpeed";
    rpmPerSpeed.label = "RPM per Speed";
    rpmPerSpeed.baseUnit = "RPM/km/h";
    rpmPerSpeed.units.push_back(Unit{"RPM/km/h", "RPM/km/h", 1.0, 0.0, "%.1f"});
    rpmPerSpeed.units.push_back(Unit{"RPM/mph", "RPM/mph", 1.609344, 0.0, "%.1f"});
    m_quantities[rpmPerSpeed.id] = rpmPerSpeed;

    // Voltage — analog sensor raw axis (mV) selectable as V. base = (value - offset) / factor.
    Quantity voltage;
    voltage.id = "Voltage";
    voltage.label = "Voltage";
    voltage.baseUnit = "mV";
    voltage.units.push_back(Unit{"mV", "mV", 1.0, 0.0, "%.0f"});
    voltage.units.push_back(Unit{"V", "V", 0.001, 0.0, "%.2f"});
    m_quantities[voltage.id] = voltage;
}

void UnitManager::configureAnalogRaw(int fullScale, int avFullscaleMv)
{
    if (fullScale <= 0 || avFullscaleMv <= 0)
        return;
    const double mvPerCount = static_cast<double>(avFullscaleMv) / static_cast<double>(fullScale);

    // Distinct unit ids (ADC / ADC_mV / ADC_V) so they don't collide with the Voltage quantity's mV/V
    // in findQuantityForUnit() — the raw pin axis is its OWN quantity (a sensor's eng volts, e.g. a
    // divided battery, is NOT the pin voltage). Labels stay clean: counts / mV / V.
    Quantity a;
    a.id = "AnalogRaw";
    a.label = "Analog Raw";
    a.baseUnit = "ADC";
    a.units.push_back(Unit{"ADC", "counts", 1.0, 0.0, "%.0f"});
    a.units.push_back(Unit{"ADC_mV", "mV", mvPerCount, 0.0, "%.0f"});
    a.units.push_back(Unit{"ADC_V", "V", mvPerCount / 1000.0, 0.0, "%.3f"});
    m_quantities[a.id] = a;

    if (!m_preferences.count(a.id))   // default display = V (5.00); persists once the user changes it
        m_preferences[a.id] = "ADC_V";
    preferencesChanged.emit();
}

double UnitManager::convert(double value, const std::string &fromUnit, const std::string &toUnit) const
{
    if (fromUnit == toUnit || fromUnit.empty() || toUnit.empty())
        return value;

    std::string qId = findQuantityForUnit(fromUnit);
    if (qId.empty() || qId != findQuantityForUnit(toUnit))
        return value;

    const Quantity &q = m_quantities.at(qId);
    const Unit *uFrom = nullptr;
    const Unit *uTo = nullptr;
    for (const auto &u : q.units) {
        if (u.id == fromUnit)
            uFrom = &u;
        if (u.id == toUnit)
            uTo = &u;
    }
    if (!uFrom || !uTo)
        return value;

    const double base = (value - uFrom->offset) / uFrom->factor;   // from -> base
    return (base * uTo->factor) + uTo->offset;                     // base -> to
}

std::string UnitManager::preferredUnit(const std::string &quantity) const
{
    auto pit = m_preferences.find(quantity);
    if (pit != m_preferences.end())
        return pit->second;
    auto qit = m_quantities.find(quantity);
    if (qit != m_quantities.end())
        return qit->second.baseUnit;
    return {};
}

std::string UnitManager::displayUnitFor(const std::string &sourceUnit) const
{
    const std::string qId = findQuantityForUnit(sourceUnit);
    auto pit = m_preferences.find(qId);
    if (qId.empty() || pit == m_preferences.end())
        return sourceUnit;   // no quantity match or no preference: show as-is
    return pit->second;
}

std::string UnitManager::unitLabel(const std::string &unitId) const
{
    const std::string qId = findQuantityForUnit(unitId);
    if (!qId.empty())
        for (const Unit &u : m_quantities.at(qId).units)
            if (u.id == unitId)
                return u.label;          // pretty form ("°C", "psi"), if registered
    return unitId;                       // unknown unit: show the raw string
}

std::string UnitManager::unitFormat(const std::string &unitId) const
{
    const std::string qId = findQuantityForUnit(unitId);
    if (!qId.empty())
        for (const Unit &u : m_quantities.at(qId).units)
            if (u.id == unitId)
                return u.format;
    return {};                           // unregistered: the unit has no opinion, ask the channel
}

// The same answer as a decimal count, for the spin boxes that need a STEP rather than a format. Parsed
// from the unit's own format so there is still exactly one place the precision is stated.
int UnitManager::unitDigits(const std::string &unitId) const
{
    const std::string f = unitFormat(unitId);
    const size_t dot = f.find('.');
    if (dot == std::string::npos || dot + 1 >= f.size()) return -1;
    int n = 0; size_t i = dot + 1;
    for (; i < f.size() && f[i] >= '0' && f[i] <= '9'; ++i) n = n * 10 + (f[i] - '0');
    return (i > dot + 1) ? n : -1;
}

void UnitManager::setPreferredUnit(const std::string &quantity, const std::string &unitId)
{
    auto it = m_preferences.find(quantity);
    if (it != m_preferences.end() && it->second == unitId)
        return;
    m_preferences[quantity] = unitId;
    savePreferences();
    preferencesChanged.emit();
}

void UnitManager::setPreferences(const std::map<std::string, std::string> &prefs)
{
    bool changed = false;
    for (const auto &[key, val] : prefs) {
        auto it = m_preferences.find(key);
        if (it == m_preferences.end() || it->second != val) {
            m_preferences[key] = val;
            changed = true;
        }
    }
    if (changed) {
        savePreferences();
        preferencesChanged.emit();
    }
}

void UnitManager::applySystemDefaults(UnitSystem system)
{
    setPreferences(getSystemDefaults(system));
}

std::map<std::string, std::string> UnitManager::getSystemDefaults(UnitSystem system) const
{
    std::map<std::string, std::string> defaults;
    if (system == UnitSystem::Metric) {
        defaults["Torque"] = "Nm";
        defaults["Power"] = "kW";
        defaults["Speed"] = "km/h";
        defaults["PulseDensity"] = "pulses/km";
        defaults["RpmPerSpeed"] = "RPM/km/h";
        defaults["Pressure"] = "kPa";
        defaults["Temperature"] = "C";
    } else {
        defaults["Torque"] = "lb-ft";
        defaults["Power"] = "hp";
        defaults["Speed"] = "mph";
        defaults["PulseDensity"] = "pulses/mi";
        defaults["RpmPerSpeed"] = "RPM/mph";
        defaults["Pressure"] = "psi";
        defaults["Temperature"] = "F";
    }
    return defaults;
}

std::vector<std::string> UnitManager::quantities() const
{
    std::vector<std::string> keys;
    keys.reserve(m_quantities.size());
    for (const auto &[id, q] : m_quantities)
        keys.push_back(id);
    return keys;
}

std::string UnitManager::findQuantityForUnit(const std::string &unitId) const
{
    for (const auto &[qid, q] : m_quantities)
        for (const auto &u : q.units)
            if (u.id == unitId)
                return qid;
    return {};
}

void UnitManager::loadPreferences()
{
    // Preferences persist under the "unitPreferences." key prefix in the app settings. We read one
    // per known quantity (quantities are registered before this runs) rather than enumerating keys.
    auto &s = jf::JSettings::instance();
    for (const auto &[qid, q] : m_quantities) {
        const std::string key = "unitPreferences." + qid;
        jf::JVariant v = s.value(key);
        std::string pref = v.toString();
        if (!pref.empty())
            m_preferences[qid] = pref;
    }
}

void UnitManager::savePreferences()
{
    auto &s = jf::JSettings::instance();
    for (const auto &[qid, unitId] : m_preferences)
        s.setQuiet("unitPreferences." + qid, jf::JVariant(unitId));
    s.save();
}
