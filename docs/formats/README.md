# Ecstatica File Format Specs

Tech reference for on-disk formats used by Ecstatica 1 (DOS) and Ecstatica 2 (Win95). Derived from the ported reader/writer code in `src/` (`file.c`, `topo.c`, `init.c`, `music.c`, `asm_f.c`) plus hex inspection of shipped data under `data/e1`, `data/e1-dos`, `data/e2`.

Endianness in this codebase is inconsistent — many multi-byte fields are big-endian on disk (unusual for a DOS x86 title) and read via `getw_be` / `getl` (hi-word first). A minority (sound header, save-game timers) are little-endian and read via `getwLoHi` / `getlLoHi`. Each spec below states which applies.

## Index

| Spec | Files | Purpose |
|---|---|---|
| [game-versions.md](game-versions.md) | All | Five game distributions: E1 DOS Standalone (v27), E1 DOS Bundled (v30), E1 Windows (v48), E1 Win DOS-compat (v51), E2 (v55) — format variations, runtime behavior, table sizes |
| [fan-archive.md](fan-archive.md) | `*.FAN`, `CODE/ECSTATIC.FAN`, `things/*.fan` | Tagged-block resource archive (things, actions, scenes, code, sounds, reps, map areas, textures) |
| [fan-thing.md](fan-thing.md) | Thing blocks in `.FAN` | 3D actor model: ellipsoidal parts hierarchy, triangles, attachment points |
| [fan-action.md](fan-action.md) | Action blocks in `.FAN` | Keyframed animation: keys, events, INTERACT sub-types, index remapping |
| [fan-scene.md](fan-scene.md) | Scene blocks in `.FAN` | Scene definitions: action references (max 18), PSEUDO_SCENE events |
| [fan-code.md](fan-code.md) | Code blocks in `.FAN` | Scripting bytecode: token streams with typed references, text lines |
| [fan-sound.md](fan-sound.md) | Sound blocks in `.FAN` | Audio: little-endian headers, 8-bit unsigned PCM, sentinel-delimited |
| [fan-repertoire.md](fan-repertoire.md) | Repertoire blocks in `.FAN` | Action-type-to-action mapping: 208 slots, E1 movement variation expansion |
| [fan-map.md](fan-map.md) | Map data in `.FAN` | 128x128 grid, map elements, cameras, map areas — mixed endianness |
| [fan-texture.md](fan-texture.md) | Texture blocks in `.FAN` | Surface textures: 8-bit indexed pixels, E2 only (v37+) |
| [pak-archive.md](pak-archive.md) | `ARCHIVE/*.PAK` | E1-DOS simple name+size+payload container |
| [raw-image.md](raw-image.md) | `*.RAW` (title / logo / graphics) | 32-byte header + 768-byte VGA palette + 8-bpp indexed pixels. `mhwanh` DPaint signature. |
| [packed-background.md](packed-background.md) | `hires/NNNN.raw` | Camera background: signature branch + packed 8-bpp bitmap + packed 16-bpp heightmap via `unpack_bitmap` / `unpack_mask` |
| [palette-files.md](palette-files.md) | `views/NNNN.pal`, `views/NNNN.pa2` | Per-camera VGA palette + control block |
| [offsets.md](offsets.md) | `OFFSETS`, `OFF2` | Big-endian offset tables into merged `.FAN` blob for lazy resource loading |
| [remaster-files.md](remaster-files.md) | `Views/ErPalLut`, `Views/Fallback/NNNN` | Re-release camera palettes, wide re-rendered views, new script tokens |
| [shademap.md](shademap.md) | `shademap.dat` | 128×128 ellipsoid shade+profile table used by ellipse rasterizer |
| [shadow-tab.md](shadow-tab.md) | `SHADOW.DAT` | 3×16×256 shadow palette lookup, raw dump |
| [sound-block.md](sound-block.md) | Sound blocks inside `.FAN` | 32-byte header + unsigned 8-bit PCM sample |
| [music-bin.md](music-bin.md) | `MUSIC/*.BIN`, `*.SCC/.GUS/.SBL/.AWE/.LAP` | Per-driver custom MIDI-like tune stream (deferred — not yet fully reversed) |
| [save-game.md](save-game.md) | `save%d.dat` | Player+actor state snapshot |
| [config-files.md](config-files.md) | `E_CONFIG`, `D_CONFIG`, `CDPATH`, `*.STE` | Setup / install / demo text/binary configs |

## Conventions

- **be16 / be32**: big-endian 16- / 32-bit integer.
- **le16 / le32**: little-endian.
- **u8**: unsigned byte.
- **VGA palette entry**: 3 × u8 with each channel in 0..63 (DAC range).
- **Fixed-point 16.16**: signed int32, upper 16 bits integer part.

## Global constants (from `game.h`)

| Constant | Value | Notes |
|---|---:|---|
| `THING_TAB_SIZE` | (see game.h) | Actor / thing index space |
| `SCENE_TAB_SIZE` | | Scene index space |
| `ACTION_TAB_SIZE` | | Action index space |
| `SOUND_TAB_SIZE` | | Sound index space |
| `REPERTOIRE_TAB_SIZE` | | Repertoire index space |
| `PALETTES_MAX` | 1200 | Per-camera palette offset entries |
| `PART_TAB_SIZE`, `POINT_TAB_SIZE`, `TRIANGLE_TAB_SIZE`, `CODE_TAB_SIZE`, `MAP_AREA_TAB_SIZE`, `TEXTURE_TAB_SIZE` | | See `game.h` |

Consult `src/game.h` for the current numeric values (they differ between E1 and E2 tunings).
