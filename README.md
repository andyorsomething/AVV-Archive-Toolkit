# AVV-Archive-Toolkit

A C++20 Virtual File System with a custom `.avv` binary archive format, a command-line interface, and a graphical file browser.

## Project Structure

```
AVV-Archive-Toolkit/
├── vfs_core/          Core archiving library (reader, writer, types)
├── vfs_cli/           Command-line pack/unpack/list tool
├── vfs_browser/       Dear ImGui graphical archive browser
├── vfs_tests/         Catch2 unit tests
└── third_party/       SDL2, Dear ImGui, LZ4, stb_image, tiny-aes-c (local copies)
```

## Documentation

The documentation has been split into specific guides depending on what you are trying to accomplish:

- **[VFS CLI Guide](CLI_MANUAL.md)** — Guide for packing and unpacking archives using the command line (`vfs_cli`), including Bash/PowerShell examples and arguments.
- **[Virtual File Browser GUI Guide](VFB_MANUAL.md)** — Manual for utilizing the immersive GUI (`vfs_browser`) to explore, extract natively, and append to archives via right-clicks and drag-and-drops.

---

## Quick Start (Building)

Open `virtualFileSystem_program.slnx` in **Visual Studio 2022** (v145 toolset, C++20). The solution contains three projects:

- **vfs_cli** — the CLI tool (`vfs_cli/main.cpp`)
- **vfs_browser** — the GUI browser (set as Startup Project to launch)
- **vfs_tests** — the unit test runner (`vfs_tests/test_vfs.cpp`)

Dependencies are included locally in `third_party/` — no package manager required.

---

## `.avv` Archive Formats

### AVV4 — Single-File Archive

| Section | Contents |
|---|---|
| **Header** (24 B) | Magic `AVV4`, version `uint32`, directory hash `uint64`, compression level `uint8`, padding `7 B` |
| **File Data Blocks** | Raw or LZ4-compressed file payloads, appended sequentially |
| **Central Directory** | Array of `CentralDirectoryEntryBase` (28 B) + path strings |
| **Footer** (16 B) | Directory offset `uint64` ending with magic signature `4VVA_EOF` |

### AVV5 — Split (VPK-Style) Archive

Splits data across multiple chunk files while keeping the directory in a separate index:

| File | Contents |
|---|---|
| `<stem>_dir.avv` | AVV5 header + Central Directory (V5 entries, 32 B each) + footer |
| `<stem>_000.avv` | Raw data chunk (file payloads only, no header/footer) |
| `<stem>_001.avv` | Next data chunk, etc. |

Each `CentralDirectoryEntryBaseV3` includes a `chunk_index` field identifying which `_NNN.avv` file holds the payload.

**Compression flag (per file):**

| `flags` bit | Meaning |
|---|---|
| `0x0001` | File is LZ4 Frame compressed |
| `0x0000` | File is stored raw |

**Encryption flag (per file, stored in bits [11:8]):**

| `flags` value (bits [11:8]) | Meaning |
|---|---|
| `0x0100` | File is XOR encrypted |
| `0x0200` | File is AES-256 CTR encrypted |

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

// Single-file archive (AVV4)
writer.pack_directory("assets/", "game_data.avv");
 
// Single-file with AES-256 encryption
vfs::EncryptionOptions enc;
enc.algorithm = vfs::EncryptionAlgorithm::Aes256Ctr;
enc.key = "SuperSecret123";
writer.pack_directory("assets/", "secure_data.avv", 3, nullptr, enc);
 
// Append a single file (auto-encrypts if archive is encrypted and key is provided)
writer.append_file("C:/Temp/new.dds", "textures/new.dds", "secure_data.avv", 3, enc);

// Delete a single file natively without trashing the entire archive
writer.delete_file("textures/new.dds", "secure_data.avv");
 
// Split archive (AVV5), 4 GiB chunks, level 9
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
| `HashMismatch` | The archive's computed integrity hash does not match the stored hash |
| `DecryptionFailed` | The file payload could not be decrypted (e.g., wrong password) |

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
| AVV4 round-trip | Pack → unpack with content verification |
| LZ4 compression | Flag validation, compressible vs. incompressible fallback |
| AVV5 single-chunk | Split archive with all data in one chunk |
| AVV5 multi-chunk | Forced rollover (1-byte limit), distinct chunk indices |
| AVV5 / AVV4 API | `read_file_data` / `extract_file` / `append_file` / `delete_file` logic |
| Error handling | Bad magic, unsupported version, truncated archive, missing file, missing directory |
| Edge cases | Zero-byte files, empty directories |

Build the **vfs_tests** project to run the Catch2 test suite.

---

## Key Design Decisions

- **C++20** with `std::filesystem`, custom `Result<T>` (fallback for `std::expected`)
- **No exceptions in hot paths** — all errors use `ErrorCode` enum + `Result<T>`
- **Little-endian on disk** — explicit `bswap` helpers ensure portability
- **LZ4 frame compression** — configurable level (1–12), per-file, only when it reduces size
- **VPK-style split archives** (AVV5) — data in sized chunks, directory in a separate index
- **Progress callbacks** — both CLI and GUI display per-file progress
- **Threaded extraction** in the GUI — non-blocking with modal progress bar
- **Minimal dependencies** — SDL2, Dear ImGui, LZ4, stb_image, tiny-aes-c, Catch2 (all vendored in `third_party/`)
