# GPU Spoofer (Ninja Gaiden 4)

A lightweight DXGI proxy DLL that spoofs your GPU identity, making games believe you have an **8GB NVIDIA GeForce RTX 3060**. This project produces a .dll file, that you can rename to dxgi.dll and copy it to same location as your game.exe file so that the game thinks you have 8GB Nividia RTX 3060 Graphics card. Originally built for Ninja Gaiden 4, but works with any DirectX 11/12 Windows game.

## How It Works

The DLL acts as a proxy for `dxgi.dll`. When placed in a game's directory, Windows loads it instead of the system DXGI library. It forwards all calls to the real `dxgi.dll` while intercepting adapter description queries to report spoofed GPU information:

| Property | Spoofed Value |
|----------|---------------|
| GPU Name | NVIDIA GeForce RTX 3060 |
| Dedicated VRAM | 8 GB |
| Shared Memory | 4 GB |
| Vendor ID | 0x10DE (NVIDIA) |
| Device ID | 0x2504 |

## Installation

> **⚠️ NOTE:** You can download the .dll file from [Releases](https://github.com/krish-jb/NG4VRAMSpoofer/releases/tag/v1.0.0)

1. Build the project (see below)
2. Rename the output `.dll` to `dxgi.dll`
3. Copy it to the same folder as your game's `.exe`
4. Launch the game normally

To uninstall, simply delete `dxgi.dll` from the game folder.

## Compatibility

- ✅ Windows games using DirectX 11 or 12
- ✅ Works without any injector or loader
- ❌ Does not work with Vulkan or OpenGL games
- ❌ Some games may require additional DXGI exports not currently forwarded

If a game fails to launch with `0xc000007b` or similar errors, it likely needs more DXGI exports. You can check what's needed with:

```
dumpbin /imports game.exe | findstr dxgi
```

## Building

### Prerequisites

- Visual Studio 2019 or later
- [MinHook](https://github.com/tsudakageyu/minhook) — you must build MinHook yourself and link it

### Steps

1. Clone this repository
2. Build MinHook as a static library (x64 Release)
3. In project properties:
   - Add MinHook's `include` folder to **Additional Include Directories**
   - Add MinHook's `.lib` output folder to **Additional Library Directories**
   - Add `libMinHook.x64.lib` to **Additional Dependencies**
4. Set platform to **x64**
5. Build in Release mode
6. Rename output DLL to `dxgi.dll`

## Notes

- If used alongside other DXGI wrappers (ReShade, DXVK, OptiScaler, etc.), load order conflicts may occur
