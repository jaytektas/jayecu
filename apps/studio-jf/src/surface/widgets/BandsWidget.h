#pragma once

// BandsWidget — the calibration of a MULTI-POSITION SWITCH: one voltage band per position.
//
// Several buttons on one analog wire through a resistor ladder (a cruise stalk, steering-wheel buttons, a
// rotary selector) each put their own voltage on the pin, and the firmware decodes WHICH position the pin
// is in (Stages.h, decode_bands). The bands live in the sensor's ordinary calibration arrays — breakpoint
// pair [2k, 2k+1] is band k's low and high edge in raw ADC counts, and value [2k] its position number — so
// a curve editor over the same bytes would show a sawtooth that means nothing. This is what edits them
// instead, and CanvasWidget::showsCalibrationOf keeps the two from ever being on screen together.
//
// THE WORKFLOW IS HOLD AND CAPTURE, not type. A ladder's voltages come from resistors in a switch nobody
// has a data sheet for, and the ECU reads its pins a few percent off a meter (the board's front-end gain),
// so a number typed from a multimeter lands in the wrong place. Hold the button, press Capture on its row:
// the band is centred on what THE ECU reads, sized to stay clear of its neighbours and of the 0 V floor.
// The edges can still be typed, for the one-in-a-hundred switch that needs it.
//
// THE SAME RULES AS THE FIRMWARE, said before the calibration is written: whole bands, low <= high, a real
// gap between neighbours, and nothing under an ARMED Raw Low Threshold — which is the user's own
// declaration of where a fault starts, not a fixed floor: a button that grounds the line is a real
// position on most cruise stalks. The firmware refuses a calibration that breaks one of the structural
// rules; the editor says which rule, and which band, while the user is still looking at it.

#include "../CanvasWidget.h"
#include <j/core/JDoubleSpinBox.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

class BandsWidget : public CanvasWidget {
public:
    explicit BandsWidget(jf::JSceneGraph& g) : CanvasWidget(g, "bands") {}
    std::string elementType()  const override { return "bands"; }
    std::string paletteTitle() const override { return "Switch Positions (voltage bands)"; }
    float       defaultW()     const override { return 600.f; }
    float       defaultH()     const override { return 360.f; }
    bool        interactive()  const override { return true; }
    // Shown for exactly the calibration a curve is NOT: a multi-position switch's.
    bool showsCalibrationOf(const std::string& typeId) const override { return typeId == "multi_switch"; }

    void render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;
    bool handleControlInput(const jf::JRect& screen, const ControlInput& in) override;

    // One band as stored: its edges in raw ADC counts, and its position number.
    struct Band { double lo = 0.0, hi = 0.0; int pos = 0; };

    // Geometry, shared by the paint and the hit test so a control is hit where it is drawn.
    static constexpr float kLiveH = 26.f, kHeadH = 22.f, kRowH = 30.f;

    // Test access — the questions a user asks, answered from the stored bytes.
    std::vector<Band> bandsForTest() const { return _read(_store()); }
    std::string       problemForTest() const { const Store s = _store(); return _problem(s, _read(s)); }
    // Capture `counts` into band `row` (-1 = a new band); "" on success, else why not.
    std::string       captureForTest(int row, double counts) { return _capture(row, counts); }
    bool              removeForTest(int row) { return _remove(row); }
    int               rowControlsForTest() const { return static_cast<int>(m_rows.size()); }

private:
    // Where this widget's bytes are, resolved from its binding each time it is asked (the page may have
    // been pointed at another input, the type may have changed under it).
    struct Store {
        bool        isMulti = false;     // bound to a multi-position switch's calibration
        bool        analog  = false;     // …read through an analog pin (else it is a plain 2-state switch)
        std::string base;                // "sensors.sensor[aux_1]"
        int         breaks = 0, bStride = 0;  std::string bType;   // cal_raw
        int         cells  = 0, cStride = 0;  std::string cType;   // cal_val
        double      cScale = 1.0;
        int         nBase  = -1, nMax = 0;                          // cal_n and its ceiling
        double      floorCounts = 0.0;   // the user's armed Raw Min, in counts (0 = not armed)
        std::string rawChan;             // the pin's raw channel ("hw_av5"), "" when unassigned
        std::string unit, unitLabel;     // the AnalogRaw display unit — counts, mV or V
        double      perCount = 1.0;      // one count, in that unit
        int         digits = 3;
    };
    Store             _store() const;
    std::vector<Band> _read(const Store& s) const;
    void              _write(const Store& s, std::vector<Band> bands, int oldN, const char* what);
    std::string       _problem(const Store& s, const std::vector<Band>& b) const;
    std::string       _capture(int row, double counts);
    bool              _remove(int row);
    void              _syncRows(const Store& s, const std::vector<Band>& b);
    void              _writeEdge(int row, bool high, double displayValue);
    void              _writePos(int row, double pos);

    struct Row { std::unique_ptr<jf::JDoubleSpinBox> pos, lo, hi; };
    std::vector<Row> m_rows;
    jf::JControl*    m_active  = nullptr;   // the box the keyboard is going to
    bool             m_syncing = false;     // suppress the echo while pushing values in
    std::string      m_note;                // what the last Capture / Add did, or why it could not
    float            m_liveH = kLiveH;      // the pin line as painted (it wraps); the hit test reads it
    bool             m_noteBad = false;
    // Is the ECU's reading LIVE? Frames arriving, not merely a channel that once had a value.
    uint64_t         m_lastFrames = 0;
    std::chrono::steady_clock::time_point m_lastFrameAt{};
    bool             _liveNow(const Cache& c, const std::string& chan, double& counts);
    std::string      m_unitBuilt;           // the display unit the row boxes were made for
};
