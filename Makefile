# JayECU convenience Makefile
# ─────────────────────────────
#  make                  → codegen (the default goal)
#  make help             → list these targets
#  make all              → build the firmware, then the studio (Linux, then Windows)
#  make package          → both studio packages: the Linux AppImage and the Windows installer
#  make release          → everything one GitHub release carries, in release/v<studio version>/
#
#  Firmware — every target here takes BOARD=<name>, default jaytek_v1 (definition/boards/*.board.yaml)
#  make codegen          → regenerate generated/ and shared/ (ecu.ini, ecu.json, tuneit-meta.json)
#  make firmware         → codegen + build the ECU firmware (no flashing)
#  make kit              → firmware + the kit the studio ships (image, meta, dashboard, kit.json)
#  make flash            → build firmware and flash to hardware via ST-Link
#  make dfu              → build firmware and update the ECU over USB DFU (no ST-Link needed;
#                          a virgin board is put into DFU by hand: hold BOOT, toggle RESET)
#  make push             → push meta + studio layout to the ECU's SD card (no reflash)
#  make dash             → install the board's authored dashboard into the local studio
#  make tests            → build and run native unit tests
#  make stim             → clone the bench stim (our Ardu-Stim fork, branch jaytek) into tools/Ardu-Stim
#
#  Studio
#  make sdk              → build + install JFramework (the studio's SDK) at the pinned commit, Linux
#  make sdk-win          → the same for the Windows build (needs mingw-w64)
#  make studio           → build the desktop tuning studio (apps/studio-jf, on JFramework)
#  make studio-run       → build the studio and launch it
#  make studio-win       → cross-build studio.exe for Windows (MinGW-w64)
#  make studio-appimage  → Linux package: jayecu-studio-<ver>-x86_64.AppImage
#  make studio-installer → Windows package: jayecu-studio-<ver>-setup.exe (Inno Setup under wine)
#  make bench-studio     → package it, put it on the bench (BENCH=<ssh host>) and check it starts there
#  make manual           → build the user manual (HTML) into manual/site
#  make manual-publish   → build it and publish it to GitHub Pages (jaytektas.github.io/jayecu)
#
#  Cleaning
#  make clean            → remove EVERY build: tests, firmware (all boards, with their kits), studio
#                          (Linux + Windows + installer output) and the manual
#  make tests-clean      → remove the unit-test build
#  make firmware-clean   → remove this board's firmware build (BOARD=<name>), kits included
#  make studio-clean     → remove the studio builds (Linux + Windows) and the installer output
#
#  generated/ holds ONE board at a time (codegen overwrites it), so codegen and the firmware build are
#  both driven from BOARD, and the firmware refuses to compile if generated/ was built for another board.

