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

### What Phase 1 found

Both bugs are fixed in OpenDE (`jmaccs/OpenDE`, branch `feature/native-arm64-bridge`):

- `ucc make` now loads the game's packages, with every export instantiated, before compiling.
  A package that is about to be built from its sources is left out, so it is still built.
- A class named before its own script is parsed no longer leaves a broken placeholder. The
  placeholder no longer points back at the class that named it, the class is fully reset when
  its own script arrives, and a class that is never defined is reported as an unrecognized type
  instead of crashing the compiler.

With those, DeusEdix's runtime package compiles against the original 1112f game except for
**four places that only exist in Revision**:

| What | Where | What it is |
| --- | --- | --- |
| `RevRootWindow` | `UCMenuRoot` extends it | Revision's root window |
| `RevMenuScreenNewGame` | `UCNewGame` extends it | Revision's new-game screen |
| `AttemptPickup` | `UCCustomCarcass` calls it on Super | not in the original 1112f |
| `SpecialText` | `UCBoot` reads it | "Revision shows SpecialText when..." |

The first two are base classes, so they cannot simply be stubbed: `UCMenuStage` casts to
`UCMenuRoot`, and the compiler then reports the cast as one that can never succeed. For the
original game these two classes have to be given a base of their own, or the editor's menu has
to be written against the original's windows.

## Licensing

DeusEdix is GPLv3. OpenDE is MIT.

They must **not** be merged into one binary: OpenDE's MIT grant would then be contradicted by the
GPLv3 of the combined work. They stay separate, and they talk over the plain C interface and the
file protocol — which is exactly where a boundary belongs. The native bridge is a separate
program that DeusEdix starts, like the Windows one.

## The work

1. **Fix the two compiler bugs** in OpenDE. Small, and they unblock everything else. **DONE.**
2. **The host**: a program that reads commands on stdin and answers in the protocol's format.
   It is the existing `UCMapKitHost.cpp`, which is portable apart from its job object and its
   `LoadLibraryExW`; both have POSIX equivalents. **DONE.**
3. **The bridge**: the same `UCBridgeApi`, implemented against OpenDE instead of `Editor.dll`.
   Most of it is the dump functions. **DONE** — `mapkit/host/openbridge/`.
4. **The Python layer**: the host's name and extension, `creationflags`, the process handle used
   by the CPU preference, the Windows process and window control in `mapkit/game_process.py`, and
   the launcher, which on macOS can open the page in the browser instead of a WebView2 window.
5. **`DeusEdixNav`**: a small DLL the *game* loads, linked against the game's `Core.dll` and so
   x86-only. Its route finding has to move to OpenDE's own path builder.

Every bridge function is checked against the Windows bridge running under Wine, comparing the
files they write byte for byte.

### The bridge, on OpenDE

`mapkit/host/openbridge/` is a second bridge, with the same `UCBridgeApi`, that runs OpenDE's
engine in the host's process. It never opens a window and never starts a renderer: it uses the
engine's own packages, brush builder, BSP builder, lightmap builder, path builder and map writer
directly. The UnrealEd command lines the editor sends are understood, so `MAP LOAD`, `MAP REBUILD`,
`LIGHT APPLY`, `PATHS DEFINE`, `MAP SAVE` and the rest work as they do on Windows.

All forty-seven functions are implemented. On a map of the game the numbers are the editor's own:
the same actors, the same brushes, nodes, surfaces and zones, the same lights, and a map saved
and loaded again gives back the same geometry, byte for byte.

Two are not exactly what the SDK bridge does, and neither is faked:

- `meshImport` ignores the old vertex format argument (OpenDE's importer reads Deus Ex's own
  format only), and OpenDE's picture importer does not turn a picture the right way up.
- `texFlags` derives the flags from the texture's booleans, as OpenDE's renderer does, because
  Deus Ex textures have no flags of their own.

### The host, ported

`mapkit/host/UCMapKitHost.cpp` was Windows-only: the bridge was a DLL loaded with
`LoadLibraryExW`, a crash in the engine was caught with SEH, the process was put in a job object,
and the paths and the clock came from `windows.h`. All of it is in `mapkit/host/platform.h` now,
with the Windows calls behind `_WIN32` unchanged.

On POSIX the bridge is a shared library opened with `dlopen`, a crash is caught with a signal
handler that reports the faulting image and offset (`UCMapKit|av|<module>+0x<offset>|read 0x...`,
the same shape as Windows) and lets the session go on, and the process is put in a session of its
own with `setsid` and its limits set.

The protocol, the command modes, the exit codes and `bridge.h` are unchanged, so the editor does
not change.

Two things are not a faithful port, and cannot be:

- The Windows handler reports *every* first-chance access violation, including the ones the engine
  swallows; POSIX has no such thing as a first-chance signal, so only the fatal fault is reported.
  `UCMapKit|fault|0x...` carries the signal number instead of an NTSTATUS.
- The clipboard, desktop and display restrictions of the Windows job have no equivalent without a
  graphical system. The process limits that do exist are set instead.

### The runtime package, and the original game

DeusEdix's runtime is written for Deus Ex: *Revision*, and a few of its classes use what only
Revision has. `runtime/vanilla/Classes` already holds the versions for the original game, made
from the Revision ones by `dev/vanilla_runtime.py`, and they are the ones to use: with them the
whole package of 87 classes compiles against the original 1112f build.

Two more things were needed for that, both in OpenDE's compiler:

- The packages the ini lists are loaded before compiling, not a fixed handful: a script naming a
  class of `DeusExUI` (the fonts) failed with "Can't find Font" otherwise.
- `DeusEdixNav` (the native DLL the game loads) is a package of its own, and its class has to be
  there before `UCNavMesh` can name it.

