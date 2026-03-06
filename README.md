# AVV-Archive-Toolkit

A C++20 Virtual File System with a custom `.avv` binary archive format, a command-line interface, and a graphical file browser.

## Project Structure

```
AVV-Archive-Toolkit/
├── vfs_core/          Core archiving library (reader, writer, types)
├── vfs_packer/        Command-line pack/unpack/list tool
├── vfs_browser/       Dear ImGui graphical archive browser
├── vfs_tests/         Catch2 unit tests
└── third_party/       SDL2, Dear ImGui, LZ4 (local copies)
```

---

## Quick Start

### 1 — Build

Open `AVV-Archive-Toolkit.slnx` in **Visual Studio 2022** (v145 toolset, C++20). The solution contains three projects:

- **vfs_packer** — the CLI tool (`vfs_cli/main.cpp`)
- **vfs_browser** — the GUI browser (set as Startup Project to launch)
- **vfs_tests** — the unit test runner (`vfs_tests/test_vfs.cpp`)

Dependencies (SDL2, Dear ImGui, LZ4) are included locally in `third_party/` — no package manager required.

### 2 — Pack a directory

```bash
# Single-file AVV2 archive
vfs_cli pack game_data.avv assets/

# Split AVV3 archive (12 GiB chunks by default)
vfs_cli packs game_data assets/
# Produces: game_data_dir.avv, game_data_000.avv, game_data_001.avv, ...
```

Files are compressed with LZ4 where it helps. Already-compressed data (images, audio) is stored raw automatically.

### 3 — Browse the archive

Launch `vfs_browser.exe` and either:
- Drag any `.avv` or `_dir.avv` file onto the window, **or**
- Use **File → Open Archive…** (`Ctrl+O`) to browse for it

You will see a sortable file table with search. Click any row to preview the file contents as text or hex dump.

### 4 — Extract files

**Single file:** Right-click any entry in the browser → **Extract to CWD**.

**All files (GUI):** **File → Extract All…** — runs threaded with a progress bar.

**All files (CLI):**
```bash
vfs_packer unpack game_data.avv extracted/
vfs_packer unpack game_data_dir.avv extracted/   # also works for split archives
```

---

## CLI Reference

```
vfs_packer [-v] [-c <1..12>] pack  <output.avv> <input_dir>
vfs_packer [-v] [-c <1..12>] [-s <GB>] packs <stem> <input_dir>
vfs_packer [-v] unpack <input.avv|_dir.avv> <output_dir>
vfs_packer list <input.avv|_dir.avv>
```

### Commands

| Command | Description |
|---|---|
| `pack` | Pack a directory into a single AVV2 archive |
| `packs` | Pack a directory into a VPK-style split AVV3 archive set |
| `unpack` | Extract all files from an archive (single or split) |
| `list` | Print the central directory without extracting |

### Flags

| Flag | Default | Description |
|---|---|---|
| `-v` | off | Verbose — print each file with size and compression method |
| `-c <level>` | `3` | LZ4 compression level (1–12; higher = slower + smaller) |
| `-s <GB>` | `12` | Maximum chunk size in GiB for split archives (`packs` only) |

### Examples

```bash
# Pack with max LZ4 compression
vfs_cli -c 12 pack game_data.avv assets/

# Split-pack with 4 GiB chunks and level 9 compression
vfs_cli -c 9 -s 4 packs game_data assets/

# List a split archive
vfs_cli list game_data_dir.avv

# Unpack with verbose output and progress bar
vfs_cli -v unpack game_data_dir.avv extracted/
```

All pack and unpack operations display an inline progress bar:
```
  [############--------]  58%  (305/524)  textures/sky_hdr.dds
```

### Drag-and-Drop (Windows Explorer)

You can drag a **folder** or an **`.avv` file** directly onto `vfs_cli.exe`:

| Dropped item | Action |
|---|---|
| A **folder** | Split-packs it (`packs`) into `<foldername>_dir.avv` + chunks |
| An **`.avv` file** | Unpacks it into `<stem>/` in the same directory |

The console stays open after completion so you can read the output.

---

## Virtual File Browser

An ImGui-based GUI for inspecting `.avv` archives (single and split) without unpacking.

**Features:**
- Sortable file table with live search/filter bar (shows chunk index for split archives)
- File details pane with text preview and clipped hex dump viewer (ImGuiListClipper — constant frame time)
- Right-click context menu → `Copy Path` or `Extract to CWD`
- **File → Extract All…** with threaded progress bar modal
- Drag-and-drop `.avv` / `_dir.avv` files onto window to open
- Dockable panel layout with multi-viewport support
- Custom dark theme with rounded styling

**Keyboard shortcuts:**

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open archive dialog |
| `Alt+F4` | Exit |

Launch by setting `vfs_browser` as the Startup Project, or pass an archive on the command line:

```
vfs_browser.exe path/to/archive.avv
vfs_browser.exe path/to/archive_dir.avv
```

---

## `.avv` Archive Formats

### AVV2 — Single-File Archive