PYTHON    ?= python3
# the target board — one name, used everywhere (change it to target a different board)
BOARD     ?= jaytek_v1
BUILD_DIR  = tests/build
# PER BOARD, so switching boards does not invalidate the other's CMake cache and object
# tree. Both live under firmware/build/, which .gitignore already covers.
FW_BUILD   = firmware/build/$(BOARD)
STUDIO_BUILD = apps/studio-jf/build
STUDIO_WIN_BUILD = apps/studio-jf/build-win
# The studio consumes JFramework as an INSTALLED SDK, never as a source subdirectory — nothing
# from the studio leaks back into the framework. `make sdk` (Linux) and `make sdk-win` (Windows) build and
# install it, at the JFramework commit the studio is pinned to (JF_PIN).
JF_SDK     ?= $(HOME)/jframework-sdk
JF_SDK_WIN ?= $(HOME)/jframework-sdk-win
# The Windows build's toolchain and Vulkan come from the Windows SDK: `make sdk-win` puts them there — the
# toolchain file and Vulkan headers copied out of JFramework, and the import library generated from them.
JF_MINGW_TOOLCHAIN ?= $(JF_SDK_WIN)/mingw-w64.cmake
VULKAN_WIN_INC ?= $(JF_SDK_WIN)/include
VULKAN_WIN_LIB ?= $(JF_SDK_WIN)/lib/libvulkan-1.a
# THE JFRAMEWORK THIS STUDIO IS BUILT AGAINST. The SDK is a separate repository, so nothing else ties a
# jayecu commit to the framework it needs: a fresh clone of the framework's newest could be ahead of the
# studio or behind it. Move the pin in the same commit as a studio change that needs a newer framework.
JF_REPO ?= https://github.com/jaytektas/JFramework.git
JF_PIN  := $(shell cat apps/studio-jf/jframework.commit 2>/dev/null)
# make's own clone: checked out at the pin, so it never moves a JFramework checkout you are working in.
JF_SRC  ?= $(CURDIR)/.jframework
# STM32CubeProgrammer's CLI. Found on PATH by default; point it elsewhere with  make ST_PROG=/path/to/STM32_Programmer_CLI
ST_PROG   ?= STM32_Programmer_CLI
# Studio meta library. MUST match the org/app names the studio uses ("jayecu" / "jayecu Studio")
# → ~/.local/share/jayecu/jayecu Studio/meta. studio-jf kept the same data root as the retired Qt
# studio, so this path did not move when the app was replaced. The studio
# matches a connected ECU to a meta here by layout_hash, so codegen installs the fresh meta into it.
# (Legacy ~/.local/share/jayecu/studio/meta is the pre-rename path — no longer read by the app.)
META_LIB  ?= $(HOME)/.local/share/jayecu/jayecu Studio/meta

.DEFAULT_GOAL := codegen
.PHONY: help all package release manual-publish ship-kits stim sdk sdk-win jframework-src sdk-check sdk-win-check codegen tests clean tests-clean firmware-clean studio-clean firmware flash dfu push dash studio-meta studio studio-run studio-win studio-appimage studio-installer kit bench-studio bench-beta beta-release manual

# codegen.py emits shared/tuneit-meta.json (the TuneIt Data Dictionary) along with the C++
# headers, ecu.ini and ecu.json — so a bare `make` already produces it, then installs it into
# the studio's meta library.
codegen:
	@git config core.hooksPath tools/hooks 2>/dev/null || true   # the change-note reminder (tools/hooks)
	bash codegen/update_version.sh $(BOARD)
	$(PYTHON) codegen/codegen.py --board $(BOARD)
	$(PYTHON) codegen/board_codegen.py definition/boards/$(BOARD).board.yaml generated/boards/$(BOARD)_board.h
	@$(MAKE) --no-print-directory studio-meta

# Install shared/can_templates/*.json into the studio's data root, beside the meta library. The
# published protocols are studio DATA now, not a firmware table, so they are installed rather than
# compiled — and a corrected template reaches the studio by reinstalling, without touching the ECU.
#
# THE DIRECTORY IS REPLACED, NOT ADDED TO. This used to copy over the top, so a template that was
# renamed or withdrawn stayed installed for ever: retiring the unnamed wideband template left it on
# the list beside the Haltech WB1 that replaced it, two names offering the same frame on 0x2B1, and
# the only machine where it looked right was one that had never had the old one.
CAN_TPL_LIB ?= $(HOME)/.local/share/jayecu/jayecu Studio/can_templates

# The SHIPPED dashboard library, beside the meta library. A dashboard is authored per board and
# lives in the firmware tree (definition/boards/<board>.dashboard.gui); this installs it where the
# studio looks when it meets an ECU it has no document for. That is what makes `git clone`, `make
# flash`, open-the-studio work on a machine that has never seen this project: the authored layout
# is on disk before the studio ever asks for it.
DASH_LIB  ?= $(HOME)/.local/share/jayecu/jayecu Studio/dashboards

