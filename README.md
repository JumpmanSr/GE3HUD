# GE3HUD 1.0.1 — memory-tool edition

In-game monster HP overlay for God Eater 3, reading the game's **own** enemy
list instead of scanning memory for things that look like monsters.

By Jumpman.

| Key | Does |
|-----|------|
| `F1` | Toggle the HUD (the mod is dormant until you press it) |
| `F2` | Dump the list and all resolved addresses to the console |

## Building

```
build.bat
```
Finds MSVC via `vswhere`, produces `build\GE3HUD.asi`. Dependencies are
vendored in `third_party\` (Dear ImGui 1.91.5 + dx11/win32 backends, and
Detours). 

## Installing

```
copy GE3HUD.asi -> "C:\SteamLibrary\steamapps\common\GOD EATER 3\plugins\"
```

Ultimate ASI Loader (`dinput8.dll`) picks it up. Delete to uninstall. YOU WILL NEED GE3-MOD-LOADER installed! 
https://github.com/VelouriasMoon/GE3-Mod-Loader/

I made this with Remove_RandomSeed = TRUE but it should work regardless. 

## Ingame Screenshot

<img width="2560" height="1440" alt="GE3" src="https://github.com/user-attachments/assets/10c9ac66-73f6-4c15-9528-1bd056cacc03" />

## Known Bugs
In the hub, a few "Targets" with 1/1 hp show up. 
