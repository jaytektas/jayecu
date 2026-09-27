# JayECU

Engine control firmware for the STM32F767ZI, plus the codegen that feeds it and a desktop
tuning studio (`apps/studio-jf`, built on JFramework).

Everything in `generated/` and `shared/` is produced from `definition/` by
`codegen/codegen.py` — the schema is the source of truth, not the C++. None of it is stored in git:
every target below runs codegen first, so a fresh clone makes it on the first build, and the
firmware refuses to compile if `generated/` was last built for a different board. (The studio's own
tests read the meta, so run `make codegen` before building them on a fresh clone.)

---

## Licence

JayECU is **GPL-3.0-or-later** — see `LICENSE`. Modify it, build on it, sell it;
if you distribute a modified version, its source goes with it.

`LICENSE.exception` grants one additional permission: to combine JayECU with
STMicroelectronics middleware under SLA0044 (the STM32 USB Device Library) and
distribute the result. That permission is needed because the GPL does not let a
distributor pass on ST's device restriction, and it can only be given by the
copyright holder.

`THIRD-PARTY.md` lists every component this builds or links and its licence.

The **hardware is not covered by any of this.** A published schematic is for
understanding and repair; it grants no right to manufacture.

---

## 1. Host requirements

Developed and tested on Ubuntu 26.04. Anything with an `arm-none-eabi` toolchain and CMake
≥ 3.21 should work.

### Everything except the studio and flashing

```bash
sudo apt install build-essential cmake git python3 python3-yaml python3-serial \
                 gcc-arm-none-eabi binutils-arm-none-eabi \
                 libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
```

`ninja-build` is optional — the CMake configs here default to Unix Makefiles.

### Flashing

```bash
sudo apt install dfu-util stlink-tools
```

`dfu-util` is required by `make dfu`. `stlink-tools` is not needed by `make flash`, but it
provides `st-info` (probe diagnostics) and an alternate flash path — see §5.

### Studio (optional)

The studio is `apps/studio-jf`, built on **JFramework** — an in-house toolkit with no Qt,
no GTK. It lives in a sibling repo (`../JFramework`) and is consumed as an *installed SDK*,
never as a source subdirectory.

```bash
sudo apt install pkg-config ninja-build ccache mold glslc \
                 libxcb1-dev libxcb-keysyms1-dev libxcb-sync-dev \
                 libdbus-1-dev libatspi2.0-dev libvulkan-dev \
                 libsqlite3-dev libssl-dev
```

`atspi-2` (accessibility) and `sqlite3` are optional to CMake but installed above so the
feature set does not vary silently between machines. `glslc` is optional: with it the
Vulkan shaders are compiled from `shaders/`, without it CMake falls back to the pre-built
`ShaderSpirv.h`. `ccache` and `mold` are picked up automatically if present — and the
studio's CMake **caches their paths**, so a build tree configured on a machine that had
them will fail with `/usr/bin/ccache: No such file or directory` on one that does not.
Install them, or delete the build tree.

### Cross-compiling the studio to Windows (optional)

```bash
sudo apt install g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev
```

No cross-OpenSSL is needed: on Windows the framework's TLS comes from WinHTTP, an OS
component, so `libj_platform.a` carries no OpenSSL symbols at all. Vulkan comes from the
import library checked into the framework (`libvulkan-1.a` + `cmake/vulkan-1.def`).

### Network, on the first firmware build only

`firmware/CMakeLists.txt` clones **STM32CubeF7 v1.17.1** from GitHub into `.stm32_cache/`
via FetchContent. That directory persists across builds and is outside `build/`, so it is
fetched once. The first `make firmware` on a fresh checkout needs working network and git.

---

## 2. STM32CubeProgrammer — required by `make flash`

`make flash` drives ST's `STM32_Programmer_CLI`. It is **not** in apt, not on snap, and the
download is behind an ST account and licence acceptance, so it cannot be scripted. Get
`SetupSTM32CubeProgrammer_linux_64.zip` from
<https://www.st.com/en/development-tools/stm32cubeprog.html>, then:

