"""The Deus Ex SDK 1112f's C++ headers, and the import libraries of your own game's DLLs, into build_tools/sdk.

Both are for the **Windows** engine bridge only: mapkit/host/UCEngineBridge.cpp is compiled against the SDK's own
Editor.dll (mapkit/host/build_host.ps1 reads them). The native bridge (mapkit/host/openbridge, built with CMake) is
compiled against OpenDE's headers and linked against OpenDE's libraries, so it needs neither.

- the headers come from the official Deus Ex SDK 1112f (Headers\\DxHeaders.zip), which you download yourself;
- the import libraries (Core/Engine/Editor/Window/Extension/ConSys.lib) are made from the exports of your own game's
  DLLs, with Visual Studio's lib.exe.

    python dev\\prepare_sdk.py --headers C:\\path\\to\\DxHeaders.zip --game "C:\\...\\steamapps\\common\\Deus Ex"
    python3 dev/prepare_sdk.py --headers /path/to/DxHeaders.zip --out build_tools/sdk     (the headers alone)

On a platform that is not Windows there is nothing to make the import libraries with (lib.exe is Visual Studio's)
and no Windows bridge to make them for: this says so, extracts the headers if it was given them, and stops without
failing. Nothing of it is in the repository.
"""
import argparse
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
PACKAGES = ('Core', 'Engine', 'Editor', 'Window', 'Extension', 'ConSys')


def extract_headers(archive_path, out):
    root = out / 'headers'
    root.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive_path) as archive:
        for entry in archive.infolist():
            target = (root / entry.filename).resolve()
            if not target.is_relative_to(root.resolve()):
                raise SystemExit('Unsafe path in the SDK archive: ' + entry.filename)
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(entry))
    # a syntax-only change for a modern MSVC parser
    template = root / 'Core' / 'Inc' / 'UnTemplate.h'
    text = template.read_text(encoding='latin1')
    text = re.sub(r'(?<!typename )TTypeInfo<([^>]+)>::ConstInitType', r'typename TTypeInfo<\1>::ConstInitType', text)
    template.write_text(text, encoding='latin1')
    # the three loops that use their variable after the loop (old for-scope rules): the variable is declared
    # before, so code including the headers builds without the deprecated /Zc:forScope-
    for name, old, new in FOR_SCOPE:
        path = root / name
        path.write_text(path.read_text(encoding='latin1').replace(old, new), encoding='latin1', newline='')


FOR_SCOPE = [
    ('Engine/Inc/UnActor.h', '\tfor( AActor* Top=this; Top->Owner; Top=Top->Owner );\n',
     '\tAActor* Top;\n\tfor( Top=this; Top->Owner; Top=Top->Owner );\n'),
    ('Core/Inc/FFileManagerGeneric.h', '\t\tfor( INT i=0; i<List.Num(); i++ )\n\t\t\tif( !Delete(',
     '\t\tINT i;\n\t\tfor( i=0; i<List.Num(); i++ )\n\t\t\tif( !Delete('),
    ('Core/Inc/FOutputDeviceFile.h', '\t\t\t\t\tfor( INT i=0; Ch[i]; i++ )\n',
     '\t\t\t\t\tINT i;\n\t\t\t\t\tfor( i=0; Ch[i]; i++ )\n'),
]


