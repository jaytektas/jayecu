#pragma once

// Trigger-wheel designer UI (a library + editor; the config is written only by the Settings tab's
// Apply to ECU). Layout:
//   • TriggerDesignerView — the CENTER editor tab, itself a JTabWidget of child tabs:
//       - Visualizer : firmware-faithful dial + per-stream signal trace of the WORKING wheel (full size)
//       - Settings   : the form that edits the working wheel (crank/cam/TDC …)
//     More tabs can be added as the settings grow, so the visualizer never gets squished.
//   • TriggerLibraryDock — the LEFT dock: the wheel library list. Selecting a wheel LOADS it into the
//     editor to preview/edit (never a config write).
// The pure-logic core (buildWheel / decodeWheel / wheelGeometry / rolesForWheel) does the real work.

#include "TriggerDiagram.h"   // the shared painter — dial + per-stream trace over WheelGeometry
#include "WrapText.h"
#include <j/core/JWidget.h>
#include <j/core/JTabWidget.h>
#include <j/core/JContainer.h>
#include <j/core/JDataGrid.h>
#include <j/core/JTextHelper.h>
#include <j/core/JStyle.h>
#include <j/core/JLabel.h>
#include <j/core/JLineEdit.h>
#include <j/core/JComboBox.h>
#include <j/core/JSpinBox.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JCheckBox.h>
#include <j/core/JButton.h>
#include <j/graphics/VectorGraphics.h>

#include "../model/Cache.h"
#include "../model/TriggerWheel.h"
#include "../model/TriggerGeometry.h"
#include "../model/TriggerFit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>


// A cam's "which cam" is a SLOT of trigger.streams[]: 2 Intake B1, 3 Exhaust B1, 4 Intake B2,
// 5 Exhaust B2 (kSlotCamBase, TriggerWheel.h). It is NOT the 1-based display role the dial labels with.

// Shared working-wheel model: the Settings tab edits it, the Visualizer tab draws it. `changed` fires
// on any edit so the visualizer repaints. Never the live config — installing is Apply to ECU's job.
struct TriggerEditModel {
    WheelParams   working{};
    // Where the crank has been wound to for the DRAWING, 0 = TDC #1. View state only: it is not part
    // of the wheel, never saved and never written to the config. It exists so the user can turn the
    // engine over on screen and watch the reference tooth come round to the pickup.
    double        crankAngle = 0.0;
    // Has the working wheel been edited since it was loaded, saved or installed? The designer is an
    // EDITOR — every keystroke and every drag changes the working copy and nothing else — so without
    // this the user's only clue that a wheel had been altered was remembering that they had altered
    // it, and picking another library entry threw the work away without a word.
    bool          dirty = false;
    // THE MEASURED WHEEL, from a capture off the running engine. Kept beside the working wheel and
    // never merged into it: what the engine is doing and what the tune says it does are two
    // different claims, and the whole value of showing both is being able to see them disagree.
    // Empty until a capture has been fitted.
    triggerfit::CaptureFit measured{};
    jf::JSignal<> changed;
};

// ============================ Visualizer tab =====================================================
class TriggerVisualizer : public jf::JWidget {
public:
    TriggerVisualizer(jf::JSceneGraph& g, TriggerEditModel& m)
        : jf::JWidget(g, "TriggerVisualizer"), model_(m) {
        model_.changed.connect([this] { invalidate(); });
    }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        const jf::JRect b = bounds();
        if (b.width < 48.f || b.height < 48.f) return;

        const Wheel w = buildWheel(model_.working);
        const WheelGeometry g = wheelGeometry(w);
        const int n = static_cast<int>(g.streams.size());

        // Two lines: what it is, and what is wrong — and the second WRAPS, pushing the dial down by the
        // lines it takes, rather than stopping mid-sentence at the pane's edge.
        const std::vector<WheelIssue> issues = validateWheel(w);
        std::string issueText;
        if (!issues.empty()) {
            issueText = issues.front().text;
            if (issues.size() > 1) issueText += "   (+" + std::to_string(issues.size() - 1) + " more)";
        }
        const float pad = 14.f, rowH = 22.f;
        const float readoutH = 44.f + wraptext::extra(issueText, b.width - 2.f * pad);
        const float traceH  = std::clamp(n * rowH + 12.f, 30.f, b.height * 0.5f);
        const float traceY  = b.y + b.height - traceH;
        // TURNING THE ENGINE OVER IS A VIEW CONTROL, so it lives with the view. It was a spin box near
        // the bottom of the form — the one row there that changes nothing about the wheel — and the
        // gesture it exists for (wind the crank round and watch the reference tooth come to the
        // pickup) is the last thing you want to do by typing degrees into a box on the other side of
        // the pane. Dragging the wheel itself cannot serve: that moves the OFFSET, deliberately.
        const float scrubH  = 28.f;
        const float scrubY  = traceY - scrubH;
        const float dialTop = b.y + readoutH, dialBot = scrubY - pad;
        const float cx = b.x + b.width * 0.5f;
        const float cy = dialTop + (dialBot - dialTop) * 0.5f;
        const float R  = std::min(b.width, dialBot - dialTop) * 0.42f;
        const float labelW = 66.f, plotX = b.x + pad + labelW, plotW = b.width - 2 * pad - labelW;

        dialCx_ = cx; dialCy_ = cy; dialR_ = R;      // remembered for the drag hit-test below

        jf::JVectorCanvas vg;
        vg.setAntiAlias(1.2f);
        if (R > 10.f) trigger_ui::drawDial(vg, g, cx, cy, R, model_.working.sensorAngle, model_.crankAngle);
        trigger_ui::drawTrace(vg, g, plotX, traceY, plotW, traceH, rowH);
        vg.flush(buf);

        // THE MEASURED WHEEL UNDER THE DRAWN ONE. Ghost ticks just inside the rim at the angles the
        // capture actually produced, so "does my wheel match the engine" is a look rather than an
        // argument: where a tick has no tooth above it, the drawing is wrong.
        const std::vector<double>* ghostEdges = nullptr;
        if (model_.measured.ok && model_.measured.crankIdx >= 0)
            ghostEdges = &model_.measured.streams[size_t(model_.measured.crankIdx)].edgeAngles;
        if (R > 10.f && ghostEdges && !ghostEdges->empty()) {
            const double base = -model_.working.sensorAngle + model_.working.tdcOffset + model_.crankAngle;
            for (double a : *ghostEdges) {
                const double br = (base - a) * trigger_ui::kPi / 180.0;
                const float ux = static_cast<float>(std::sin(br)), uy = static_cast<float>(-std::cos(br));
                static const uint8_t ghost[4] = { 120, 200, 255, 150 };
                buf.pushRectangle(cx + ux * R * 0.70f - 1.f, cy + uy * R * 0.70f - 1.f, 2.f, 2.f, ghost, 1.f);
            }
        }

        // The scrub: a track across the cycle with the crank's position on it. Full cycle, so a
        // sequential wheel scrubs through both revolutions and a cam pulse can be watched arriving.
        scrubX_ = plotX; scrubW_ = plotW; scrubY_ = scrubY; scrubH_ = scrubH;
        const double cycle = (n > 1) ? 720.0 : 360.0;
        scrubCycle_ = cycle;
        const float trackY = scrubY + scrubH * 0.5f - 2.f;
        buf.pushRectangle(plotX, trackY, plotW, 4.f, jf::Colors::Surface2, 2.f);
        const float t = static_cast<float>(std::fmod(model_.crankAngle, cycle) / cycle);
        buf.pushRectangle(plotX + t * plotW - 3.f, scrubY + 4.f, 6.f, scrubH - 8.f,
                          jf::Colors::Accent, 3.f);

