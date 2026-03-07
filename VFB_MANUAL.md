# Virtual File Browser (VFB Manual)

The Virtual File Browser is an ImGui-based GUI for inspecting `.avv` archives (single and split) without unpacking.

## Features

- **Hierarchical Folder Navigation**: Browse archives via a directory tree with clickable breadcrumbs, or use the live search/filter bar to find files.
- **Native Drag-and-Drop Operations**:
  - **Drag-Out (Extract)**: Click and drag any file from the explorer onto your Windows desktop or folders to extract it natively.
  - **Drag-In (Append)**: Drag any new file into the browser (while an AVV4 archive is open) to append it dynamically without a full repack. The dropped file automatically inherits the original archive's compression and encryption and utilizes context-aware routing (drops the item into whichever subdirectory you're currently viewing).
- **Rich Previews**: The file details pane includes instant image previews (for `.png`, `.jpg`, `.bmp` via `stb_image`), a text viewer, and a fast clipped hex dump viewer.
- **Quick File Deletion**: Right-click to safely delete files directly from the archive securely without destroying data alignment.
- **Context Menus & Extraction**: Right-click to `Copy Path` or `Extract to CWD`. Use **File → Extract All…** for a threaded progress bar modal.
- Dockable panel layout with multi-viewport support and a custom, rounded dark theme.

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open archive dialog |
| `Alt+F4` | Exit |

## Encryption / Decryption in VFB

- An archive containing encrypted entries will display an amber **`(Encrypted)`** tag next to its path in the top menu bar.
- Use the **Password** field in the menu bar to type your key. **You must press `Enter` to verify it.**
  - If the password successfully decrypts an encrypted entry, the tag turns green and reads **`(Decrypted)`**.
  - If you edit or delete the password text, the tag will immediately snap back to unverified `(Encrypted)`.
- When the tag is green, clicking any encrypted file will seamlessly decrypt it in-memory for the text/hex/image viewer preview.
- Drag-and-dropping to append new files to an encrypted archive automatically inherits the active encryption settings.

## Quick Start

Launch by setting `vfs_browser` as the Startup Project, or pass an archive on the command line:

```
vfs_browser.exe path/to/archive.avv
vfs_browser.exe path/to/archive_dir.avv
```

1. **Browse**: Drag an archive onto the window or use `Ctrl+O`.
2. **Extract**: Right click any entry to extract it, drag it to your OS, or use `File` -> `Extract All` for everything.
3. **Append**: Navigate to your desired nested path view in the VFB, and drop an external file right into it.
4. **Close**: When you're done viewing the file, just click `File -> Close Archive` to flush the memory.