| Section | Contents |
|---|---|
| **Header** (16 B) | Magic `AVV2`, version `uint32`, reserved `uint64` |
| **File Data Blocks** | Raw or LZ4-compressed file payloads, appended sequentially |
| **Central Directory** | Array of `CentralDirectoryEntryBase` (28 B) + path strings |
| **Footer** (16 B) | Directory offset `uint64`, magic end `2VVA_EOF` |

### AVV3 — Split (VPK-Style) Archive

Splits data across multiple chunk files while keeping the directory in a separate index:

| File | Contents |
|---|---|
| `<stem>_dir.avv` | AVV3 header + Central Directory (V3 entries, 32 B each) + footer |
| `<stem>_000.avv` | Raw data chunk (file payloads only, no header/footer) |
| `<stem>_001.avv` | Next data chunk, etc. |

Each `CentralDirectoryEntryBaseV3` includes a `chunk_index` field identifying which `_NNN.avv` file holds the payload.

**Compression flag (per file):**

| `flags` bit | Meaning |
|---|---|
| `0x01` | File is LZ4 Frame compressed |
| `0x00` | File is stored raw |

All multi-byte values are stored **little-endian**.

---

## `vfs_core` API Reference

Embed `vfs_core` in your own project by including `archive_reader.h` / `archive_writer.h` and compiling the `.cpp` files plus the LZ4 sources.

### Reading an archive

```cpp
#include "vfs_core/archive_reader.h"

vfs::ArchiveReader reader;

// Open single-file or split archive (auto-detected from _dir.avv suffix)
auto res = reader.open("game_data_dir.avv");

// Iterate entries
for (const auto& e : reader.get_entries()) {
    printf("[%03u] %s  (%llu bytes)\n", e.chunk_index, e.path.c_str(), e.size);
}

// Extract one file to disk
reader.extract_file("textures/sky.dds", "C:/Temp/sky.dds");

// Read one file into memory
auto data = reader.read_file_data("shaders/main.hlsl");
if (data) {
    // data.value() is std::vector<char> of decompressed bytes
}

// Extract everything with a progress callback
reader.unpack_all("output/", [](uint32_t cur, uint32_t tot, const std::string &path) {
    printf("  %u/%u  %s\n", cur, tot, path.c_str());
});
```

### Writing an archive

```cpp
#include "vfs_core/archive_writer.h"

vfs::ArchiveWriter writer;

// Single-file archive (AVV2)
writer.pack_directory("assets/", "game_data.avv");

// Single-file with custom compression level (1-12)
writer.pack_directory("assets/", "game_data.avv", 9);

// Split archive (AVV3), 4 GiB chunks, level 9
writer.pack_directory_split("assets/", "game_data",
                            4ULL * 1024 * 1024 * 1024, 9);

// With progress callback
writer.pack_directory("assets/", "game_data.avv", 3,
    [](uint32_t cur, uint32_t tot, const std::string &path) {
        printf("  %u/%u  %s\n", cur, tot, path.c_str());
    });
```

### Error codes

| `vfs::ErrorCode` | Meaning |
|---|---|
| `Success` | Operation completed successfully |
| `FileNotFound` | Path does not exist or is not a regular file/directory |
| `InvalidMagic` | Archive header does not contain expected magic bytes |
| `UnsupportedVersion` | Archive version is not supported by this reader |
| `CorruptedArchive` | Archive is truncated or structurally invalid |
| `IOError` | OS-level read/write failure |
| `PermissionDenied` | Insufficient permissions on the target path |

### `Result<T>` pattern

All fallible functions return `Result<T>` (analogous to `std::expected<T, ErrorCode>`):

```cpp
auto res = reader.open("archive.avv");
if (res)               { /* success */ }
if (!res)              { /* res.error() is a vfs::ErrorCode */ }
if (res.has_value())   { /* same as operator bool */ }
```

---

## Testing

Unit tests use **Catch2** (single-header, in `vfs_tests/`) and cover:

| Area | Tests |
|---|---|
| Endianness | Round-trip `to_disk` / `from_disk` identity |
| AVV2 round-trip | Pack → unpack with content verification |
| LZ4 compression | Flag validation, compressible vs. incompressible fallback |
| AVV3 single-chunk | Split archive with all data in one chunk |
| AVV3 multi-chunk | Forced rollover (1-byte limit), distinct chunk indices |
| AVV3 single-file API | `read_file_data` / `extract_file` on split archives |
| Error handling | Bad magic, unsupported version, truncated archive, missing file, missing directory |
| Edge cases | Zero-byte files, empty directories |

Build the **vfs_packer** project with `test_vfs.cpp` included (and CLI `main.cpp` excluded) to run Catch2.

---

## Key Design Decisions

- **C++20** with `std::filesystem`, custom `Result<T>` (fallback for `std::expected`)
- **No exceptions in hot paths** — all errors use `ErrorCode` enum + `Result<T>`
- **Little-endian on disk** — explicit `bswap` helpers ensure portability
- **LZ4 frame compression** — configurable level (1–12), per-file, only when it reduces size
- **VPK-style split archives** (AVV3) — data in sized chunks, directory in a separate index
- **Progress callbacks** — both CLI and GUI display per-file progress
- **Threaded extraction** in the GUI — non-blocking with modal progress bar
- **Minimal dependencies** — SDL2, Dear ImGui, LZ4, Catch2 (all vendored in `third_party/`)