# Install shared/tuneit-meta.json into the studio's meta library as "<board> <layout_hash>.meta".
# Board + layout_hash is the complete identity — firmware name/version/build are noise for lookup.
studio-meta:
	@$(PYTHON) -c "import os,shutil,glob; \
d=os.path.expanduser('$(CAN_TPL_LIB)'); os.makedirs(d,exist_ok=True); \
stale=[os.remove(f) for f in glob.glob(os.path.join(d,'*.json'))]; \
n=[shutil.copy(f,d) for f in glob.glob('shared/can_templates/*.json')]; \
print('  can templates -> %s (%d installed, %d stale removed)'%(d,len(n),len(stale)))"
	@$(PYTHON) -c "import os,sys; sys.path.insert(0,'tools'); import stamp_dashboard as S; \
src=os.path.join('definition','boards','$(BOARD).dashboard.gui'); \
d=os.path.expanduser('$(DASH_LIB)'); os.makedirs(d,exist_ok=True); dst=os.path.join(d,'$(BOARD).gui'); \
h=S.load_meta('shared/tuneit-meta.json')['meta']['layout_hash']; ex=os.path.join(d,'$(BOARD) '+h+'.gui'); \
(S.stamp_file(src,dst), S.stamp_file(src,ex), print('  dashboard -> '+dst+' and '+ex)) if os.path.exists(src) \
else print('  no authored dashboard for $(BOARD) — the studio will seed a tree from the meta')"
	@$(PYTHON) -c "import json,os,shutil; \
raw=open('shared/tuneit-meta.json','rb').read(); \
m=json.loads(raw[:-4])['meta']; \
d=os.path.expanduser('$(META_LIB)'); os.makedirs(d,exist_ok=True); \
n='%s %s.meta'%(m['board'],m['layout_hash']); \
shutil.copy('shared/tuneit-meta.json',os.path.join(d,n)); \
print('  studio meta -> '+os.path.join(d,n))"

tests: codegen
	@mkdir -p $(BUILD_DIR)
	cmake -S tests -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-Wall -Wextra" 2>&1
	cmake --build $(BUILD_DIR) --parallel
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# WHAT GOES IN THE ECU IS OPTIMISED. This passed no build type, so CMake took its default — Debug,
# -Og — and every ECU flashed from this tree has been running an unoptimised image. Measured on the
# rig at 8000 rpm with a full tune: 44 % of the 1 kHz engine frame against 31 %, and a worst frame of
# 59 % against 43 %. That is thirteen points of the loop that decides whether spark is late, spent on
# a build nobody chose.
#
#   make flash                          -> Release (-Os), what an engine should run
#   make flash FW_BUILD_TYPE=Debug      -> -Og -g3, for stepping through it with a debugger
FW_BUILD_TYPE ?= Release

# Build the ECU firmware for $(BOARD), without touching any hardware. -DBOARD is passed
# EXPLICITLY on every configure: CMake caches it, so omitting it here would silently keep
# whatever board the directory was first configured for while codegen had already moved on.
firmware: codegen
	@mkdir -p $(FW_BUILD)
	cmake -S firmware -B $(FW_BUILD) -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/cmake/stm32f7xx.cmake \
	      -DCMAKE_BUILD_TYPE=$(FW_BUILD_TYPE) -DBOARD=$(BOARD)
	cmake --build $(FW_BUILD) --parallel
	@echo "  firmware: $(FW_BUILD)/jayecu.elf  (board $(BOARD), $(FW_BUILD_TYPE))"

