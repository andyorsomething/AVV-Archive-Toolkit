# Virtual File Browser Manual

The Virtual File Browser is an ImGui-based GUI for inspecting `.avv` archives without unpacking them first.

## Features

- Hierarchical folder navigation with breadcrumbs and live search
- Native drag-out extraction on Windows
- Drag-in append for AVV4 archives
- Image, text, and hex previews
- Right-click actions for copy path, extract, and delete
- Dockable layout with multi-viewport support

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

## Quick Start

```text
vfs_browser.exe path/to/archive.avv
vfs_browser.exe path/to/archive_dir.avv
```

1. Browse: Drag an archive onto the window or use `Ctrl+O`.
2. Extract: Right-click an entry, drag it out, or use `File -> Extract All`.
3. Append: Navigate to a target folder in an AVV4 archive and drop a file into the window.
4. Close: Use `File -> Close Archive` when you are done.
