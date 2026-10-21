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

Not yet traced: grass, plants and trees (`GRASS1`, `cull_trees`,
`white_plants`) and the per-material surface rules.
