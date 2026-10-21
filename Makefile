# Ecstatica — root Makefile
#
# Builds the C99 port using CMake, then stages the binary into
# data/e1 or data/e2 for a native run.
#
# Usage:
#   make            — build ecstatica binary
#   make run        — alias for `make e2`
#   make e2         — build + copy to data/e2 + run
#   make e1         — build + copy to data/e1/W + run
#   make e2-viewer  — same as e2, but opens the model/animation viewer
#   make e1-viewer  — same as e1, but opens the model/animation viewer
#   make e2-scenes  — same, opening on the scripted-scene browser
#   make e1-scenes  — same, opening on the scripted-scene browser
#   make e2-images  — same, opening on the raw image browser
#   make e1-images  — same, opening on the raw image browser
#   make e2-pointcloud / e1-pointcloud — write pointcloud.ply from every view
#   make e1-neo-demo / e2-neo-demo — build + run the re-release demo in place
#                    (WIDE=1 for the 16:9 mode, WIDE=0 for 4:3)
#   make clean      — remove build artifacts
#   make dump       — regenerate wdump text for E2WIN95P.EXE
#
# DOS target (Open Watcom + DOS/4GW, run under DOSBox-X):
#   make dos        — build platforms/dos/ecstatic.exe
#   make dos-e1     — build + run the E1 DOS data in DOSBox-X
#   make dos-e2     — build + run the E2 data in DOSBox-X
#   make dos-clean  — remove DOS build artifacts
#
# Win9x target (Open Watcom, 32-bit PE, run under Wine):
#   make win9x      — build platforms/win9x/ecstatica.exe
#   make win9x-e1   — build + run the E1 Win95 data under Wine
#   make win9x-e2   — build + run the E2 data under Wine
#   make win9x-clean
#
# PSP target (pspdev toolchain, runs on hardware or PPSSPP):
#   make psp          — build platforms/psp/EBOOT.PBP
#   make psp-release  — stage platforms/psp/build/PSP/GAME/ECSTATICA with the game data
#   make psp-install  — copy that onto a Memory Stick
#   make psp-clean
#
# PS Vita target (VitaSDK, runs on hardware or Vita3K):
#   make vita         — build platforms/vita/build/ecstatica-e1.vpk and ecstatica-e2.vpk
#   make vita-clean
#
# Web target (Emscripten, runs in any current browser):
#   make web          — build platforms/web/build/dist/
#   make web-serve    — serve it on http://localhost:8080 with COOP/COEP headers
#   make web-clean
#
# Android target (SDK + NDK command-line tools, no Gradle):
#   make android         — build platforms/android/build/ecstatica.apk
#   make android-install — install it with adb
#   make android-clean
#
# iOS / iPadOS target (Xcode, through CMake's Xcode generator):
#   make ios-sim      — build for the iOS simulator
#   make ios          — build an unsigned platforms/ios/build/ecstatica.ipa for sideloading
#   make ios-clean
#
# Set WATCOM if Open Watcom is not in ~/watcom. Both cross builds live in their
# own directories and are driven by Watcom's wmake, not by this file. The PSP
# build lives in platforms/psp/ and is GNU make, driven by the PSP SDK's build.mak.

TARGET   = ecstatica
SRCDIR   = src
BUILDDIR = build
BUILT    = $(BUILDDIR)/bin/$(TARGET)

E2_DIR   		= data/e2
E1_DIR   		= data/e1/W
E1_DIR_DOS   	= data/e1
E1_DIR_ORIG		= data/e1-dos
# The re-release ("Neo") demos, run in place: the binary is not copied in.
E1_NEO_DEMO_DIR	= data/Ecstatica Demo
E2_NEO_DEMO_DIR	= data/Ecstatica II Demo

# ── DOS ───────────────────────────────────────────────────────
WATCOM     ?= $(HOME)/watcom
# Open Watcom ships a host binary directory per platform: bino64 on macOS,
# binl64 on Linux. CI builds these targets on Linux runners.
ifeq ($(shell uname -s),Darwin)
WATCOM_BIN  = $(WATCOM)/bino64
else
WATCOM_BIN  = $(WATCOM)/binl64
endif
DOS_EXE     = platforms/dos/ecstatic.exe
DOSBOX     ?= dosbox-x
# DOS/4GW has to sit next to the executable for the stub to find it.
DOS4GW      = $(WATCOM)/binw/dos4gw.exe

