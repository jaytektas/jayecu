// Run the fitter over every raw capture in a directory and say what it made of each.
//
// The captures come off the bench rig with the ECU told NOTHING about the wheel — one generic
// both-edges config for all of them — so what this prints is exactly what a person would see after
// cranking an unknown engine. Compare it against the stim's own pattern tables, never against a
// tooth count read off a web page.
#include "model/TriggerFit.h"
#include "model/TriggerLog.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static const char* shapeName(triggerfit::Shape s) {
    switch (s) {
        case triggerfit::Shape::Even:  return "Even";
        case triggerfit::Shape::Gap:   return "Gap";
        case triggerfit::Shape::Width: return "Width";
        case triggerfit::Shape::Coded: return "Coded";
        default:                       return "?";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: fit_report <dir>\n"); return 2; }
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(argv[1]))
        if (e.path().extension() == ".csv") files.push_back(e.path());
    std::sort(files.begin(), files.end());

    for (const auto& p : files) {
        std::vector<triggerlog::Record> recs;
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) {
            std::replace(line.begin(), line.end(), ',', ' ');
            std::istringstream ss(line);
            unsigned long t, lv, fl;
            if (!(ss >> t >> lv >> fl)) continue;
            triggerlog::Record r;
            r.tUs = uint32_t(t); r.levels = uint8_t(lv); r.flags = uint8_t(fl);
            recs.push_back(r);
        }
        std::printf("\n%-34s %5zu records\n", p.filename().string().c_str(), recs.size());
        std::vector<triggerlog::Lane> lanes;
        const auto res = triggerlog::decodeLanes(recs, { "Crank 1", "Crank 2", "Cam 1", "Cam 2" }, lanes);
        if (!res.ok || lanes.empty()) { std::printf("    lanes: %s\n", res.message.c_str()); continue; }

        const triggerfit::CaptureFit c = triggerfit::fitCapture(lanes);
        std::printf("    capture: %s", c.ok ? "ok" : ("REFUSED — " + c.why).c_str());
        if (c.ok) std::printf("  rpm %.0f  crank=stream %d  phase %s  sync %s",
                              c.rpm, c.crankIdx >= 0 ? c.streams[c.crankIdx].stream : -1,
                              c.phase ? "yes" : "no", c.canSync ? "yes" : ("NO — " + c.syncNote).c_str());
        std::printf("\n");
        for (const auto& s : c.streams) {
            std::printf("    s%d %-8s %-5s ", s.stream, s.name.c_str(), shapeName(s.shape));
            if (!s.ok) { std::printf("refused — %s\n", s.why.c_str()); continue; }
            if (s.shape == triggerfit::Shape::Even && s.teeth == 0)
                std::printf("%3d teeth seen at %.0f us, no landmark — the turn must come from "
                            "elsewhere", s.edgesSeen, s.pitchUs);
            else
                std::printf("teeth %3d  missing %d  edges %3d  %s  cycle 1/%d  key %s",
                            s.teeth, s.missing, s.edges, s.bothEdges ? "both" : "one ",
                            s.cycleRatio, s.uniqueKey ? "yes" : "no");
            if (!s.anomalies.empty()) {
                std::printf("  anomalies:");
                for (size_t i = 0; i < s.anomalies.size() && i < 8; ++i)
                    std::printf(" %.2f@%.0f", s.anomalies[i].ratio, s.anomalies[i].atDeg);
            }
            std::printf("\n");
        }
    }
    return 0;
}
