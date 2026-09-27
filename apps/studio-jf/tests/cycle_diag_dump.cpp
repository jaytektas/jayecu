// cycle_diag_dump — run REAL ECU capture bytes through the SHIPPED studio decoder and diagnostics.
//
// The unit tests build cycles in memory, which proves the logic and nothing about the wire. This
// takes the actual 0x26 page bytes off the bench link and puts them through cyclewire::decode() and
// enginecycle::diagnose() — the same two functions the studio itself calls — so the hardware rig
// exercises the code that ships rather than a second implementation that could agree with the first
// and both be wrong.
//
// Input: a file of frames, each as
//     [u32 little-endian byte length][that many bytes of concatenated 0x26 pages]
// which is what tools/bench_cycle_diag.py writes. Prints one line per frame plus every finding, and
// exits non-zero if any frame is UNTRUSTWORTHY — so a bench rig can gate on it.
#include "../src/model/CycleDiagnostics.h"
#include "../src/model/CycleWire.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cycle_diag_dump <frames.bin>\n"); return 2; }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

    enginecycle::Cycle prev;
    bool  havePrev = false;
    int   frames = 0, flagged = 0, untrusted = 0;

    for (;;) {
        uint8_t lenb[4];
        if (std::fread(lenb, 1, 4, f) != 4) break;
        const uint32_t len = static_cast<uint32_t>(lenb[0]) | (static_cast<uint32_t>(lenb[1]) << 8) |
                             (static_cast<uint32_t>(lenb[2]) << 16) | (static_cast<uint32_t>(lenb[3]) << 24);
        std::vector<uint8_t> buf(len);
        if (len && std::fread(buf.data(), 1, len, f) != len) break;

        enginecycle::Cycle c;
        const auto r = cyclewire::decode(buf.data(), buf.size(), c);
        if (!r.ok) { std::printf("  frame %-3d DECODE FAILED: %s\n", frames, r.message.c_str()); ++untrusted; ++frames; continue; }
        c.sort();

        const auto d = enginecycle::diagnose(c, havePrev ? &prev : nullptr);
        std::printf("  frame %-3d seq=%-6u %-52s %s\n", frames, c.sequence(), d.summary().c_str(),
                    d.trustworthy ? "" : "UNTRUSTWORTHY");
        for (const auto& fi : d.findings)
            std::printf("            %s %s\n",
                        fi.severity == enginecycle::Severity::Error   ? "[!]" :
                        fi.severity == enginecycle::Severity::Warning ? "[~]" : " · ",
                        fi.text.c_str());
        if (!d.findings.empty())  ++flagged;
        if (!d.trustworthy)       ++untrusted;

        prev = c; havePrev = true;
        ++frames;
    }
    std::fclose(f);
    std::printf("\n  %d frames, %d with findings, %d untrustworthy\n", frames, flagged, untrusted);
    return untrusted ? 1 : 0;
}