WIN9X_EXE   = platforms/win9x/ecstatica.exe
WINE       ?= wine

.PHONY: ios-configure
.PHONY: all run e1 e2 e1-neo-demo e2-neo-demo e1-viewer e2-viewer e1-scenes e2-scenes e1-images e2-images e1-pointcloud e2-pointcloud build clean dump \
        dos dos-e1 dos-e2 dos-clean win9x win9x-e1 win9x-e2 win9x-clean \
        psp psp-release psp-install psp-clean vita vita-clean \
        web web-serve web-clean android android-install android-clean \
        ios ios-sim ios-clean

all: build

build:
	cmake -S $(SRCDIR) -B $(BUILDDIR) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILDDIR)

$(BUILT): build

run: e2

e2: $(BUILT)
	cp $(BUILT) $(E2_DIR)/
	chmod +x $(E2_DIR)/$(TARGET)
	cd $(E2_DIR) && ./$(TARGET)

e1: $(BUILT)
	mkdir -p $(E1_DIR)
	cp $(BUILT) $(E1_DIR)/
	chmod +x $(E1_DIR)/$(TARGET)
	cd $(E1_DIR) && ./$(TARGET)

# WIDE overrides the saved Aspect Ratio setting for this run.
NEO_ENV = $(if $(WIDE),ECSTATICA_WIDESCREEN=$(WIDE))

e1-neo-demo: $(BUILT)
	cd "$(E1_NEO_DEMO_DIR)" && $(NEO_ENV) "$(CURDIR)/$(BUILT)"

e2-neo-demo: $(BUILT)
	cd "$(E2_NEO_DEMO_DIR)" && $(NEO_ENV) "$(CURDIR)/$(BUILT)"

e2-viewer: $(BUILT)
	cp $(BUILT) $(E2_DIR)/
	chmod +x $(E2_DIR)/$(TARGET)
	cd $(E2_DIR) && ./$(TARGET) --viewer

e1-viewer: $(BUILT)
	mkdir -p $(E1_DIR)
	cp $(BUILT) $(E1_DIR)/
	chmod +x $(E1_DIR)/$(TARGET)
	cd $(E1_DIR) && ./$(TARGET) --viewer

e2-pointcloud: $(BUILT)
	cp $(BUILT) $(E2_DIR)/
	chmod +x $(E2_DIR)/$(TARGET)
	cd $(E2_DIR) && ./$(TARGET) --pointcloud

e1-pointcloud: $(BUILT)
	mkdir -p $(E1_DIR)
	cp $(BUILT) $(E1_DIR)/
	chmod +x $(E1_DIR)/$(TARGET)
	cd $(E1_DIR) && ./$(TARGET) --pointcloud

e2-scenes: $(BUILT)
	cp $(BUILT) $(E2_DIR)/
	chmod +x $(E2_DIR)/$(TARGET)
	cd $(E2_DIR) && ./$(TARGET) --scenes

e1-scenes: $(BUILT)
	mkdir -p $(E1_DIR)
	cp $(BUILT) $(E1_DIR)/
	chmod +x $(E1_DIR)/$(TARGET)
	cd $(E1_DIR) && ./$(TARGET) --scenes

e2-images: $(BUILT)
	cp $(BUILT) $(E2_DIR)/
	chmod +x $(E2_DIR)/$(TARGET)
	cd $(E2_DIR) && ./$(TARGET) --images

e1-images: $(BUILT)
	mkdir -p $(E1_DIR)
	cp $(BUILT) $(E1_DIR)/
	chmod +x $(E1_DIR)/$(TARGET)
	cd $(E1_DIR) && ./$(TARGET) --images

e1-dos-bundle: $(BUILT)
	mkdir -p $(E1_DIR_DOS)
	cp $(BUILT) $(E1_DIR_DOS)/
	chmod +x $(E1_DIR_DOS)/$(TARGET)
	cd $(E1_DIR_DOS) && ./$(TARGET)