        if (jf::JTextHelper::hasAtlas()) {
            char line[160];
            std::snprintf(line, sizeof(line), "%s        %s        %d stream%s",
                          trigger_ui::describe(w).c_str(), w.sync.c_str(), n, n == 1 ? "" : "s");
            jf::JTextHelper::pushText(buf, b.x + pad, b.y + 5.f, line, jf::Colors::TextPrimary);
            // WHY IT WILL NOT DECODE, said while it is being drawn. An undecodable wheel looks
            // exactly like a working one — the dial draws it, the trace draws it, describe() names
            // it — and the only way to find out used to be to apply it to an ECU and watch an engine
            // not start. The worst issue leads, because a designer fixing one fault at a time wants
            // the one that stops it working, not an inventory.
            if (model_.measured.ok && model_.measured.crankIdx >= 0) {
                const triggerfit::StreamFit& cs = model_.measured.streams[size_t(model_.measured.crankIdx)];
                char m[160];
                // Named as what it IS — a gap wheel by its count, an even track by its slit count,
                // a coded track by how many departures carry the code.
                if (cs.shape == triggerfit::Shape::Gap)
                    std::snprintf(m, sizeof(m), "measured %d-%d at %.0f rpm  (%zu stream%s)",
                                  cs.teeth, cs.missing, model_.measured.rpm,
                                  model_.measured.streams.size(),
                                  model_.measured.streams.size() == 1 ? "" : "s");
                else if (cs.shape == triggerfit::Shape::Even)
                    std::snprintf(m, sizeof(m), "measured %d even at %.0f rpm  (%zu stream%s)",
                                  cs.teeth, model_.measured.rpm, model_.measured.streams.size(),
                                  model_.measured.streams.size() == 1 ? "" : "s");
                else
                    std::snprintf(m, sizeof(m), "measured coded, %zu marks at %.0f rpm",
                                  cs.anomalies.size(), model_.measured.rpm);
                jf::JTextHelper::pushText(buf, b.x + b.width - pad - 280.f, b.y + 5.f, m,
                                          jf::Colors::TextSecondary, 280.f);
            }
            if (!issues.empty()) {
                const WheelIssue& top = issues.front();
                const uint8_t* col = (top.level == WheelIssue::Error) ? jf::Colors::Danger
                                   : (top.level == WheelIssue::Warn)  ? jf::Colors::Warning
                                                                      : jf::Colors::TextSecondary;
                wraptext::draw(buf, b.x + pad, b.y + 5.f + jf::JTextHelper::lineHeight() + 2.f,
                               issueText, col, b.width - 2.f * pad);
            }
            char sc[48];
            std::snprintf(sc, sizeof(sc), "crank %.1f\xC2\xB0", model_.crankAngle);
            jf::JTextHelper::pushText(buf, b.x + pad, scrubY + 6.f, sc, jf::Colors::TextSecondary);
            for (int i = 0; i < n; ++i)
            {
                static const char* kEdge[] = { "\xE2\x86\x91", "\xE2\x86\x93", "\xE2\x86\x95" };   // rise / fall / both
                const int e = (i < static_cast<int>(w.streams.size())) ? w.streams[i].edge : 0;
                jf::JTextHelper::pushText(buf, b.x + pad, traceY + i * rowH + 3.f,
                                          std::string(trigger_ui::roleShort(g.streams[i].displayRole)) + " " +
                                          kEdge[e < 0 || e > 2 ? 0 : e], jf::Colors::TextSecondary);
            }
        }
    }

    // Drag the picture into agreement with the engine. Grab the PICKUP (outside the rim) to say where
    // it bolts on; grab the WHEEL (inside) to turn it. Either way the thing that changes is the
    // offset, because the offset IS the angle between the reference tooth and the pickup — dragging
    // the pickup does not move the wheel, and dragging the wheel does not move the pickup.
    //
    // This is the "I do not know the number but I can see the engine" route in: match the picture,
    // read the offset off it, then fix the timing and finish with a light.
    void handleMousePress(float mx, float my) override {
        if (scrubW_ > 0.f && my >= scrubY_ && my < scrubY_ + scrubH_) {
            drag_ = Drag::Scrub; _scrubTo(mx); requestFocus(); return;
        }
        if (dialR_ <= 0.f) return;
        const float dx = mx - dialCx_, dy = my - dialCy_;
        const float r  = std::sqrt(dx * dx + dy * dy);
        if (r < dialR_ * 0.25f || r > dialR_ * 1.45f) return;      // hub and outside: not a handle
        drag_      = (r > dialR_ * 1.02f) ? Drag::Pickup : Drag::Wheel;
        dragPrev_  = bearingAt(mx, my);
        pressR_    = r;                 // remembered so a CLICK on the tooth ring can be told from a drag
        pressBear_ = dragPrev_;
        moved_     = false;
        requestFocus();
    }
    void handleMouseMove(float mx, float my) override {
        if (drag_ == Drag::None) return;
        if (!jf::JWidget::s_leftDown) { drag_ = Drag::None; return; }   // released elsewhere
        if (drag_ == Drag::Scrub) { _scrubTo(mx); return; }
        const double now = bearingAt(mx, my);
        double d = now - dragPrev_;
        while (d >  180.0) d -= 360.0;                              // shortest way round
        while (d < -180.0) d += 360.0;
        if (std::fabs(d) > 0.4) moved_ = true;   // a real drag, not the jitter of a click
        dragPrev_ = now;
        if (drag_ == Drag::Pickup) {
            // The pickup moves, the wheel stays put: hold (sensorAngle + tdcOffset) so no tooth
            // shifts on screen, and the offset absorbs it.
            model_.working.sensorAngle = wrap180(model_.working.sensorAngle - d);   // anticlockwise +
            model_.working.tdcOffset   = wrap360(model_.working.tdcOffset   - d);
        } else {
            model_.working.tdcOffset   = wrap360(model_.working.tdcOffset   + d);
        }
        model_.changed.emit();
    }
    // A CLICK ON THE TOOTH RING TAKES THAT TOOTH OUT, or puts it back. Dragging still turns the
    // wheel, so the two gestures share the ring without competing: a drag moves, a click edits.
    //
    // This is the half of the designer that was missing. The dial could be dragged into agreement
    // with the engine but every tooth in it had to be described by typing indices into "Gap @ tooth"
    // — you could SEE the wheel and still had to author it as a list of numbers somewhere else.
    void handleMouseRelease(float, float) override {
        const Drag was = drag_;
        drag_ = Drag::None;
        if (was != Drag::Wheel || moved_) return;
        if (pressR_ < dialR_ * 0.78f || pressR_ > dialR_ * 1.02f) return;   // the ring, not the hub
        _toggleToothAt(pressBear_);
    }

