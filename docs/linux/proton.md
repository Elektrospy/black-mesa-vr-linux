# Black Mesa through Proton: `scripts/bmvr-linux.sh`

Status: reported working by users (PSVR2, SteamVR on Linux, OpenXR helper; see `docs/open-work-2026-10.md`). This is the
**Windows build of the mod** running under Proton. It has nothing to do with the native port described in the rest of this
folder.

Needs the Windows build of the game: force a Proton tool in Steam (the game folder must contain `bms.exe`). The mod patches
fixed offsets in the Windows `client.dll` / `server.dll`.

## What the script does

It ports the README install steps for the release zip (`drop-into-Black-Mesa-folder`) plus the cleanups `scripts/install.ps1`
does:

- finds the game in the Steam libraries (`libraryfolders.vdf`, Flatpak, Snap);
- refuses to run while `bms.exe` or the helper is running;
- installs `d3d9.dll` into all three required places (`AGENTS.md` rule 19);
- backs up stock DXVK / `openvr_api.dll` once;
- keeps an existing `VR/config.txt`;
- writes the VR `dxvk.conf`;
- disables `bin/sourcevr.dll`;
- clears stale `bmvr_in_*.flag` files and false `bmvr_skip.txt` bans.

`scripts/bmvr-linux.sh uninstall` reverses it.

## Launch options

The `WINEDLLOVERRIDES` part is **unverified**; drop it if the DLL loads without it:

    WINEDLLOVERRIDES="d3d9=n,b" %command% -heapsize 524288 -processheap -high -novid -windowed -oldgameui -enabledxvk

## Separate copy, Steam library entry, Blue Shift

    scripts/bmvr-linux.sh install --separate                          # copy + install the mod into the copy only
    scripts/bmvr-linux.sh install --separate --steam-shortcut         # ... plus a Steam library entry (Steam closed)
    scripts/bmvr-linux.sh install --separate --steam-shortcut --campaign bms --campaign bshift

- `--separate` makes a copy-on-write copy (plain copy when the filesystem has no reflinks; never hardlinks) named
  "Black Mesa VR" next to the original and installs only there. The original game and its normal launch stay untouched, both
  can be started independently, and the copy has its own saves and settings.
- `--steam-shortcut` adds a non-Steam shortcut per `--campaign` (`bms`, `bshift` = Blue Shift `-game bshift`, `asheep` =
  Azure Sheep `-game asheep`) to `userdata/<id>/config/shortcuts.vdf`, with a timestamped backup, and refuses to touch a file
  it cannot rewrite byte for byte. Proton must still be selected for the shortcut in Steam (Properties -> Compatibility).
- Blue Shift: the mod detects `-game bshift` itself and always reads `bms/cfg/bmvr.cfg`, so the same install covers it.

Not verified: the shortcut writer was tested only against hand-built files, not a live Steam; `steam_appid.txt` for the copy
is unverified.

## OpenXR runtime under Proton

The OpenXR helper needs an active OpenXR runtime on the Linux side; see [openxr-results.md](openxr-results.md), "Making SteamVR
the active runtime". The root `README.md` has the same steps.
