# Re-release files (`Views/ErPalLut`, `Views/Fallback/NNNN`, script tokens)

The modern re-release ships the original data in a new tree (see the Data Files
section of `CLAUDE.md`) plus a few files of its own. All integers are
little-endian.

## `Views/ErPalLut` (E2 only)

Replaces the camera palettes that E2 kept in `FILES/ECSTATIC`. Reader:
`file_read_remaster_palette()` in `src/file.c`. Original loader:
`sub_1401189A0` in `Ecstatica II.exe`.

```
char[8] magic "ErPalLut"
u32     version          # 1
u32     count            # palettes (570 in the demo)
u32     palette_size     # 0x300
u32     lut_size         # 0x40000, decompressed
u32     entries_offset   # 0x30
u32     lut_base         # zstd LUT frames are relative to this
u32     views_offset
u32     num_views        # 1063
u32     intro_offset
u32     num_intro        # 9

entry[count]             # 0x308 bytes each, at entries_offset
    u8[768] palette      # 0..63 RGB, same bytes as the original
    u32     lut_offset   # relative to lut_base
    u32     lut_size     # compressed
u32 views[num_views]     # palette << 16 | camera
u32 intro[num_intro]     # palette << 16 | camera, the original VIEWS/NNNN.PA2 set
```

The `views` map matches the original `palette_offset` table, and every palette
is byte-identical to the original. The 24 `palette_control` bytes that came
before each original palette are dropped; the port never reads them.

The LUTs (one per palette, zstd) are a colour lookup for the re-release's own
renderer. The port does not read them.

## `Views/Fallback/NNNN` (`ErBgView`)

A wider re-render of each camera, used by the widescreen mode. Reader:
`file_read_wide_view()` in `src/file.c`; composited by `widen_background()` in
`src/topo.c`.

```
char[8] magic "ErBgView"
u32     version          # 1
u32     header_size      # 0x48
u16     width            # 640 (a few are narrower: 600, 427)
u16     height           # 320
u16     flags            # 526 (0x20E), or 782 (0x30E) with 24-bit depth
u16     ?                # 3
u32     image_size       # width * height, decompressed
u32     depth_size       # width * height * 2, or * 3
u32     camera
u32     sizes[8]         # compressed sizes of up to 4 (image, depth) zstd pairs
```

Only the first (image, depth) pair is the camera's view; the port ignores the
other pairs.

- **Image**: 8-bit indices in the camera's palette.
- **Depth**: byte planes, all low bytes first. With two planes the value is in
  the same units as the original `mask_map` heightmap (fit ≈ 1.0 × mask) and
  0xFFFF is sky. With three planes it is a signed 24-bit value and -1 is sky.
- **Geometry**: the same camera at exactly 2/3 scale, centred. The 320 rows
  cover the original 480; 640 columns cover 960 original columns, a 2:1 field.
  So a 16:9 screen (854×480) shows the original 640 columns in the middle and
  107 columns either side from this image, scaled back up by 3/2.

## Script tokens

The re-release recompiled a few scripts with two new tokens, appended after the
original token set (names from its token table):

| Token | Name | Behaviour |
|------:|---|---|
| 125 | `PlaySceneAfterVoice` | Like `PlayScene`, but waits until the calling scene's voice clip has finished |
| 126 | `SkipKeyPressed` | One-shot latch of the skip input; replaces `AnyKeyPressed` in intro-skip checks |

The port runs them as `PlayScene` and `AnyKeyPressed` (`CT_PLAY_SCENE_AFTER_VOICE`
and `CT_SKIP_KEY_PRESSED` in `src/types.h`).

## `Code/camera-patches.json`

Per-view settings for the re-release's software-rendered backgrounds:
`position`, `rotation`, `zoom`, `fade`, `min_z`, `fov_clamp`, `cull_trees`,
`white_plants` and `review`. The port does not apply them. Their camera changes
are large (zoom 790 → 685 on E1 view 3, positions moved by hundreds of units),
so using them for actors would misplace them on the original pre-rendered art.

## The Software background (how the re-release draws the world)

`background_type` in `settings.json` is 0 = Software, 1 = Classic HIGH,
2 = Classic LOW. Software is a real 3D render at any aspect and resolution
(`background_pixel_budget`), rebuilt from data the original games already
carry. What is established so far, from `Ecstatica II.exe`:

- **Blocks.** `MapAccelEnsure` (inside `sub_1401189A0`) voxelises the map:
  each cell becomes 8 triangular columns of 256 height levels, each level
  holding the element that fills it (`height2 + 1` .. `def_height`).
  `block_config` maps to the triangles through an 8-bit mask table at
  `0x140339040`: 1 = all 8, 2-5 = 4 (diagonal halves), 16 + quarter mask = 2
  per quarter. Material 8 is skipped.
