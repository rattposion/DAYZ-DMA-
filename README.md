# DAYZ-DMA
Features

ESP overlay
Skeleton or box ESP
Real player names from the server's player list (not the character model), plus distance
Item in hand for every player, including you
Zombies on/off (off also stops reading them)
Valuable loot only mode: guns, ammo, attachments, explosives, bags, rare
View distance 25-900
Crosshair with adjustable size, corner minimap, FPS counter, uncapped FPS
Direct3D 11 renderer, GDI fallback (--gdi)

Radar
Survivors, infected, vehicles, animals and loot on the real map, each one toggleable
Online players list: everyone on the server in a searchable dropdown near you with distance, then everyone else A-Z
15 loot categories with their own colours, presets (All / None / Gear / Survive), right-click a category to show only that one
Item filter: blacklist or whitelist by classname, * wildcards, suggestions from loot in range, F on a hovered item adds it
dayz_rare.txt for items that should always stand out
Live counts, grid reference and position
Heading lines with adjustable length, labels, loot labels within a set range, marker size
Detects the map from the game and downloads it on first use: satellite or topo, 8k or 16k, town names
Wheel / +- zoom, drag to pan, right-click or Space re-centres, Tab hides the panel, F8 / Esc hides the radar

Control panel
Pick the output display, with Identify to tell screens apart
Start / stop, live game PID, server name and DMA speed
Settings saved to Ickza.ini


Setup
Install Visual Studio 2022"
Put the MemProcFS / LeechCore / FTDI DLLs in dma\ (vmm.dll, leechcore.dll, leechcore_driver.dll, info.db, FTD3XX.dll, FTD3XXWU.dll, dbghelp.dll, symsrv.dll) - get them from the MemProcFS release and FTDI's site
Run build.bat x64 (or open Ickza.sln)
Copy the build folder to the second PC and run run_ickza.bat. END quits.

Troubleshooting
Stuck on "waiting for DayZ" - DayZ was updated.
Players show as Elias / Seth etc. - that's the character model; the NetworkClient offset is out of date
MemProcFS init failed - something else has the card open (another Ickza, pcileech...).
vmm.dll not found - keep the dma\ folder next to Ickza.exe
DMA not found - install the FTDI D3XX driver, use a USB 3 port
Blank map - the PC needs internet the first time each map is used