```bash
cd ~/Downloads
unzip SetupSTM32CubeProgrammer_linux_64.zip -d cubeprog_setup
cd cubeprog_setup
./SetupSTM32CubeProgrammer-*.linux            # GUI
./SetupSTM32CubeProgrammer-*.linux -console   # or text mode, no X needed
```

**Accept the default install path.** The Makefile's `ST_PROG` hardcodes
`~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI`, which is the
installer's default. Install it elsewhere and you must pass `ST_PROG=...` on every
invocation.

Two traps in the console installer:

- **The pack prompts default to "no".** It asks `Enter Y for Yes, N for No:` for
  `STM32CubeProgrammer` and `STM32TrustedPackageCreator`. Pressing Enter selects
  `Core Files` only — an install with no programmer in it. Answer `Y` to the first.
  `STM32TrustedPackageCreator` is not used here; `N` is fine.
- **It is Java.** The zip ships a bundled `jre/`. If the installer will not start,
  `sudo apt install default-jre`.

Verify:

```bash
~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI -l
```

That should list your ST-LINK with its serial and firmware version.

---

## 3. USB device access — read this before you throw your computer

The ECU appears as a USB CDC serial port. On a stock Linux install you will not be able to
open it, and the failure surfaces at the *end* of `make flash`, after the firmware is
already written:

```
serial.serialutil.SerialException: [Errno 13] could not open port /dev/ttyACM0
```

Install the project's udev rules and join `dialout` and `plugdev`:

```bash
sudo rm -f /etc/udev/rules.d/99-jayecu.rules   # an older copy under the old name
sudo cp tools/udev/70-jayecu.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG dialout,plugdev $USER
```

Then **log fully out and back in, or reboot.**

Supplementary groups are fixed at login. Opening a new terminal tab is not enough — it
inherits the group list of the desktop session that started before `usermod` ran. Until you
re-login, every tool that opens the port keeps failing while `id -nG $USER` cheerfully
shows `dialout`, because that reads the user database, not your process. Compare `id -nG`
(your shell) against `id -nG $USER` (the database); if they disagree, that is this.

Both groups matter, for different devices: `dialout` for the ECU's serial port
(`/dev/ttyACM0`, symlinked `/dev/jayecu`), `plugdev` for the DFU bootloader node that
`make dfu` writes to.

To run anything without re-logging in, `sudo -u $USER -i` starts a login shell, which
rebuilds the group list from the user database:

```bash
sudo -u $USER -i bash -c "cd $PWD && make dfu"
sudo -u $USER -i bash -c "cd $PWD && python3 tools/push_meta.py --meta shared/tuneit-meta.json"
```

(`newgrp` / `sg` are not installed by default on Ubuntu 26.04, so the usual shortcut is
not available.)

Why the rules file matters beyond the group: `dfu-util`'s packaged rule tags the bootloader
`uaccess`, which grants access only to the active *local seat* session. Over SSH, from CI,
or with the seat at a login greeter, DFU fails — and it fails *after* the board is already
in the bootloader, where the only way out is a successful write or a power cycle. The
project rule adds `GROUP`/`MODE` alongside the tag. See the comments in
`tools/udev/70-jayecu.rules`.

---

## 4. Building

Every target takes `BOARD=<name>`, defaulting to `jaytek_v1`. Boards are
`definition/boards/*.board.yaml` — currently `jaytek_v1` and `proteus_f7`.