# Flash over USB DFU — the path for a board with NO ST-LINK. The firmware's `dfu` CLI
# command reboots into the STM32 system bootloader and dfu-util writes it.
#
# Needs tools/udev/70-jayecu.rules installed on Linux, and dfu_flash.py CHECKS that before
# it puts the board into the bootloader: dfu-util's packaged rule grants access only to the
# active local seat session, so over SSH or from a script the write fails AFTER the board is
# already in DFU and no longer reachable over serial. Ask first, strand never.
#
# Writes code.bin (the application only), so the config banks are left alone — unlike
# `make flash`, which mass-erases. A tune already burned to bank A or B survives a DFU
# update, which is what you want for a firmware bump that does not move the layout_hash.
# Update the ECU over USB (no ST-LINK). A board running jayecu is told to restart into its bootloader;
# a virgin board is put there by hand (hold BOOT, press and release RESET, release BOOT) and this waits.
# Writes the application only, so the tune survives; the SD card is `make push`, as for `make flash`.
# PORT=/dev/ttyACMn picks the ECU when more than one is connected.
dfu: firmware
	$(PYTHON) tools/dfu_flash.py --board $(BOARD) $(if $(PORT),--port $(PORT))

# Show the authored dashboard in the LOCAL studio. The scripts in apps/studio-jf/tools/layout
# author definition/boards/<board>.dashboard.gui — in the working tree, in git, where a shipped
# artifact belongs. The studio reads a different file: its own per-ECU copy under
# ~/.local/share/jayecu/jayecu Studio/ecus/<uid>/dashboard.gui. This copies the first over the
# second, for every ECU folder whose ecu.json says it is this board.
#
# ONE DIRECTION ONLY, AND IT OVERWRITES. The repo file is the source; the studio's copy is a view
# of it. Nothing here ever copies the other way — the studio does not author what ships, and a
# studio that happened to save its own layout must never become the thing a release is cut from.
dash:
	@$(PYTHON) -c "import json,os,shutil,glob,sys; \
src=os.path.join('definition','boards','$(BOARD).dashboard.gui'); \
sys.exit('  no dashboard for $(BOARD) at '+src) if not os.path.exists(src) else None; \
root=os.path.expanduser('$(HOME)/.local/share/jayecu/jayecu Studio/ecus'); \
dirs=[d for d in glob.glob(os.path.join(root,'*')) if os.path.isfile(os.path.join(d,'ecu.json')) \
      and json.load(open(os.path.join(d,'ecu.json'))).get('board')=='$(BOARD)']; \
[shutil.copy(src,os.path.join(d,'dashboard.gui')) for d in dirs]; \
print('  dashboard -> %d $(BOARD) ecu dir(s)'%len(dirs)) if dirs else \
print('  no $(BOARD) ecu dirs yet (connect the studio to the board once)')"

# Push the descriptor + studio layout to the SD card WITHOUT touching the firmware. `make flash`
# ends with this anyway, but nothing about redrawing a layout needs the ECU reflashed — the file
# goes over the same serial link the studio uses.
push:
	$(PYTHON) tools/push_meta.py --meta shared/tuneit-meta.json \
	                            --gui definition/boards/$(BOARD).dashboard.gui

flash: firmware
	# MASS ERASE first. Without it the programmer only erases the sectors the image
	# covers, so config bank B (0x081C0000 — deliberately not in the ELF) survives a
	# reflash carrying whatever tune was last burned there. Because every burn takes a
	# higher sequence than bank A's seeded default (which is always sequence 1), that
	# stale tune then WINS boot arbitration on the next start, and if its layout_hash no
	# longer matches it is rejected — leaving the ECU on compiled defaults with no
	# stored tune at all. Erasing makes a flash mean what it says: this image, nothing else.
	$(ST_PROG) -c port=SWD -e all
	# Flash the .elf (not .bin): it writes only populated sections — the app AND the
	# default-tune image at bank A (0x08180000) — skipping the gap between them. A raw
	# .bin would zero-pad that 1.5 MB gap.
	#
	# THIS TARGET WRITES FIRMWARE. THAT IS ALL IT DOES. It used to push the meta to the SD
	# card afterwards, which cost a minute of serial on every flash and bought nothing: on
	# the development machine the studio resolves its meta from the local library that
	# `make codegen` fills, and reads its dashboard straight out of the ECU's own directory,
	# where the studio itself saved it. The SD copies exist for a machine that has NEITHER —
	# someone else's laptop plugging into this board — and that is `make push`, run when you
	# are shipping, not fifty times a day while you are working.
	$(ST_PROG) -c port=SWD -w $(FW_BUILD)/jayecu.elf -v -rst
	@echo "  flashed. the studio reads its meta from the local library (make codegen puts it"
	@echo "  there) and its dashboard from the ecu's own directory, so nothing else is needed"
	@echo "  here. 'make push' writes both to the SD CARD, which is for OTHER machines."

