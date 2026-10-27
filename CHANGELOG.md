# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

## [0.9.7] - 2026-10-27

### Added
- Runs the 2026 re-release of both games (GOG, Steam) and its demos from their folders as installed: point the game at the folder that holds `Code/`. Sound effects, speech and MIDI music come from its `Audio/` folder, E2 palettes from `Views/ErPalLut`, and the two new script tokens it uses work. `make e1-neo` / `make e2-neo` run the full release in place, `make e1-neo-demo` / `make e2-neo-demo` the demos.
- Wide screens with the re-release data. The original 4:3 view stays in the middle, and the re-release's wider renders of each camera fill the sides, depth included, so actors still go behind the scenery there. Settings → Enhanced → Aspect Ratio picks 4:3, 16:10, 16:9 (the default), 21:9 or 32:9 on desktop. The demos reach 16:9 and the Vita stays at 16:9, which fits its screen. Set it in `ecstatica.cfg` with `aspect_ratio = 21:9`, or for one run with `ECSTATICA_WIDESCREEN` (or `WIDE=` in make).
- 3D Map (OpenGL): the world's block prisms instead of one flat square per cell, coloured from the pre-rendered art; roofs from the original `DrawRoof` scripts; E2's face textures; grass, bushes and trees grown like the re-release's; and, with the E1 re-release, its sky and dry-stone walls. `ECSTATICA_MAP3D=1` turns the view on without the menu.

### Changed
- 3D Map (OpenGL): plants are cheaper to draw, and grow on a background thread when the hero crosses a cell. Camera 87 of the E1 demo goes from 8 to 60 fps.

### Fixed
- Loading an actor file no longer cuts parts out of actors already loaded. With the re-release, which loads one file per actor, Joe's horse in E2's opening scene lost most of its parts and rode off without Ecstatica.
- E2 textures keep their pixels. They were skipped on load, so textured faces had no image.

## [0.9.6] - 2026-10-13

### Added
- Image browser for the game's raw art: `--images`, or `make e2-images` / `make e1-images`. It lists VIEWS, HIRES, GRAPHICS, LOWGRAPH and the title art in the data folder, plus the other half of an E1 install (the DOS bundle or its `W/` folder). Each background can be shown as the picture or as its depth plane, and the `.PA2`/`.PAL` camera palettes are shown as colour swatches. Images open in the palette the game uses for them. `P` switches to the image's own palette, `PALLETTE.RAW`, any E2 camera palette, or grey. In the viewer, `V` now cycles models, scenes and images.
- Web build: both games run in the browser. The page imports your game folder or a zip of it into the browser's private storage once, and saves stay there. Keyboard, mouse, gamepad and an on-screen pad for touch screens. `make web`.
- Android build: one APK for both games (Android 8.0 and later, arm64 and x86_64). The launcher imports a game folder or zip, and the game has an on-screen pad, gamepad, keyboard and mouse support. `make android`.
- iOS and iPadOS build: one app for both games (iOS 15 and later). Import a game folder from the Files app, or copy it into the app's folder there. On-screen pad, game controllers and hardware keyboards. `make ios` makes an unsigned IPA for sideloading.
- Music on web, Android and iOS through a General MIDI soundfont (.sf2) you supply. Without one, sound effects and speech still play.
- Releases include `ecstatica-web-*.zip`, `ecstatica-android-*.apk` and `ecstatica-ios-*.ipa`.