private:
    TriggerEditModel& model_;
    enum class Drag { None, Wheel, Pickup, Scrub };
    // The scrub writes model_.crankAngle ONLY — view state, never saved, never a config write, and
    // deliberately not `dirty`: winding the engine over is looking, not editing.
    void _scrubTo(float mx) {
        if (scrubW_ <= 0.f) return;
        const double t = std::clamp<double>((mx - scrubX_) / scrubW_, 0.0, 1.0);
        model_.crankAngle = t * scrubCycle_;
        model_.changed.emit();
    }
    float scrubX_ = 0.f, scrubW_ = 0.f, scrubY_ = 0.f, scrubH_ = 0.f;
    double scrubCycle_ = 720.0;
    float  pressR_ = 0.f;
    double pressBear_ = 0.0;
    bool   moved_ = false;

    // Which tooth is under that bearing, and flip whether it is there.
    //
    // The inverse of the drawing relation stated below: bearing(D) = -sensorAngle + tdcOffset +
    // crankAngle - D, so D = -sensorAngle + tdcOffset + crankAngle - bearing. Solving it here rather
    // than guessing from pixels is what keeps the tooth you click the tooth that changes, whatever
    // the wheel has been wound to or the pickup moved to.
    void _toggleToothAt(double bearing) {
        WheelParams& p = model_.working;
        if (p.crankType != WheelParams::CRANK_MISSING) return;   // only a gap wheel has teeth to remove
        const WheelGeometry g = wheelGeometry(buildWheel(p));
        if (g.crankIdx < 0) return;
        const StreamGeometry& cs = g.streams[static_cast<size_t>(g.crankIdx)];
        if (cs.slots <= 0 || cs.toothAngle <= 0.0) return;
        const double D = wrap360(-p.sensorAngle + p.tdcOffset + model_.crankAngle - bearing);
        int idx = static_cast<int>(std::lround(D / cs.toothAngle)) % cs.slots;
        if (idx < 0) idx += cs.slots;
        auto it = std::find(p.gapPos.begin(), p.gapPos.end(), idx);
        if (it != p.gapPos.end()) p.gapPos.erase(it);
        else                      p.gapPos.push_back(idx);
        std::sort(p.gapPos.begin(), p.gapPos.end());
        // The count follows the list. They are two views of one fact, and letting them disagree is
        // how a wheel comes to claim one missing tooth while naming three.
        p.missing = std::max<int>(1, static_cast<int>(p.gapPos.size()));
        model_.dirty = true;
        model_.changed.emit();
    }
    Drag  drag_ = Drag::None;
    double dragPrev_ = 0.0;
    float dialCx_ = 0.f, dialCy_ = 0.f, dialR_ = 0.f;

    // Screen point → bearing in the dial's own terms (degrees clockwise from vertical).
    double bearingAt(float mx, float my) const {
        const double a = std::atan2(static_cast<double>(my - dialCy_), static_cast<double>(mx - dialCx_));
        return a * 180.0 / trigger_ui::kPi + 90.0;
    }
    static double wrap360(double d) { d = std::fmod(d, 360.0); return d < 0.0 ? d + 360.0 : d; }
    static double wrap180(double d) { d = wrap360(d); return d > 180.0 ? d - 360.0 : d; }

    // The gear as it sits on the engine — front view, turning clockwise, pickup fixed on the rim.
    //
    //     bearing(D) = -sensorAngle + tdcOffset + crankAngle - D
    //
    // sensorAngle is ANTICLOCKWISE-positive (a 2JZ's pickup is +105, 105 to the left), which is how
    // it gets described out loud; bearings here are clockwise-positive, hence the negation.
    // D is a tooth's decoder angle; crankAngle is 0 at TDC #1. The minus on D is what makes it a
    // GEAR rather than a timing diagram: a tooth the decoder meets later has still to travel to the
    // pickup, so it sits further anticlockwise. Plot D directly and you draw the mirror image.
    //
    // Everything the user can point at is in here: the pickup where it actually bolts on, the wheel
    // wherever they have wound it, and the offset as the angle between the reference tooth and the
    // pickup. Which is the point — an offset you can arrive at by matching the picture to the engine
    // beats one copied out of somebody else's tune.

    // One labeled row per stream (up to 6) over the shared 360°/720° cycle, so crank/cam/VVT phase
    // relationships are visible at a glance. Crank-rate streams (360°) repeat across a 720° cycle.
    // `w` rides along for the per-stream CAPTURE EDGE. The trace draws every tooth as an identical
    // block, so with the edge now settable there was no way to see which edge the decoder actually
    // takes — a wheel set to Falling looked exactly like one set to Rising.
};

// ============================ Settings tab =======================================================
class TriggerSettings : public jf::JWidget {
public:
    TriggerSettings(jf::JSceneGraph& g, TriggerEditModel& m)
        : jf::JWidget(g, "TriggerSettings"), model_(m), form_(g) {
        addChild(&form_);        // the form is this pane's child; its rows hang off it
        buildForm(g);
        syncControls();
        // The dial can edit the wheel too — dragging the pickup or turning the wheel moves the same
        // numbers this form shows. Without this the two halves of the designer drifted apart: drag
        // the visual and the fields still read the old values, so whichever you looked at last was
        // the one you believed.
        //
        // Skipped while WE are the source of the change (inEdit_): the form is already correct, and
        // re-entering a spin box mid-commit to write back the value it just gave us is a good way to
        // fight the user's typing.
        model_.changed.connect([this] { if (!inEdit_) syncControls(); });
    }

    std::function<void(const Wheel&)> onSaveToLibrary;
    std::function<void(const Wheel&)> onApplyToEcu;      // install into the live config (RAM; burn is separate)
    std::function<void(const std::string&)> onSaveFailed;
    std::function<void()> onUseMeasured;         // the view owns the model; the shell wires this up