# Build the desktop tuning studio (apps/studio-jf, on JFramework). No source-list sync step — its
# CMakeLists globs with CONFIGURE_DEPENDS, so a new file is picked up on the next build.
# The studio's runtime meta library is installed separately by `make codegen` / studio-meta.
studio: sdk-check
	cmake -S apps/studio-jf -B $(STUDIO_BUILD) -DCMAKE_PREFIX_PATH=$(JF_SDK)
	cmake --build $(STUDIO_BUILD) --target studio --parallel

studio-run: studio
	$(STUDIO_BUILD)/studio

# Cross-build studio.exe for Windows (the target is further down). Needs the MinGW-w64 toolchain and a
# WINDOWS build of the framework at $(JF_SDK_WIN) — a Linux SDK cannot satisfy a mingw link: make sdk-win.
# The Linux release file. Only an AppImage can update itself (JFramework JSelfInstaller), so this is the form
# the studio ships in and the form the bench runs. Needs appimagetool on PATH.
studio-appimage: studio manual ship-kits
	KITS=$(SHIP_KITS) apps/studio-jf/tools/make_appimage.sh $(STUDIO_BUILD)

# The firmware KIT the studio ships with: image, descriptor, stamped dashboard and kit.json, in
# firmware/build/<board>/kits/<board>-<version>. Both studio packages carry the newest one.
FW_VERSION = $(shell cat firmware/version.txt)
kit: firmware
	$(PYTHON) tools/make_kit.py --board $(BOARD)

# EVERY BOARD'S KIT GOES IN BOTH PACKAGES, not just $(BOARD)'s. Recovery installs from the kits the studio
# has, and it only downloads kits for boards it has already met — so a studio shipped with one board's kit
# had no way to put firmware on a virgin board of any other. This gathers the kit of THIS firmware version
# for every board that has one built (`make release` builds them all) into one folder the packages copy.
SHIP_KITS = firmware/build/ship-kits
ship-kits: kit
	@rm -rf $(SHIP_KITS) && mkdir -p $(SHIP_KITS)
	@for d in firmware/build/*/kits/*-$(FW_VERSION); do \
	    [ -f "$$d/kit.json" ] && cp -r "$$d" $(SHIP_KITS)/ && echo "  ships kit: $$(basename $$d)"; done; true

# The Windows installer: studio.exe, the manual, the newest kit and the CAN templates, made with Inno
# Setup (under wine here). The version is the studio's own, from its CMakeLists project().
ISCC ?= wine C:/InnoSetup/ISCC.exe
STUDIO_VERSION = $(shell sed -n 's/^project.[a-z_]* VERSION \([0-9.]*\).*/\1/p' apps/studio-jf/CMakeLists.txt)
studio-installer: studio-win manual ship-kits
	cd apps/studio-jf && WINEDEBUG=-all $(ISCC) /DAppVersion=$(STUDIO_VERSION) \
	    '/DKitsDir=..\..\..\firmware\build\ship-kits' installer/studio.iss
	@echo "  installer: apps/studio-jf/installer/Output/jayecu-studio-$(STUDIO_VERSION)-setup.exe"