### Changed
- PSP: both games start at 320x200, a quarter of the pixels to render on its single core. Settings → Enhanced → Graphics still switches to 640x480.
- E1: Y / Triangle picks up and drops with the right hand, as A / Cross does with the left. PSP, Vita and the Pocket had the right hand only on Select + R.
- E2: Settings → Enhanced → Graphics switches between Original (320x200) and Enhanced (640x480), as in E1, and so do G and R3. Original uses the original game's low-res HUD art. Enhanced works without the `HIRES` backgrounds too, by doubling the 320x200 ones.
- Analogue Pocket: both games start at 320x200. E1 started at 640x480 when its enhanced `W/` set was on the ISO, and E2 always did, stretching its 320x200 backgrounds whenever the ISO had no `HIRES/`. E2 uses the original game's low-res HUD. Its inventory page has no low-res art, so it opens at 640x480 and the game returns to 320x200 when it closes.
- Analogue Pocket: B cycles E1's speed mode (walk, run, sneak) during gameplay, as on PSP and Vita, instead of opening the pause menu. In menus and in E2 it is still back. Start opens the pause menu.
- Only the newest edge pre-release is kept: each push to `main` deletes the edge releases and tags of earlier versions, and a tagged release deletes them all.
- `assets/generate_packaging_art.py` takes platform names, so it can write one platform's art without rewriting the others.

### Fixed
- E1: Enter, or Y / Triangle on a pad, no longer switches the game to 640x480 and leaves it on camera 0. It opened E2's inventory screen, which E1 does not have.
- Analogue Pocket: sound effects no longer stop for good after about 32 have played. The mixer frees a finished voice only when its handle is queried, and the effects path never queried it. Stopping speech or recycling a slot could also cut a note of the music, and stopping all effects stopped the music too.

## [0.9.5] - 2026-10-13

### Fixed
- E2: the sceptre that appears on the chest in the orc's room vanishes when the hero walks up to it and gives its treasure, instead of floating there for good. Scripts that name their own actor or target as `SUB`/`OBJ` now reach them; before, those commands went nowhere.

## [0.9.4] - 2026-10-13

### Fixed
- DOS and Win9x builds compile again. Open Watcom rejected a held-object array initialised from runtime values.

### Removed
- 113 unreachable functions and globals left over from the original editor, debug output, installer and DOS/VGA/DirectX hardware code. `decomp/removed_functions.md` lists each one with its original Watcom name and E1/E2 address, so IDA comparisons can skip them.

## [0.9.3] - 2026-10-13

### Fixed
- E2: the stable horse, the beast on the sewer well, the big door near it and other objects are in the world again. Only the first of the five StartUp scripts ran. Saves made with older builds get them back when loaded.
- E2: loading a game no longer gives the hero a different body. Cutting a part left a stale link to the freed part, so the save wrote another actor's parts as the hero's. Saves made with older builds keep the wrong model; save again to fix them.
- E1/E2: held objects stay in the holder's hands after loading, instead of floating or going back to where they were picked up. Saves are now version 9. Older saves still load and put the hero's held object in the first free hand.
- Confirming Load Game (or any menu item) with Enter no longer opens the inventory as the game resumes.
- Loading a save during a running scene could quit with "Can't remove Scene in active list".

## [0.9.2] - 2026-10-12

### Changed
- Vita VPKs carry the release version in `APP_VER` (v0.9.1 -> 09.01) instead of a fixed 01.00.
- Only tagged releases publish to VitaDB; edge builds stay on the GitHub pre-release.

## [0.9.1] - 2026-10-12

### Added
- `prof.log` reports threaded work, `load_raw` and `prepare_parts` sub-phases, per-opcode script time and Vita file I/O.
- Profiling builds log limb snaps (`[JUMP]`) and changes in what the hero holds (`[HOLD]`).
- Edge builds on `main` are published to VitaDB too, not only tagged releases.

### Changed
- Vita: the frame copy runs below the raster bands, so the bands no longer wait behind it.
- Vita: the frame copy expands each source row once and bursts it into display memory.
- The directory-listing cache holds every asset directory instead of six.
- E2 visibility maps are read with one `fread` instead of a locked `fgetc` per byte.
- The view prefetcher keeps eight views on multi-core machines and reads the nearest first.
- PSP and Vita write the debug log from a background thread and no longer echo it to stderr.

