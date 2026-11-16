"""The program's folder as it is installed (build/app), from this repository: the editor's code laid out
the way it expects (tools/mapkit, tools/ue1pkg.py), DeusEdix's UnrealScript (src/DeusEdix) and the compiled host
with its engine bridge. The launcher (PyInstaller) and the installer (Inno Setup) are built around it on Windows;
on macOS and Linux it is the folder the editor is run from.

    python dev/stage.py [--host path/to/UCMapKitHost] [--bridge path/to/UCOpenBridge.dylib] [--game /path/to/DeusEx]

The host and its bridge are the build's own files: mapkit/host/build_host.ps1 writes them into mapkit/bin on
Windows, and elsewhere CMake writes both into build/host/bin:

    cmake -S mapkit/host -B build/host -DCMAKE_BUILD_TYPE=Release -DOPENDE_ROOT=/path/to/OpenDE
    cmake --build build/host
    python3 dev/stage.py --clean

--game writes a deusedix.json (what the installer writes) so the staged folder can be tried as it is.
Everything the editor makes at run time (caches, runs, the workspace, built maps) is made next to
the code, in the user's install folder, never here.
"""
import argparse
import json
import os
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / 'mapkit'))
from experimental_payload import distribution_files
APP = HERE / 'build' / 'app'
WINDOWS = os.name == 'nt'

# The host: mapkit\host\build_host.ps1 on Windows (32 bit, with Deus Ex's engine), mapkit/host/CMakeLists.txt
# elsewhere (standard C++, with one of the two bridges).
HOST_NAME = 'UCMapKitHost.exe' if WINDOWS else 'UCMapKitHost'
DEFAULT_HOST = ((HERE / 'mapkit' / 'bin' / HOST_NAME) if WINDOWS
                else (HERE / 'build' / 'host' / 'bin' / HOST_NAME))
HOST_HINT = ('Build it: powershell -File mapkit\\host\\build_host.ps1' if WINDOWS else
             'Build both: cmake -S mapkit/host -B build/host -DCMAKE_BUILD_TYPE=Release '
             '-DOPENDE_ROOT=<the OpenDE tree>, then cmake --build build/host')

# The bridges. On Windows the one next to the host is Deus Ex's engine, with its settings and the game's own
# native packages. Elsewhere it is the OpenDE bridge, built by openbridge/CMakeLists.txt.
WINDOWS_BRIDGE = ('UCEngineBridge.dll', 'UCEngineBridge.ini', 'DeusEdixNav.u', 'DeusEdixNav.dll',
                  'DeusEdixGeometry.dll')
BRIDGE_LIBRARY = 'UCOpenBridge.dylib' if sys.platform == 'darwin' else 'UCOpenBridge.so'
# What the host looks for beside itself when it is given no --bridge (UC_BRIDGE_FILE in platform.h).
HOST_BRIDGE_NAME = 'UCEngineBridge.dylib' if sys.platform == 'darwin' else 'UCEngineBridge.so'
BRIDGE_HINT = ('Build it: cmake -S mapkit/host -B build/host -DCMAKE_BUILD_TYPE=Release '
               '-DOPENDE_ROOT=<the OpenDE tree>, then cmake --build build/host')

STATIC_EXT = {'.js', '.css', '.html', '.json', '.png', '.jpg', '.jpeg', '.svg', '.ico',
              '.woff', '.woff2', '.ttf', '.otf', '.mp3', '.ogg', '.wav'}
TUTORIAL_EXT = {'.py', '.json', '.png', '.jpg', '.jpeg', '.wav', '.ogg', '.mp3', '.md'}
GENERATED = {'UCPortraits.uc', 'UCVoiceDubs.uc'}
USER_PATHS = ('Projects', 'DevInstall', 'maps', 'webview', 'deusedix.json', 'DeusEdix.exe',
              'deusedix-project.json', '.deusedix-owned.json', '.deusedix-editor-base.zip', '.deusedix-updates', '.deusedix-ui.json', '.deusedix-project-ui.json',
              *('tools/mapkit/' + name for name in
                ('maps', 'story', 'sounds', 'dubs', 'models', 'textures', 'characters', 'prefabs', 'anims')))


def no_links(path):
    for part in (path, *path.parents):
        if part.exists() or part.is_symlink():
            if part.is_symlink() or getattr(part.stat(), 'st_file_attributes', 0) & 1024:
                raise SystemExit('Staging paths cannot contain links or junctions: ' + str(part))


