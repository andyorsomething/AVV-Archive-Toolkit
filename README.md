# AVV-Archive-Toolkit

A C++20 virtual file system with a custom `.avv` binary archive format, a command-line interface, and a graphical file browser.

## Project Structure

```text
AVV-Archive-Toolkit/
|- vfs_core/          Core archiving library (reader, writer, types)
|- vfs_cli/           Command-line pack/unpack/list tool
|- vfs_browser/       Dear ImGui graphical archive browser
|- vfs_tests/         Catch2 unit tests
`- third_party/       SDL2, Dear ImGui, LZ4, stb_image, tiny-aes-c (local copies)
```

## Documentation

- **[VFS CLI Guide](CLI_MANUAL.md)** - Guide for packing and unpacking archives with `vfs_cli`.
- **[Virtual File Browser GUI Guide](VFB_MANUAL.md)** - Guide for exploring, extracting, and appending archives with `vfs_browser`.

## Quick Start

Open `virtualFileSystem_program.slnx` in Visual Studio 2022 (v145 toolset, C++20). The solution contains:

- `vfs_cli` - the CLI tool in `vfs_cli/main.cpp`
- `vfs_browser` - the GUI browser in `vfs_browser/main.cpp`
- `vfs_tests` - the unit test runner in `vfs_tests/test_vfs.cpp`

Dependencies are vendored in `third_party/`; no package manager is required.

## `.avv` Archive Formats

### AVV4 - Single-file archive

| Section | Contents |
|---|---|
| Header (24 B) | Magic `AVV4`, version `uint32`, directory hash `uint64`, compression level `uint8`, padding `7 B` |
| File Data Blocks | Raw or LZ4-compressed file payloads, appended sequentially |
| Central Directory | Array of `CentralDirectoryEntryBase` (28 B) plus path strings |
| Footer (16 B) | Directory offset `uint64`, ending with magic `4VVA_EOF` |

The AVV4 integrity hash covers every byte after the header.

### AVV5 - Split archive

| File | Contents |
|---|---|
| `<stem>_dir.avv` | AVV5 header, Central Directory (32 B entries), and footer |
| `<stem>_000.avv` | Raw data chunk (payload bytes only) |
| `<stem>_001.avv` | Next data chunk, and so on |

Each `CentralDirectoryEntryBaseV3` includes a `chunk_index` field identifying which `_NNN.avv` file holds the payload.

The AVV5 integrity hash covers every referenced chunk payload in chunk order, followed by the `_dir.avv` contents after the header.

### Flags

Compression:

| `flags` bit | Meaning |
|---|---|
| `0x0001` | File is LZ4 frame-compressed |
| `0x0000` | File is stored raw |

Encryption nibble in bits `[11:8]`:

| `flags` value | Meaning |
|---|---|
| `0x0100` | File is XOR-encrypted |
| `0x0200` | File is AES-256-CTR encrypted |

All multi-byte values are stored little-endian.

## `vfs_core` API Reference

Embed `vfs_core` by including `archive_reader.h` and `archive_writer.h`, then compiling the `.cpp` files plus the LZ4 sources.

### Reading an archive

```cpp
#include "vfs_core/archive_reader.h"

vfs::ArchiveReader reader;
auto res = reader.open("game_data_dir.avv");

for (const auto &e : reader.get_entries()) {
    printf("[%03u] %s (%llu bytes)\n", e.chunk_index, e.path.c_str(), e.size);
}

reader.extract_file("textures/sky.dds", "C:/Temp/sky.dds");

auto data = reader.read_file_data("shaders/main.hlsl");
if (data) {
    // data.value() is a std::vector<char> of decompressed bytes
}

reader.unpack_all("output/", [](uint32_t cur, uint32_t tot, const std::string &path) {
    printf("  %u/%u %s\n", cur, tot, path.c_str());
});
```

### Writing an archive

```cpp
#include "vfs_core/archive_writer.h"

vfs::ArchiveWriter writer;

writer.pack_directory("assets/", "game_data.avv");

vfs::EncryptionOptions enc;
enc.algorithm = vfs::EncryptionAlgorithm::Aes256Ctr;
enc.key = "SuperSecret123";
writer.pack_directory("assets/", "secure_data.avv", 3, nullptr, enc);

writer.append_file("C:/Temp/new.dds", "textures/new.dds", "secure_data.avv", 3, enc);
writer.delete_file("textures/new.dds", "secure_data.avv");

writer.pack_directory_split("assets/", "game_data", 4ULL * 1024 * 1024 * 1024, 9);
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
| `ArchiveTooLarge` | A split archive would exceed the format's chunk-count limit |
| `HashMismatch` | The computed integrity hash does not match the stored hash |
| `DecryptionFailed` | The file payload could not be decrypted |

### `Result<T>` pattern

All fallible public APIs return `Result<T>`:

```cpp
auto res = reader.open("archive.avv");
if (res) { /* success */ }
if (!res) { /* res.error() is a vfs::ErrorCode */ }
if (res.has_value()) { /* same as operator bool */ }
```

## Testing

Unit tests use Catch2 (single-header, in `vfs_tests/`) and cover:

| Area | Tests |
|---|---|
| Endianness | Round-trip `to_disk` / `from_disk` identity |
| AVV4 round-trip | Pack -> unpack with content verification |
| LZ4 compression | Compressible vs. incompressible fallback |
| AVV5 single-chunk | Split archive with all data in one chunk |
| AVV5 multi-chunk | Forced rollover with distinct chunk indices |
| API coverage | `read_file_data`, `extract_file`, `append_file`, `delete_file` |
| Error handling | Bad magic, unsupported version, truncated archive, missing paths |
| Crypto/integrity | AES/XOR round-trip, single-file tamper detection, split tamper detection |
| Security | Rejection of unsafe entry paths |
| Async | Thread-pool behavior and async read plumbing |

## Key Design Decisions

- C++20 with `std::filesystem`
- `Result<T>` instead of exceptions at public API boundaries
- Explicit little-endian serialization helpers
- LZ4 frame compression, applied only when it reduces size
- Optional XOR or AES-256-CTR encryption
- Split archive support with central directory metadata in `_dir.avv`
- Vendored dependencies only
