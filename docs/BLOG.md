# Building Ickza: a two-PC DayZ radar over DMA

![Radar](images/radar.png)

Most game overlays run on the same PC as the game. Ickza doesn't touch the gaming PC at all. A PCIe DMA
card sits in the gaming PC and reads its physical memory, a laptop on the other end of the USB cable
makes sense of that memory, and the laptop's screen is merged back onto the gaming monitor through a
fuser. The gaming PC runs DayZ and nothing else.

## Reading a game through a cable

The DMA side is built on Ulf Frisk's MemProcFS and LeechCore, which turn raw physical reads into "read
this virtual address in that process". Physical memory isn't one flat block, though. Reading outside the
real RAM ranges is slow at best, so on the first run Ickza asks the card for the gaming PC's memory map,
writes it to `mmap.txt` and reopens with it. Every run after that starts straight away.

Speed is everything. Each frame needs every nearby player's position, skeleton and held item, and loot
across a large area. Reads are grouped and scattered rather than done one at a time, and the control
panel shows the live link speed, so a bad cable or port shows up right away.

## Finding the world

DayZ keeps everything under one global `World` object. After every game patch it moves. Ickza keeps a
single `kWorld` offset; when an update lands the radar waits on "waiting for DayZ" until that offset is
found again, set, and rebuilt. One number to change per patch keeps the rest of the code stable.

## Two screens out of one window

![Heading lines](images/radar-headings.png)

The ESP layer draws skeletons, names, distances and held items in Direct3D 11 on a transparent fullscreen
window, which the fuser lays over the game. The radar is a second screen (`F8`) built with Dear ImGui. It
shows survivors, infected, animals, vehicles and 15 categories of loot on the real map, which it works
out by itself and downloads on first use, in satellite or topographic and at up to 16384 px.

Loot is the noisy part, so it gets filters: categories on and off, a name whitelist or blacklist, and a
`dayz_rare.txt` list for the items that should always stand out.

![Radar settings](images/radar-panel.png)

## Making it pleasant to run

The laptop lives across the desk, so it should never need touching. `deploy.bat` copies a new build to
a shared folder on the laptop, and the running copy picks it up and restarts into it. The output display
is picked once in the control panel (with an "Identify" button for when the screens all look the same)
and remembered. If something crashes, the log line decodes against the linker map from the same build.

The whole thing ships as one folder: an exe built with the static runtime, a `dma\` folder of DLLs and a
batch file. Unzip it, double-click it, done.

---

Source and feature list: [README](../README.md).
