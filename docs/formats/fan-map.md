# FAN Map Data Type

The map defines a 128x128 navigable grid, a variable-length list of map elements describing terrain properties, a camera table, and optional map areas. This data type uses mixed endianness: the grid and most element fields are little-endian, while the element count and map areas are big-endian.

## Grid

128x128 array of le16 values, each an index into the map element table.

| Offset | Size | Endian | Field | Description |
|--------|------|--------|-------|-------------|
| 0x0000 | 65536 | LE | new_map[128][128] | Cell indices (le16 per cell), row-major order |

Total: 32768 entries, 65536 bytes.

## Element Count

| Offset | Size | Endian | Field | Description |
|--------|------|--------|-------|-------------|
| +0 | 4 | BE | top_of_map_elements | Number of map elements that follow |

### Version 34-39 Padding

When `file_version` is in range [34, 39], an extra 4 bytes are read and discarded (two be16 values) before the element array.

## Map Elements

Each element describes terrain properties for the cells that reference it. Repeated `top_of_map_elements` times.

### E1 Layout (Ecstatica 1)

| Offset | Size | Endian | Field | Description |
|--------|------|--------|-------|-------------|
| +0 | 1 | -- | height | Terrain height |
| +1 | 1 | -- | block_config | Blocking/collision configuration |
| +2 | 1 | -- | camera_index | Camera index (also used as jump_camera_index) |
| +3 | 1 | -- | height2 | Secondary height value |
| +4 | 1 | -- | material | Surface material type |
| +5 | 1 | -- | (unused) | Skipped byte (version >= 21 only) |
| +5/+6 | 2 | LE | code_index | Code trigger index with flags (see below) |
| +7/+8 | 2 | LE | (extra) | Extra field (version >= 10 only) |

E1 element size: 7-10 bytes depending on version. `wanderer_spawn` is implicitly 0.

### E2 Layout (Ecstatica 2)

| Offset | Size | Endian | Field | Description |
|--------|------|--------|-------|-------------|
| +0 | 1 | -- | height | Terrain height |
| +1 | 1 | -- | block_config | Blocking/collision configuration |
| +2 | 2 | LE | camera_index | Camera index |
| +4 | 2 | LE | jump_camera_index | Camera index used during jumps |
| +6 | 4 | LE | (reserved) | Two le16 values, skipped |
| +10 | 1 | -- | height2 | Secondary height value |
| +11 | 1 | -- | material | Surface material type |
| +12 | 1 | -- | (unused) | Skipped byte (version >= 21 only) |
| +12/+13 | 2 | LE | code_index | Code trigger index with flags (see below) |
| +14/+15 | 2 | LE | (extra) | Extra field (version >= 10 only) |
| +16/+17 | 4 | LE | (reserved) | Two le16 values, skipped |
| +20/+21 | 14 | LE | (reserved) | Seven le16 values, skipped |
| +34/+35 | 1 | -- | wanderer_spawn | Wanderer spawn flag |

E2 element size: 21-37 bytes depending on version.

### What the fields hold (E2, version 55)

Measured over the 49,650 elements of the E2 re-release database. The
re-release keeps every field (34-byte records); the port reads only what the
game logic needs. Meanings marked *likely* are not yet confirmed in code.

| Field | Values seen | Meaning |
|---|---|---|
| height / height2 | 0-255 | Top and bottom of the block (*likely*: floor and ceiling of a stacked block) |
| block_config | 20 distinct | Block shape (flat, slopes, roof forms) |
| material | 29 distinct | Surface material |
| camera_index, jump_camera_index, first reserved pair | camera numbers | Four camera indices per element |
| unused byte (v >= 21) | 4, 16, 17, 25-29 | Stored by the re-release (v >= 46), default 4; *likely* a second material class |
| second reserved pair | 0-127 each | The element's own grid column and row |
| seven reserved words | -1, or 0-11, 59 | *Likely* per-face texture slots (the database has 61 texture names) |

The grid is a 128x128 world plan: each cell points at a stack of elements
(the last one has bit 0x8000 set in code_index). Coloured by material and
shaded by height, it shows the world's layout - halls, rooms, walls, towers.
The re-release's Software background renders the world from this data at any
resolution and aspect; see [remaster-files.md](remaster-files.md).

### code_index Field Encoding

The 16-bit `code_index` field packs a code name reference and flags:

| Bits | Mask | Description |
|------|------|-------------|
| 0-13 | 0x3FFF | Code name index; 0x3FFF means no code trigger |
| 14-15 | 0xC000 | Flags (preserved as-is during load) |

When bits 0-13 hold a valid index, the value is remapped through `new_code_name[index] + 1` during load.

## Cameras

Camera definitions follow the map elements.

| Offset | Size | Endian | Field | Description |
|--------|------|--------|-------|-------------|
| +0 | 2 | LE | no_of_cameras | Number of camera entries |

Each camera entry:

| Offset | Size | Endian | Field | Description |
|--------|------|--------|-------|-------------|
| +0 | 2 | LE | view_pos.X | Camera position X |
| +2 | 2 | LE | view_pos.Y | Camera position Y |
| +4 | 2 | LE | view_pos.Z | Camera position Z |
| +6 | 2 | LE | view_rot.X | Camera rotation X |
| +8 | 2 | LE | view_rot.Y | Camera rotation Y |
| +10 | 2 | LE | view_rot.Z | Camera rotation Z |
| +12 | 2 | LE | zoom_factor | Camera zoom |
| +14 | 2 | LE | top_clip | Top clipping plane (E2, version >= 36 only) |

Camera entry size: 14 bytes (E1 / E2 before v36) or 16 bytes (E2 v36+).

When `top_clip` is not present in the file, it defaults to `-127 << height_shift`.

Maximum cameras: 1200.

## Map Areas (version >= 20)

Map areas follow the camera table, delimited by sentinel bytes. The reader calls `fgetc()` in a loop; a non-zero byte signals another area entry, while zero terminates the list.

### Tagged-Block Format (Block Type 0x07)

All fields are big-endian signed 16-bit integers.

| Offset | Size | Endian | Field | Description |
|--------|------|--------|-------|-------------|
| +0 | 2 | BE | name_index | Index into the name table identifying this area |
| +2 | 2 | BE | num_elements | Number of element references (max 10) |
| +4 | 2 * num_elements | BE | element_nums | Array of map element indices |

Total size: 4 + (2 * num_elements) bytes per area.

## Version Requirements

| Feature | Minimum Version |
|---------|----------------|
| Map grid and elements | 9 |
| Extra field in elements | 10 |
| Unused byte in elements | 21 |
| Map areas | 20 |
| Version 34-39 padding | 34 |
| Camera top_clip (E2) | 36 |

## Endianness Summary

| Data | Endianness |
|------|------------|
| Grid cells (new_map) | Little-endian |
| Element count (top_of_map_elements) | Big-endian |
| Element fields (camera, code, extras) | Little-endian |
| Camera fields | Little-endian |
| Map area fields | Big-endian |
