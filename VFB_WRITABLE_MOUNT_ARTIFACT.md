# VFB Writable Mounted Namespace Artifact

## Summary

Yes, this is possible, but not as a transparent "every mounted source is editable"
feature in the current codebase.

The existing mounted namespace already gives VFB a merged virtual directory tree.
The missing piece is a write-routing layer that decides which mounted source owns a
mutation, then executes the mutation through the existing archive writer APIs and
publishes a new mounted snapshot.

The key constraint is format support:

- `AVV4` archives are writable today through `ArchiveWriter::append_file` and
  `ArchiveWriter::delete_file`.
- `AVV5` archives are currently read-only from a mutation standpoint.
- Host-directory mounts can be treated as writable in the future, but that is a
  different write backend than archive mutation.

So the practical Phase 1 answer is:

- VFB can open and browse a virtual directory in mounted mode.
- VFB can append/remove files through that virtual directory.
- The mutation must be routed to a specific writable mount.
- Initial writable support should be limited to `AVV4` archive mounts.

## Goal

Let the user work from the mounted virtual tree in VFB and perform archive edits
without leaving mounted mode:

- append a dropped OS file into the current virtual directory
- delete the selected mounted file
- optionally replace an existing mounted file
- rebuild the merged mounted snapshot so the UI immediately reflects the result

This should preserve the current mounted read-path goals:

- lock-free steady-state `read/stat/list`
- explicit mount priority and overlay behavior
- parent-directory conflict rules remain enforced

## Why This Is Feasible

The codebase already has most of the required pieces:

- `MountedFileSystem` resolves a virtual path to the winning mounted entry.
- `MountedFileSystem` can rebuild and republish a snapshot after structural changes.
- `ArchiveWriter` already mutates `AVV4` archives with:
  - `append_file`
  - `delete_file`
- VFB already supports append/delete in single-archive mode.

What does not exist yet is a mounted write coordinator that bridges:

1. a virtual path in the mounted namespace
2. the owning writable mount
3. the source-specific mutation backend
4. snapshot refresh and UI refresh

## Non-Goals

- Writing to `AVV5` in the first phase
- Recursive directory delete in the first phase
- Cross-mount rename or move in the first phase
- "Delete from merged namespace forever" semantics that also remove lower layers
- Automatic write routing when the target is ambiguous

## Core Design

## 1. Separate read mounts from writable mounts

Extend mount metadata with explicit write capability instead of inferring it from
source kind.

```cpp
enum class MountWriteMode : uint8_t {
  ReadOnly,
  ArchiveAvv4Writable
};

struct MountRecord {
  uint32_t mount_id = 0;
  uint64_t mount_order = 0;
  MountedFileSystem::MountSourceKind source_kind;
  std::filesystem::path source_root;
  std::string mount_point;
  int priority = 0;
  PathCasePolicy case_policy;
  MountWriteMode write_mode = MountWriteMode::ReadOnly;
  std::string password;
  std::shared_ptr<IMountedSource> source;
};
```

Initial rules:

- `AVV4` archive mount may opt into `ArchiveAvv4Writable`
- `AVV5` archive mount is always `ReadOnly`
- host-directory mount remains `ReadOnly` for this feature set

This avoids pretending that all mounted sources support the same mutation model.

## 2. Add a mounted mutation service

Do not overload `MountedFileSystem` read operations with archive mutation logic.
Add a small coordinator service:

```cpp
class MountedMutationService {
public:
  explicit MountedMutationService(MountedFileSystem& fs);

  Result<void> append_file_to_directory(
      const std::filesystem::path& source_file,
      const std::string& virtual_directory,
      const MutationOptions& options = {});

  Result<void> replace_file(
      const std::filesystem::path& source_file,
      const std::string& virtual_path,
      const MutationOptions& options = {});

  Result<void> delete_file(const std::string& virtual_path,
                           const MutationOptions& options = {});
};
```

Reasoning:

- `MountedFileSystem` stays primarily a read namespace plus mount registry.
- mutation code can own writer-specific rules, mount locking, and refresh flow.
- VFB can call the mutation service from mounted mode just like it currently calls
  `ArchiveWriter` in single-archive mode.

## 3. Introduce explicit write routing

The hard problem is not writing bytes. It is deciding which archive should receive
the mutation.

The routing rules should be explicit and conservative.

### Existing file delete

Deleting `/game/config/settings.json` should target the mount that currently owns
the visible winning file.

Rules:

- resolve the visible mounted entry
- if its owning mount is writable, delete from that mount
- if its owning mount is not writable, reject with `ReadOnlyMount`

Effect with overlays:

- deleting the top layer reveals the next lower layer if one exists
- that is correct and matches layered archive behavior

### Existing file replace

Replacing an existing visible file should target the mount that owns the visible
winner, not an arbitrary lower layer.

Rules:

- resolve visible winner
- require owner mount writable
- call append/replace into that same archive using the same virtual path inside
  the mount

### New file append into a directory

This is the ambiguous case.

Example:

- `/game` contains files from base archive, patch archive, and a read-only AVV5 DLC
- user drops `new.cfg` into `/game/config`

There is no safe universal default unless the target writable mount is obvious.

Recommended rule:

- if the current directory already exists in one writable mounted archive at the
  highest visible precedence, use that mount
- if multiple writable mounts are plausible targets, require explicit user choice
- if no writable mount covers the directory, reject

This leads to a write-target concept in the UI.

## 4. Define the write target model

VFB mounted mode should have a selected write target for append-like operations.

```cpp
struct WriteTargetSelection {
  std::optional<uint32_t> preferred_mount_id;
  bool sticky_for_session = true;
};
```

Resolution order for append into a virtual directory:

1. If the user selected a preferred writable mount and it covers the directory,
   use it.
2. Otherwise, if exactly one writable mount maps that virtual directory subtree,
   use it.
3. Otherwise, if the visible directory is sourced from a single writable winner,
   use it.
4. Otherwise, fail with `AmbiguousWriteTarget`.

This is stricter than "always write to highest priority mount", which would be
surprising when multiple editable overlays exist.

## Routing Back to Archive Paths

Mounted entries already carry:

- `mount_id`
- `virtual_path`
- `source_path`
- `source_root`

For mutation, each writable archive mount also needs a reversible mapping between:

- mounted virtual path: `/game/config/settings.json`
- archive-internal path: `config/settings.json`

That mapping is:

```text
archive_virtual_path = virtual_path stripped of mount_point prefix
```

Examples:

- mount `C:\packs\base.avv` at `/game`
- mounted path `/game/config/settings.json`
- archive path `config/settings.json`

For append into a directory:

```text
archive_virtual_path = rebase(virtual_directory, mount_point) + "/" + filename
```

This only works if the chosen mount actually covers the target virtual path.

## Source Mutation Flow

## 1. Writable archive adapter

Add a dedicated writable source wrapper instead of having VFB call
`ArchiveWriter` directly.

```cpp
class IWritableMountedSource {
public:
  virtual ~IWritableMountedSource() = default;
  virtual Result<void> append_file(const std::filesystem::path& source_file,
                                   const std::string& source_path) = 0;
  virtual Result<void> delete_file(const std::string& source_path) = 0;
  virtual bool is_writable() const = 0;
};
```

Then let `ArchiveMountedSource` optionally implement it when the mounted archive is
an `AVV4` file.

This keeps mutation backend logic near the source implementation instead of
spreading writer-specific special cases through the browser.

## 2. Archive write sequence

For `AVV4` mutation:

1. validate target mount is writable
2. acquire that mount's write mutex
3. stop or invalidate mounted async preview work for that mount
4. close the mounted source reader so file handles are released
5. run `ArchiveWriter::append_file` or `ArchiveWriter::delete_file`
6. reopen the source
7. ask `MountedFileSystem` to rebuild and atomically publish a new snapshot
8. notify VFB to refresh selection/tree/preview

This is slower than a read, but mutations are expected to be rare.

## 3. Per-mount write lock

Mounted reads should remain lock-free at namespace level.

Mutation should use a per-mount write mutex:

```cpp
struct MountRecord {
  ...
  std::mutex write_mutex;
  std::atomic<uint64_t> generation = 0;
};
```

Why per-mount:

- appending to `patch.avv` should not block unrelated reads from `base.avv`
- a global write lock would over-serialize independent archives

The mounted namespace rebuild still uses the existing rebuild mutex because the
mount registry and published snapshot change together.

## Race Conditions To Handle

## 1. Read vs write on the same mount

Problem:

- preview thread is reading from `patch.avv`
- user deletes a file from `patch.avv`

If the reader and writer both touch the same file concurrently, stale handles or
undefined behavior are possible.

Required rule:

- mounted writes are exclusive per mount
- before mutation, cancel or invalidate new async preview work for that mount
- in-flight reads may finish against the old source instance
- reopen a fresh source and publish a new snapshot afterward

This mirrors the existing single-archive browser behavior where the reader is
closed before writing.

## 2. Snapshot vs mutation visibility

Problem:

- UI list shows old mounted contents
- delete succeeds
- user immediately clicks stale row

Required behavior:

- mutation returns only after snapshot rebuild succeeds
- UI reloads mounted tree from the fresh snapshot
- selected item is revalidated by virtual path
- if no longer visible, clear selection and preview

## 3. Delete revealing lower layers

Problem:

- user deletes `/game/config/settings.json`
- file still appears because a lower-priority archive also has it

This is not a bug. It is the correct overlay result.

VFB must explain this clearly:

- status message should say which mount was modified
- overlays panel should show that the previous lower layer is now visible

## 4. Parent-directory conflicts after mutation

Mounted conflict rules must still hold after append/delete.

Examples:

- appending file `/game/config` must fail if `/game/config/settings.json` exists
- appending `/game/config/new.txt` must fail if visible file `/game/config` exists

This means mutation validation must run against the full mounted namespace before
executing the writer, not only against the target archive in isolation.

The safest approach is:

