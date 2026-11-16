"""Where the game is, and the names the editor gives to its own things in it.

The editor runs in two ways:
 - development mode, inside a mod's own working copy (no deusedix.json): the mod's package, the maps
   folder the mod adds to Revision and the mod's other packages, named in devmode.json next to the
   working copy (DEV below: a local file, never published; neutral names without it), the game where
   Steam put it;
 - as DeusEdix, installed: deusedix.json at the top of the install folder (the installer writes it)
   says so, with the game folder found or picked there. The package is then DeusEdix, compiled on
   the user's PC, and the maps go to Revision\\DeusEdixMaps.

The game folder is the one holding Deus Ex's System and Revision (Steam: steamapps\\common\\Deus Ex). On Windows
that is the game as it ships, its programs and all; on macOS and Linux it is the game's data, which OpenDE runs.

Two games can be the target, one per project (TARGET, fixed when the project is made): Deus Ex: Revision
('revision', in the game folder's Revision\\ with its own System, Maps, ini files) and the original Deus Ex,
Game of the Year Edition 1112fm ('vanilla', the game folder itself). Everything that depends on the game
comes from TARGETS below: GAME_ROOT is the targeted game's own folder (REVISION, its older name, is the
same folder, so for the original game it is the Deus Ex folder itself). A development working copy is
always Revision.
"""
import json
import os
import re
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

# The games, their programs and Steam's registry are Windows'; macOS and Linux read the same game as data (a folder
# of System, Maps, Textures...) started by a wrapper (here, OpenDE), so nothing of the game is a program there.
WINDOWS = os.name == 'nt'
MACOS = sys.platform == 'darwin'


def _project_folder(code):
    """The folder DeusEdix keeps its files in: its install folder, which holds the editor's code in tools/mapkit (the
    installer's layout: settings, the workspace, the caches and the user's work all next to it), or the editor's own
    folder when the code is checked out as itself (mapkit/ at the top, its usual development layout)."""
    parent = Path(code).resolve().parent
    return parent.parent if parent.name == 'tools' else parent


def build_bin():
    """Where the host and its engine bridge are: the editor's own folder in the install layout (the build puts them
    in tools/mapkit/bin, next to this code), or build/host/bin in a checkout (CMake writes them there). Which layout
    this is, is told by where the editor's own code is, not by a folder that happens to exist."""
    here = Path(__file__).resolve().parent
    if here.parent.name == 'tools':                  # the install layout: this code is in tools/mapkit
        return here / 'bin'
    return here.parent / 'build' / 'host' / 'bin'