def exports(dll):
    """The names a DLL exports, ' DATA' after the ones that are not code."""
    raw = dll.read_bytes()
    pe = struct.unpack_from('<I', raw, 60)[0]
    count = struct.unpack_from('<H', raw, pe + 6)[0]
    optional = struct.unpack_from('<H', raw, pe + 20)[0]
    sections = [struct.unpack_from('<8s8I', raw, pe + 24 + optional + 40 * i) for i in range(count)]

    def section(rva):
        return next(s for s in sections if s[2] <= rva < s[2] + max(s[1], s[3]))

    def offset(rva):
        s = section(rva)
        return s[4] + rva - s[2]

    directory = offset(struct.unpack_from('<I', raw, pe + 24 + 96)[0])
    _, _, _, _, _, _, _, names, functions_rva, names_rva, ordinals_rva = struct.unpack_from('<IIHH7I', raw, directory)
    out = []
    for i in range(names):
        name_at = offset(struct.unpack_from('<I', raw, offset(names_rva) + i * 4)[0])
        name = raw[name_at:raw.index(b'\0', name_at)].decode('ascii')
        ordinal = struct.unpack_from('<H', raw, offset(ordinals_rva) + i * 2)[0]
        address = struct.unpack_from('<I', raw, offset(functions_rva) + ordinal * 4)[0]
        data = not section(address)[8] & 0x20000000
        out.append(name + (' DATA' if data else ''))
    return out


def lib_exe():
    """Visual Studio's lib.exe, or None: a machine without Visual Studio still extracts the headers."""
    if sys.platform != 'win32':
        return None
    vswhere = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe')
    if not vswhere.exists():
        return None
    root = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property',
                                         'installationPath']).decode().strip())
    tools = root / 'VC' / 'Tools' / 'MSVC'
    if not tools.is_dir():
        return None
    return sorted(tools.iterdir())[-1] / 'bin' / 'Hostx64' / 'x86' / 'lib.exe'


def main():
    ap = argparse.ArgumentParser(description='The SDK headers and import libraries of the Windows engine bridge.')
    ap.add_argument('--headers', type=Path, help="the SDK's Headers\\DxHeaders.zip (the Windows bridge's headers)")
    ap.add_argument('--game', type=Path, help='the Deus Ex folder (the one with System\\): its DLLs make the libraries')
    ap.add_argument('--out', type=Path, default=HERE / 'build_tools' / 'sdk')
    ap.add_argument('--no-libs', action='store_true', help='the headers alone: no lib.exe and no game folder')
    a = ap.parse_args()
    out = a.out.resolve()
    windows = sys.platform == 'win32'
    if a.headers:
        extract_headers(a.headers, out)
        print('headers: %s' % (out / 'headers'))
    if a.no_libs:
        print('import libraries: skipped (--no-libs)')
        return
    # The import libraries are Visual Studio's, and only mapkit/host/UCEngineBridge.cpp (Windows, Deus Ex's own
    # engine) is linked against them: skip them with a reason instead of failing.
    if not windows:
        print('import libraries: skipped. This is not Windows: they are made with Visual Studio\'s lib.exe and are')
        print('only used by the Windows engine bridge (mapkit/host/UCEngineBridge.cpp). The native bridge')
        print('(mapkit/host/openbridge, OpenDE) needs no SDK at all:')
        print('  cmake -S mapkit/host -B build/host -DCMAKE_BUILD_TYPE=Release -DOPENDE_ROOT=<OpenDE>')
        return
    if not a.headers:
        ap.error('--headers is needed: the Windows engine bridge is compiled against the SDK headers')
    if not a.game:
        ap.error('--game is needed: the import libraries are made from the exports of the game\'s DLLs')
    lib = lib_exe()
    if lib is None:
        print('lib.exe was not found (Visual Studio or the Build Tools with the C++ tools are not installed): the')
        print('import libraries were skipped. Only the Windows engine bridge needs them (the SDK headers are in %s).'
              % (out / 'headers'))
        return
    for package in PACKAGES:
        dll = a.game / 'System' / (package + '.dll')
        if not dll.exists():
            raise SystemExit('Not found: ' + str(dll))
        definition = out / (package + '.def')
        definition.write_text('LIBRARY %s.dll\nEXPORTS\n%s\n' % (package, '\n'.join(exports(dll))), encoding='ascii')
        subprocess.run([str(lib), '/nologo', '/machine:x86', '/def:' + str(definition),
                        '/out:' + str(out / (package + '.lib'))], check=True, capture_output=True)
    print('headers and %d import libraries in %s' % (len(PACKAGES), out))


if __name__ == '__main__':
    main()