- **Roofs.** Map elements with a code in their extra word (version >= 10)
  run that code. Those codes are one-line `DrawRoof a b c d e` scripts in
  the original databases (E1 22, E2 9; names such as `shop_roof`,
  `tavern_roof`). Token 21 collects the five values as roof parameters;
  `sub_14013A1E0` builds a thatched slope from them as rows of jittered
  ellipsoids: `a` is a direction in 45 degree steps, `b`/`c` offsets from
  the element, `d` the number of rows, `e` the width (x 64 units).
- **Props.** `DrawScene "name"` (token 20; E1 65, E2 577) names scenes whose
  objects stand in the world.
- **E2 faces.** The seven words per E2 element are texture slots into the
  database's texture names (`FLOOR`, `WALLS`, `CARPET`, `WOOD1`-`3`,
  `BRICK1`, `FLAG1`-`6`, `STONE1`...), whose images are `Code/Textures`.
- **Surfaces.** `Graphics/bumpmap*.raw` (128x128) texture the materials;
  `antialia.dat` and the `ErPalLut` LUTs blend in palette space.

- **Materials (E2).** A per-material footstep table names them: 0 FOOT,
  1 WADE2 (water), 2 MUD2, 3 STNSTEP1 (stone), 4 ECHO02A, 5 GRASS1,
  6 LEAVES1, 7 THUDSM1, 8 MUD1, 9 LEAVES2, 10 SOIL2, 11 ECHO01A, 12 KICK1,
  13 THUD.
- **Camera patches** go into a 48-byte-per-camera table (position,
  rotation, zoom, flags) at `qword_1404F81B0 + 0x6088B8`.

Code map of the Software background (all in `Ecstatica II.exe`):

| Function | Role |
|---|---|
| `sub_14013ED20` | Background frame builder; schedules tasks over a background arena |
| `sub_1401189A0` | `MapAccelEnsure`: map voxelisation, roof entries |
| `sub_14013A1E0` | Thatch roof from DrawRoof parameters |
| `sub_14013CBE0` | Adds one generated ellipsoid (used by every generator) |
| `sub_14013A9A0` | Adds a generated patch/strip |
| `sub_140146430`, `sub_14014B330`, `sub_14014FFE0`, `sub_140151030`, `sub_14013E080` | Generators calling the two above: the per-material scenery (grass, plants, trees, still to be identified) |

`sub_140146430` draws one element face. It picks the E2 texture slot from
the face normal: 0 = -Z side, 1 = +Z side, 2 = -X side, 3 = +X side,
4 = top, 5 = bottom, 6 = diagonal face. A slot >= 0 is a texture (records
of 0x4018 bytes, a 128x128 image and header). Slot -1 falls to a switch on
material (E2's executable):

| Materials | Path |
|---|---|
| 0-3, 9, 10, 12, 14, 15, 18, 20, 22, 23 | Plain surface |
| 4, 11, 17, 19, 21, 24-29 | Patterned surface; 24-29 take parameters from a 6-entry table at `0x140339230` |
| 5, 13 | Generator adding ellipsoids (5 is the grass footstep material) |
| 6, 7, 16 | Own paths, not identified yet |
| 8 | Nothing drawn |

After a face, a second switch on material (`jpt_140147FAA`) adds generated
decoration, every random value from the generator's LCG (`state * 0x41C64E6D
+ 0x3039`, kept per task at `+0x5C`):

| Material | Decoration |
|---|---|
| 1, 22 | Own paths, not identified yet |
| 2 | Pieces of size 0x600-0x9FF |
| 9, 10 | Depends on the neighbouring materials (bit set `0x3FAF29F9`) |
| 12 | A radial fan of ellipsoids: random size 0x200-0x5FF and start angle, each next leaf turned 0x5000 and 0x80 smaller, down to 0x88 - a plant |
| 14, 15 | `sub_14014F8D0`, variant 1 / 2 |
| 20 | Pieces of size 0x400-0x5FF |

Grass (materials 5 and 13) is in the first switch. Each top-face triangle is
filled on a 16-step grid in its own coordinates (i + j <= 15), every point
jittered by -16..+15 units in X and Z; an 8x8 ordered-dither table at
`0x1403390C0` thins the inside by screen density, the edges are always kept.
`sub_14014E7A0` gives each point a shade and `sub_14014F070` emits the tuft:
up to 8 ellipsoid blades, 8 x 8 x (80..175) units, each turned 0x5000
(112.5 degrees) from the last and leaning 0x180 (8.4 degrees) further, from
upright to about 59 degrees, thinned by the 8-entry table at `0x140339200`.
Blade type is mostly 0x0A, sometimes 0x02; material 13 sets a second variant
flag at random.

Not yet traced: the sizes and shapes each generator emits in full. E1's
executable has its own tables.
