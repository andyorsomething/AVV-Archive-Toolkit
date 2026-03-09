# Mounting Walkthrough

This is a standalone walkthrough for using the mounting functionality. It is
not linked from the repo docs.

## Goal

Start with a normal asset folder, pack it into an archive, then mount that
archive and an override folder into one virtual namespace.

## 1. Pack a Base Archive

Assume this source tree:

```text
assets/
|- config/settings.json
|- audio/music/theme.ogg
`- textures/ui/logo.png
```

Pack it into a single-file archive:

```powershell
.\vfs_cli.exe pack game_data.avv assets
```

If you want split output instead:

```powershell
.\vfs_cli.exe packs game_data assets
```

That produces:

- `game_data.avv` for `pack`
- `game_data_dir.avv` plus `game_data_000.avv`, `game_data_001.avv`, ... for `packs`

## 2. Verify What Was Packed

Single-file archive:

```powershell
.\vfs_cli.exe list game_data.avv
```

Split archive:

```powershell
.\vfs_cli.exe list game_data_dir.avv
```

You should see archive-relative paths such as:

- `config/settings.json`
- `audio/music/theme.ogg`
- `textures/ui/logo.png`

## 3. Mount the Archive Into a Virtual Root

Mount the archive at `/game` and inspect the merged directory:

```powershell
.\vfs_cli.exe vmount list /game --archive game_data.avv --at /game
```

For a split archive, mount the directory file:

```powershell
.\vfs_cli.exe vmount list /game --archive game_data_dir.avv --at /game
```

Now the archive entries are visible as mounted paths:

- `/game/config/settings.json`
- `/game/audio/music/theme.ogg`
- `/game/textures/ui/logo.png`

## 4. Read a Mounted File Without Unpacking

Show metadata for a mounted path:

```powershell
.\vfs_cli.exe vmount stat /game/config/settings.json --archive game_data.avv --at /game
```

Print the file bytes to stdout:

```powershell
.\vfs_cli.exe vmount cat /game/config/settings.json --archive game_data.avv --at /game
```

Extract just one mounted file:

```powershell
.\vfs_cli.exe vmount extract /game/config/settings.json extracted\settings.json --archive game_data.avv --at /game
```

## 5. Layer a Higher-Priority Override

Create a live override folder:

```text
live_override/
`- config/settings.json
```

Mount both the archive and the host directory at the same mount point, with the
host directory at higher priority:

```powershell
.\vfs_cli.exe vmount stat /game/config/settings.json `
  --archive game_data.avv --at /game --priority 0 `
  --dir live_override --at /game --priority 100
```

The reported source should now point at `live_override`.

If you read the mounted file:

```powershell
.\vfs_cli.exe vmount cat /game/config/settings.json `
  --archive game_data.avv --at /game --priority 0 `
  --dir live_override --at /game --priority 100
```

you will get the override file, not the archived one.

## 6. Inspect Overlays

To see every source contributing the same exact mounted path:

```powershell
.\vfs_cli.exe vmount overlays /game/config/settings.json `
  --archive game_data.avv --at /game --priority 0 `
  --dir live_override --at /game --priority 100
```

The first row is the active winner. Lower rows are shadowed.

## 7. Open the Same Data in the Browser

In `vfs_browser`:

1. Use `File -> Mount Archive...` and choose `game_data.avv` or `game_data_dir.avv`.
2. Use `File -> Mount Host Directory...` if you also want a live override folder.
3. Browse the merged tree rooted at `/`.
4. Select files to preview them.
5. Use the `Mounts` panel to confirm which sources are active.

## Notes

- Archive mounts are read-only in mounted mode.
- Host-directory mounts snapshot file metadata when mounted, but read file bytes live.
- Higher priority wins. Equal priority breaks in favor of the later mount.
- Exact-path overlays are allowed. Parent-directory conflicts are rejected.
- For encrypted archives, provide `--key <pass>` in CLI, or enter the password in the browser after mounting.