| Command | What it does |
|---|---|
| `make` | codegen only — regenerates `generated/` and `shared/`, installs studio meta |
| `make tests` | build and run the native unit tests |
| `make firmware` | codegen + build the ECU firmware, no hardware touched |
| `make flash` | build, mass-erase, write over SWD, reset, push meta (needs CubeProgrammer) |
| `make dfu` | build and flash over USB DFU — no ST-LINK needed |
| `make studio` | build the tuning studio (`apps/studio-jf`) |
| `make studio-run` | build and launch it |
| `make studio-win` | cross-build `studio.exe` for Windows |
| `make all` | build the firmware, then the studio for Linux and Windows |
| `make package` | both studio packages: the Linux AppImage and the Windows installer |
| `make stim` | clone the bench stim firmware (our [Ardu-Stim fork](https://github.com/jaytektas/Ardu-Stim/tree/jaytek)) into `tools/Ardu-Stim` |
| `make clean` | remove every build: tests, firmware, studio and the manual |
| `make help` | list every target |

```bash
make firmware                      # jaytek_v1, Release
make firmware BOARD=proteus_f7
make flash FW_BUILD_TYPE=Debug     # -Og -g3 for debugger work
```

Firmware defaults to **Release** (`-Os`). That is deliberate: an unoptimised image costs
roughly thirteen points of the 1 kHz engine frame on the rig, spent on a build nobody
chose. Only pass `FW_BUILD_TYPE=Debug` when you intend to step through it.

Build output lands in `firmware/build/$(BOARD)/` — per board, so switching boards does not
invalidate the other's CMake cache.

### Building the studio

`make studio` will fail with `Could not find a package configuration file provided by
"JFramework"` until the framework SDK is installed. Build and install it first — this is a
one-time step, repeated only when a framework header changes:

```bash
cmake -S ../JFramework -B ../JFramework/build
cmake --build ../JFramework/build --target j_platform --parallel
cmake --install ../JFramework/build --prefix ~/jframework-sdk
make studio            # -> apps/studio-jf/build/studio
```

For Windows, the framework must be built **again** with the MinGW toolchain into a separate
prefix — a Linux `.a` cannot satisfy a mingw link:

```bash
cmake -S ../JFramework -B ../JFramework/build-win \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/../JFramework/cmake/mingw-w64.cmake
cmake --build ../JFramework/build-win --target j_platform --parallel
cmake --install ../JFramework/build-win --prefix ~/jframework-sdk-win
make studio-win        # -> apps/studio-jf/build-win/studio.exe
```

Override `JF_SDK`, `JF_SDK_WIN` or `JF_MINGW_TOOLCHAIN` on the make line if your checkout
does not sit beside `JFramework`.

`libj_platform.a` is a *static* library, so changing a public framework header means
reinstalling the SDK before relinking the studio — and adding or removing a base-class
virtual shifts the vtable, which needs `j_platform` rebuilt and copied, not just the header.

---

## 5. Flashing notes

`make flash` **mass-erases first**, then writes the `.elf`. Both halves are deliberate.

Without the mass erase, the programmer erases only the sectors the image covers, so config
bank B (`0x081C0000`, deliberately not in the ELF) survives carrying whatever tune was last
burned there. Every burn takes a higher sequence than bank A's seeded default, so that
stale tune then *wins boot arbitration* on the next start — and if its `layout_hash` no
longer matches it is rejected, leaving the ECU on compiled defaults with no stored tune at
all. Writing the `.elf` rather than a `.bin` means only populated sections are written: the
app and the default tune at bank A, skipping the 1.5 MB gap between them.

`make dfu` writes `code.bin` (application only), so the config banks are left alone. That
is what you want for a firmware bump that does not move the `layout_hash` — a tune already
burned to bank A or B survives.

There is also a CMake `flash` target built on `st-flash`, available when `stlink-tools` was
installed at configure time:

```bash
cmake --build firmware/build/jaytek_v1 --target flash
```

It writes `tune.bin` and `code.bin` at their own addresses (~10 s, skipping the gap) and
**does not mass-erase**, so the bank B hazard above applies. It also skips the meta push.
Prefer `make flash` unless you know why you want this one.

---

## 6. Troubleshooting

**`is not a full path to an existing compiler tool`, naming a path that does not exist.**
A stale `CMakeCache.txt` is pinning a toolchain from another machine. CMake will not fall
back to the toolchain file. The build tree is gitignored, so delete it:

```bash
rm -rf firmware/build/<board>
```

**`make flash` → `No such file or directory` on `STM32_Programmer_CLI`.** CubeProgrammer
is not installed, or not at the default path. See §2.

**`Permission denied: '/dev/ttyACM0'` at the end of `make flash`.** Group membership is not
active in your session yet. See §3 — the firmware *was* flashed and verified; only the meta
push failed, and re-running `push_meta.py` is enough.

**Probe diagnostics.** `st-info --probe` should report the chip id and flash size
(`0x451` / `STM32F76x_F77x` / 2 MB for this target).