# The user manual: MkDocs + Material, pinned in manual/requirements.txt and installed into its own venv
# the first time. The settings reference is generated from the definition, so codegen runs first.
MANUAL_VENV = manual/.venv
manual: codegen
	@[ -x $(MANUAL_VENV)/bin/mkdocs ] || { $(PYTHON) -m venv $(MANUAL_VENV) && $(MANUAL_VENV)/bin/pip install -q -r manual/requirements.txt; }
	$(PYTHON) manual/tools/gen_settings.py
	$(PYTHON) manual/tools/gen_reference.py
	$(PYTHON) manual/tools/gen_indexes.py
	$(PYTHON) manual/tools/check_src.py
	cd manual && .venv/bin/mkdocs build --strict --quiet
	@echo "  manual: manual/site/index.html"

# The bench machine runs the studio; this machine builds it. Deploy the AppImage there under its fixed
# name (which the launcher and the updater both use) and prove it starts on the bench's own desktop.
# The bench is whatever machine you run the hardware from, reached over ssh:  make bench-studio BENCH=<host>
# A BETA ON THE BENCH: build, kit, version above what the bench has seen, install — in that order, checked
# (tools/bench_beta.sh; the `beta` skill). STUDIO=1 deploys the studio too when it is closed there.
bench-beta:
	@[ -n "$(BENCH)" ] || { echo "make bench-beta: say which machine — BENCH=<ssh host>" >&2; exit 1; }
	STUDIO=$(STUDIO) tools/bench_beta.sh $(BENCH) $(BOARD)

# A BETA RELEASE for the opt-in studios: beta studio packages + a beta kit per board in release/v<beta>/,
# published as a GitHub PRE-RELEASE (tools/beta_release.sh; the `beta` skill). Builds only — never publishes.
BETA_BOARDS ?= $(BOARD)
beta-release:
	BOARD=$(BOARD) tools/beta_release.sh "$(BETA_BOARDS)"

bench-studio: studio-appimage
	@[ -n "$(BENCH)" ] || { echo "make bench-studio: say which machine — BENCH=<ssh host>" >&2; exit 1; }
	apps/studio-jf/tools/bench_deploy.sh \
	    "$$(ls -t $(STUDIO_BUILD)/jayecu-studio-*-x86_64.AppImage | head -1)" $(BENCH)

studio-win: sdk-win-check
	cmake -S apps/studio-jf -B $(STUDIO_WIN_BUILD) \
	      -DCMAKE_TOOLCHAIN_FILE=$(JF_MINGW_TOOLCHAIN) \
	      -DJFramework_DIR=$(JF_SDK_WIN)/lib/cmake/JFramework \
	      -DVulkan_INCLUDE_DIR=$(VULKAN_WIN_INC) -DVulkan_LIBRARY=$(VULKAN_WIN_LIB)
	cmake --build $(STUDIO_WIN_BUILD) --target studio --parallel
	@echo "  studio: $(STUDIO_WIN_BUILD)/studio.exe"

# The bench stim's firmware is our fork of Ardu-Stim, kept in its own repository (GPL-3, not part of this
# one). tools/extract_stim_wheels.py reads its wheel tables from here. Build and upload it with PlatformIO
# from tools/Ardu-Stim/ardustim — `pio run -e nanonew -t upload` for a Nano with the new bootloader.
STIM_REPO ?= https://github.com/jaytektas/Ardu-Stim.git
stim:
	@if [ -d tools/Ardu-Stim/.git ]; then echo "  stim: tools/Ardu-Stim is already a clone"; \
	else git clone -b jaytek $(STIM_REPO) tools/Ardu-Stim; fi

# ---- The studio's SDK ------------------------------------------------------------------------------------
jframework-src:
	@[ -n "$(JF_PIN)" ] || { echo "no pin: apps/studio-jf/jframework.commit is missing" >&2; exit 1; }
	@[ -d "$(JF_SRC)/.git" ] || git clone -q $(JF_REPO) "$(JF_SRC)"
	@git -C "$(JF_SRC)" cat-file -e "$(JF_PIN)^{commit}" 2>/dev/null || git -C "$(JF_SRC)" fetch -q origin
	git -C "$(JF_SRC)" checkout -q --detach $(JF_PIN)