e1-dos: $(BUILT)
	mkdir -p $(E1_DIR_ORIG)
	cp $(BUILT) $(E1_DIR_ORIG)/
	chmod +x $(E1_DIR_ORIG)/$(TARGET)
	cd $(E1_DIR_ORIG) && ./$(TARGET)

clean:
	rm -rf $(BUILDDIR)

dump:
	wdump -Dx $(E2_DIR)/E2WIN95P.EXE > decomp/E2WIN95P.EXE.orig.txt

# ── DOS build and run ─────────────────────────────────────────
#
# platforms/dos/Makefile is a wmake makefile, so it is invoked through
# Watcom's wmake with WATCOM/PATH/INCLUDE set the way the toolchain expects.

dos:
	@test -x $(WATCOM_BIN)/wmake || { \
	  echo "Open Watcom not found at $(WATCOM)."; \
	  echo "Install it or pass WATCOM=/path/to/watcom."; exit 1; }
	cd platforms/dos && WATCOM=$(WATCOM) PATH=$(WATCOM_BIN):$$PATH INCLUDE=$(WATCOM)/h \
	  $(WATCOM_BIN)/wmake

$(DOS_EXE): dos

# Stage the executable and DOS/4GW into a run directory, mount the game data as
# C: and that directory as D:, and start the game. The data directory is left
# untouched — nothing is copied into it.
define dos_run
	@test -d $(1) || { echo "No game data at $(1)."; exit 1; }
	@test -f $(DOS4GW) || { echo "DOS/4GW not found at $(DOS4GW)."; exit 1; }
	@command -v $(DOSBOX) >/dev/null || { \
	  echo "$(DOSBOX) not found. Install DOSBox-X or pass DOSBOX=<path>."; exit 1; }
	@mkdir -p $(BUILDDIR)/dosrun
	@cp $(DOS_EXE) $(DOS4GW) $(BUILDDIR)/dosrun/
	@printf '[dosbox]\nmemsize=32\n[autoexec]\nmount c %s\nmount d %s\nset PATH=D:\nc:\nd:ecstatic.exe\n' \
	  "$(abspath $(1))" "$(abspath $(BUILDDIR)/dosrun)" > $(BUILDDIR)/dosrun/dosbox.conf
	$(DOSBOX) -conf $(BUILDDIR)/dosrun/dosbox.conf -nomenu
endef

dos-e1: dos
	$(call dos_run,$(E1_DIR_DOS))

dos-e1-original: dos
	$(call dos_run,$(E1_DIR_ORIG))

dos-e2: dos
	$(call dos_run,$(E2_DIR))

dos-clean:
	cd platforms/dos && rm -f *.obj *.exe *.map link.lnk
	rm -rf $(BUILDDIR)/dosrun

# ── Win9x build and run ───────────────────────────────────────
#
# The same Watcom toolchain targeting Win32. Wine runs the result for testing;
# on a real Win9x machine the executable is simply copied next to the data.

win9x:
	@test -x $(WATCOM_BIN)/wmake || { \
	  echo "Open Watcom not found at $(WATCOM)."; \
	  echo "Install it or pass WATCOM=/path/to/watcom."; exit 1; }
	cd platforms/win9x && WATCOM=$(WATCOM) PATH=$(WATCOM_BIN):$$PATH \
	  $(WATCOM_BIN)/wmake

# Wine runs with the data directory as the working directory, matching how the
# game finds its archives everywhere else.
define win9x_run
	@test -d $(1) || { echo "No game data at $(1)."; exit 1; }
	@command -v $(WINE) >/dev/null || { \
	  echo "$(WINE) not found. Install Wine or pass WINE=<path>."; exit 1; }
	cd $(1) && WINEDEBUG=-all $(WINE) $(abspath $(WIN9X_EXE))
endef

win9x-e1: win9x
	$(call win9x_run,$(E1_DIR))

win9x-e2: win9x
	$(call win9x_run,$(E2_DIR))

win9x-clean:
	cd platforms/win9x && rm -f *.obj *.exe *.map link.lnk