1. simulate the target mount's post-mutation file set in memory
2. rebuild a prospective namespace snapshot
3. if rebuild succeeds, commit the mutation
4. if rebuild fails, abort without touching the archive

## Browser UX Plan

## 1. Mounted mode remains the entry point

The user already browses the virtual tree in mounted mode. Extend that mode with
write-aware controls instead of inventing a third browser mode.

New UI elements:

- `Mounts` panel:
  - show read-only vs writable badge per mount
  - show format (`AVV4` or `AVV5`)
  - allow selecting the preferred write target from writable mounts
- file context menu:
  - `Delete From Owning Archive`
  - `Show Overlays`
- directory context menu:
  - `Import File Here...`
  - `Set As Write Target`
- drag-drop into mounted mode:
  - if dropping `.avv`, mount it
  - if dropping a regular file onto a mounted directory, append through the current
    write target

## 2. Status and confirmation language

The UI wording matters because mounted writes are not equivalent to simple file
deletes in a normal filesystem.

Delete confirmation should say:

```text
Delete /game/config/settings.json from archive patch.avv?
If lower-priority overlays exist, another version may remain visible afterward.
```

Append status should say:

```text
Imported new.cfg into patch.avv as /game/config/new.cfg
```

## 3. Password and encryption handling

Mounted writable `AVV4` archives may be encrypted.

Rules:

- if the writable mount is encrypted, password must be verified before mutation
- append must reuse the archive's existing encryption mode
- mixed-encryption archives remain non-writable through mounted mode

This matches the existing single-archive append rules.

## API Additions

## 1. Mounted FS introspection

Add enough API surface so VFB can reason about write routing without reaching into
private mount internals.

```cpp
struct MountInfo {
  uint32_t mount_id = 0;
  std::filesystem::path source_root;
  MountedFileSystem::MountSourceKind source_kind;
  std::string mount_point;
  int priority = 0;
  PathCasePolicy case_policy;
  MountWriteMode write_mode = MountWriteMode::ReadOnly;
  bool is_encrypted = false;
  bool is_password_verified = false;
};

Result<std::vector<MountInfo>> list_mounts() const;
Result<std::vector<MountInfo>> candidate_write_mounts(const std::string& virtual_dir) const;
```

## 2. Mutation errors

Add explicit errors:

- `ReadOnlyMount`
- `AmbiguousWriteTarget`
- `WriteTargetNotFound`
- `MountBusy`
- `UnsupportedMountedMutation`
- `MountedMutationConflict`

These should be separate from general archive I/O errors so VFB can show useful
messages.

## Phase Breakdown

## Phase 1: Writable mounted `AVV4` file operations

Implement:

- writable mount metadata
- write-target selection
- mounted append of one OS file into current directory
- mounted delete of one visible file from owning writable archive
- snapshot rebuild after mutation
- VFB mounted-mode UI for append/delete/write-target

Not yet:

- `AVV5` mutation
- recursive folder import
- rename/move
- directory delete

This is the smallest version that is still coherent.

## Phase 2: Better mounted editing UX

Add:

- overlay inspector actions on hidden entries
- replace file action
- conflict preview before commit
- recursive directory import
- host-directory write support if desired

## Phase 3: Format-level writable layering improvements

If true mounted editing becomes a major workflow, consider a better backend than
direct in-place archive mutation:

- writable patch archive generated on top of read-only base archives
- tombstone records for deletions
- session journal for grouped edits

That would behave more like a real layered package filesystem and avoid rewriting
large base archives during iteration.

## Recommended Implementation Plan

1. Extend `MountRecord` with `write_mode`, encryption metadata, and a per-mount
   write mutex.
2. Add mounted introspection APIs so VFB can list writable candidates and show
   them in the mounts panel.
3. Add `MountedMutationService` with:
   - `append_file_to_directory`
   - `delete_file`
   - `replace_file`
4. Implement writable `ArchiveMountedSource` support for `AVV4` only.
5. Add pre-commit conflict simulation against the mounted namespace.
6. Update VFB mounted mode:
   - write-target selector
   - mounted directory import
   - mounted file delete
   - clearer overlay-aware confirmation/status text
7. Add tests for:
   - delete visible winner reveals lower layer
   - append into writable overlay creates visible file immediately
   - append fails for ambiguous write target
   - append/delete fails for read-only `AVV5` mount
   - mutation preserves parent-directory conflict rules
   - encrypted writable mount requires verified password
   - write during async preview is safe

## Recommendation

This feature is worth doing, but only with explicit write routing and explicit
writable mount capability.

The wrong design would be "mounted mode behaves like a normal writable filesystem"
because the current backends do not have uniform write semantics, especially once
overlays, `AVV5`, and encrypted archives are involved.

The right design is:

- mounted mode remains the user-facing virtual directory
- reads continue to resolve through the merged snapshot
- writes route to one clearly identified writable archive mount
- snapshot rebuild makes the result immediately visible in the same virtual tree

That gives VFB the workflow you want without hiding the archive-layer realities
that still matter for correctness.
