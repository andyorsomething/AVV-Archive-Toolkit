# VFS CLI Manual

`vfs_cli` packs, unpacks, and lists `.avv` archives.

## CLI Reference

```text
vfs_cli [-v] [-c <1..12>] [--encrypt <xor|aes>] [--key <pass>] [--no-journal] pack  <output.avv> <input_dir>
vfs_cli [-v] [-c <1..12>] [--encrypt <xor|aes>] [--key <pass>] [-s <GB>] [--no-journal] packs <stem> <input_dir>
vfs_cli [-v] [--key <pass>] unpack <input.avv|_dir.avv> <output_dir>
vfs_cli list <input.avv|_dir.avv>
```

## Commands

| Command | Description |
|---|---|
| `pack` | Pack a directory into a single AVV4 archive |
| `packs` | Pack a directory into a split AVV5 archive set |
| `unpack` | Extract all files from an archive |
| `list` | Print the central directory without extracting |

## Flags

| Flag | Default | Description |
|---|---|---|
| `-v` | off | Verbose output |
| `-c <level>` | `3` | LZ4 compression level, `1` through `12` |
| `-s <GB>` | `12` | Maximum chunk size in GiB for `packs` |
| `--encrypt <alg>` | `none` | Encryption algorithm: `xor` or `aes` |
| `--key <pass>` | empty | Password for encryption or decryption |
| `--no-journal` | off | Disable resume journaling during packing |

## Examples

### Bash / Command Prompt

```bash
vfs_cli -c 12 pack game_data.avv assets/
vfs_cli --encrypt aes --key "fair" pack game_data.avv assets/
vfs_cli --encrypt aes --key "fair" packs game_data assets/
vfs_cli -c 9 -s 4 packs game_data assets/
vfs_cli list game_data_dir.avv
vfs_cli -v --key "fair" unpack game_data_dir.avv extracted/
```

### PowerShell

```powershell
.\vfs_cli.exe -c 12 pack game_data.avv assets/
.\vfs_cli.exe --encrypt aes --key "fair" pack game_data.avv assets/
.\vfs_cli.exe --encrypt aes --key "fair" packs game_data assets/
.\vfs_cli.exe -c 9 -s 4 packs game_data assets/
.\vfs_cli.exe list game_data_dir.avv
.\vfs_cli.exe -v --key "fair" unpack game_data_dir.avv extracted/
```

All pack and unpack operations display an inline progress bar:

```text
  [############--------]  58%  (305/524)  textures/sky_hdr.dds
```

## Drag-and-Drop (Windows Explorer)

You can drag a folder or an `.avv` file directly onto `vfs_cli.exe`.

| Dropped item | Action |
|---|---|
| Folder | Split-packs it into `<foldername>_dir.avv` plus chunks |
| `.avv` file | Unpacks it into `<stem>/` beside the archive |

Drag-and-drop mode honors the same parsed options as the normal CLI path, including `--key`, `--encrypt`, and `--no-journal`.
