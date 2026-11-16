"""The desktop app's bridge to the Game panel (deusedix.py's Api, pywebview's js_api): the page's game_cover goes to
NativeGamePanel.cover, which keeps the editor's window above every other window while an embedded Play starts. Then
what the launcher does with a platform: the window's backend, the browser it falls back to (macOS's open, Linux's
xdg-open, Windows's own shell), and the page's links, which go to that browser.

    .venv/Scripts/python.exe -m unittest discover -s launcher -p "test_*.py"
    python3 -m unittest discover -s launcher -p "test_*.py"            (macOS, Linux)
"""
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deusedix                                                     # noqa: E402


class GamePanelBridgeTests(unittest.TestCase):
    def test_the_page_asks_the_panel_for_the_cover_and_gets_its_answer(self):
        api, asked = deusedix.Api(), []
        api._game_panel = SimpleNamespace(cover=lambda on: asked.append(on) or dict(covered=on, error=''))
        self.assertEqual((api.game_cover(True), api.game_cover(False)),
                         (dict(covered=True, error=''), dict(covered=False, error='')))
        self.assertEqual(asked, [True, False])

    def test_continue_in_game_window_asks_the_panel_to_bring_the_game_to_the_front(self):
        # review su3 #5 (31 Oct 2026): the page's game_detach(true) is Continue in game window's; with no argument it
        # is every other detach's (Stop's hand-back, the Game view left, an error)
        api, asked = deusedix.Api(), []
        api._game_panel = SimpleNamespace(detach=lambda front=False: asked.append(front) or dict(attached=False))
        self.assertEqual((api.game_detach(), api.game_detach(True)), (dict(attached=False), dict(attached=False)))
        self.assertEqual(asked, [False, True])


class PlatformTests(unittest.TestCase):
    """The launcher on a platform: its own window where pywebview has a backend for it, and the user's browser
    everywhere else (macOS and Linux have no Edge WebView2)."""

    def test_the_window_backend_is_the_platforms_own(self):
        with mock.patch.object(deusedix.sys, 'platform', 'win32'):
            self.assertEqual(deusedix._window_backend(), 'edgechromium')     # WebView2 through .NET
        with mock.patch.object(deusedix.sys, 'platform', 'darwin'):
            self.assertEqual(deusedix._window_backend(), 'cocoa')            # pywebview's WebKit window
        with mock.patch.object(deusedix.sys, 'platform', 'linux'):
            self.assertIsNone(deusedix._window_backend())                    # pywebview takes GTK or Qt

    def test_windows_needs_webview2_and_macos_needs_pyobjc(self):
        with mock.patch.object(deusedix.sys, 'platform', 'win32'):
            with mock.patch.object(deusedix, '_webview2', lambda: ''):
                self.assertIn('WebView2', deusedix._window_ready())
            with mock.patch.object(deusedix, '_webview2', lambda: '120.0.0.0'):
                self.assertEqual(deusedix._window_ready(), '')
        with mock.patch.object(deusedix.sys, 'platform', 'darwin'), \
                mock.patch.dict(sys.modules, {'objc': None}):                # pyobjc not installed
            self.assertIn('pyobjc', deusedix._window_ready())
        with mock.patch.object(deusedix.sys, 'platform', 'linux'):
            self.assertEqual(deusedix._window_ready(), '')                   # pywebview's GTK or Qt, tried as it is

    def test_the_browser_is_opened_with_the_platforms_own_command(self):
        for platform, command in (('darwin', 'open'), ('linux', 'xdg-open')):
            with self.subTest(platform), \
                    mock.patch.object(deusedix.sys, 'platform', platform), \
                    mock.patch.object(deusedix.subprocess, 'Popen') as popen:
                deusedix._open_url('http://127.0.0.1:8790/')
            self.assertEqual(popen.call_args[0][0], [command, 'http://127.0.0.1:8790/'])
            # never waits for the browser, and is not taken down with the launcher
            self.assertTrue(popen.call_args[1]['start_new_session'])

    def test_windows_opens_the_shells_default_browser(self):
        with mock.patch.object(deusedix.sys, 'platform', 'win32'), \
                mock.patch.object(deusedix.os, 'startfile', create=True) as startfile:
            deusedix._open_url('https://store.steampowered.com/')
        startfile.assert_called_once_with('https://store.steampowered.com/')

    def test_a_browser_command_that_is_not_there_falls_back_to_pythons_browser(self):
        with mock.patch.object(deusedix.sys, 'platform', 'linux'), \
                mock.patch.object(deusedix.subprocess, 'Popen', side_effect=FileNotFoundError('xdg-open')), \
                mock.patch.object(deusedix.webbrowser, 'open') as fallback:
            deusedix._open_url('http://127.0.0.1:8790/')
        fallback.assert_called_once_with('http://127.0.0.1:8790/')

    def test_the_pages_links_go_to_that_browser(self):
        api, opened = deusedix.Api(), []
        with mock.patch.object(deusedix, '_open_url', opened.append):
            api.open('https://store.steampowered.com/app/6910/')             # the start page's Get Deus Ex links
            api.open('https://example.com/')                                 # no store: nothing
            api.open_url('http://127.0.0.1:8790/guide')                      # the editor's own guide
            api.open_url('file:///etc/passwd')                               # not the guide or a page: nothing
        self.assertEqual(opened, ['https://store.steampowered.com/app/6910/', 'http://127.0.0.1:8790/guide'])


if __name__ == '__main__':
    unittest.main()