    // Push model_.working into the controls without re-triggering their change callbacks.
    void syncControls() {
        syncing_ = true;
        const WheelParams& p = model_.working;
        name_->setText(p.name);
        crankType_->setCurrentIndex(p.crankType);
        teeth_->setValue(p.crankType == WheelParams::CRANK_EVEN ? p.evenTeeth : p.teeth);
        missing_->setValue(p.missing);
        gaps_->setText(joinList(p.gapPos));
        seq_->setText(joinList(p.seqAngles));
        crankEdge_->setCurrentIndex(p.crankEdge);
        cwin_->setText(joinList(std::vector<double>{ p.cwMin, p.cwMax, p.cwTgt }));
        tol_->setValue(p.windowPct);
        camCount_->setValue(static_cast<int>(p.cams.size()));
        for (size_t i = 0; i < cam_.size(); ++i) {
            if (i >= p.cams.size()) continue;
            const CamParams& c = p.cams[i];
            cam_[i].type->setCurrentIndex(c.type);
            cam_[i].role->setCurrentIndex(_camRoleIndex(c.role, i));
            cam_[i].win->setText(joinList(std::vector<double>{ c.wMin, c.wMax, c.wTgt }));
            cam_[i].pat->setText(joinList(c.angles));
        }
        layoutForm();                                    // one VVT row-group per live cam
        for (int i = 0; i < static_cast<int>(p.cams.size()) && i < 4; ++i) {
            cam_[i].type->setCurrentIndex(p.cams[i].type);
            cam_[i].role->setCurrentIndex(_camRoleIndex(p.cams[i].role, i));
            cam_[i].edge->setCurrentIndex(p.cams[i].edge == 2 ? 1 : 0);
            cam_[i].nominal->setValue(p.cams[i].nominal);
        }
        tdc_->setValue(p.tdcOffset);
        pickup_->setValue(p.sensorAngle);
        crank_->setValue(model_.crankAngle);
        // One even wheel and nothing else is SYNC ALWAYS: every tooth is a sync tooth, which the ECU
        // accepts in Ignition Mode Distributor (TriggerConfigCheck::even_wheel_sync_always).
        const bool evenAlone = p.cams.empty() && p.crankType == WheelParams::CRANK_EVEN;
        syncLabel_->setText(evenAlone ? "ALWAYS (distributor mode)"
                            : p.cams.empty() ? "CRANK (wasted spark)" : "PHASE (sequential)");
        syncing_ = false;
    }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        const jf::JRect b = bounds();
        const float w = std::min(b.width, 300.f);   // fixed-width panel; the empty space at right = the spring
        // THE ACTIONS PINNED TO THE BOTTOM. As form rows they scrolled with the fields and moved every
        // time a cam was added or the crank type changed which rows applied — so the two controls that
        // commit the work were never in the same place twice.
        const bool haveFit = model_.measured.ok;
        const float footH = 34.f;
        const float bw = haveFit ? (w - 24.f) / 3.f : (w - 18.f) * 0.5f;
        form_.setBounds({ b.x, b.y, w, std::max(0.f, b.height - footH) });
        form_.populateRenderPrimitives(buf);
        const float fy = b.y + b.height - footH + 4.f;
        float fx = b.x + 6.f;
        // USE MEASURED appears only when a capture has actually fitted. A button that is present and
        // refuses is a worse answer than one that is not there: the reason it cannot work is in the
        // strip above, where the fit already said why.
        if (fitBtn_) {
            fitBtn_->setVisible(haveFit);
            if (haveFit) { fitBtn_->setBounds({ fx, fy, bw, 26.f }); fitBtn_->populateRenderPrimitives(buf); fx += bw + 6.f; }
        }
        if (saveBtn_)  { saveBtn_->setBounds({ fx, fy, bw, 26.f });  saveBtn_->populateRenderPrimitives(buf); fx += bw + 6.f; }
        if (applyBtn_) { applyBtn_->setBounds({ fx, fy, bw, 26.f }); applyBtn_->populateRenderPrimitives(buf); }
    }
    // THE FOOTER TAKES ITS OWN CLICKS. This forwarded everything to the form, which was right while
    // the buttons WERE form rows; pinning them to the bottom without this left two controls that
    // painted, highlighted on hover and did nothing at all when pressed.
    void handleMousePress(float mx, float my) override {
        if (jf::JWidget* w = _footerAt(mx, my)) { w->handleMousePress(mx, my); return; }
        form_.handleMousePress(mx, my);
    }
    void handleMouseMove(float mx, float my) override {
        if (fitBtn_ && fitBtn_->isVisible()) fitBtn_->handleMouseMove(mx, my);
        if (saveBtn_)  saveBtn_->handleMouseMove(mx, my);    // hover state for all, wherever the cursor is
        if (applyBtn_) applyBtn_->handleMouseMove(mx, my);
        form_.handleMouseMove(mx, my);
    }
    void handleMouseRelease(float mx, float my) override {
        if (fitBtn_ && fitBtn_->isVisible()) fitBtn_->handleMouseRelease(mx, my);
        if (saveBtn_)  saveBtn_->handleMouseRelease(mx, my);   // the click FIRES on release; all get it
        if (applyBtn_) applyBtn_->handleMouseRelease(mx, my);
        form_.handleMouseRelease(mx, my);
    }
    bool handleScroll(float mx, float my, float w) override { return form_.handleScroll(mx, my, w); }