sdk: jframework-src
	cmake -S "$(JF_SRC)" -B "$(JF_SRC)/build" -DCMAKE_BUILD_TYPE=Release
	cmake --build "$(JF_SRC)/build" --target j_platform --parallel
	cmake --install "$(JF_SRC)/build" --prefix $(JF_SDK)
	@echo $(JF_PIN) > $(JF_SDK)/jframework.commit
	@echo "  sdk: $(JF_SDK)  (JFramework $(JF_PIN))"

# Windows has no libvulkan to link: an application links an IMPORT library naming each entry point in
# vulkan-1.dll, which the graphics driver installs. The cross toolchain has none, so it is generated from
# the framework's own Vulkan headers — every VKAPI_CALL entry point, vulkan_win32.h included, because the
# swapchain is created through vkCreateWin32SurfaceKHR and leaving it out fails only at the link.
VK_HDR = $(JF_SRC)/include/vulkan
sdk-win: jframework-src
	@command -v x86_64-w64-mingw32-dlltool >/dev/null || { echo "sdk-win needs mingw-w64: sudo apt install mingw-w64" >&2; exit 1; }
	mkdir -p $(JF_SDK_WIN)/lib $(JF_SDK_WIN)/include
	{ echo "LIBRARY vulkan-1.dll"; echo "EXPORTS"; \
	  grep -ohE "VKAPI_CALL (vk[A-Za-z0-9]+)" $(VK_HDR)/vulkan_core.h $(VK_HDR)/vulkan_win32.h | awk '{print $$2}' | sort -u; \
	} > $(JF_SDK_WIN)/lib/vulkan-1.def
	x86_64-w64-mingw32-dlltool -d $(JF_SDK_WIN)/lib/vulkan-1.def -l $(JF_SDK_WIN)/lib/libvulkan-1.a -D vulkan-1.dll
	cp -r "$(JF_SRC)/include/vulkan" "$(JF_SRC)/include/vk_video" $(JF_SDK_WIN)/include/
	cp "$(JF_SRC)/cmake/mingw-w64.cmake" $(JF_SDK_WIN)/mingw-w64.cmake
	cmake -S "$(JF_SRC)" -B "$(JF_SRC)/build-win" -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_TOOLCHAIN_FILE="$(JF_SRC)/cmake/mingw-w64.cmake" \
	      -DVulkan_INCLUDE_DIR=$(JF_SDK_WIN)/include -DVulkan_LIBRARY=$(JF_SDK_WIN)/lib/libvulkan-1.a
	cmake --build "$(JF_SRC)/build-win" --target j_platform --parallel
	cmake --install "$(JF_SRC)/build-win" --prefix $(JF_SDK_WIN)
	@echo $(JF_PIN) > $(JF_SDK_WIN)/jframework.commit
	@echo "  sdk-win: $(JF_SDK_WIN)  (JFramework $(JF_PIN))"

# A studio built against another framework than its pin still builds, which is how it goes unnoticed — so
# say so. An SDK installed by hand (no record of its commit) is taken as the developer's own.
sdk-check:
	@[ -d $(JF_SDK)/lib/cmake/JFramework ] || { echo "no JFramework SDK at $(JF_SDK): make sdk" >&2; exit 1; }
	@[ ! -f $(JF_SDK)/jframework.commit ] || [ "$$(cat $(JF_SDK)/jframework.commit)" = "$(JF_PIN)" ] || \
	  echo "  warning: the SDK at $(JF_SDK) is JFramework $$(cat $(JF_SDK)/jframework.commit), the studio is pinned to $(JF_PIN) — make sdk"
