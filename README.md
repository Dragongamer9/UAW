# uaw-native

A clean-room, native 64-bit reimplementation of the Alamo engine as used by
**Universe at War: Earth Assault** (Petroglyph, 2007). It loads the original
game data from your own install. No Petroglyph assets are included or
redistributed.

## Status

| Stage | Piece | State |
|---|---|---|
| 1 | MEG v1 archives (+ virtual file system, loose-file overrides) | done, verified on retail Config/Maps/Shaders/Cinematics/Retailers.meg (1,668 files, all CRCs match) |
| 1 | XML game data (`Data/XML`), `MegaFiles.xml` load order | next |
| 1 | String tables (`MasterTextFile_*.dat`) | planned |
| 2 | ALO models / ALA animations, DDS textures → viewer | planned |
| 2 | TED maps → terrain renderer | planned |
| 3 | Lua scripting host (AI, missions); scripts ship as precompiled Lua 5.1 bytecode with a custom header (format byte 0x70, 4-byte float numbers) | planned |
| 3 | Deterministic RTS simulation (units, pathing, combat) | planned |
| 4 | UI, audio, LAN multiplayer | planned |

## Build

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## megtool

```sh
megtool list    Config.meg
megtool verify  Config.meg               # checks stored filename CRCs
megtool cat     Config.meg Data/XML/GameObjectFiles.xml
megtool extract Config.meg ./extracted
```

Lookups ignore case and slash direction. Extraction refuses paths that would
escape the output folder.

## Format notes

MEG v1 (shared with Empire at War / Forces of Corruption), little-endian:

```
uint32 numFilenames
uint32 numFiles
numFilenames × { uint16 len; char name[len]; }
numFiles     × { uint32 crc32; uint32 index; uint32 size; uint32 start; uint32 nameIndex; }
file data
```

The file table is sorted by CRC-32 of the uppercase, backslash-separated
name. The engine merges every archive in `Data/MegaFiles.xml`; later archives
win, and loose files on disk override archives.

Format knowledge comes from the Empire at War community (Mike Lankamp's
Petrolution docs, AlamoEngine-Tools/PetroglyphTools, the Blender ALAMO plugin).
