# Salvatore Gon’ Getcha — Sprite Companion

A Fallout: New Vegas mod by **2Shay**, made with [Sprite Forge Studio](https://github.com/2Shay96/sprite-forge) and GECK.

He finds you. He fights you. Then he fits in your inventory.

Salvatore hunts you across the Mojave after 1–3 in-game days. Defeat him, collect him, then equip the item to deploy a dancing billboard sprite companion. He follows, fights and explores nearby while you stand still.

## Download v1.0

Download **SalvatoreGanacci-v1.0.zip** from [Nexus Mods](https://www.nexusmods.com/newvegas/mods/99947) or [GitHub Releases](https://github.com/2Shay96/salvatore-gon-getcha/releases/tag/salvatore-v1.0).
Install with your mod manager and enable `SalvatoreGanacci.esp`, then restart the game. Requires Fallout: New Vegas only; no DLC or NVSE required.
Use a save from before installation for a fresh start. The Ganacci Remote is added to your Aid tab.

## Features

- Separate main, idle and attack animations; main and idle use forward/backward loops at 24 fps.
- Constant spatial theme, an occasional attack quip and two random idle sounds.
- Defeated collectible, deploy/store loop and repairable companion condition (66%, 33%, 0%).
- Companion following, fighting and idle exploration.
- Ganacci Remote for status, relocation, encounter reset and test mode.

## Known issues

Remote relocation while carrying Salvatore can create duplicates with shared broken condition. Avoid Send away/Summon while carrying him; a fix is planned for v1.1. Repair-bag compatibility is untested. Music continues in menus, scripted pickup can be silent, and a brief first-appearance flicker can occur.

See [the v1.0 readme](docs/SalvatoreGanacci-Readme.txt) for full instructions.
The optional filming plugin is excluded from this release.

## Making sprites

This sprite mod was made with [Sprite Forge Studio](https://github.com/2Shay96/sprite-forge). The tool is not needed to play the mod. Its Nexus link will be added when Sprite Forge Studio v1.0 is published.
The [Salvatore Nexus page](https://www.nexusmods.com/newvegas/mods/99947) includes screenshots and the [showcase video](https://youtu.be/BFRHDjeNHd4). Deathclaw footage uses a separate filming add-on with invulnerability and increased damage; that add-on is excluded from this v1.0 download.

## Source and building

This repository contains a scoped v1.0 source snapshot, media inputs, stable FormIDs, compiled script cache and asset build tools. It does not contain the private workshop's other projects or history, game archives, extracted vanilla meshes or generated build output.

For source builds use Windows, Python 3.12, Pillow, PyFFI and the public [GECK MCP bridge](https://github.com/2Shay96/geck-mcp-nv) v0.2.1 or newer. Set `GECK_BRIDGE_ROOT` to that checkout's `bridge` folder. The bridge supplies the NIF/ESP implementation and creature skeleton tools. You need your own Fallout: New Vegas installation; adjust `gameData` in `projects/salvatore/animation-runtime.json` if necessary.

```powershell
$env:GECK_BRIDGE_ROOT = 'C:/path/to/geck-mcp-nv/bridge'
py -3.12 tools/build_salvatore_animation_runtime.py projects/salvatore/animation-runtime.json
py -3.12 build_plugin.py projects/salvatore/salvatore.spec.json --state state
```

The asset builder reads original game archives to create the skeleton/collision outputs locally. Do not commit extracted game material. Script changes require a clean GECK compile through the bridge before rebuilding; the included cache corresponds to accepted v1.0. Live GECK operations need a project profile configured for your paths; do not use the private workshop's Windows profiles verbatim.

Validation: accepted v1.0 passed six user game-regression checks and seven asset unit tests. The release ZIP has 57 entries: 55 runtime assets, plugin and readme. Its checksum and file hashes are in [release-manifest.json](release-manifest.json).

## Credits and permissions

Mod author/media preparation: 2Shay. Some sound effects come from [Pixabay](https://pixabay.com/sound-effects/) and its creators. Individual source links will be added if recovered.
Salvatore Ganacci imagery and artist theme music belong to their respective rights holders. The image is not 2Shay's original artwork. The theme is used with satirical/parody intent; challenged music will be replaced with copyright-free work. No third-party reuse permission is granted.
Fallout: New Vegas is Bethesda/Obsidian material. This is an unofficial fan mod; no endorsement implied.
Code reuse terms remain unconfirmed; publication does not add a blanket licence to third-party media.
