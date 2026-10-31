# PS4 Torrent Browser

A native SDL2 controller-driven BitTorrent client for jailbroken PS4. The application shell and OpenOrbis packaging are based on [OptiTronOffical/PS4-Browser-Downloader](https://github.com/OptiTronOffical/PS4-Browser-Downloader); the protocol engine is imported from [angeloINTJ/ps4-torrent](https://github.com/angeloINTJ/ps4-torrent) (MIT).

## Features
- SDL2 1080p controller interface.
- Scans `/data/pkg/torrents/` for `.torrent` files.
- Starts the selected torrent and displays progress.
- Saves files under `/data/pkg/downloads/`.
- Imports upstream bencode, metainfo, HTTP tracker, peer-wire and SHA-1 verification modules.

## Controls
| DualShock 4 | Action |
|---|---|
| D-pad Up/Down | Select torrent |
| X | Start selected torrent |
| Circle | Request stop |
| Square | Refresh list |
| Options | Exit |

Copy legally obtained `.torrent` files into `/data/pkg/torrents/` using FTP, refresh, select and press X.

## Current engine limitations
This upstream engine revision supports HTTP trackers and download-only peer transfers. HTTPS/UDP trackers, DHT, PEX, magnet links, seeding, and pause/resume are not implemented. Circle requests a stop; it does not pause or preserve a resumable session. The interface lists multiple torrent files but runs one selected session at a time. The engine should be further tested on-console before relying on it for long-running downloads.

## Build
Requires Clang/LLD, Python 3 and OpenOrbis Toolchain v0.5.4. SDL2 headers and library must be available in the PS4 toolchain environment.

```bash
export OO_PS4_TOOLCHAIN=/opt/openorbis/OpenOrbis/PS4Toolchain
make clean
make
```

PKG output is written to `output/`. On Ubuntu 24.04+, the bundled PkgTool may require OpenSSL 1.1; see the [upstream torrent build notes](https://github.com/angeloINTJ/ps4-torrent#build).

## Credits and licensing
The imported torrent engine is Copyright (c) 2026 Ângelo Moisés Alves and licensed under MIT. Its license is retained in this repository. Browser shell/packaging baseline: OptiTronOffical/PS4-Browser-Downloader.