def data_dir():
    """The platform's own folder for this user's DeusEdix data, for when the editor's folder is not one the user
    writes in (a system-wide install, a read-only package): %LOCALAPPDATA%\\DeusEdix on Windows,
    ~/Library/Application Support/DeusEdix on macOS, $XDG_DATA_HOME/DeusEdix (or ~/.local/share/DeusEdix) on Linux.
    DEUSEDIX_DATA_DIR names it instead, then DEUSEDIX_APP_ROOT (the launcher gives its workers this one): the editor
    must find the same folder for the whole of a run, so one of them is set before settings is imported."""
    given = os.environ.get('DEUSEDIX_DATA_DIR') or os.environ.get('DEUSEDIX_APP_ROOT')
    if given:
        return Path(given).expanduser().resolve()
    if WINDOWS:
        return (Path(os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData' / 'Local') / 'DeusEdix').resolve()
    if MACOS:
        return (Path.home() / 'Library' / 'Application Support' / 'DeusEdix').resolve()
    return (Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local' / 'share') / 'DeusEdix').resolve()


PROJECT = _project_folder(Path(__file__).resolve().parent)
CONFIG = PROJECT / 'deusedix.json'
APP_ROOT = Path(os.environ.get('DEUSEDIX_APP_ROOT') or PROJECT).resolve()
PROJECT_FILE = PROJECT / 'deusedix-project.json'
# where the workspace, the compiled packages and the logs go: next to the code, as the installer lays it out, or the
# platform's own place for this user's data when that folder is not writable (DATA_DIR is the same folder as PROJECT
# on Windows and in a checkout: the layout is unchanged there). DEUSEDIX_DATA_DIR names it outright
DATA_DIR = (Path(os.environ['DEUSEDIX_DATA_DIR']).expanduser().resolve() if os.environ.get('DEUSEDIX_DATA_DIR')
            else (PROJECT if os.access(PROJECT, os.W_OK) else data_dir()))
STEAM_DEFAULT = Path(r'C:\Program Files (x86)\Steam') if WINDOWS else None
APPS = {'6910': 'Deus Ex: Game of the Year Edition', '397550': 'Deus Ex: Revision'}
# the games a project can be made for: where each one lives in the Deus Ex folder (root), its program, its
# ini files and their sections with the folders the game reads (Paths), the default user ini beside its user ini that
# the engine makes a missing user ini from (def_user_ini: say the player deleted theirs; Revision.exe's %sDefUser.ini, the
# original Core.dll's DefUser.ini), its Steam app, whether it reads the copies of its ini files a launch of DeusEdix's
# starts it on from its System folder (launch_inis), and the words of its command line that start it in a window
# (windowed: the original game has none, its ini's StartupFullscreen=False alone does it)
TARGETS = {
    'revision': dict(name='Deus Ex: Revision', short='Revision', root='Revision', exe='Revision.exe',
                     ini='Revision.ini', user_ini='RevisionUser.ini', def_user_ini='RevisionDefUser.ini', app='397550',
                     sections=('Core.System', 'RevisionInternal.LaunchSystem'), launch_in_system=True,
                     windowed=('-windowed',)),
    'vanilla': dict(name='Deus Ex (original game)', short='Deus Ex', root='', exe='DeusEx.exe', ini='DeusEx.ini',
                    user_ini='User.ini', def_user_ini='DefUser.ini', app='6910', sections=('Core.System',),
                    launch_in_system=False, windowed=()),
}
DEFAULT_TARGET = 'revision'
# development mode's names (devmode.json): the mod's display name, its package, the maps folder it adds to the
# game, its voices, the Studio's animations, the imported models, sounds and scripts, the web address of its
# repository's files (links of its guide; '' = none)
DEV_DEFAULTS = dict(name='My mod', package='MyMod', mapsDir='MyModMaps', voicesPackage='MyModVoices',
                    animsPackage='MyModAnims', modelsPackage='MyModModels', soundsPackage='MyModSounds',
                    scriptsPackage='MyModScripts', repo='')


def _config():
    try:
        return json.loads(CONFIG.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def devmode(folder=None):
    """The development names of a working copy (`folder`, this one by default): its devmode.json over
    DEV_DEFAULTS. This one's falls back to the app's (APP_ROOT), so that the installed editor and its
    projects know the names of a mod made with it on the same PC (its lines in the game's ini)."""
    for root in [folder] if folder else [PROJECT, APP_ROOT]:
        try:
            value = json.loads((Path(root) / 'devmode.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        if isinstance(value, dict):
            return DEV_DEFAULTS | {k: v for k, v in value.items() if k in DEV_DEFAULTS and isinstance(v, str)}
    return dict(DEV_DEFAULTS)


def _project():
    if not PROJECT_FILE.exists():
        return None
    try:
        value = json.loads(PROJECT_FILE.read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise ValueError('Cannot read this project manifest: %s' % PROJECT_FILE) from exc
    ident = value.get('id', '') if isinstance(value, dict) else ''
    if (not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,47}', ident)
            or value.get('version') != 1 or value.get('kind') not in ('custom', 'original', 'tutorial')
            or not isinstance(value.get('name'), str) or not value['name'].strip()):
        raise ValueError('Invalid DeusEdix project manifest: %s' % PROJECT_FILE)
    package = 'DXP_' + ident.replace('-', '_')
    if value.get('package') != package:
        raise ValueError('The project package does not match its immutable ID.')
    if value.get('target', DEFAULT_TARGET) not in TARGETS:
        raise ValueError('Unknown game in the project manifest: %s' % PROJECT_FILE)
    # "target" only for the original game: absent means Revision (as in projects.py)
    return {key: value[key] for key in ('id', 'name', 'kind', 'package', 'target') if key in value} | {'path': str(PROJECT), 'legacy': False}


def _steam_roots():
    """The Steam folders of this PC: the ones its installer writes in the registry on Windows, the usual ones on
    macOS (~/Library/Application Support/Steam) and Linux (~/.steam/steam, the Flatpak's data folder...)."""
    if not WINDOWS:
        home = Path.home()
        return [home / 'Library' / 'Application Support' / 'Steam',
                home / '.steam' / 'steam',
                home / '.local' / 'share' / 'Steam',
                home / '.var' / 'app' / 'com.valvesoftware.Steam' / 'data' / 'Steam']
    roots = []
    try:
        import winreg
        for hive, key, value in ((winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam', 'SteamPath'),
                                 (winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath'),
                                 (winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Valve\Steam', 'InstallPath')):
            try:
                with winreg.OpenKey(hive, key) as k:
                    roots.append(Path(winreg.QueryValueEx(k, value)[0]))
            except OSError:
                pass
    except ImportError:
        pass
    roots.append(STEAM_DEFAULT)
    return roots


def _library(path):
    """A library folder as this system names folders: the vdf files write Windows paths with doubled backslashes on
    Windows (where '/' would work too), and POSIX paths elsewhere."""
    text = str(path)
    return Path(text.replace('\\\\', '\\').replace('/', '\\')) if WINDOWS else Path(text)


def steam_libraries():
    """The Steam library folders of this PC: the Steam folder from the registry (Windows) or the usual ones (macOS,
    Linux), then the ones in steamapps/libraryfolders.vdf."""
    out = []
    for root in _steam_roots():
        for lib in [root] + _vdf_paths(Path(root) / 'steamapps' / 'libraryfolders.vdf'):
            lib = _library(lib)
            if lib.is_dir() and all(lib.resolve() != o.resolve() for o in out):
                out.append(lib)
    return out


def _vdf_paths(vdf):
    try:
        text = vdf.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    return [Path(m) for m in re.findall(r'"path"\s+"([^"]+)"', text)]


# the packages a Deus Ex install's System folder holds (the game's own code; Revision's own in Revision\System): the
# game is its data on macOS and Linux, where the Windows programs it ships are not run as they are
GAME_PACKAGES = ('Core.u', 'DeusEx.u')
REVISION_PACKAGES = ('Revision.u', 'RevisionMission.u')


def _data_missing(folder, packages):
    """What a game's data folder lacks: its System with these packages, its Maps with a map in them ('' when nothing
    is missing)."""
    wanted = ['System/' + name for name in packages if not (folder / 'System' / name).is_file()]
    if wanted:
        return ', '.join(wanted)
    return '' if any((folder / 'Maps').glob('*.dx')) else 'Maps'


def check_game(folder, target=DEFAULT_TARGET):
    """What is missing for the editor in a game folder to make maps for `target`: [] when it is good.
    Both need the original game (its Editor.dll is the editor's engine, Revision reads some of its files). On Windows
    it is the game as it ships, its program and all. On macOS and Linux it is the game's data (System with its own
    packages, Maps): the Windows programs it ships are started through a wrapper, so their absence says nothing."""
    folder = Path(folder)
    missing = []
    if WINDOWS:
        if not (folder / 'System' / 'Editor.dll').is_file():
            missing.append('Deus Ex (System\\Editor.dll)')
        if target == 'vanilla':
            if not (folder / 'System' / 'DeusEx.exe').is_file():
                missing.append('Deus Ex (System\\DeusEx.exe)')
        elif not (folder / 'Revision' / 'System' / 'Revision.exe').is_file():
            missing.append('Deus Ex: Revision (Revision\\System\\Revision.exe)')
        return missing
    lacking = _data_missing(folder, GAME_PACKAGES)
    if lacking:
        missing.append('Deus Ex (%s)' % lacking)
    if target == 'revision':
        lacking = _data_missing(folder / 'Revision', REVISION_PACKAGES)
        if lacking:
            missing.append('Deus Ex: Revision (Revision/%s)' % lacking)
    return missing


def game_installed(folder, target=DEFAULT_TARGET):
    """Is the game of `target` in this game folder? Its program on Windows, where the game is a program and the
    uninstaller always asked for it; its data on macOS and Linux (check_game), where there is no such program."""
    folder = Path(folder)
    if WINDOWS:
        profile = TARGETS[target]
        return ((folder / profile['root'] if profile['root'] else folder) / 'System' / profile['exe']).is_file()
    return not check_game(folder, target)


def games_in(folder):
    """The targets a game folder can be used for, Revision first: [] when it is not a Deus Ex folder."""
    return [t for t in TARGETS if folder and not check_game(folder, t)]


# the original game from GOG (GOG Galaxy or its offline installer): no Steam, its DeusEx.exe starts by itself. GOG
# installs for Windows; macOS and Linux have the game as the data folder given below (default_game)
GOG_FOLDERS = ((r'C:\GOG Games\Deus Ex GOTY', r'C:\GOG Games\Deus Ex',
                r'C:\Program Files (x86)\GOG Galaxy\Games\Deus Ex GOTY', r'C:\Program Files (x86)\GOG Galaxy\Games\Deus Ex',
                r'C:\Program Files (x86)\GOG.com\Deus Ex GOTY', r'C:\Program Files (x86)\GOG.com\Deus Ex')
               if WINDOWS else ())


def gog_games():
    """Deus Ex folders installed by GOG: the ones in GOG's registry (named Deus Ex), then the usual folders."""
    if not WINDOWS:
        return []                       # GOG ships the game for Windows; elsewhere it is the data folder, not an install
    out = []
    try:
        import winreg
        for root in (r'SOFTWARE\WOW6432Node\GOG.com\Games', r'SOFTWARE\GOG.com\Games'):
            try:
                games = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, root)
            except OSError:
                continue
            with games:
                for i in range(winreg.QueryInfoKey(games)[0]):
                    try:
                        with winreg.OpenKey(games, winreg.EnumKey(games, i)) as game:
                            name = str(winreg.QueryValueEx(game, 'gameName')[0])
                            path = Path(winreg.QueryValueEx(game, 'path')[0])
                    except OSError:
                        continue
                    if 'deus ex' in name.lower():
                        out.append(path)
    except ImportError:
        pass
    out += [Path(p) for p in GOG_FOLDERS]
    return [p for i, p in enumerate(out) if p.is_dir() and all(str(p).lower() != str(o).lower() for o in out[:i])]


def default_game():
    """Where the game (its data, where the game is not a program) may be before it is looked for: Steam's usual folder
    on Windows; on macOS and Linux the folder OpenDE is given the game in (README: DEUSEX_GAME_DATA), else a
    game-data folder next to the editor or next to the editor's folder."""
    if WINDOWS:
        return Path(r'C:\Program Files (x86)\Steam\steamapps\common\Deus Ex')
    given = os.environ.get('DEUSEX_GAME_DATA')
    if given:
        return Path(given).expanduser().resolve()
    for base in (APP_ROOT, PROJECT, PROJECT.parent):
        folder = Path(base) / 'game-data'
        if (folder / 'System').is_dir():
            return folder.resolve()
    return (APP_ROOT / 'game-data').resolve()


def find_game(target=DEFAULT_TARGET):
    """The Deus Ex folder good for `target` (with Revision in it, for Revision), from the folder it is given in on
    macOS and Linux (default_game), Steam's manifests (apps 6910 and 397550 both install into steamapps\\common\\
    Deus Ex), then GOG's (the original game), or None."""
    if not WINDOWS:
        given = default_game()
        if not check_game(given, target):
            return given
    for lib in steam_libraries():
        for app in APPS:
            manifest = lib / 'steamapps' / ('appmanifest_%s.acf' % app)
            try:
                m = re.search(r'"installdir"\s+"([^"]+)"', manifest.read_text(encoding='utf-8', errors='replace'))
            except OSError:
                continue
            folder = lib / 'steamapps' / 'common' / m.group(1) if m else None
            if folder and not check_game(folder, target):
                return folder
    for folder in gog_games():
        if not check_game(folder, target):
            return folder
    return None


def from_steam(folder):
    """True when Steam installed this game folder (one of its libraries holds it): Steam's DeusEx.exe has
    Steam's launch check, so it starts through Steam; a GOG (or any other) copy starts by itself."""
    try:
        folder = Path(folder).resolve()
        return any(folder.is_relative_to((lib / 'steamapps' / 'common').resolve()) for lib in steam_libraries())
    except OSError:
        return False


def game_program():
    """The program that runs the game's data on macOS and Linux: OpenDE's own (DEUSEDIX_GAME_PROGRAM, the build of
    the tree the editor is in, or one on PATH). '' when there is none: the game's own .exe needs a Windows wrapper."""
    if WINDOWS:
        return ''
    given = os.environ.get('DEUSEDIX_GAME_PROGRAM')
    if given:
        return given
    for base in (PROJECT, PROJECT.parent, APP_ROOT, APP_ROOT.parent):
        for name in ('deusex', os.path.join('deusex.app', 'Contents', 'MacOS', 'deusex')):
            program = Path(base) / 'build' / 'bin' / name
            if program.is_file():
                return str(program)
    return shutil.which('deusex') or ''


def steam_program():
    """The Steam program that starts a game with its own launch options: SteamExe from the registry on Windows,
    `steam` (or Steam.app's own program) on macOS and Linux. '' when it is not found."""
    if not WINDOWS:
        found = shutil.which('steam')
        if found:
            return found
        app = Path('/Applications/Steam.app/Contents/MacOS/steam_osx')
        return str(app) if MACOS and app.is_file() else ''
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
        return str(Path(winreg.QueryValueEx(key, 'SteamExe')[0]).resolve())


def game_command(args, target=None, folder=None):
    """How to start the game with these arguments: (argv, working folder). Through Steam for a Steam copy
    (it adds the player's own launch options), the game's own program for GOG and other copies; on macOS and Linux
    the program that runs the game's data (game_program: OpenDE's), with the game folder as its working folder."""
    target = target or TARGET
    profile = TARGETS[target]
    folder = Path(folder or GAME)
    if from_steam(folder):
        steam = steam_program()
        if steam:
            return [steam, '-applaunch', profile['app']] + list(args), None
    if not WINDOWS:
        return [game_program()] + list(args), str(folder)
    system = system_folder(target, folder)
    return [str(system / profile['exe'])] + list(args), str(system)


def system_folder(target=None, folder=None):
    """The System folder of `target`'s game (TARGET's by default) in the Deus Ex folder `folder` (GAME by default):
    Revision\\System for Revision, the Deus Ex folder's own for the original game."""
    profile = TARGETS[target or TARGET]
    folder = Path(folder or GAME)
    return (folder / profile['root'] if profile['root'] else folder) / 'System'


# the names of DeusEdix's launch copies in a game's System folder (launch_names): a Play's, followed by its project's
# id (live.py), and a mod's game's from the Mods panel (mods.py); the uninstaller removes them by these names
# (workspace.game_files)
LAUNCH_PLAY = 'DeusEdixPlay_'
LAUNCH_MOD = 'DeusEdixMod'


def launch_names(name):
    """The file names of one launch's copies in a game's System folder: (`name`.ini, `name`User.ini)."""
    return name + '.ini', name + 'User.ini'


def launch_inis(name, folder, target=None):
    """The copies of the game's ini file and user ini file that one launch of DeusEdix's starts it on (INI=, USERINI=:
    a Play, a mod's game), so that nothing the game saves while it plays reaches the player's own: (ini, user ini).
    Revision's launcher reads them only by a file name of its System folder ("Malformed system ini" for a path): there,
    by `name` (launch_names); the original game's go in `folder`\\Play, under the game's own names."""
    profile = TARGETS[target or TARGET]
    if profile['launch_in_system']:
        system = system_folder(target)
        return tuple(system / file for file in launch_names(name))
    play = Path(folder) / 'Play'
    return play / profile['ini'], play / profile['user_ini']


def workspace_id(path):
    """Stable UI/session identity, without renaming an existing mod's packages."""
    key = os.path.normcase(str(Path(path).resolve()))
    return 'workspace-' + hashlib.sha256(key.encode('utf-8')).hexdigest()[:32]


CFG = _config()
DEV = devmode()
if any(not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', DEV[k]) for k in DEV_DEFAULTS if k.endswith(('ackage', 'Dir'))):
    raise ValueError('Invalid package or folder name in devmode.json.')
PROJECT_META = _project()
MANAGED_PROJECT = PROJECT_META is not None
EXTERNAL_WORKSPACE = os.environ.get('DEUSEDIX_WORKSPACE_ID', '')
if EXTERNAL_WORKSPACE and (MANAGED_PROJECT or os.environ.get('DEUSEDIX_PROJECT_WORKER') != '1'
                           or EXTERNAL_WORKSPACE != workspace_id(PROJECT)):
    raise ValueError('This worker was started for a different existing workspace.')
PROJECT_ID = PROJECT_META['id'] if MANAGED_PROJECT else EXTERNAL_WORKSPACE or 'legacy'
if (os.environ.get('DEUSEDIX_PROJECT_WORKER') == '1'
        and os.environ.get('DEUSEDIX_PROJECT_ID', PROJECT_ID) != PROJECT_ID):
    raise ValueError('This worker was started for a different project identity.')
PROJECT_KIND = PROJECT_META['kind'] if MANAGED_PROJECT else 'legacy'
INDEPENDENT_PROJECT = MANAGED_PROJECT and PROJECT_KIND != 'original'
ALLOW_ORIGINAL_MAPS = not MANAGED_PROJECT or PROJECT_KIND == 'original'
STANDALONE = bool(CFG) or MANAGED_PROJECT
# the editor's own package (stories, interactions, cutscenes... are compiled into it)
PACKAGE = CFG.get('package', 'DeusEdix') if STANDALONE else DEV['package']
# the folder in Revision that the game reads first: the built maps (and, installed, the package)
MAPS_DIR_NAME = 'DeusEdixMaps' if STANDALONE else DEV['mapsDir']
if MANAGED_PROJECT:
    PACKAGE = PROJECT_META['package']
    MAPS_DIR_NAME = 'DeusEdixProjects/' + PROJECT_ID
    if CFG.get('package', PACKAGE) != PACKAGE or str(CFG.get('mapsDir', MAPS_DIR_NAME)).replace('\\', '/') != MAPS_DIR_NAME:
        raise ValueError('Project settings point to another package or install folder.')
elif CFG.get('mapsDir'):
    candidate = str(CFG['mapsDir']).replace('\\', '/')
    if not re.fullmatch(r'[A-Za-z0-9_-]+(?:/[A-Za-z0-9_-]+)*', candidate):
        raise ValueError('Invalid mapsDir setting.')
    MAPS_DIR_NAME = candidate
# the game the maps are made for: a project's own (its manifest), the installed editor's (deusedix.json),
# Revision for a development working copy
if MANAGED_PROJECT:
    TARGET = PROJECT_META.get('target', DEFAULT_TARGET)
    if CFG.get('target', TARGET) != TARGET:
        raise ValueError('Project settings point to another game.')
else:
    TARGET = CFG.get('target', DEFAULT_TARGET) if STANDALONE else DEFAULT_TARGET
    if TARGET not in TARGETS:
        raise ValueError('Invalid target setting: %r' % TARGET)
PROFILE = TARGETS[TARGET]
# Caches made from the game itself (its maps for the Pieces, the models of its objects and their pictures, its
# textures, conversations, voices, animations and characters) are the same in every project made for that game:
# a managed project keeps them once per game, in the app's editor cache (17 Oct 2026: about 3.7 GB, and the slow
# first scans and previews, were made again in every project). The working copy and the app's own workspace keep
# theirs where they were. A project's own maps and models stay in its own cache (editor/data.py CACHE).
EDITOR_CACHE = Path(__file__).resolve().parent / 'editor' / 'cache'
GAME_CACHE = (APP_ROOT / 'tools' / 'mapkit' / 'editor' / 'cache' / ('game-' + TARGET)) if MANAGED_PROJECT else EDITOR_CACHE
IS_REVISION = TARGET == 'revision'
GAME_NAME = PROFILE['name']
_default = default_game()
GAME = Path(CFG['game']) if CFG.get('game') else (_default if not check_game(_default, TARGET)
                                                 else (find_game(TARGET) or _default))
# the targeted game's own folder: Revision\ for Revision, the Deus Ex folder itself for the original game
GAME_ROOT = GAME / PROFILE['root'] if PROFILE['root'] else GAME
REVISION = GAME_ROOT                    # its older name (the editor began with Revision only)
GAME_SYSTEM = GAME_ROOT / 'System'
GAME_EXE = GAME_SYSTEM / PROFILE['exe']
GAME_INI = GAME_SYSTEM / PROFILE['ini']             # its folders (Paths), its save folder, its engine
USER_INI = GAME_SYSTEM / PROFILE['user_ini']        # its config(User) settings (the editor's runner)
INI_SECTIONS = PROFILE['sections']                  # the sections of GAME_INI with a Paths list
STEAM_APP = PROFILE['app']
ORIGINAL_MAPS = GAME_ROOT / 'Maps'                  # the game's own maps (Revision's are rebuilt copies)
REVISION_MAPS = ORIGINAL_MAPS
MAPS_DIR = GAME_ROOT / MAPS_DIR_NAME
# where the game saves while this project or workspace plays (Play): a folder of DeusEdix's, never the player's own
# Save folder (project_game: the game's SavePath then; live: a map's stale state is cleared there). A managed project's
# is its MAPS_DIR\Save; any other workspace's is DeusEdixProjects\legacy (or its workspace id), never a project's id
SAVE_DIR_NAME = 'DeusEdixProjects/' + PROJECT_ID + '/Save'
SAVE_DIR = GAME_ROOT / SAVE_DIR_NAME
# where the game finds its packages and textures, in the order it looks (Revision first, then what it
# reads from the original game); the original game never reads Revision's
SYSTEM_DIRS = ([GAME / 'System', GAME_ROOT / 'System', GAME_ROOT / 'HDTP' / 'System'] if IS_REVISION
               else [GAME / 'System'])
TEXTURE_DIRS = ([GAME_ROOT / 'Textures', GAME_ROOT / 'NewVision' / 'Textures', GAME / 'Textures'] if IS_REVISION
                else [GAME / 'Textures'])
# what the game adds to the engine's packages: loaded by every editor session (EditPackages), loaded with
# the maps (OBJ LOAD: the mission scripts of the levels)
GAME_EDIT_PACKAGES = ('Revision',) if IS_REVISION else ()
GAME_LOADS = ('RevisionMission.u',) if IS_REVISION else ()
# the conversations of the original story (gamecons.SETS) and what a new map's DeusExLevelInfo names
CONVERSATIONS = 'RevisionConversations' if IS_REVISION else 'DeusExConversations'
# what only Revision has: music as OGG files (DXOgg), rain and snow zones (Precipitation), the text a
# book or datacube shows set on the object itself (SpecialText), HDTP's models
FEATURES = frozenset(('ogg_music', 'weather', 'special_text', 'hdtp')) if IS_REVISION else frozenset()
# the original game's mission scripts (DeusEx.u: no 07 and no 13, as in the campaign)
_VANILLA_MISSIONS = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 14, 15}


def mission_script(mission):
    """The Script of a level of mission `mission` (DeusExLevelInfo), as UC_PROP writes it: 'None' when the
    game has no script for that mission."""
    mission = int(mission)
    if IS_REVISION:
        return "Class'RevisionMission.RevisionMission%02d'" % mission
    return "Class'DeusEx.Mission%02d'" % mission if mission in _VANILLA_MISSIONS else 'None'


def _running_names(names):
    """Do programs of these names run? From ps's own list of process names (macOS, Linux): the whole name, case
    aside, as tasklist's IMAGENAME filter matches it on Windows."""
    try:
        listing = subprocess.run(['ps', '-A', '-o', 'comm='], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    running = {Path(line.strip()).name.casefold() for line in listing.splitlines() if line.strip()}
    return any(name.casefold() in running for name in names)


def game_running(target=None):
    """Is the game (`target`'s, this process's by default; 'any': either) running on this PC? Its program by name, from
    any folder, as tasklist's IMAGENAME filter saw it, from one list of the processes (game_process: 15-20 ms; tasklist
    took 0.3-0.5 s at the start of every Play's preparation and every Play, 30 Oct 2026). On macOS and Linux the game
    is its data, run by OpenDE's own program (deusex), so that is what is looked for."""
    if not WINDOWS:
        return _running_names(['deusex'])
    import game_process
    names = [TARGETS[t]['exe'] for t in TARGETS] if target == 'any' else [TARGETS[target or TARGET]['exe']]
    return game_process.WindowsProcesses(GAME_EXE).named(names)
# the Animation Studio's and the imported models' packages: other names for DeusEdix, so that a PC with
# both never loads one in place of the other
ANIMS_PACKAGE = 'DeusEdixAnims' if STANDALONE else DEV['animsPackage']
MODELS_PACKAGE = 'DeusEdixModels' if STANDALONE else DEV['modelsPackage']
SOUNDS_PACKAGE = 'DeusEdixSounds' if STANDALONE else DEV['soundsPackage']     # the sounds imported in the editor's browser
SCRIPTS_PACKAGE = 'DeusEdixScripts' if STANDALONE else DEV['scriptsPackage']  # the scripts written in the editor (editor/scripts.py)
# the development mod's recorded voices (made by its own tools, not by the editor): one of its packages, never
# another mod's, in every mode
VOICES_PACKAGE = DEV['voicesPackage']
if MANAGED_PROJECT:
    ANIMS_PACKAGE, MODELS_PACKAGE, SOUNDS_PACKAGE = (PACKAGE + suffix for suffix in ('Anims', 'Models', 'Sounds'))
    SCRIPTS_PACKAGE = PACKAGE + 'Scripts'
    for key, expected in (('animsPackage', ANIMS_PACKAGE), ('modelsPackage', MODELS_PACKAGE), ('soundsPackage', SOUNDS_PACKAGE)):
        if CFG.get(key, expected) != expected:
            raise ValueError('Project resource package setting does not match its ID: ' + key)
# where those packages go in the game: a development mod's into Revision\System (its install.ps1), DeusEdix's into its
# maps folder (read by the game through Revision.ini's Paths: workspace.install)
PACKAGES_DIR = MAPS_DIR if STANDALONE else REVISION / 'System'


def current():
    """Identity of this process; fixed before any project-dependent backend is imported."""
    if EXTERNAL_WORKSPACE:
        return dict(id=PROJECT_ID, name=PROJECT.name, kind='workspace', path=str(PROJECT),
                    legacy=False, external=True, package=PACKAGE,
                    **({'target': TARGET} if TARGET != DEFAULT_TARGET else {}))
    return dict(PROJECT_META) if PROJECT_META else dict(id='legacy', name='Existing workspace', kind='legacy',
        path=str(PROJECT), legacy=True, package=PACKAGE, **({'target': TARGET} if TARGET != DEFAULT_TARGET else {}))


def python():
    """The command that runs one of the editor's Python scripts: this Python, or (DeusEdix installed, a
    frozen program: sys.executable is the program itself) its console twin, deusedix-cli.exe --run (deusedix-cli on
    macOS and Linux)."""
    if getattr(sys, 'frozen', False):
        return [str(Path(sys.executable).with_name('deusedix-cli.exe' if WINDOWS else 'deusedix-cli')), '--run']
    return [sys.executable]


def documents_dir():
    """The folder the user's documents are in: Documents of the user's home on Windows and macOS, Linux's
    XDG_DOCUMENTS_DIR when it is set (its user-dirs.dirs; a translated folder name), else ~/Documents."""
    if not WINDOWS and not MACOS:
        given = os.environ.get('XDG_DOCUMENTS_DIR')
        if not given:
            try:
                found = re.search(r'(?m)^\s*XDG_DOCUMENTS_DIR\s*=\s*"([^"]+)"',
                                  (Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config')
                                   / 'user-dirs.dirs').read_text(encoding='utf-8', errors='replace'))
            except OSError:
                found = None
            given = found.group(1).replace('$HOME', str(Path.home())) if found else ''
        if given:
            return Path(given).expanduser()
    return Path.home() / 'Documents'


def ffmpeg():
    """The ffmpeg program the textures and the contact sheets are made with: the one on PATH, else where each system
    keeps it (Windows' ffmpeg folder, Homebrew and MacPorts on macOS, /usr/bin and /usr/local/bin on Linux), else
    PATH's own name again, so that the run that needs it says it is not installed."""
    found = shutil.which('ffmpeg')
    if found:
        return found
    if WINDOWS:
        return r'C:\Program Files (x86)\ffmpeg\bin\ffmpeg.exe'
    for name in ('/opt/homebrew/bin/ffmpeg', '/usr/local/bin/ffmpeg', '/opt/local/bin/ffmpeg', '/usr/bin/ffmpeg'):
        if Path(name).is_file():
            return name
    return 'ffmpeg'                     # PATH's own name: the run that needs it says it is not installed


def label_font():
    """The font ffmpeg's drawtext sets the catalogues' labels in, named as its filter syntax takes a file (Windows'
    Arial, its ':' escaped; the systems' own sans serif on macOS and Linux). 'Arial' when none of them is there:
    ffmpeg then asks fontconfig for a font by that name."""
    if WINDOWS:
        return r'C\:/Windows/Fonts/arial.ttf'
    for name in ('/System/Library/Fonts/Supplemental/Arial.ttf',
                 '/System/Library/Fonts/Supplemental/Verdana.ttf',
                 '/Library/Fonts/Arial.ttf',
                 '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
                 '/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf',
                 '/usr/share/fonts/TTF/DejaVuSans.ttf',
                 '/usr/share/fonts/dejavu/DejaVuSans.ttf'):
        if Path(name).is_file():
            return name
    return 'Arial'


if __name__ == '__main__':
    print('standalone' if STANDALONE else DEV['name'] + ' working copy', '| package', PACKAGE, '| target', TARGET)
    print('game', GAME, check_game(GAME, TARGET) or 'ok', '| games here:', games_in(GAME))
    print('found by Steam:', find_game(TARGET))
    print('libraries:', [str(x) for x in steam_libraries()])
    print('data', DATA_DIR, '| platform data folder', data_dir(), '| default game', default_game())
