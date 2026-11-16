# PyInstaller: DeusEdix.exe (the window) and deusedix-cli.exe (its console twin) sharing one folder.
#   .venv\Scripts\pyinstaller launcher\DeusEdix.spec --distpath build\pyi --workpath build\pyi-work --noconfirm
# The editor's code is not frozen: it stays as files next to the program (dev\stage.py), so the program
# carries every module that code imports, found here by reading it.
#
# This is the Windows packaging (one folder with the two .exe files, the icon and the version resources). It
# builds on macOS and Linux too, with the window library of the platform rather than WebView2: there the
# launcher is normally run from the repository (python3 launcher/deusedix.py), which needs no packaging at
# all, and a .app BUNDLE (macOS's) is not made here. Nothing of the Windows build changes on Windows.
import ast
import sys
from pathlib import Path

HERE = Path(SPECPATH).resolve().parent
CODE = [HERE / 'mapkit', HERE / 'ue1pkg.py']
local = {p.stem for root in CODE for p in ([root] if root.is_file() else root.rglob('*.py'))}
wanted = set()
for root in CODE:
    for f in ([root] if root.is_file() else root.rglob('*.py')):
        for node in ast.walk(ast.parse(f.read_text(encoding='utf-8', errors='replace'))):
            if isinstance(node, ast.Import):
                wanted |= {a.name for a in node.names}
            elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
                wanted.add(node.module)
wanted = sorted(m for m in wanted if m.split('.')[0] not in local and m.split('.')[0] not in ('mcp',))
print('modules for the editor:', ', '.join(wanted))

WINDOWS = sys.platform == 'win32'
# pywebview's modules for the platform: .NET and WebView2 on Windows, its Cocoa (pyobjc) backend on macOS,
# GTK on Linux. Only the ones the platform really uses, so that a missing one elsewhere is no error.
WINDOW = (['webview', 'webview.platforms.edgechromium', 'webview.platforms.winforms', 'clr_loader', 'pythonnet']
          if WINDOWS else
          ['webview', 'webview.platforms.cocoa'] if sys.platform == 'darwin' else
          ['webview', 'webview.platforms.gtk'])

a = Analysis([str(HERE / 'launcher' / 'deusedix.py')], pathex=[], binaries=[], datas=[],
             hiddenimports=wanted + ['PIL.Image', 'PIL.ImageDraw', 'PIL.ImageFont', 'PIL.ImageFilter', 'PIL.ImageOps',
                                     'PIL.PngImagePlugin', 'PIL.JpegImagePlugin', 'PIL.BmpImagePlugin',
                                     'PIL.PcxImagePlugin', 'PIL.TgaImagePlugin', 'PIL.GifImagePlugin',
                                     *WINDOW, 'runpy', 'importlib', 'socket'],
             excludes=['tkinter', 'matplotlib', 'numpy', 'scipy', 'pandas', 'IPython', 'pytest'], noarchive=False)
pyz = PYZ(a.pure)
# deusedix.ico is the Windows program's icon (its version resources too); elsewhere it is none (macOS takes an
# .icns, which this build does not make).
icon = str(HERE / 'launcher' / 'deusedix.ico') if WINDOWS else None


def info(name):
    """The EXE's name, description and version (written by dev\\build_installer.py)."""
    f = HERE / 'build' / ('version_%s.txt' % name)
    return str(f) if f.exists() else None


gui = EXE(pyz, a.scripts, [], exclude_binaries=True, name='DeusEdix', console=False, icon=icon,
          version=info('DeusEdix') if WINDOWS else None, upx=False)
cli = EXE(pyz, a.scripts, [], exclude_binaries=True, name='deusedix-cli', console=True, icon=icon,
          version=info('deusedix-cli') if WINDOWS else None, upx=False)
coll = COLLECT(gui, cli, a.binaries, a.datas, name='DeusEdix', upx=False)
