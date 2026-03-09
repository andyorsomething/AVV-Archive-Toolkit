# VFS CLI Manual

`vfs_cli` packs, unpacks, and lists `.avv` archives.

## CLI Reference

```text
vfs_cli [-v] [-c <1..12>] [--encrypt <xor|aes>] [--key <pass>] [--no-journal] pack  <output.avv> <input_dir>
vfs_cli [-v] [-c <1..12>] [--encrypt <xor|aes>] [--key <pass>] [-s <GB>] [--no-journal] packs <stem> <input_dir>
vfs_cli [-v] [--key <pass>] unpack <input.avv|_dir.avv> <output_dir>
vfs_cli list <input.avv|_dir.avv>
vfs_cli vmount list <virtual_dir> <mount_spec>...
vfs_cli vmount cat <virtual_path> <mount_spec>...
vfs_cli vmount extract <virtual_path> <output_path> <mount_spec>...
vfs_cli vmount stat <virtual_path> <mount_spec>...
vfs_cli vmount overlays <virtual_path> <mount_spec>...
```

## Commands

| Command | Description |
|---|---|
| `pack` | Pack a directory into a single AVV4 archive |
| `packs` | Pack a directory into a split AVV5 archive set |
| `unpack` | Extract all files from an archive |
| `list` | Print the central directory without extracting |
| `vmount ...` | Inspect a merged mounted namespace built from archives and/or host directories |

## Flags

| Flag | Default | Description |
|---|---|---|
| `-v` | off | Verbose output |
| `-c <level>` | `3` | LZ4 compression level, `1` through `12` |
| `-s <GB>` | `12` | Maximum chunk size in GiB for `packs` |
| `--encrypt <alg>` | `none` | Encryption algorithm: `xor` or `aes` |
| `--key <pass>` | empty | Password for encryption or decryption |
| `--no-journal` | off | Disable resume journaling during packing |

## Mounted Namespace

Mounted CLI commands build an in-memory read-only namespace from one or more
sources.

Mount specs:

```text
--archive <path> [--at <mount>] [--priority <n>] [--key <pass>] [--case <archive|host|sensitive|insensitive>]
--dir <path>     [--at <mount>] [--priority <n>] [--case <archive|host|sensitive|insensitive>]
```

Mounted query modifiers:

```text
[--unmount <mount-number>]...
```

Defaults:

- archive mounts default to mount point `/`, priority `0`, case policy `archive`
- host-directory mounts default to mount point `/`, priority `0`, case policy `host`
- higher priority wins; equal priority breaks in favor of the later mount
- `--unmount` removes a mount spec by 1-based position before the query runs

Examples:

```powershell
.\vfs_cli.exe vmount list /game `
  --archive base_dir.avv --at /game --priority 0 `
  --archive patch_dir.avv --at /game --priority 100 `
  --dir C:\mods\live --at /game --priority 200

.\vfs_cli.exe vmount cat /game/config/settings.json `
  --archive base_dir.avv --at /game `
  --archive patch_dir.avv --at /game --priority 100

.\vfs_cli.exe vmount stat /game/config/settings.json `
  --archive base_dir.avv --at /game `
  --dir C:\mods\live --at /game --priority 200

.\vfs_cli.exe vmount list /game `
  --archive base_dir.avv --at /game `
  --dir C:\mods\live --at /game --priority 200 `
  --unmount 2
```

Mounted mode is read-only in this phase. It supports:

- merged directory listing
- file reads to stdout
- extraction to disk
- stat-style source inspection
- overlay inspection for conflicting exact paths

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