sdk-win-check:
	@[ -d $(JF_SDK_WIN)/lib/cmake/JFramework ] || { echo "no Windows JFramework SDK at $(JF_SDK_WIN): make sdk-win" >&2; exit 1; }
	@[ -f $(VULKAN_WIN_LIB) ] || { echo "no Windows Vulkan import library at $(VULKAN_WIN_LIB): make sdk-win" >&2; exit 1; }
	@[ ! -f $(JF_SDK_WIN)/jframework.commit ] || [ "$$(cat $(JF_SDK_WIN)/jframework.commit)" = "$(JF_PIN)" ] || \
	  echo "  warning: the SDK at $(JF_SDK_WIN) is JFramework $$(cat $(JF_SDK_WIN)/jframework.commit), the studio is pinned to $(JF_PIN) — make sdk-win"

# THE MANUAL ON THE WEB is the same HTML the studio ships, so the two can never disagree. The built site
# becomes the ONE commit of the gh-pages branch, pushed from a throwaway repository: the working tree is
# never switched, and the branch carries no history of old builds.
PAGES_REMOTE ?= $(shell git remote get-url origin)
manual-publish: manual
	@T=$$(mktemp -d) && cp -r manual/site/. $$T/ && touch $$T/.nojekyll && \
	  git -C $$T init -q -b gh-pages && git -C $$T add -A && \
	  git -C $$T -c user.name="$$(git config user.name)" -c user.email="$$(git config user.email)" \
	      commit -q -m "manual from $$(git rev-parse --short HEAD)" && \
	  git -C $$T push -q -f $(PAGES_REMOTE) gh-pages && rm -rf $$T && \
	  echo "  manual published: https://jaytektas.github.io/jayecu/  (from $$(git rev-parse --short HEAD))"

test_codegen: codegen
	$(PYTHON) tests/test_codegen.py

# The target list is the comment block at the top of this file — print it rather than keep a second copy.
help:
	@sed -n '3,/^$$/p' $(firstword $(MAKEFILE_LIST)) | sed '/^$$/d; s/^# \{0,1\}//'

# Firmware first, then the studio — one after the other, so a -j build does not interleave them.
all:
	$(MAKE) firmware
	$(MAKE) studio
	$(MAKE) studio-win

# Both release packages, each carrying the newest firmware kit and the manual.
package:
	$(MAKE) studio-appimage
	$(MAKE) studio-installer

# ONE RELEASE CARRIES BOTH: the studio packages and a firmware kit for every board, tagged with the STUDIO's
# version (the studio's updater compares the tag with its own). A kit for each other board first, then the
# packages for $(BOARD) — last, because generated/ holds one board at a time and the packages bundle the
# newest kit. FIRST, tools/release_notes.py prepare: if the firmware changed since the last published kit, its
# notes (firmware/CHANGES.md, Unreleased) must exist, and the version is raised and committed — nothing to
# remember at release time. tools/make_release.py then gathers it all into release/v$(STUDIO_VERSION)/, named the way both
# update checks read it, with the SHA256SUMS they verify against. Upload that folder's files as the release.
RELEASE_BOARDS ?= $(patsubst definition/boards/%.board.yaml,%,$(wildcard definition/boards/*.board.yaml))
release:
	$(PYTHON) tools/release_notes.py prepare --board $(BOARD)
	@for b in $(filter-out $(BOARD),$(RELEASE_BOARDS)); do $(MAKE) kit BOARD=$$b || exit 1; done
	$(MAKE) package
	$(PYTHON) tools/make_release.py --studio-version $(STUDIO_VERSION) --boards "$(RELEASE_BOARDS)"

clean: tests-clean studio-clean
	rm -rf firmware/build manual/site

tests-clean:
	rm -rf $(BUILD_DIR)

firmware-clean:
	rm -rf $(FW_BUILD)

studio-clean:
	rm -rf $(STUDIO_BUILD) $(STUDIO_WIN_BUILD) apps/studio-jf/installer/Output