### Fixed
- Vita: background threads no longer inherit the game thread's core-0 pin.
- Crash on exit when the view prefetcher was still reading as newlib freed the heap.
- E1: the lady-at-the-table scene replayed on every visit with the 320x200 data (scene matched by name now).
- `quit()` logs its reason, so handheld logs show why a session ended.
- Crash on exit when the audio mixer was still reading freed sample buffers.
- E2 sessions quitting with "Event heap overflow": a full event pool now evicts old actions, and evicted assets can load again.

## [0.9.0] - 2026-10-11

### Changed
- The frame wait sleeps until the next game tick instead of spinning, so
  a fast frame no longer holds a core at 100%. Desktop CPU use at the
  title sequence drops from about 100% to 38%.
- Vita: the palette expand and scale to 960x544 runs on cores 1 and 2
  while the game thread, pinned to core 0, starts the next frame.
- PSP: the GPU copies the frame into video memory and draws it while the
  CPU goes on with the next frame. Frames are triple-buffered and only
  shown once fully drawn.
- The backgrounds of the cameras around the hero are read from disk on a
  background thread, so a camera cut no longer waits on the memory card.
- Characters are drawn in horizontal screen bands, one per core, with the
  same pixels as before. PSP, DOS and Pocket have one core and draw as
  before.
- `platform.h` has an optional thread and semaphore API. DOS and Pocket
  have no threads and do the work inline.

## [0.8.4] - 2026-10-02

### Fixed
- Compiler warnings on the Linux, PSP, Vita and Pocket builds.

## [0.8.3] - 2026-10-02

### Added
- Tagged releases publish the Vita VPKs to VitaDB.

## [0.8.2] - 2026-10-01

### Added
- Circle cycles E1's speed mode during play on PSP and Vita.

### Changed
- Packaging art uses the finished header, cover and logo art, and the
  Vita and PSP wide art no longer crops the logo.

## [0.8.1] - 2026-09-30

### Fixed
- Vita VPKs failed to install on real hardware (error 0x8010113D) because the LiveArea art was truecolor PNG instead of indexed.

## [0.8.0] - 2026-09-30

### Added
- Packaging art for Vita, PSP, Analogue Pocket and Steam Deck, generated
  from two in-engine screenshots by `assets/generate_packaging_art.py`. The
  PSP EBOOT now has an icon, and `steamdeck/` holds library art for adding
  the games as a non-Steam shortcut.
- Speed Mode row in Settings -> Enhanced, so E1's sneak/walk/run is
  reachable on PSP and handheld Vita, which have no L3 click.
- CI publishes a rolling `vX.Y.Z-edge` pre-release on every push to `main`.

### Changed
- Comments trimmed across `src/`: restated code and change-history notes
  removed; original addresses and non-obvious reasons kept.

### Fixed
- Material walls (bushes and other scenery) did not block movement because
  `init_material_flags` was never called. The player could reach camera-0
  ground and be dropped into the dragon scene. Affected E1 and E2.
- E1 altar scene did not fire: actor-presence checks now match the original.
- E1 NPCs now finish turning toward the hero and use their hand actions.
- E1 hero speech was muted in scenes by a port-only gender filter.
- Assets loaded earlier in a session could fail to load again after a saved
  game was loaded.
- Saves now store the hero's gender (save version 6). Older E1 saves
  recover it from the hero.
- Subtitles stay inside the visible area under the crop scale mode on Vita
  and PSP.
- Analogue Pocket: Select + shoulder now gives LT/RT, so E1's hand
  pick-up/drop and E2's magic modifier are reachable.

## [0.7.1] - 2026-09-29

### Fixed
- `tools/pointcloud.c` failing to build under Open Watcom's `-za99` on the DOS
  and Win9x targets (non-constant aggregate initializers, missing float math
  prototypes).

## [0.7.0] - 2026-09-29

### Added
- PlayStation Vita platform (`vita/`), one VPK per game with its own LiveArea
  bubble.
- PSP frame-phase profiler (`make PROFILE=1`), later moved into shared
  `prof.c` so the Vita build gets it too.
