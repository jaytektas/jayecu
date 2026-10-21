#pragma once

// EcuPickRows — one ECU as a row of a ListPickerDialog, for Open ECU… and for Connect when more than one
// ECU answers. Both questions are "which of my ECUs is this", and the board and uid alone answer it badly:
// two jaytek_v1s differ only in 24 hex digits. The car answers it — the tune's Vehicle Identity — so the
// row leads with it when the tune names one, and the facts panel under the list says the rest in full.
// The row carries the WHOLE id, not a tail: chips from one batch share their last digits.

#include "ListPickerDialog.h"
#include "../model/Ecu.h"

#include <string>

namespace ecurows {

// What the car is called: its Name, else Make and Model, else the Engine. Empty when the tune says nothing.
inline std::string carName(const Ecu::Vehicle& v) {
    if (!v.name.empty()) return v.name;
    std::string mm = v.make;
    if (!v.model.empty()) mm += (mm.empty() ? "" : " ") + v.model;
    return mm.empty() ? v.engine : mm;
}

// "2026-10-21T09:14:03" (UTC, as ecu.json keeps it) → "2026-10-21 09:14 UTC".
inline std::string seenText(const std::string& iso) {
    if (iso.size() < 16) return iso;
    return iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC";
}

// `e` is the library's entry (its uid empty for an ECU the library has never seen); `uid` and `board` are
// what the ECU itself says. `port` is set when the ECU is on a port now, with its firmware `fw`.
inline ListPickerDialog::Row row(const Ecu::Summary& e, const std::string& uid, const std::string& board,
                                 const std::string& port = {}, const std::string& fw = {}) {
    const Ecu::Vehicle& v = e.vehicle;
    const std::string label = e.label.empty() ? board : e.label;
    const std::string car = carName(v);
    ListPickerDialog::Row r;
    r.text = (car.empty() ? label : car + "  \xC2\xB7  " + label) + "  \xC2\xB7  " + uid
           + (port.empty() ? std::string() : "  (" + port + ")");
    std::string makeModel = v.make;
    if (!v.model.empty()) makeModel += (makeModel.empty() ? "" : " ") + v.model;
    r.facts = {
        { "Name",    v.name },
        { "Vehicle", makeModel },
        { "Engine",  v.engine },
        { "VIN",     v.vin },
        { "Notes",   v.notes },
        { "Tune",    e.uid.empty() ? std::string("not in this studio's library yet") : e.currentTune },
        { "Board",   label },
        { "ECU id",  uid },
    };
    if (!fw.empty()) r.facts.push_back({ "Firmware", fw });
    if (!e.uid.empty()) r.facts.push_back({ "Last seen", seenText(e.lastSeen)
                                            + (e.lastPort.empty() ? std::string() : " on " + e.lastPort) });
    return r;
}

} // namespace ecurows
