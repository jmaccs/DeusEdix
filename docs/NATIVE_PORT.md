# Native port: an editor bridge that is not the game's own engine

DeusEdix runs on Windows today because the engine work is done by `mapkit/host/UCEngineBridge.cpp`,
a DLL that drives the editor engine of the Deus Ex SDK 1112f (`Editor.dll`) headless. That engine is
**32-bit x86 Windows PE**, so the editor cannot run natively on Apple Silicon or on a 64-bit Linux.

This note records how the editor gets a native engine without porting the SDK.

## What does not have to change

The editor is layered, and only one layer is bound to Windows:

| Layer | Size | What it needs |
| --- | --- | --- |
| The page (`mapkit/editor/static`) | ~156k lines of JS/CSS | three.js, vendored; talks to `/api/*` on the local server |
| The editor's logic and server (`mapkit/*.py`) | ~51k lines of Python | a host it can start as a subprocess |
| The launcher (`launcher/`) | ~4.6k lines of Python | a window for the page, or a browser |
| **The engine bridge** (`mapkit/host/UCEngineBridge.cpp`) | **~131k bytes of C++** | **the game's own engine** |

The interface between the editor and the engine work is already plain and narrow:

- `mapkit/host/bridge.h` — a **plain C** struct of ~50 function pointers, one version number.
- `mapkit/host/PROTOCOL.md` — the commands, their answers and every file they write.

The Python side starts the host as a subprocess (`mapkit/kit.py`) and never links against it, so
**any program that speaks that protocol can replace the host**, including one built for a different
machine.

## What a native bridge is

A second bridge, written against **OpenDE** (`https://github.com/jmaccs/OpenDE`) instead of the
SDK's engine. OpenDE is a portable C++23 reimplementation of the Deus Ex engine: it reads and
writes `.dx` maps and `.u` packages, builds BSP geometry, lighting and AI paths, runs the
UnrealScript compiler, and exposes the engine's own property reflection.

It implements the **same `bridge.h`**, so the editor does not change: the host gains a
`--bridge <dll>` that can be a native one.

### The SDK is not ported

`Headers/DxHeaders.zip` is in the SDK (it is a WinZip self-extracting archive and can be
extracted), and the headers are mostly platform-neutral. But the headers are not the obstacle —
the **machine code** is. `Core.dll`, `Engine.dll` and `Editor.dll` are `PE32, Intel 80386`, the
SDK ships no source, and `appInit` is a mangled export of `Core.dll`. Making the SDK's engine
run on ARM64 would mean rewriting it; OpenDE has already done that work.

## What OpenDE already has

Measured against the ~50 functions of `bridge.h`:

- **Native already** (~40%): loading and saving packages, meshes and animations; moving,
  deleting and scaling actors; setting any property by name (`UProperty::fromText`, the
  equivalent of UnrealEd's `ImportText`); reading and writing the class tree; point-in-solid
  and line traces; the UnrealScript compiler; the map checker.
- **Needs an adapter** (~55%): almost all of these are *dump* functions — the UE1 structures
  (`UModel`, `UPolys`, `LightMapIndex`, `UTexture`, `UMesh`) are already the engine's own, and
  only a JSON writer and a headless command loop are missing. OpenDE writes no JSON today.
- **Missing** (~5%): a few UnrealEd-specific editor operations (lowering and lifting path points
  for a navmesh) and some surface-alignment modes.

The BSP polygon format, the lightmap layout and the `.dx` file format all match the original:
OpenDE's `writePolys` is the inverse of its own `UPolys::serialize`, and a map saved by OpenDE's
editor loads in OpenDE's game with its actors, its travel URLs and its geometry intact.

## Two compiler bugs this port found

Both are in OpenDE's UnrealScript compiler, both are worth fixing on their own, and the second
blocks compiling DeusEdix's own runtime package.

1. **Cross-package types are invisible.** The compiler never loads the game's packages, so a
   script cannot name a class of another package: `var Conversation con;` fails with
   `Unrecognized type 'Conversation'`. Fix: load the packages and instantiate all their exports
   before compiling, the way the compiler's own `OBJ LOAD` does.

2. **A class referenced before it is parsed leaves a placeholder.** The placeholder has no
   default object, and writing the package dereferences it: the compiler crashes.

Together they stop DeusEdix's `runtime/DeusEdix` package (87 classes) from compiling at all.
A third, separate matter is that some of those classes use `AttemptPickup`, which is a Deus Ex:
*Revision* function and is not in the original 1112f build.

## Licensing

DeusEdix is GPLv3. OpenDE is MIT.

They must **not** be merged into one binary: OpenDE's MIT grant would then be contradicted by the
GPLv3 of the combined work. They stay separate, and they talk over the plain C interface and the
file protocol — which is exactly where a boundary belongs. The native bridge is a separate
program that DeusEdix starts, like the Windows one.

## The work

1. **Fix the two compiler bugs** in OpenDE. Small, and they unblock everything else.
2. **The host**: a program that reads commands on stdin and answers in the protocol's format.
   It is the existing `UCMapKitHost.cpp`, which is portable apart from its job object and its
   `LoadLibraryExW`; both have POSIX equivalents.
3. **The bridge**: the same `UCBridgeApi`, implemented against OpenDE instead of `Editor.dll`.
   Most of it is the dump functions.
4. **The Python layer**: the host's name and extension, `creationflags`, the process handle used
   by the CPU preference, the Windows process and window control in `mapkit/game_process.py`, and
   the launcher, which on macOS can open the page in the browser instead of a WebView2 window.
5. **`DeusEdixNav`**: a small DLL the *game* loads, linked against the game's `Core.dll` and so
   x86-only. Its route finding has to move to OpenDE's own path builder.

Every bridge function is checked against the Windows bridge running under Wine, comparing the
files they write byte for byte.