private:
    TriggerEditModel& model_;
    bool              syncing_ = false;
    bool              inEdit_  = false;   // this form is the source of the in-flight change (see ctor)
    jf::JContainer    form_;
    jf::JComboBox*      crankType_  = nullptr;
    jf::JSpinBox*       teeth_      = nullptr;
    jf::JSpinBox*       missing_    = nullptr;
    jf::JLineEdit*      gaps_       = nullptr;   // GAP: every gap position, comma separated
    jf::JLineEdit*      seq_        = nullptr;   // SEQUENCE: inter-event angles (deg)
    jf::JComboBox*      crankEdge_  = nullptr;   // the crank stream's capture edge
    jf::JLineEdit*      cwin_       = nullptr;   // CRANK_WIDTH reference window
    jf::JSpinBox*       tol_        = nullptr;   // match tolerance ±%
    jf::JSpinBox*       camCount_   = nullptr;   // 0..4 cams
    // One VVT control-group per cam (identity by list order: cam 0 = Intake·B1, 1 = Exhaust·B1,
    // 2 = Intake·B2, 3 = Exhaust·B2). Pre-created for all four; layoutForm() adds only the live count.
    struct CamRow { jf::JLabel* head = nullptr; jf::JComboBox* type = nullptr;
                    jf::JLabel* lRole = nullptr; jf::JComboBox* role = nullptr;
                    jf::JLabel* lEdge = nullptr;
                    jf::JComboBox* edge = nullptr; jf::JLabel* lNom = nullptr; jf::JDoubleSpinBox* nominal = nullptr;
                    // PULSE: the width window as "min, max, target" (deg). PATTERN: the edge angles.
                    jf::JLabel* lWin = nullptr;  jf::JLineEdit* win = nullptr;
                    jf::JLabel* lPat = nullptr;  jf::JLineEdit* pat = nullptr; };
    std::array<CamRow, 4> cam_{};
    // Which item of the "which cam" list a stored role is. UNSET is -1, not 0 — a library wheel carries
    // -1 on every cam ("place me by position"), and reading it as a set value put the combo at index -4.
    static int _camRoleIndex(int role, int i) {
        const int slot = (role >= kSlotCamBase) ? role : kSlotCamBase + i;
        return std::clamp(slot - kSlotCamBase, 0, 3);
    }

    jf::JDoubleSpinBox* tdc_        = nullptr;
    jf::JButton*        fitBtn_     = nullptr;   // adopt the measured wheel (only with a fit in hand)
    jf::JButton*        applyBtn_   = nullptr;
    jf::JDoubleSpinBox* pickup_     = nullptr;   // where the pickup sits (drawing only)
    jf::JDoubleSpinBox* crank_      = nullptr;   // where the crank is wound to (view only)
    jf::JButton*        saveBtn_    = nullptr;
    jf::JLineEdit*      name_       = nullptr;   // the design's name — what "Save to library" stores it as
    jf::JLabel*         syncLabel_  = nullptr;   // sync technique readout (CRANK / PHASE)
    // Fixed-row labels, created once and re-added by layoutForm() so the cam section rebuilds without leaking.
    jf::JLabel *secWheel_ = nullptr, *secDecode_ = nullptr, *secCams_ = nullptr, *secRef_ = nullptr,
               *spacer_ = nullptr;
    jf::JLabel *lCrank_ = nullptr, *lTeeth_ = nullptr, *lMissing_ = nullptr, *lGap_ = nullptr,
               *lCams_ = nullptr, *lTdc_ = nullptr, *lSyncRow_ = nullptr, *lSave_ = nullptr,
               *lPickup_ = nullptr, *lCrank2_ = nullptr, *lApply_ = nullptr,
               *lName_ = nullptr, *lSeq_ = nullptr, *lCrankEdge_ = nullptr,
               *lCWin_ = nullptr, *lTol_ = nullptr;

    // Every control AND label is owned by THIS widget via adopt() and laid out into form_ via the legacy
    // non-owning form_.add(raw), because layoutForm() repeatedly form_.clear()s and re-adds the live subset;
    // adopt() ownership persists across every relayout — so clear() detaches but never destroys them.
    // Which footer button is under the pointer, by the boxes the paint last set.
    jf::JWidget* _footerAt(float mx, float my) const {
        for (jf::JButton* b : { fitBtn_, saveBtn_, applyBtn_ }) {
            if (b && !b->isVisible()) continue;
            if (!b) continue;
            const jf::JRect r = b->bounds();
            if (mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height) return b;
        }
        return nullptr;
    }

    void edited() { model_.dirty = true; inEdit_ = true; model_.changed.emit(); inEdit_ = false; }

    // Vector-valued fields (gap positions, sequence angles, a cam's pattern) are edited as a plain
    // comma-separated list. A spin box can only ever express ONE value, which is why the form could
    // not describe a 36-2-2-2 (gaps at 0, 1 and 14) or any sequence wheel at all.
    static std::string joinList(const std::vector<double>& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            char b[32]; std::snprintf(b, sizeof(b), "%g", v[i]);
            out += (i ? ", " : "") + std::string(b);
        }
        return out;
    }
    static std::string joinList(const std::vector<int>& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + std::to_string(v[i]);
        return out;
    }
    // Tolerant on purpose: commas, spaces or both. A half-typed list simply yields what parses so
    // far, so the visualizer follows along as the user types instead of blanking on every keystroke.
    static std::vector<double> parseList(const std::string& t) {
        std::vector<double> out;
        const char* c = t.c_str();
        while (*c) {
            while (*c && (*c == ',' || *c == ' ' || *c == '\t')) ++c;
            if (!*c) break;
            char* end = nullptr;
            const double v = std::strtod(c, &end);
            if (end == c) { ++c; continue; }
            out.push_back(v); c = end;
        }
        return out;
    }
    static std::vector<int> parseIntList(const std::string& t) {
        std::vector<int> out;
        for (double d : parseList(t)) out.push_back(static_cast<int>(d));
        return out;
    }   // a control changed working_ → visualizer repaints

    // Cam identity by list order — the same IN/EX × bank order VvtControl's loops use (IntakeB1, ExhaustB1,
    // IntakeB2, ExhaustB2), so the designer labels match the firmware's cam mapping.
    static std::string camIdentity(int i) {
        return std::string(i % 2 == 0 ? "Intake" : "Exhaust") + " \xC2\xB7 Bank " + std::to_string(i / 2 + 1);
    }

    void buildForm(jf::JSceneGraph& g) {
        using namespace jf;
        WheelParams& p = model_.working;
        form_.setLayoutMode(JLayoutMode::Form)->setGap(6.f)->setPadding(JEdges{ 8.f, 8.f, 8.f, 8.f });

        secWheel_  = adopt(std::make_unique<JLabel>(g, "WHEEL",     110.f));
        secDecode_ = adopt(std::make_unique<JLabel>(g, "DECODE",    110.f));
        secCams_   = adopt(std::make_unique<JLabel>(g, "CAMS",      110.f));
        secRef_    = adopt(std::make_unique<JLabel>(g, "REFERENCE", 110.f));
        spacer_    = adopt(std::make_unique<JLabel>(g, "", 150.f));
        lName_ = adopt(std::make_unique<JLabel>(g, "Name", 110.f));
        name_  = adopt(std::make_unique<JLineEdit>(g, "wheel name\xE2\x80\xA6", 150.f));
        name_->onTextChanged.connect([this, &p](const std::string& t){ if (!syncing_) { p.name = t; } });
        crankType_ = adopt(std::make_unique<JComboBox>(g, std::vector<std::string>{ "Missing tooth", "Even", "Sequence", "Width" }));
        // Relayout as well as edit: changing the crank type changes WHICH rows are meaningful.
        crankType_->onIndexChanged.connect([this, &p](int i){
            if (syncing_) return;
            p.crankType = i;
            layoutForm();
            edited();
        });
        teeth_ = adopt(std::make_unique<JSpinBox>(g, 1, 200, 90.f));
        teeth_->onValueChanged.connect([this, &p](int v){ if (!syncing_) { p.teeth = v; p.evenTeeth = v; edited(); } });
        missing_ = adopt(std::make_unique<JSpinBox>(g, 1, 8, 90.f));
        missing_->onValueChanged.connect([this, &p](int v){ if (!syncing_) { p.missing = v; edited(); } });
        gaps_ = adopt(std::make_unique<JLineEdit>(g, "0", 150.f));
        gaps_->onTextChanged.connect([this, &p](const std::string& t){
            if (!syncing_) { p.gapPos = parseIntList(t); edited(); } });
        lCrankEdge_ = adopt(std::make_unique<JLabel>(g, "Crank edge", 110.f));
        crankEdge_  = adopt(std::make_unique<JComboBox>(g, std::vector<std::string>{ "Rising", "Falling", "Both" }));
        crankEdge_->onIndexChanged.connect([this, &p](int v){ if (!syncing_) { p.crankEdge = v; edited(); } });
        lCWin_ = adopt(std::make_unique<JLabel>(g, "Window min,max,tgt", 110.f));
        cwin_  = adopt(std::make_unique<JLineEdit>(g, "0, 360, 0", 150.f));
        cwin_->onTextChanged.connect([this, &p](const std::string& t){
            if (syncing_) return;
            const std::vector<double> v = parseList(t);
            if (v.size() > 0) p.cwMin = v[0];
            if (v.size() > 1) p.cwMax = v[1];
            if (v.size() > 2) p.cwTgt = v[2];
            edited();
        });
        // Match tolerance. The decoder compares every candidate against the pattern within +/-this;
        // too tight and a real wheel never locks, too loose and two shapes look alike. It was fixed
        // at 25% with no way to see or change it.
        lTol_ = adopt(std::make_unique<JLabel>(g, "Match window \xC2\xB1%", 110.f));
        tol_  = adopt(std::make_unique<JSpinBox>(g, 5, 60, 90.f));
        tol_->onValueChanged.connect([this, &p](int v){ if (!syncing_) { p.windowPct = v; edited(); } });
        lSeq_ = adopt(std::make_unique<JLabel>(g, "Angles (deg)", 110.f));
        seq_  = adopt(std::make_unique<JLineEdit>(g, "0, 135", 150.f));
        seq_->onTextChanged.connect([this, &p](const std::string& t){
            if (!syncing_) { p.seqAngles = parseList(t); edited(); } });


        // Cam count grows/shrinks p.cams; each cam is then edited through its OWN VVT row-group below.
        camCount_ = adopt(std::make_unique<JSpinBox>(g, 0, 4, 90.f));
        camCount_->onValueChanged.connect([this, &p](int n){
            if (syncing_) return;
            n = std::clamp(n, 0, 4);
            while (static_cast<int>(p.cams.size()) < n) p.cams.push_back(CamParams{});
            if (static_cast<int>(p.cams.size()) > n) p.cams.resize(n);
            edited(); syncControls();                     // rebuild rows + refresh the sync readout
        });

        // Per-cam VVT controls (all four pre-built; layoutForm shows the live count). Each cam's callbacks
        // guard on i < cams.size() so a stale row (mid-resize) can't write past the vector.
        for (int i = 0; i < 4; ++i) {
            cam_[i].head = adopt(std::make_unique<JLabel>(g, camIdentity(i), 110.f));
            cam_[i].type = adopt(std::make_unique<JComboBox>(g, std::vector<std::string>{ "Pulse", "Pattern" }));
            cam_[i].type->onIndexChanged.connect([this, &p, i](int v){ if (syncing_ || i >= (int)p.cams.size()) return; p.cams[i].type = v; layoutForm(); edited(); });
            // WHICH cam this is. Position used to decide it, so two cams were always Intake B1 +
            // Exhaust B1 — and an engine with phasers on both intakes and nothing on the exhausts
            // (Intake B1 + Intake B2) could not be described at all.
            cam_[i].role = adopt(std::make_unique<JComboBox>(g, std::vector<std::string>{
                "Intake bank 1", "Exhaust bank 1", "Intake bank 2", "Exhaust bank 2" }));
            cam_[i].role->onIndexChanged.connect([this, &p, i](int v){
                if (syncing_ || i >= (int)p.cams.size()) return;
                p.cams[i].role = kSlotCamBase + v;
                edited();
            });
            cam_[i].lRole = adopt(std::make_unique<JLabel>(g, "\xC2\xB7 which cam", 110.f));
            cam_[i].lEdge = adopt(std::make_unique<JLabel>(g, "\xC2\xB7 edge", 110.f));
            cam_[i].edge = adopt(std::make_unique<JComboBox>(g, std::vector<std::string>{ "Rising", "Falling", "Both" }));
            cam_[i].edge->onIndexChanged.connect([this, &p, i](int v){ if (!syncing_ && i < (int)p.cams.size()) { p.cams[i].edge = v; edited(); } });
            cam_[i].lNom = adopt(std::make_unique<JLabel>(g, "\xC2\xB7 nominal\xC2\xB0", 110.f));
            cam_[i].nominal = adopt(std::make_unique<JDoubleSpinBox>(g, -360.0, 360.0, 1.0, 1, 90.f));
            cam_[i].nominal->onValueChanged.connect([this, &p, i](double v){ if (!syncing_ && i < (int)p.cams.size()) { p.cams[i].nominal = v; edited(); } });

            // PULSE: the width window that identifies the reference pulse, as "min, max, target"
            // degrees. PATTERN: the cam's edge angles. Neither existed, so a cam could only ever be
            // whatever the loaded wheel already had.
            cam_[i].lWin = adopt(std::make_unique<JLabel>(g, "Window min,max,tgt", 110.f));
            cam_[i].win  = adopt(std::make_unique<JLineEdit>(g, "0, 720, 0", 150.f));
            cam_[i].win->onTextChanged.connect([this, &p, i](const std::string& t){
                if (syncing_ || i >= (int)p.cams.size()) return;
                const std::vector<double> v = parseList(t);
                if (v.size() > 0) p.cams[i].wMin = v[0];
                if (v.size() > 1) p.cams[i].wMax = v[1];
                if (v.size() > 2) p.cams[i].wTgt = v[2];
                edited();
            });
            cam_[i].lPat = adopt(std::make_unique<JLabel>(g, "Pattern angles", 110.f));
            cam_[i].pat  = adopt(std::make_unique<JLineEdit>(g, "0, 360", 150.f));
            cam_[i].pat->onTextChanged.connect([this, &p, i](const std::string& t){
                if (!syncing_ && i < (int)p.cams.size()) { p.cams[i].angles = parseList(t); edited(); } });
        }

        tdc_ = adopt(std::make_unique<JDoubleSpinBox>(g, 0.0, 720.0, 1.0, 1, 90.f));
        tdc_->onValueChanged.connect([this, &p](double v){ if (!syncing_) { p.tdcOffset = v; edited(); } });
        // Where the pickup bolts on, and where the crank is wound to. Both are the typed half of
        // the dial's drag handles — some users know the figures, some would rather match the picture.
        pickup_ = adopt(std::make_unique<JDoubleSpinBox>(g, -180.0, 180.0, 5.0, 1, 90.f));
        pickup_->onValueChanged.connect([this, &p](double v){
            if (syncing_) return;
            // TYPING it is a statement of fact — "the pickup is at -105" — so it sets that and
            // nothing else. Only the DRAG couples it to the offset, because dragging is moving the
            // pickup rather than describing it, and moving it changes which tooth is under it.
            // Coupling both ways destroyed the number the user came to enter: type the 2JZ's -105
            // against a correct 155 and the offset walked to 260.
            p.sensorAngle = v;
            edited();
        });
        crank_ = adopt(std::make_unique<JDoubleSpinBox>(g, -360.0, 360.0, 10.0, 1, 90.f));
        crank_->onValueChanged.connect([this](double v){
            if (syncing_) return;
            model_.crankAngle = v;              // view only — not part of the wheel
            model_.changed.emit();
        });
        // Installing is a DELIBERATE act, and a separate one from saving to the library: a wheel can
        // be worth keeping without being the one this ECU runs, and vice versa. Writes the config in
        // RAM only — burning stays the per-dialog footer's job.
        fitBtn_ = adopt(std::make_unique<JButton>(g, "Use measured", 150.f));
        fitBtn_->setVisible(false);
        fitBtn_->onClicked.connect([this]{ if (onUseMeasured) onUseMeasured(); });
        applyBtn_ = adopt(std::make_unique<JButton>(g, "Apply to ECU", 150.f));
        applyBtn_->onClicked.connect([this]{
            if (onApplyToEcu) onApplyToEcu(buildWheel(model_.working));
            model_.dirty = false;
            model_.changed.emit();
        });
        saveBtn_ = adopt(std::make_unique<JButton>(g, "Save to library", 150.f));
        // An unnamed wheel would save as "" and every subsequent save would replace it, so the
        // library would hold exactly one nameless entry however many designs were saved.
        saveBtn_->onClicked.connect([this]{
            if (model_.working.name.empty()) { if (onSaveFailed) onSaveFailed("name the wheel before saving it"); return; }
            if (onSaveToLibrary) onSaveToLibrary(buildWheel(model_.working));
            model_.dirty = false;
            model_.changed.emit();
        });
        syncLabel_ = adopt(std::make_unique<JLabel>(g, "CRANK (wasted spark)", 110.f));

        lCrank_ = adopt(std::make_unique<JLabel>(g, "Crank type", 110.f)); lTeeth_ = adopt(std::make_unique<JLabel>(g, "Teeth", 110.f));    lMissing_ = adopt(std::make_unique<JLabel>(g, "Missing", 110.f));
        lGap_   = adopt(std::make_unique<JLabel>(g, "Gap @ tooth", 110.f)); lCams_  = adopt(std::make_unique<JLabel>(g, "Cams", 110.f));     lTdc_     = adopt(std::make_unique<JLabel>(g, "TDC offset", 110.f));
        lSyncRow_ = adopt(std::make_unique<JLabel>(g, "Sync", 110.f));      lSave_  = adopt(std::make_unique<JLabel>(g, "", 110.f));
        lApply_  = adopt(std::make_unique<JLabel>(g, "", 110.f));
        lPickup_ = adopt(std::make_unique<JLabel>(g, "Pickup angle", 110.f));
        lCrank2_ = adopt(std::make_unique<JLabel>(g, "Crank angle", 110.f));

        layoutForm();
    }

    // (Re)lay the form: fixed crank rows, the cam count, one VVT row-group per LIVE cam, then TDC/sync/save.
    // form_.clear() only DETACHES these children (form_ never owned them — they went in via the non-owning
    // form_.add(raw)); their lifetime is this widget's (adopt()), so they persist. Thus this runs on
    // every cam-count change or wheel load without recreating or leaking widgets.
    // A SECTION HEAD, as a row of its own. The form is a label/control grid, so a heading is a label
    // in the label column with a blank in the control column — which is all a heading needs to be.
    // Twenty rows in one undivided list is a list you read by counting; four short groups is one you
    // read by name, and the groups are the questions a wheel actually answers: what shape is it, how
    // is it decoded, what cams ride with it, where is TDC.
    void section(jf::JLabel* head) { form_.add(head); form_.add(spacer_); }

    void layoutForm() {
        form_.clear();
        section(secWheel_);
        form_.add(lName_);    form_.add(name_);
        form_.add(lCrank_);   form_.add(crankType_);
        form_.add(lTeeth_);   form_.add(teeth_);
        // Missing-tooth count and gap positions describe a GAP wheel. An EVEN wheel has neither, and
        // a SEQUENCE wheel is described by its angles instead — showing the rows anyway left a
        // 180-slit even track reading "missing 1" (the spin box cannot go below 1), which is both
        // meaningless and the exact wrong reading that had the firmware a tooth short per rev.
        // Only the rows that DESCRIBE the chosen crank type. A missing-tooth wheel has a missing
        // count and gap positions; an even wheel has neither; a sequence wheel is its angles.
        if (model_.working.crankType == WheelParams::CRANK_MISSING) {
            form_.add(lMissing_); form_.add(missing_);
            form_.add(lGap_);     form_.add(gaps_);
        } else if (model_.working.crankType == WheelParams::CRANK_SEQ) {
            form_.add(lSeq_);     form_.add(seq_);
        } else if (model_.working.crankType == WheelParams::CRANK_WIDTH) {
            form_.add(lCWin_);    form_.add(cwin_);
        }
        // Apply to every stream, so they sit outside the per-type block.
        section(secDecode_);
        form_.add(lCrankEdge_); form_.add(crankEdge_);
        form_.add(lTol_);       form_.add(tol_);
        section(secCams_);
        form_.add(lCams_);    form_.add(camCount_);
        const int n = std::min<int>(4, static_cast<int>(model_.working.cams.size()));
        for (int i = 0; i < n; ++i) {
            form_.add(cam_[i].head);  form_.add(cam_[i].type);      // identity label heads each cam's group
            form_.add(cam_[i].lRole); form_.add(cam_[i].role);
            form_.add(cam_[i].lEdge); form_.add(cam_[i].edge);
            form_.add(cam_[i].lNom);  form_.add(cam_[i].nominal);
            // A cam is either a single reference PULSE (matched by width window) or a repeating
            // PATTERN of edges. Show whichever describes this cam — neither was editable before, so
            // no cam could be authored at all.
            if (model_.working.cams[static_cast<size_t>(i)].type == WheelParams::CAM_PULSE) {
                form_.add(cam_[i].lWin); form_.add(cam_[i].win);
            } else {
                form_.add(cam_[i].lPat); form_.add(cam_[i].pat);
            }
        }
        section(secRef_);
        form_.add(lTdc_);     form_.add(tdc_);
        form_.add(lPickup_);  form_.add(pickup_);
        form_.add(lSyncRow_); form_.add(syncLabel_);
        // Crank angle is gone from here: it is the VIEW's business and it is the scrub under the dial
        // now. Save and Apply are gone too — they are what you do to the wheel, not a property of it,
        // and they sit in the footer where an action belongs.
    }
};

