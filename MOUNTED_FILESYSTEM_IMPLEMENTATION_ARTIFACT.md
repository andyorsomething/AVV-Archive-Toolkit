# Mounted File System Implementation Artifact

## Goal

Add mounted file system semantics on top of the current AVV archive model so callers can treat one or more archives as a read-only virtual namespace instead of unpacking them first. The intended usage is similar to Unreal `.pak` search paths or Valve `.vpk` mounts: mount one or more archives at a virtual root, resolve a logical path, then read the winning entry according to mount priority.

This artifact describes a practical implementation path that fits the current codebase instead of replacing it.

## Current State

The codebase already has most of the low-level pieces needed for mounted reads:

- `ArchiveReader` can open a single `AVV4` archive or `AVV5` split archive set and build an in-memory entry table.
- Each entry already has a stable virtual path (`FileEntry::path`) and enough metadata to locate payload bytes.
- `read_file_data`, `extract_file`, async reads, prefetch, integrity verification, and decryption already exist.
- `AVV5` already behaves like a mini mounted data store internally because payloads are resolved indirectly through a directory file and chunk files.

What is missing is a higher-level abstraction that:

- mounts multiple archives at once;
- rebases archive-relative paths under a mount point;
- resolves conflicts by priority;
- enumerates a merged directory tree;
- exposes this through core API, CLI, and browser surfaces.

## Recommended Scope

Implement mounting as a runtime VFS layer first, and include both AVV archive
mounts and host-directory mounts in phase 1. Do not change the AVV on-disk
format in phase 1.

Reasoning:

- The existing format already stores the metadata needed for mounted reads.
- Runtime mounting is enough to unlock game-style asset lookup.
- Host-directory mounts make the namespace useful for local overrides and mod/dev workflows immediately.
- Avoiding an immediate format revision keeps compatibility with all current archives, tests, and tools.
- Format-level embedded mount manifests can be added later once the runtime behavior is proven.

## Design Summary

Introduce a new `MountedFileSystem` service in `vfs_core` that owns multiple
mounted sources and presents a merged read-only namespace.

Core idea:

1. A mount operation opens an archive.
2. Every archive entry path is rebased under a caller-supplied mount point such as `/game`, `/dlc`, or `/`.
3. The mount system inserts the rebased path into a global lookup table.
4. If multiple mounted archives provide the same rebased path, the highest-priority mount wins.
5. Reads, extraction, async reads, and enumeration are resolved through that lookup table.

## Proposed API

Add a new pair of files:

- `vfs_core/mounted_file_system.h`
- `vfs_core/mounted_file_system.cpp`

Suggested public surface:

```cpp
namespace vfs {

class MountedFileSystem {
public:
  struct MountOptions {
    std::string mount_point = "/";
    int priority = 0;
    std::string password;
    bool case_sensitive = false;
  };

  struct MountedEntry {
    std::string virtual_path;
    std::string source_archive;
    std::string source_path;
    uint16_t flags = 0;
    uint16_t chunk_index = 0;
    uint64_t size = 0;
    uint64_t compressed_size = 0;
    int priority = 0;
  };

  Result<uint32_t> mount(const std::filesystem::path& archive_path,
                         const MountOptions& options = {});
  Result<void> unmount(uint32_t mount_id);
  void unmount_all();

  [[nodiscard]] bool is_mounted() const;
  [[nodiscard]] bool exists(const std::string& virtual_path) const;
  [[nodiscard]] Result<MountedEntry> stat(const std::string& virtual_path) const;
  [[nodiscard]] std::vector<MountedEntry> list_all() const;
  [[nodiscard]] std::vector<MountedEntry> list_directory(const std::string& virtual_dir) const;

  [[nodiscard]] Result<std::vector<char>> read_file_data(const std::string& virtual_path);
  [[nodiscard]] Result<void> extract_file(const std::string& virtual_path,
                                          const std::filesystem::path& output_path);

  void read_file_async(const std::string& virtual_path,
                       std::function<void(Result<std::vector<char>>)> on_complete);
  void pump_callbacks();
};

} // namespace vfs
```

## Internal Model

Use three layers of state.

### 1. Mount records

One record per mounted archive:

```cpp
struct MountRecord {
  uint32_t mount_id;
  std::filesystem::path archive_path;
  std::string mount_point;
  int priority;
  bool case_sensitive;
  std::string password;
  std::unique_ptr<ArchiveReader> reader;
};
```

Purpose:

- owns the live `ArchiveReader`;
- preserves mount options;
- supports unmount and rebuild.

### 2. Resolved path index

Map rebased virtual path to the winning mounted source:

```cpp
struct ResolvedSource {
  uint32_t mount_id;
  std::string rebased_path;
  std::string source_path;
  size_t entry_index;
  int priority;
  uint64_t mount_order;
};
```

Suggested container:

```cpp
std::unordered_map<std::string, ResolvedSource> resolved_entries_;
```

Conflict rule:

- higher `priority` wins;
- ties break by latest mount order so newer mounts override older ones.

That gives expected patch/DLC behavior.

### 3. Directory index

Build a secondary index for merged directory views:

```cpp
std::unordered_map<std::string, std::vector<std::string>> directory_children_;
```

This enables:

- browser tree rendering;
- CLI listing of a mounted subtree;
- future wildcard queries.

## Path Rules

Define path normalization once and use it everywhere.

Recommended rules:

- accept `/` and `\` from callers, normalize to `/` internally;
- trim repeated separators;
- disallow absolute host paths;
- normalize `.` and reject `..` after normalization;
- represent root as `/`;
- require mount points to begin with `/`.

Examples:

- mount point `/game`, source entry `textures/sky.dds` -> virtual path `/game/textures/sky.dds`
- mount point `/`, source entry `ui/main.json` -> virtual path `/ui/main.json`
- mount point `/dlc/pack1/`, source entry `levels/a.bin` -> virtual path `/dlc/pack1/levels/a.bin`

Add one shared helper in `vfs_core`, rather than duplicating the current path validation logic again.

## Why A New Runtime Layer Instead of Extending ArchiveReader

`ArchiveReader` is currently a single-archive primitive. Keeping it that way preserves a clean separation:

- `ArchiveReader`: parse one archive, validate it, and read entry payloads.
- `MountedFileSystem`: compose many readers into one virtual namespace.

If mounting logic is pushed into `ArchiveReader`, the class becomes responsible for both archive parsing and namespace policy, which will complicate async behavior, conflict resolution, and future host-directory mounts.

## Read Path

A `read_file_data("/game/textures/sky.dds")` request should do the following:

1. Normalize the incoming virtual path.
2. Look up the winning `ResolvedSource`.
3. Fetch the owning `MountRecord`.
4. Call `mount.reader->read_file_data(source_path, mount.password)`.
5. Return the bytes unchanged.

This reuses the existing decompression, decryption, chunk handling, and integrity validation logic with almost no duplication.

## Enumeration Path

Mounted enumeration is the main new behavior and should be explicit.

Two list modes are useful:

- `list_all()`: return the fully merged file set after conflict resolution.
- `list_directory(path)`: return immediate children for GUI tree and CLI traversal.

Recommendation:

- expose merged results by default;
- add an internal debug mode later if you want to inspect shadowed entries.

For browser/debugging, it may also be useful to add a non-default API later:

```cpp
std::vector<MountedEntry> list_overlays(const std::string& virtual_path) const;
```

That would show every archive contributing the same path, ordered by effective precedence.

## Async Behavior

Do not add a second thread pool in phase 1.

Preferred behavior:

- `MountedFileSystem::read_file_async` resolves the virtual path to a mount and forwards to the owning `ArchiveReader::read_file_async`.
- `MountedFileSystem::pump_callbacks` calls `pump_callbacks()` on every mounted reader.

This keeps concurrency behavior consistent with the current implementation and avoids callback routing complexity.

## CLI Changes

The current CLI is pack/unpack/list only. Add a mounted inspection mode instead of trying to turn the CLI into an OS-level mounter.

Suggested commands:

```text
vfs_cli mount-list <archive.avv|_dir.avv> [--at /mount] [--json]
vfs_cli mount-cat <archive.avv|_dir.avv> <virtual_path> [--at /mount] [--key <pass>]
vfs_cli mount-extract <archive.avv|_dir.avv> <virtual_path> <output_path> [--at /mount] [--key <pass>]
vfs_cli overlay-list <virtual_path> <archive1> <archive2> ...
```

More useful medium-term command:

```text
vfs_cli vmount list /game archive_base_dir.avv archive_patch_dir.avv archive_dlc_dir.avv
vfs_cli vmount cat /game/config/settings.json archive_base_dir.avv archive_patch_dir.avv
```

That lets users test precedence without any OS integration.

## Browser Changes

The browser is where mounted behavior will be most visible.

Recommended additions:

- `File -> Mount Archive...`
- mount table panel showing:
  - archive path
  - mount point
  - priority
  - encrypted/decrypted status
- merged tree view rooted at `/`
- optional per-entry tooltip showing source archive and source path
- optional overlay inspector for conflicts

Important UX rule:

- opening a single archive directly should still work exactly as it does today;
- mounted mode should be additive, not a replacement.

A practical approach is to add an application-level mode:

- single-archive mode: existing `ArchiveReader` path
- mounted mode: new `MountedFileSystem` path

## Format-Level Manifest Extension (Phase 2, Optional)

Once runtime mounts exist, consider a manifest entry that describes default mount behavior for a package.

Examples:

- mount point: `/game`
- package kind: `base`, `patch`, `dlc`, `mod`
- suggested priority: `100`
- dependency tags: `requires=base_assets`

Do not embed this in phase 1.

If added later, prefer one of these options:

### Option A: Reserved metadata file inside the archive

Store a normal AVV entry such as:

- `__avv__/mount.json`

Advantages:

- no format revision required;
- easy to inspect and edit;
- old readers ignore it safely.

### Option B: New archive version with explicit package manifest block

Advantages:

- cleaner binary layout;
- better for future signing/catalog features.

Disadvantages:

- requires AVV format revision and broader compatibility work.

Recommendation: use option A first if embedded metadata is needed.

## Conflict Resolution Policy

Define this up front so tools behave consistently.

Recommended precedence:

1. higher mount priority wins;
2. if priorities match, later mount wins;
3. directories are synthetic and exist if they have at least one visible child;
4. a file path and a directory path cannot both exist at the same virtual path;
5. if a conflict creates file-vs-directory ambiguity, reject the later mount with `CorruptedArchive` or a new mount-specific error.

The file-vs-directory ambiguity rule matters. Example:

- mount A provides `config/settings.json`
- mount B provides `config`

This must not silently succeed.

## Error Model

The current `ErrorCode` enum is archive-centric. Mount operations likely need additional values.

Suggested additions:

- `AlreadyMounted`
- `MountNotFound`
- `PathConflict`
- `InvalidMountPoint`

If you want to avoid expanding `ErrorCode` immediately, phase 1 can temporarily map these to `CorruptedArchive` or `FileNotFound`, but that will be harder to diagnose in the GUI and CLI.

Recommendation: extend `ErrorCode` now.

## Testing Plan

Add a dedicated test section in `vfs_tests/test_vfs.cpp` first, then split it into a separate test file if it grows too large.

Minimum test matrix:

### Core mount behavior

- mount one AVV4 archive at `/` and read a file through mounted API
- mount one AVV5 archive at `/game` and read a file through mounted API
- unmount removes visibility
- list_all returns rebased paths
- list_directory returns immediate children only

### Overlay behavior

- two mounts with different priorities, higher priority wins
- two mounts with same priority, later mount wins
- shadowed file is not returned by merged listing
- overlay inspection returns both candidates in order, if implemented

### Path normalization

- `/game//textures\\sky.dds` resolves correctly
- invalid mount point is rejected
- `..` in caller virtual path is rejected
- file-vs-directory conflict is rejected