# ── PSP build and packaging ───────────────────────────────────
#
# platforms/psp/Makefile is GNU make on top of the PSP SDK's build.mak, so it
# is invoked directly. GAME_DATA picks which game gets staged onto the Memory
# Stick; the 320x200 DOS data is the one worth shipping, see
# platforms/psp/README.md.

psp:
	@command -v psp-config >/dev/null || { \
	  echo "PSP toolchain not found. Install pspdev and put \$$PSPDEV/bin on PATH."; \
	  exit 1; }
	$(MAKE) -C platforms/psp

psp-release: psp
	$(MAKE) -C platforms/psp release GAME_DATA=$(abspath $(E2_DIR))

psp-install: psp
	$(MAKE) -C platforms/psp install GAME_DATA=$(abspath $(E2_DIR)) FORCE_DATA=$(FORCE_DATA)

psp-clean:
	$(MAKE) -C platforms/psp clean

# platforms/vita/ is a CMake project on VitaSDK's toolchain file; the game
# data is not packed into the VPK but copied to ux0:data/ecstatica, see
# platforms/vita/README.md.
vita:
	@test -n "$$VITASDK" || { \
	  echo "VITASDK not set. Install VitaSDK and export VITASDK."; exit 1; }
	cmake -S platforms/vita -B platforms/vita/build -DCMAKE_BUILD_TYPE=Release \
	  -DGAME_VERSION=$(VERSION)
	cmake --build platforms/vita/build --parallel

vita-clean:
	rm -rf platforms/vita/build

# ── Web build ─────────────────────────────────────────────────
#
# platforms/web/ is a CMake project configured through emcmake. The game
# data is imported in the browser, not packed, see platforms/web/README.md.
# emsdk puts upstream/emscripten on PATH, and it holds a cmake/ directory.
# make runs simple recipes without a shell, and its PATH search stops at that
# directory ("cmake: Permission denied"); the shell's lookup skips it.
CMAKE_BIN := $(shell command -v cmake)

web:
	@command -v emcmake >/dev/null || { \
	  echo "Emscripten not found. Install emsdk and source emsdk_env.sh."; exit 1; }
	emcmake $(CMAKE_BIN) -S platforms/web -B platforms/web/build -DCMAKE_BUILD_TYPE=Release
	$(CMAKE_BIN) --build platforms/web/build

web-serve: web
	python3 platforms/web/serve.py platforms/web/build/dist 8080

web-clean:
	rm -rf platforms/web/build

# ── Android build ─────────────────────────────────────────────
#
# platforms/android/build.sh drives the NDK, javac/d8, aapt2 and apksigner
# directly; see platforms/android/README.md.
android:
	platforms/android/build.sh $(VERSION)

android-install: android
	adb install -r platforms/android/build/ecstatica.apk

android-clean:
	rm -rf platforms/android/build

# ── iOS build ─────────────────────────────────────────────────
#
# platforms/ios/ is a CMake project for the Xcode generator. The device build
# is unsigned: sideloading tools (AltStore, Sideloadly) sign it with the
# user's own Apple ID; see platforms/ios/README.md.
IOS_BUILD = platforms/ios/build

ios-configure:
	@command -v xcodebuild >/dev/null || { echo "Xcode not found."; exit 1; }
	cmake -S platforms/ios -B $(IOS_BUILD) -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DGAME_VERSION=$(VERSION)

ios-sim: ios-configure
	cmake --build $(IOS_BUILD) --config Release -- -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO

ios: ios-configure
	cmake --build $(IOS_BUILD) --config Release -- -sdk iphoneos CODE_SIGNING_ALLOWED=NO
	rm -rf $(IOS_BUILD)/Payload $(IOS_BUILD)/ecstatica.ipa
	mkdir -p $(IOS_BUILD)/Payload
	cp -R $(IOS_BUILD)/Release-iphoneos/Ecstatica.app $(IOS_BUILD)/Payload/
	cd $(IOS_BUILD) && zip -qr ecstatica.ipa Payload
	@echo "Built $(IOS_BUILD)/ecstatica.ipa (unsigned)"

ios-clean:
	rm -rf $(IOS_BUILD)