- User-selectable scale mode (pillarbox/crop/stretch) on PSP and Vita.
- Compile-time struct layout checks against the original binary (`layout.h`),
  enforced on every 32-bit target.
- ANCHOR_PART, LOOSEN_JOINT and UNLOOSEN_JOINT keyframe events ported.
- CI builds and releases the PSP, Vita, and Analogue Pocket packages.

### Changed
- `part_s`/`actor_s` and other struct fields renamed from binary evidence;
  format docs updated to match.
- 32 KB stdio read buffer on every game file opened for reading; first-fit
  position remembered across the event, key, part and point pools (E2 intro,
  worst frame 1054 ms -> 109 ms).
- DOS E1 with a hi-res `W/` folder now boots in enhanced graphics
  automatically, matching E2 and Win95 E1.

### Fixed
- Requester drawing (bevel/title offsets), a requester hang from an unported
  `init_gadgets`, camera reset on `make_thing`, and several dead stubs
  removed.
- E2 NPCs aiming attacks by a body part's facing instead of their own, plus
  related line-of-sight and wander-probe bugs.
- PSP: a packed-struct misaligned access crashing key/event list traversal on
  real hardware, seam lines in the sliced framebuffer blit, and the stick
  read as fully back when held fully forward (also fixed on openfpga and
  Win9x).
- E1's `W/` hi-res folder failing to pair when the data root is an absolute
  path.
- Linux crash on a build with GL support compiled out.

## [0.6.1] - 2026-09-03

### Fixed
- DOS and Win9x build breakage left by the OpenGL work: missing
  `render.obj` in the DOS makefile, `libgl1-mesa-dev` missing from CI, and
  GL headers pulled into software-only Linux/Windows builds.

## [0.6.0] - 2026-09-03

### Added
- PlayStation Portable port (`psp/`, sceGu/sceAudio/sceCtrl).
- Optional OpenGL renderer, selectable from the menu.

### Fixed
- DOS/Win9x hi-res display-mode detection: falls back to VGA/mode 13h when
  the card has no VESA 2.0 linear framebuffer, instead of refusing to start.
- DOS stack overflow from a 48 KB on-stack shade-map read buffer.
- DOS keyboard make/break handling rewritten around the ISR's latch to stop
  dropped keypresses.
- Windows/Win9x: Alt+Enter fullscreen toggle, letterboxed present and mouse
  mapping for windows that aren't 4:3.

## [0.5.0] - 2026-08-29

### Added
- DOS platform backend and Open Watcom `wmake` build.
- Win9x target with DOS-style audio and vsync.
- CI releases DOS and Win9x builds; release workflow runnable manually.
- Enhanced menu: port-only options in their own main-menu entry.
- Data folder searched outside the working directory; clear stderr error and exit 2 if not found.

### Changed
- Platform audio API takes integer samples; float removed from the mix path.
- Pre-clipped blitters, hoisted per-pixel reloads, planes sized to the video mode.
- Original 320x200 subtitles keep their native layout, anchor, and hold.
- ESC opens the menu during the intro; saving refused while it runs.

### Fixed
- DOS keyboard handling.
- DOS clock driven from a timer interrupt, not by polling the PIT.
- SB16 driven with its own commands; voice-table race closed.
- DOS audio optimised away; silent `wmake` syntax error.
- Samples over 65535 bytes restarted mid-playback.
- `signed char` under Watcom; DOS audio interrupt reworked.
- Three heap allocations handed out without being zeroed.

## [0.4.0] - 2026-08-27

### Added
- Analogue Pocket / MiSTer port targeting openfpgaOS via the openfpgaSDK (`pocket/`).
- Model and animation viewer (`--viewer`) and scripted-scene browser (`--scenes`).
- This changelog.

### Fixed
- Edge rasterizer bleeding on polygon boundaries.
- Hand swapping corrupting script state during object interaction.
- Object grabbing accuracy and held-object placement.
- Steam Deck / Steam Machine gamepad handling and DOS-version speech playback.
- Subtitle rendering and enhanced-mode visuals.