def clean_output(out):
    no_links(out)
    out = out.resolve()
    source = HERE.resolve()
    if out == source or out in source.parents or any(
            part == out or part in out.parents for part in
            (source / 'mapkit', source / 'runtime', source / 'launcher', source / 'dev', source / 'installer')):
        raise SystemExit('Not a staging output directory: ' + str(out))
    if not out.exists():
        return
    found = [name for name in USER_PATHS if (out / name).exists()]
    if found:
        raise SystemExit('%s contains user/runtime data; --clean will not remove it: %s' % (out, ', '.join(found)))
    if any(out.iterdir()) and not all((out / name).exists() for name in
                                   ('NOTICE.md', 'tools/mapkit/editor/server.py', 'src/DeusEdix/Classes/UCBoot.uc')):
        raise SystemExit('Refusing to clean an unrecognized non-empty staging directory: ' + str(out))
    shutil.rmtree(out)


def copy_code(source, kit):
    # Copy application files explicitly. A developer's Projects, map edits,
    # recordings and caches must never become part of the installer.
    kit.mkdir(parents=True, exist_ok=True)
    for file in source.glob('*.py'):
        if file.name not in {'mcp_server.py', 'sheet.py'}:
            shutil.copy2(file, kit / file.name)
    editor = kit / 'editor'
    editor.mkdir(parents=True, exist_ok=True)
    for file in (source / 'editor').glob('*.py'):
        shutil.copy2(file, editor / file.name)
    (editor / 'config').mkdir(exist_ok=True)
    for file in (source / 'editor' / 'config').glob('*.json'):  # the editor's settings as data (palette.json...)
        shutil.copy2(file, editor / 'config' / file.name)
    # the library of logic patterns and the example maps (projects.py copies them into every project; 0.1.64 left
    # them out, so an installed editor had no patterns)
    for sub, pattern in (('patterns', '*.json'), ('examples', '*')):
        for file in (source / 'editor' / 'config' / sub).glob(pattern):
            if file.is_file() and file.suffix.lower() in ('.json', '.py'):
                (editor / 'config' / sub).mkdir(parents=True, exist_ok=True)
                shutil.copy2(file, editor / 'config' / sub / file.name)
    for folder, extensions in (('editor/static', STATIC_EXT), ('tutorial', TUTORIAL_EXT)):
        for file in (source / folder).rglob('*'):
            if file.is_file() and file.suffix.lower() in extensions and '__pycache__' not in file.parts:
                target = kit / file.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(file, target)
    copy_worker(kit)


