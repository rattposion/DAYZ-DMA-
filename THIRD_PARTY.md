# Third-party components

| Component | Files | Licence | Source |
|---|---|---|---|
| MemProcFS (Ulf Frisk) | `vmm.dll`, `info.db`, `third_party/pcileech/vmmdll.h` | AGPL-3.0 | https://github.com/ufrisk/MemProcFS |
| LeechCore (Ulf Frisk) | `leechcore.dll`, `leechcore_driver.dll`, `third_party/pcileech/leechcore.h` | AGPL-3.0 | https://github.com/ufrisk/LeechCore |
| FTDI D3XX | `FTD3XX.dll`, `FTD3XXWU.dll` | FTDI driver licence | https://ftdichip.com/drivers/d3xx-drivers/ |
| Microsoft Debugging Tools | `dbghelp.dll`, `symsrv.dll` | Windows SDK redistributable terms | https://learn.microsoft.com/windows-hardware/drivers/debugger/ |
| Dear ImGui (Omar Cornut) | `src/imgui/` | MIT | https://github.com/ocornut/imgui |

The DLLs are not in this repository. They come in the release zip, or from the links above.
Radar map images are downloaded at runtime from https://dayz.xam.nu and are not redistributed.
