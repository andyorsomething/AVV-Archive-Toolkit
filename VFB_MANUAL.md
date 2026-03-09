# Virtual File Browser Manual

The Virtual File Browser is an ImGui-based GUI for inspecting `.avv` archives without unpacking them first.

## Features

- Hierarchical folder navigation with breadcrumbs and live search
- Native drag-out extraction on Windows
- Drag-in append for AVV4 archives
- Image, text, and hex previews
- Right-click actions for copy path, extract, and delete
- Dockable layout with multi-viewport support
- Mounted namespace mode for mixing archives and host directories

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open archive dialog |
| `Alt+F4` | Exit |

## Encryption Behavior

- Archives containing encrypted entries show an `(Encrypted)` tag in the menu bar.
- Type a password and press `Enter` to verify it.
- If verification succeeds, the tag changes to `(Decrypted)`.
- Editing the password text clears the verified state immediately.
- Previewing encrypted files uses the verified password in memory.
- Drag-in append inherits the archive's actual encryption mode instead of guessing from UI state.

## Error Codes

The browser surfaces core VFS errors in status messages and dialogs using the numeric `ErrorCode` values from `vfs_core/vfs_types.h`.

| Number | Error Code | Meaning |
|---|---|---|
| `0` | `Success` | The operation completed normally. |
| `1` | `File Not Found` | A source file, archive, or archive entry could not be found. |
| `2` | `Invalid Magic` | The selected file is not a recognized AVV archive. |
| `3` | `Unsupported Version` | The archive version is valid AVV, but this build does not support it. |
| `4` | `Corrupted Archive` | Archive metadata or payload structure is invalid, unsafe, or truncated. |
| `5` | `I/O Error` | A low-level read or write operation failed. |
| `6` | `Permission Denied` | The browser could not read from or write to the requested path. |
| `7` | `Archive Too Large` | The requested write operation exceeds a format limit. |
| `8` | `Hash Mismatch (Integrity Error)` | The archive integrity check failed; the archive may be damaged or tampered with. |
| `9` | `Decryption Failed` | The password is wrong, missing, or the encrypted payload could not be decoded. |
| `10` | `Already Mounted` | The requested source is already mounted at this location. |
| `11` | `Mount Not Found` | The requested mount ID does not exist. |
| `12` | `Path Conflict` | The mount/path would create an ambiguous or overlapping entry. |
| `13` | `Invalid Mount Point` | The provided mount point is malformed or invalid. |
| `14` | `Not A Directory` | The operation requires a directory but found a file. |

Examples:
- `Error 8` means `Hash Mismatch (Integrity Error)`.
- `Error 9` usually means the password is wrong or missing.
- Missing or modified split chunks can produce `Error 8` or `Error 4`.

## Quick Start

```text
vfs_browser.exe path/to/archive.avv
vfs_browser.exe path/to/archive_dir.avv
```

1. Browse: Drag an archive onto the window or use `Ctrl+O`.
2. Extract: Right-click an entry, drag it out, or use `File -> Extract All`.
3. Append: Navigate to a target folder in an AVV4 archive and drop a file into the window.
4. Close: Use `File -> Close Archive` when you are done.

## Mounted Namespace Mode

The browser can also build a read-only merged namespace from multiple sources.

Menu entries:

- `File -> Mount Archive...`
- `File -> Mount Host Directory...`
- `File -> Clear Mounts`

Mounted mode behavior:

- shows a merged tree rooted at `/`
- resolves exact-path conflicts by priority and mount order
- shows active mounts in a `Mounts` panel
- previews and extracts the winning file from the merged namespace
- keeps single-archive open/edit workflows separate from mounted mode

Mounted mode is read-only in this phase. Archive append/delete and drag-in
mutation remain available only in single-archive mode.