// ============================ the designer: visualizer BESIDE the settings ========================
//
// ONE PANE, NOT TWO TABS. The dial and the form were separate tabs so that "the visualizer never gets
// squished" — and the cost of that was the whole point of a visualizer: you edited a number on one
// tab and had to switch to the other to find out what it did. Designing a wheel is a loop of change,
// look, change, and a tab is a wall through the middle of it.
//
// A JWidget writing its children's boxes, NOT a JContainer: JContainer::add() adds a scene-graph
// LAYOUT edge as well as a tree edge, and the parent's flex pass then overwrites every box set here.
// GenericCanPanel and SurfaceCanvas document the same discovery.
class TriggerDesignerView : public jf::JWidget {
public:
    explicit TriggerDesignerView(jf::JSceneGraph& g)
        : jf::JWidget(g, "TriggerDesignerView"), visual_(g, model_), settings_(g, model_) {
        addChild(&visual_);
        addChild(&settings_);
        settings_.onUseMeasured = [this]{ adoptMeasured(); };
    }

    // The form keeps a readable width and the dial takes the rest — the dial is the thing that wants
    // room, and it is the half that grows when the window does. Capped at a fraction of the pane so a
    // narrow window does not leave the dial a sliver: below that the form is the more useful half, and
    // the two swap which one is squeezed.
    static constexpr float kFormW = 360.f;

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        const jf::JRect b = bounds();
        const float formW = std::min(kFormW, std::max(180.f, b.width * 0.45f));
        visual_.setBounds({ b.x, b.y, std::max(0.f, b.width - formW), b.height });
        settings_.setBounds({ b.x + b.width - formW, b.y, formW, b.height });
        buf.pushClip(b.x, b.y, b.width, b.height);
        for (jf::JWidget* w : children())
            if (w && w->isVisible()) w->populateRenderPrimitives(buf);
        buf.popClip();
    }

    // THE HOST FORWARDS INPUT BY HAND. JTabWidget used to route to the active tab; a plain JWidget
    // routes to nobody, so with the tabs gone every control in both halves painted perfectly and
    // ignored the mouse. The boxes the paint just set are the hit-test — one source of geometry, so
    // what you click is what you see.
    void handleMousePress(float mx, float my) override {
        if (jf::JWidget* w = _at(mx, my)) w->handleMousePress(mx, my);
    }
    // Move and release go to BOTH: a drag that began on the dial and wandered over the form must keep
    // getting moves, and a button waiting on its release must get one wherever the pointer ended up.
    void handleMouseMove(float mx, float my) override {
        visual_.handleMouseMove(mx, my); settings_.handleMouseMove(mx, my);
    }
    void handleMouseRelease(float mx, float my) override {
        visual_.handleMouseRelease(mx, my); settings_.handleMouseRelease(mx, my);
    }
    bool handleScroll(float mx, float my, float d) override {
        jf::JWidget* w = _at(mx, my);
        return w ? w->handleScroll(mx, my, d) : false;
    }

    // Load a library wheel to preview/edit (no config write): update the shared model, resync the
    // form controls, and repaint the visualizer.
    void loadWheel(const Wheel& w) {
        model_.working = decodeWheel(w);
        model_.dirty = false;                 // freshly loaded IS the saved state
        settings_.syncControls();
        model_.changed.emit();
    }
    // Unsaved edits present? The shell asks before doing anything that would throw them away.
    bool isDirty() const { return model_.dirty; }
    Wheel workingWheel() const { return buildWheel(model_.working); }
    const std::string& workingName() const { return model_.working.name; }
    void discardEdits() { model_.dirty = false; }

    // A capture arrived. FIT IT, DRAW IT, CHANGE NOTHING: the measured wheel appears as ghost ticks
    // under the drawn one and the working wheel is left alone. Adopting the measurement is a
    // deliberate act (adoptMeasured), because a capture off a misfiring engine is still a capture.
    void setCapture(const std::vector<triggerlog::Lane>& lanes) {
        model_.measured = triggerfit::fitCapture(lanes);   // the whole capture, as a system
        model_.changed.emit();
    }
    const triggerfit::CaptureFit& measured() const { return model_.measured; }

    // Take the measurement as the design — the CAMS INCLUDED.
    //
    // This used to adopt the crank alone, on the grounds that "the cams are things a capture cannot
    // see". That was true of the fitter it was written against and has not been true for a while:
    // every lane is characterised now, a cam is the one whose pattern spans the engine CYCLE, and
    // the capture says how many there are and what each does. Leaving them out meant capturing a
    // wheel that plainly had a cam and adopting a trigger system with no cam in it — the default is
    // none — so the thing the user just measured was the thing that did not arrive.
    //
    // What a capture still cannot see is left alone: the pickup angle and TDC are the wheel's
    // relationship to the ENGINE, and no amount of looking at the wire reveals where the pistons
    // are. Those stay exactly as they were.
    bool adoptMeasured() {
        if (!applyMeasured(model_.measured, model_.working)) return false;
        model_.dirty = true;
        settings_.syncControls();
        model_.changed.emit();
        return true;
    }
    TriggerSettings& settings() { return settings_; }