def copy_worker(kit):
    """The optional worker has its own native host. Never recursively copy the directory: the running tool keeps
    private map copies and evidence there.

    That host is a Windows build (experimental_bsp/distribution.json lists bin/UCMapKitHost.exe, the 32-bit host),
    so a checkout elsewhere has none: the tool is left out there rather than failing the staging. On Windows a
    checkout without it stays an error, as it always was."""
    payload = HERE / 'mapkit' / 'experimental_bsp'
    if not WINDOWS and not (payload / 'bin' / 'UCMapKitHost.exe').is_file():
        print('experimental_bsp: skipped (its own native host is the Windows build bin/UCMapKitHost.exe)')
        return
    for file, relative in distribution_files(payload, required=True):
        target = kit / 'experimental_bsp' / Path(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(file, target)


def copy_native(host, bin_dir, bridge=None):
    """The host's engine bridge beside it, where the host looks for it (platform.h), with the name the platform's
    builds have."""
    if WINDOWS:
        # Deus Ex's own engine, its settings (navigation, the WinDrvLite fix) and the game's native packages
        for name in WINDOWS_BRIDGE:
            shutil.copy2(host.with_name(name), bin_dir / name)
        return
    bridge = (bridge or host.with_name(BRIDGE_LIBRARY)).absolute()
    if not bridge.is_file():
        raise SystemExit('The bridge is not built: %s\n%s' % (bridge, BRIDGE_HINT))
    shutil.copy2(bridge, bin_dir / BRIDGE_LIBRARY)
    # A host given no --bridge looks for UCEngineBridge.<ext> (UC_BRIDGE_FILE in platform.h); the native bridge is
    # UCOpenBridge.<ext>. Both names hold it, so a staged run finds it whether or not the editor names it.
    if HOST_BRIDGE_NAME != BRIDGE_LIBRARY and not (bin_dir / HOST_BRIDGE_NAME).exists():
        shutil.copy2(bridge, bin_dir / HOST_BRIDGE_NAME)


def copy_runtime(out):
    runtime = HERE / 'runtime' / 'DeusEdix'
    src = out / 'src' / 'DeusEdix'
    template = out / 'tools' / 'mapkit' / 'project-template' / 'src' / 'DeusEdix'
    no_links(src)
    no_links(template)
    # This is application-owned clean source, never a project or its generated
    # portraits, recordings or campaign classes.
    if template.exists():
        shutil.rmtree(template)
    for root in (src, template):
        (root / 'Classes').mkdir(parents=True, exist_ok=True)
        (root / 'Textures').mkdir(parents=True, exist_ok=True)
    for file in (src / 'Classes').glob('*.uc'):
        if not file.name.startswith('UCEd_') and file.name not in GENERATED:
            file.unlink()
    for file in (runtime / 'Classes').glob('*.uc'):
        if file.name.startswith('UCEd_'):
            continue
        clean = HERE / 'runtime' / 'extra' / file.name if file.name in GENERATED else file
        if not clean.is_file():
            raise SystemExit('Missing clean runtime seed: ' + str(clean))
        shutil.copy2(clean, template / 'Classes' / file.name)
        if file.name not in GENERATED or not (src / 'Classes' / file.name).exists():
            shutil.copy2(clean, src / 'Classes' / file.name)
    for file in (runtime / 'Textures').glob('*.pcx'):
        for root in (src, template):
            shutil.copy2(file, root / 'Textures' / file.name)
    for voice in (HERE / 'mapkit/tutorial/voices').glob('V*.wav'):
        for root in (src, template):
            target = root / 'Sounds/Doctor' / voice.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(voice, target)
    # the classes of a project made for the original Deus Ex (dev/vanilla_runtime.py makes them from the above)
    vanilla = out / 'tools' / 'mapkit' / 'project-template' / 'vanilla' / 'Classes'
    no_links(vanilla)
    if vanilla.parent.exists():
        shutil.rmtree(vanilla.parent)
    vanilla.mkdir(parents=True)
    for file in (HERE / 'runtime' / 'vanilla' / 'Classes').glob('*.uc'):
        shutil.copy2(file, vanilla / file.name)
    # DeusEdixNav (the routes' class): on Windows its built .u is in bin, made against the DLL; on macOS and Linux
    # there is no DLL, so the editor compiles it from this source with the editor's package (mapkit/native's build
    # scripts are not shipped there)
    if not WINDOWS:
        nav = out / 'src' / 'DeusEdixNav' / 'Classes'
        no_links(nav)
        shutil.rmtree(nav.parent, ignore_errors=True)
        nav.mkdir(parents=True)
        for file in (HERE / 'mapkit' / 'native' / 'DeusEdixNav' / 'Classes').glob('*.uc'):
            shutil.copy2(file, nav / file.name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', type=Path, default=DEFAULT_HOST)
    ap.add_argument('--bridge', type=Path, help='the bridge beside the host (macOS/Linux: UCOpenBridge.dylib/.so)')
    ap.add_argument('--game', type=Path)
    ap.add_argument('--out', type=Path, default=APP)
    ap.add_argument('--clean', action='store_true', help='start from an empty folder (a release: nothing of a test run)')
    a = ap.parse_args()
    if a.bridge and WINDOWS:
        raise SystemExit('--bridge is for the native bridge (macOS/Linux): on Windows the host and its engine bridge '
                         'are built together by mapkit\\host\\build_host.ps1')
    host = a.host.absolute()
    if not host.is_file():
        raise SystemExit('The host is not built: %s\n%s' % (host, HOST_HINT))
    out = a.out.absolute()
    no_links(out)
    if a.clean:
        clean_output(out)
    kit = out / 'tools' / 'mapkit'
    # code: replaced as a whole (what the user made next to it stays: caches, runs, workspace...)
    for sub in ('editor/static', 'host'):
        no_links(kit / sub)
        shutil.rmtree(kit / sub, ignore_errors=True)
    for f in kit.glob('*.py') if kit.exists() else []:
        f.unlink()
    for f in (kit / 'editor').glob('*.py') if (kit / 'editor').exists() else []:
        f.unlink()
    copy_code(HERE / 'mapkit', kit)
    shutil.rmtree(kit / 'host', ignore_errors=True)               # the source of the host is not needed to run
    shutil.rmtree(kit / 'native', ignore_errors=True)             # nor DeusEdixNav's: its built files are in bin
    bin_dir = kit / 'bin'
    bin_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(host, bin_dir / HOST_NAME)
    copy_native(host, bin_dir, a.bridge)
    shutil.copy2(HERE / 'ue1pkg.py', out / 'tools' / 'ue1pkg.py')
    copy_runtime(out)
    for name in ('README.md', 'NOTICE.md'):
        shutil.copy2(HERE / name, out / name)
    # the guide (dev/make_guide.py): Help > Open the full guide in the editor shows it
    (out / 'docs' / 'guide').mkdir(parents=True, exist_ok=True)
    shutil.copy2(HERE / 'docs' / 'guide' / 'guide.md', out / 'docs' / 'guide' / 'guide.md')
    shutil.copy2(HERE / 'docs' / 'guide' / 'guide.html', out / 'docs' / 'guide' / 'guide.html')
    if a.game:
        config = out / 'deusedix.json'
        cfg = json.loads(config.read_text(encoding='utf-8')) if config.exists() else {}
        cfg.update(game=str(a.game), package='DeusEdix')
        config.write_text(json.dumps(cfg, indent=1), encoding='utf-8')
    n = sum(1 for _ in out.rglob('*') if _.is_file())
    print('staged %d files in %s' % (n, out))


if __name__ == '__main__':
    main()
