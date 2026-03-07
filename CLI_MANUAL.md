# VFS CLI (CLI Manual)

The `vfs_cli` is a command-line interface for the AVV-Archive-Toolkit. It allows packing, unpacking, and listing `.avv` archives.

## CLI Reference

```
vfs_cli [-v] [-c <1..12>] [--encrypt <xor|aes>] [--key <pass>] pack  <output.avv> <input_dir>
vfs_cli [-v] [-c <1..12>] [--encrypt <xor|aes>] [--key <pass>] [-s <GB>] packs <stem> <input_dir>
vfs_cli [-v] [--key <pass>] unpack <input.avv|_dir.avv> <output_dir>
vfs_cli list <input.avv|_dir.avv>
```

### Commands

| Command | Description |
|---|---|
| `pack` | Pack a directory into a single AVV4 archive (everything in one `.avv` file). |
| `packs` | Pack a directory into a VPK-style split AVV5 archive set. Generates `<stem>_dir.avv` (the directory metadata) and `<stem>_000.avv`, etc. (the data chunks). |
| `unpack` | Extract all files from an archive (single or split) |
| `list` | Print the central directory without extracting |

### Flags

| Flag | Default | Description |
|---|---|---|
| `-v` | off | Verbose — print each file with size and compression method |
| `-c <level>` | `3` | LZ4 compression level (1–12; higher = slower + smaller) |
| `-s <GB>` | `12` | Maximum chunk size in GiB for split archives (`packs` only) |
| `--encrypt <alg>` | `none` | Algorithm to use for packing (`xor` or `aes`) |
| `--key <pass>` | `""` | Password used for encryption during `pack` or decryption during `unpack` |

### Examples

**Bash / Command Prompt:**
```bash
# Pack into a single file with max LZ4 compression
vfs_cli -c 12 pack game_data.avv assets/

# Pack into a single file with AES encryption
# Note: This creates ONLY 'game_data.avv' (no _dir.avv is created for the 'pack' command)
vfs_cli --encrypt aes --key "fair" pack game_data.avv assets/

# Split-pack with AES encryption
# This behaves like the folder drag-and-drop. It generates 'game_data_dir.avv' and the data chunks.
vfs_cli --encrypt aes --key "fair" packs game_data assets/

# Split-pack with 4 GiB chunks and level 9 compression
vfs_cli -c 9 -s 4 packs game_data assets/

# List a split archive
vfs_cli list game_data_dir.avv

# Unpack an encrypted split archive with verbose output and progress bar
vfs_cli -v --key "fair" unpack game_data_dir.avv extracted/
```

**PowerShell:**
```powershell
# Pack into a single file with max LZ4 compression
.\vfs_cli.exe -c 12 pack game_data.avv assets/

# Pack into a single file with AES encryption
# Note: This creates ONLY 'game_data.avv' (no _dir.avv is created for the 'pack' command)
.\vfs_cli.exe --encrypt aes --key "fair" pack game_data.avv assets/

# Split-pack with AES encryption
# This behaves like the folder drag-and-drop. It generates 'game_data_dir.avv' and the data chunks.
.\vfs_cli.exe --encrypt aes --key "fair" packs game_data assets/

# Split-pack with 4 GiB chunks and level 9 compression
.\vfs_cli.exe -c 9 -s 4 packs game_data assets/

# List a split archive
.\vfs_cli.exe list game_data_dir.avv

# Unpack an encrypted split archive with verbose output and progress bar
.\vfs_cli.exe -v --key "fair" unpack game_data_dir.avv extracted/
```

All pack and unpack operations display an inline progress bar:
```
  [############--------]  58%  (305/524)  textures/sky_hdr.dds
```

### Drag-and-Drop (Windows Explorer)

You can drag a **folder** or an **`.avv` file** directly onto `vfs_cli.exe`:

| Dropped item | Action |
|---|---|
| A **folder** | Default split-packs it (`packs`) into `<foldername>_dir.avv` + chunks |
| An **`.avv` file** | Unpacks it into `<stem>/` in the same directory |

The console stays open after completion so you can read the output.