private:
    jf::JWidget* _at(float mx, float my) {
        for (jf::JWidget* w : { static_cast<jf::JWidget*>(&visual_), static_cast<jf::JWidget*>(&settings_) }) {
            const jf::JRect r = w->bounds();
            if (mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height) return w;
        }
        return nullptr;
    }

    TriggerEditModel  model_;
    TriggerVisualizer visual_;
    TriggerSettings   settings_;
};

// ============================ LEFT: the wheel-library dock ========================================
class TriggerLibraryDock : public jf::JWidget {
public:
    explicit TriggerLibraryDock(jf::JSceneGraph& g)
        : jf::JWidget(g, "TriggerLibraryDock"), grid_(g, { "Trigger Wheel" }),
          newBtn_(g, "New wheel"), delBtn_(g, "Delete") {
        addChild(&grid_); addChild(&newBtn_); addChild(&delBtn_);
        // Authoring starts here. Without this the designer could only ever modify a wheel that
        // already existed, which is an editor, not a designer.
        newBtn_.onClicked.connect([this]{ if (onNew) onNew(); });
        // Only the user's own. A shipped wheel comes from the firmware's meta and would reappear on
        // the next load, so deleting it would look like a bug rather than a refusal.
        delBtn_.onClicked.connect([this]{
            const int i = grid_.selectedIndex();
            if (i < 0 || i >= static_cast<int>(wheels_.size())) return;
            if (static_cast<size_t>(i) < shipped_) {
                if (onDeleteRefused) onDeleteRefused("that wheel ships with the firmware \xE2\x80\x94 only your own can be deleted");
                return;
            }
            const std::string name = wheels_[static_cast<size_t>(i)].name;
            if (deleteUserWheel(name)) { reload(); if (onDeleted) onDeleted(name); }
        });
        reload();
        // Selecting a wheel LOADS it into the editor to preview/edit — it does NOT install to config.
        // Wire BOTH: onSelectionChanged (single-click + arrow-key navigation) and onRowActivated
        // (Enter / double-click) so any way of picking a row previews that wheel.
        auto pick = [this](int i) {
            if (onSelect && i >= 0 && i < static_cast<int>(wheels_.size())) onSelect(wheels_[i]);
        };
        grid_.onSelectionChanged.connect(pick);
        // Double-click / Enter ACTIVATES: same load, but it also brings the designer up. Selecting a
        // wheel loaded it into an editor the user could not see unless they already had the tab open,
        // which made the library look like it had done nothing.
        grid_.onRowActivated.connect([this](int i) {
            if (i < 0 || i >= static_cast<int>(wheels_.size())) return;
            if (onActivate) onActivate(wheels_[i]);
            else if (onSelect) onSelect(wheels_[i]);
        });
    }