### Encryption and split handling

- mounted read succeeds for AES-encrypted archive with correct password
- mounted read fails with wrong password
- mounted read succeeds across `AVV5` chunked payloads

### Async

- concurrent mounted async reads complete successfully
- mounted callback pumping works with multiple underlying readers

### Integrity

- tampered mounted archive fails during mount
- missing split chunk prevents mount

## Phased Implementation Plan

### Phase 1

- add shared path normalization helpers
- add `MountedFileSystem` with read-only mounts
- support `mount`, `unmount`, `exists`, `stat`, `list_all`, `list_directory`, `read_file_data`, `extract_file`, async forwarding
- add unit tests

This phase delivers the core value.

### Phase 2

- add CLI mounted inspection commands
- add browser mounted mode and mount table UI
- add source-archive metadata in GUI

### Phase 3

- add optional embedded mount manifest (`__avv__/mount.json`)
- add overlay inspection and debug tooling
- consider writable overlay or host-directory mounts if needed

## Non-Goals For Now

These features should be explicitly deferred:

- OS kernel file-system mounting (WinFSP, Dokan, FUSE)
- write-through mounted archives
- patch-delta binary transforms between packages
- cross-archive deduplication
- signed manifests / certificate chains

Those are separate projects.

## Concrete Integration Points In This Repository

Most of the implementation can be done without destabilizing the existing archive code.

### `vfs_core`

- keep `ArchiveReader` unchanged except possibly for exposing a small helper to access entries by index/path efficiently
- add `mounted_file_system.h/.cpp`
- move shared path-normalization logic into a common helper if both reader and writer need it
- extend `ErrorCode` and `error_code_to_string`

### `vfs_cli/main.cpp`

- add `vmount` or `mount-*` commands
- preserve all current `pack`, `packs`, `unpack`, `list` behavior

### `vfs_browser/main.cpp`

- add app-level mounted mode and merged tree presentation
- retain current single-archive open path for direct archive browsing

### `README.md`

- document the distinction between archive format support and mounted virtual namespace support

## Recommendation

Build the runtime mount layer now and treat format changes as optional follow-up work.

That gives you the same practical capability people usually want from `.pak` or `.vpk` style systems:

- mount archives without unpacking;
- overlay patches and DLC by priority;
- browse and extract through a merged namespace;
- keep the current AVV formats and toolchain intact.

If this is implemented cleanly, host-directory mounts and embedded package manifests can be added later without reworking the core archive reader.
