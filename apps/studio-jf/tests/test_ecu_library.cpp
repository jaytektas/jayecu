// SPDX-License-Identifier: GPL-3.0-or-later
// The ECU library's memory of a board: the board its firmware says it is NOW, and the name its STM32
// bootloader answers to — so a recovery can recognise a board it has met instead of asking for a guess.
#include "../src/model/Ecu.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    // A throwaway home: the library lives under $HOME/.local/share/jayecu.
    std::error_code ec;
    const fs::path home = fs::temp_directory_path() / "jayecu_ecu_library_test";
    fs::remove_all(home, ec);
    fs::create_directories(home, ec);
    setenv("HOME", home.string().c_str(), 1);

    const std::string uid = "004200313138510939323531";
    std::printf("=== a board reflashed to another board's firmware ===\n");
    {
        std::unique_ptr<Ecu> e(Ecu::openOrCreate(uid, "proteus_f7"));   // first met running a Proteus build
        ck(e && e->board() == "proteus_f7", "filed under the board its firmware named");
    }
    {
        std::unique_ptr<Ecu> e(Ecu::openOrCreate(uid, "jaytek_v1"));    // the same chip, reflashed
        ck(e && e->board() == "jaytek_v1", "…and under the new one once it runs that", e ? e->board() : "null");
    }
    ck(Ecu::summary(uid).board == "jaytek_v1", "the library's own summary agrees");

    std::printf("=== the bootloader's name for it ===\n");
    ck(Ecu::byDfuSerial("3974356C3138").uid.empty(), "a bootloader never seen names no ECU");
    {
        std::unique_ptr<Ecu> e(Ecu::open(uid));
        if (e) e->setDfuSerial("3974356C3138");
    }
    const Ecu::Summary s = Ecu::byDfuSerial("3974356C3138");
    ck(s.uid == uid && s.board == "jaytek_v1", "seen once, it names the ECU and its board", s.uid + " " + s.board);
    ck(Ecu::byDfuSerial("").uid.empty(), "an empty serial names nothing");
    ck(Ecu::byDfuSerial("000000000000").uid.empty(), "nor does another bootloader");

    fs::remove_all(home, ec);
    std::printf(fails ? "=== %d FAILED ===\n" : "=== the library knows its boards ===\n", fails);
    return fails ? 1 : 0;
}