    // Wired by the shell to the centre editor's loadWheel() — a pure preview/edit load, no config write.
    // The SHIPPED library, from the connected ECU's meta. Empty until a meta is loaded, which is
    // honest: with no ECU we do not know which wheels its firmware can decode.
    void setShippedWheels(std::vector<Wheel> w) { shippedWheels_ = std::move(w); reload(); }

    // A wheel the user authored. Persisted immediately, then shown — saving is the point of the
    // button, so it must not depend on the app exiting cleanly.
    bool addUserWheel(const Wheel& w) {
        const bool ok = saveUserWheel(w);
        reload();
        select(w.name);
        return ok;
    }

    // Rebuild the list: shipped first, then the user's own, so the user's additions are always at
    // the bottom where they were added rather than sorted into the middle of the shipped set.
    void reload() {
        wheels_  = shippedWheels_;
        shipped_ = wheels_.size();
        for (Wheel& u : loadUserWheels()) wheels_.push_back(std::move(u));
        std::vector<std::vector<std::string>> rows;
        rows.reserve(wheels_.size());
        for (size_t i = 0; i < wheels_.size(); ++i)
            rows.push_back({ i < shipped_ ? wheels_[i].name : wheels_[i].name + "  (mine)" });
        grid_.setRows(rows);
    }

    // Import a wheel file into the user's own library (merges by name), then show the result.
    // Returns the number added or updated, -1 if the file could not be read.
    int importFrom(const std::string& path) {
        const int n = importWheels(path);
        if (n > 0) reload();
        return n;
    }

    // Export: the SELECTED wheel if there is one, else the whole user library. Exporting the
    // selection is what someone means by "send me that wheel"; exporting everything is the backup.
    bool exportTo(const std::string& path) const {
        const int i = grid_.selectedIndex();
        if (i >= 0 && i < static_cast<int>(wheels_.size()))
            return exportWheels(path, { wheels_[static_cast<size_t>(i)] });
        return exportWheels(path, loadUserWheels());
    }

    // Move the selection to a wheel by name, so a just-saved wheel is the one on screen.
    void select(const std::string& name) {
        for (size_t i = 0; i < wheels_.size(); ++i)
            if (wheels_[i].name == name) { grid_.setSelectedIndex(static_cast<int>(i)); return; }
    }

    // "New wheel" — hand the editor a blank design to author from scratch.
    std::function<void()>             onNew;
    std::function<void(const std::string&)> onDeleted;
    std::function<void(const std::string&)> onDeleteRefused;
    std::function<void(const Wheel&)> onSelect;
    std::function<void(const Wheel&)> onActivate;   // double-click / Enter: open the designer on it

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        const jf::JRect b = bounds();
        const float headerH = 22.f, btnH = 24.f;
        if (jf::JTextHelper::hasAtlas())
            jf::JTextHelper::pushText(buf, b.x + 6.f, b.y + 4.f, "Trigger Wheels", jf::Colors::TextSecondary);
        const float halfW = (b.width - 12.f) * 0.5f;
        newBtn_.setBounds({ b.x + 4.f, b.y + headerH, halfW, btnH });
        newBtn_.populateRenderPrimitives(buf);
        delBtn_.setBounds({ b.x + 8.f + halfW, b.y + headerH, halfW, btnH });
        delBtn_.populateRenderPrimitives(buf);
        const float top = b.y + headerH + btnH + 4.f;
        grid_.setBounds({ b.x, top, b.width, b.height - (top - b.y) });
        grid_.populateRenderPrimitives(buf);
    }
    void handleMousePress(float mx, float my) override   { newBtn_.handleMousePress(mx, my); delBtn_.handleMousePress(mx, my); grid_.handleMousePress(mx, my); }
    void handleMouseMove(float mx, float my) override    { newBtn_.handleMouseMove(mx, my); delBtn_.handleMouseMove(mx, my); grid_.handleMouseMove(mx, my); }
    void handleMouseRelease(float mx, float my) override { newBtn_.handleMouseRelease(mx, my); delBtn_.handleMouseRelease(mx, my); grid_.handleMouseRelease(mx, my); }
    bool handleScroll(float mx, float my, float w) override { return grid_.handleScroll(mx, my, w); }
    bool handleKeyEvent(const jf::JKeyEvent& ke) override { return grid_.handleKeyEvent(ke); }

private:
    jf::JDataGrid      grid_;
    jf::JButton        newBtn_;
    jf::JButton        delBtn_;
    std::vector<Wheel> wheels_;
    std::vector<Wheel> shippedWheels_;
    size_t             shipped_ = 0;      // wheels_[0..shipped_) came from the meta
};
