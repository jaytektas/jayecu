// EngineCyclePanel — the recorder's ring and cursor semantics.
//
// Clicking through a GUI proves a button is wired; it does not pin down what the recorder should DO
// when frames keep arriving underneath a paused cursor, or when the ring overflows, or when the
// capacity shrinks. Those are the rules that decide whether "step back three cycles" lands on the
// cycle you meant, and they are worth stating deterministically.
//
//   cmake --build build --target cycle_recorder_panel_test && ./build/cycle_recorder_panel_test
#include "ui/EngineCyclePanel.h"

#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// A frame carrying a recognisable rpm, so "which frame is displayed" is answerable.
static enginecycle::Cycle frameWithRpm(double rpm) {
    enginecycle::Cycle c;
    c.setCycleAngle(720.0);
    c.setRpm(rpm);
    c.addEdge("Coil 1", 100.0, true,  enginecycle::Signal::Coil, 1);
    c.addEdge("Coil 1", 120.0, false, enginecycle::Signal::Coil, 1);
    return c;
}

int main() {
    jf::JSceneGraph graph;

    std::puts("=== EngineCyclePanel (recorder ring + transport) ===");

    std::puts("\n-- recording follows the newest frame and asks for the next --");
    {
        EngineCyclePanel p(graph);
        int requests = 0;
        p.onCaptureRequest = [&requests] { ++requests; };
        p.setCapacity(5);

        p.startRecording();
        ck(p.recording(), "startRecording arms it");
        ck(requests == 1, "…and asks for a capture immediately", std::to_string(requests));

        // Arrival-driven: each delivered frame asks for the next. A timer would queue requests the
        // ECU has not answered; this self-paces to whatever the link can actually deliver.
        for (int i = 0; i < 3; ++i) p.addFrame(frameWithRpm(1000.0 + i));
        ck(requests == 4, "each delivered frame requests the next", std::to_string(requests));
        ck(p.displayedRpm() == 1002.0, "the view follows the newest frame",
           std::to_string(p.displayedRpm()));
    }

    std::puts("\n-- pause stops requesting, and holds the cursor still --");
    {
        EngineCyclePanel p(graph);
        int requests = 0;
        p.onCaptureRequest = [&requests] { ++requests; };
        p.setCapacity(10);
        p.startRecording();
        for (int i = 0; i < 3; ++i) p.addFrame(frameWithRpm(1000.0 + i));

        const int before = requests;
        p.pause();
        ck(!p.recording(), "pause clears the recording flag");
        ck(requests == before, "and asks for nothing further", std::to_string(requests - before));

        // A capture already in flight when pause was pressed still lands. It must NOT drag the view
        // off the frame being read — that is the whole point of pausing.
        const double held = p.displayedRpm();
        p.addFrame(frameWithRpm(9999.0));
        ck(p.displayedRpm() == held, "a late frame does not move a paused cursor",
           std::to_string(p.displayedRpm()));
        ck(p.frameCount() == 4, "…but it IS kept", std::to_string(p.frameCount()));
        ck(requests == before, "and still nothing is requested");
    }

    std::puts("\n-- a full ring drops the OLDEST and keeps the cursor on the same CYCLE --");
    {
        EngineCyclePanel p(graph);
        p.setCapacity(3);
        p.startRecording();
        for (int i = 0; i < 3; ++i) p.addFrame(frameWithRpm(100.0 + i));   // 100,101,102
        p.pause();
        p.stepTo(0);                                    // look at the OLDEST, rpm 100
        ck(p.displayedRpm() == 100.0, "showing the oldest", std::to_string(p.displayedRpm()));

        // The ring rolls. Index 0 now means a DIFFERENT cycle, so holding the index would silently
        // slide the view onto another frame while the user is reading it.
        p.addFrame(frameWithRpm(103.0));                // drops 100 -> ring is 101,102,103
        ck(p.frameCount() == 3, "still 3 frames", std::to_string(p.frameCount()));
        ck(p.displayedRpm() == 101.0,
           "the cursor moved with the data, not with the index",
           std::to_string(p.displayedRpm()));
    }

    std::puts("\n-- stepping clamps at both ends and never wraps --");
    {
        EngineCyclePanel p(graph);
        p.setCapacity(10);
        p.startRecording();
        for (int i = 0; i < 4; ++i) p.addFrame(frameWithRpm(200.0 + i));   // 200..203
        p.pause();

        p.stepTo(3);
        p.stepBy(+5);
        ck(p.displayedRpm() == 203.0, "forward past the end stops at the newest",
           std::to_string(p.displayedRpm()));
        p.stepBy(-99);
        ck(p.displayedRpm() == 200.0, "back past the start stops at the oldest",
           std::to_string(p.displayedRpm()));
        // Wrapping would be worse than clamping: stepping back from the first frame landing on the
        // newest looks exactly like the recorder jumping, and there is no cue that it wrapped.
        p.stepBy(-1);
        ck(p.displayedRpm() == 200.0, "…and stays there");
    }

    std::puts("\n-- stepping implies pause --");
    {
        EngineCyclePanel p(graph);
        int requests = 0;
        p.onCaptureRequest = [&requests] { ++requests; };
        p.setCapacity(10);
        p.startRecording();
        for (int i = 0; i < 3; ++i) p.addFrame(frameWithRpm(300.0 + i));
        ck(p.recording(), "recording before the step");

        p.stepBy(-1);
        // Deliberate navigation must win. If it did not, the next arriving frame would yank the view
        // back to the newest and make walking the history impossible.
        ck(!p.recording(), "a step stops the recording");
        const int after = requests;
        p.addFrame(frameWithRpm(999.0));
        ck(requests == after, "and no further captures are requested");
    }

    std::puts("\n-- shrinking the ring keeps the NEWEST frames --");
    {
        EngineCyclePanel p(graph);
        p.setCapacity(10);
        p.startRecording();
        for (int i = 0; i < 8; ++i) p.addFrame(frameWithRpm(400.0 + i));   // 400..407
        p.pause();

        p.setCapacity(3);
        ck(p.frameCount() == 3, "trimmed to the new capacity", std::to_string(p.frameCount()));
        // The recent cycles are the ones being looked at; dropping them and keeping the start of the
        // session would throw away exactly what the user is chasing.
        p.stepTo(0);
        ck(p.displayedRpm() == 405.0, "the oldest kept frame is 405, not 400",
           std::to_string(p.displayedRpm()));
    }

    std::puts("\n-- an empty recorder is safe to drive --");
    {
        EngineCyclePanel p(graph);
        p.setCapacity(5);
        p.stepBy(-1);
        p.stepBy(+1);
        p.stepTo(0);
        ck(p.frameCount() == 0, "no frames, no crash, nothing displayed");
        p.clearFrames();
        ck(p.frameCount() == 0, "clearFrames on an empty ring is a no-op");
    }

    std::puts("\n-- the panel tells the user when a frame disagrees with the one before it --");
    {
        // The recorder's whole point is comparing cycles, so the frame on screen is diagnosed
        // against its NEIGHBOUR in the ring. A split cycle must be named at the moment it is
        // displayed, not left for the user to count bars.
        EngineCyclePanel p(graph);
        p.startRecording();          // live: the view follows each arriving frame, and so must the readout
        auto frame = [](int teeth) {
            enginecycle::Cycle c;
            c.setCycleAngle(720.0);
            for (int i = 0; i < teeth; ++i)
                c.addEdge("Crank", 0.1 + i * 6.0, true, enginecycle::Signal::Crank, -1);
            c.sort();
            return c;
        };
        p.addFrame(frame(116));
        ck(p.diagnosis().findings.empty(), "a steady frame reports nothing");
        ck(p.diagnosis().summary().find("116 crank") != std::string::npos,
           "and its summary states the count", p.diagnosis().summary());

        p.addFrame(frame(37));                       // the split cycle arrives
        bool named = false;
        for (const auto& f : p.diagnosis().findings)
            if (f.text.find("crank 37 edges, was 116") != std::string::npos) named = true;
        ck(named, "the split frame is named as soon as it is shown");

        // Scrubbing back to the good frame must clear it — the readout describes what is on screen.
        p.stepTo(0);
        ck(p.diagnosis().summary().find("116 crank") != std::string::npos,
           "stepping back re-diagnoses the frame now displayed", p.diagnosis().summary());
    }

    std::puts("\n-- the same engine cycle fetched twice is stored once --");
    {
        // The link reads faster than the engine turns, so the ECU hands back the last complete cycle
        // repeatedly. The ring must fill with distinct CYCLES, not with repeats of one.
        EngineCyclePanel p(graph);
        int requests = 0;
        p.onCaptureRequest = [&requests] { ++requests; };
        p.startRecording();
        auto frame = [](uint32_t seq) {
            enginecycle::Cycle c;
            c.setCycleAngle(720.0);
            c.setSequence(seq);
            for (int i = 0; i < 116; ++i)
                c.addEdge("Crank", 0.1 + i * 6.0, true, enginecycle::Signal::Crank, -1);
            c.sort();
            return c;
        };
        for (uint32_t s : {7u, 7u, 7u, 8u, 8u, 9u}) p.addFrame(frame(s));
        ck(p.frameCount() == 3, "6 captures of 3 cycles -> 3 frames",
           std::to_string(p.frameCount()));
        ck(requests >= 6, "and every duplicate still asked for the next capture",
           std::to_string(requests) + " requests");

        // A producer that does not number its frames must not be collapsed to one.
        EngineCyclePanel q(graph);
        q.startRecording();
        for (int i = 0; i < 4; ++i) q.addFrame(frame(0));
        ck(q.frameCount() == 4, "unnumbered frames are all kept", std::to_string(q.frameCount()));
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All EngineCyclePanel tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
