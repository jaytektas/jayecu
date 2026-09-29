#pragma once

// AirModelLoad — the load axis each air model wants its target-lambda and timing maps on, and moving them
// there when the user changes model.
//
// Target Lambda and the ignition map are indexed on a LOAD, and what a good load is depends on how the
// air is worked out: speed-density has MAP (Fuel Load), Alpha-N has the throttle, and MAF and Blend have
// Charge Load — the air in the cylinder as a % of a full charge, the one load that is continuous through
// Blend's crossover. Changing the air model does not move them by itself: a table's cells were tuned
// against its axis, and re-pointing it silently would leave every cell meaning something else. So the
// studio ASKS (main.cpp), and this does the moving when the answer is yes — the axis's signal, and its
// breakpoints converted into the new load's terms (kPa <-> % of a full charge at 101.3 kPa; evenly spread
// for throttle). The cells are kept; the question says to check them.

#include <string>
#include <vector>

class Cache;

namespace airload {

// "fuel_load", "tps" or "charge_load" for fuel_model 0..3; "" for an unknown model.
std::string preferredLoad(int fuelModel);

// The load-indexed maps this studio keeps in step with the air model ("module.table").
const std::vector<std::string>& loadTables();

// Which of those are NOT already on the model's preferred load (and so would be re-pointed).
std::vector<std::string> offTarget(const Cache& c, int fuelModel);

// The label a user knows a load signal by ("Charge Load"), from the meta; the id when it has none.
std::string loadLabel(const Cache& c, const std::string& signal);

// Re-point `tables` at `signal`, converting their load breakpoints. One undo step.
void apply(Cache& c, const std::vector<std::string>& tables, const std::string& signal);

}  // namespace airload