## [0.3.1] - 2026-08-17

### Fixed
- E1 combat: damage model, close-range attacks, and removal of E2-only code paths.
- E1 walk animations restarting mid-cycle; restored `next_move` re-decision cooldown.
- Off-grid player positions wrongly triggering the dragon death scene.
- Subtitle cleanup dropping composited actors; full refresh on graphics toggle.
- Zero-volume and malformed cuboid parts painting solid quads.
- Linux gamepad buttons now resolved semantically instead of by raw index.
- Objects overflowing outside the camera view.

## [0.3.0] - 2026-08-15

### Added
- Enhanced-mode toggle to switch between DOS and Win95 presentation.

### Fixed
- DOS bundle version detection.

## [0.2.1] - 2026-08-15

### Added
- Save game persistence.

### Fixed
- Scene 146 (lady at table) repeating.

## [0.2.0] - 2026-08-15

### Fixed
- E1 prop collision: `file_read_map_area` read big-endian from a little-endian stream, byte-swapping every map area name and element index.
- Direction conventions: `find_direction_and_distance` was mirrored against the original binary, breaking `blocked_dirs` and `move_angle`.

## [0.1.3] - 2026-08-13

### Added
- Gamepad support with improved bindings.
- MIDI music on Linux via runtime-loaded FluidSynth.
- Initial save/load groundwork.
- Intro skipping.

### Fixed
- Player hand interaction triggers, controls, and camera/map positioning.
- E1 terrain collisions and E2 idle animation.
- Rasterizer output.
- Music volume on Linux.

## [0.1.2] - 2026-07-21

### Fixed
- Windows build.

## [0.1.1] - 2026-07-21

### Fixed
- Cross-platform build compatibility for Linux and Windows.

## [0.1.0] - 2026-07-21

### Added
- Initial public release: C99 reimplementation of Ecstatica 1 & 2 with macOS, Linux, and Windows platform layers.

[Unreleased]: https://github.com/ecstatica-game/ecstatica/compare/v0.9.6...HEAD
[0.9.6]: https://github.com/ecstatica-game/ecstatica/compare/v0.9.5...v0.9.6
[0.9.5]: https://github.com/ecstatica-game/ecstatica/compare/v0.9.4...v0.9.5
[0.9.4]: https://github.com/ecstatica-game/ecstatica/compare/v0.9.3...v0.9.4
[0.9.3]: https://github.com/ecstatica-game/ecstatica/compare/v0.9.2...v0.9.3
[0.9.2]: https://github.com/ecstatica-game/ecstatica/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/ecstatica-game/ecstatica/compare/v0.9.0...v0.9.1
[0.9.0]: https://github.com/ecstatica-game/ecstatica/compare/v0.8.4...v0.9.0
[0.8.4]: https://github.com/ecstatica-game/ecstatica/compare/v0.8.3...v0.8.4
[0.8.3]: https://github.com/ecstatica-game/ecstatica/compare/v0.8.2...v0.8.3
[0.8.2]: https://github.com/ecstatica-game/ecstatica/compare/v0.8.1...v0.8.2
[0.8.1]: https://github.com/ecstatica-game/ecstatica/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/ecstatica-game/ecstatica/compare/v0.7.1...v0.8.0
[0.7.1]: https://github.com/ecstatica-game/ecstatica/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/ecstatica-game/ecstatica/compare/v0.6.1...v0.7.0
[0.6.1]: https://github.com/ecstatica-game/ecstatica/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/ecstatica-game/ecstatica/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/ecstatica-game/ecstatica/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/ecstatica-game/ecstatica/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/ecstatica-game/ecstatica/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/ecstatica-game/ecstatica/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/ecstatica-game/ecstatica/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/ecstatica-game/ecstatica/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/ecstatica-game/ecstatica/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/ecstatica-game/ecstatica/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/ecstatica-game/ecstatica/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/ecstatica-game/ecstatica/releases/tag/v0.1.0
