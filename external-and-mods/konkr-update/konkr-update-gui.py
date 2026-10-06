#!/usr/bin/env python3
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

RELEASES_API = 'https://api.github.com/repos/hashtagbasit/SteamOS-ARM-Port/releases'
INSTALLED_VERSION_FILE = Path('/etc/konkr-release')
MODEL_FILE = Path('/sys/firmware/devicetree/base/model')
# Kept in sync with external-and-mods/konkr-update/konkr-update.py's SOC_MODELS.
SOC_MODELS = {
    'sm8650': ['KONKR Pocket FIT', 'AYANEO Pocket S2'],
    'sm8550': ['AYN Odin 2', 'AYN Odin 2 Mini', 'AYN Odin 2 Portal', 'AYN Thor',
               'AYANEO Pocket ACE', 'AYANEO Pocket DMG', 'AYANEO Pocket DS',
               'AYANEO Pocket EVO', 'AYANEO Pocket S 1K', 'AYANEO Pocket S 2K',
               'Retroid Pocket 6', 'Retroid Pocket 6 TOP-DPAD', 'Retroid Pocket Nova'],
}


def detect_soc():
    try:
        model = MODEL_FILE.read_text().rstrip('\0\n')
    except OSError:
        return None
    return next((soc for soc, models in SOC_MODELS.items() if model in models), None)


def installed_version():
    try:
        return INSTALLED_VERSION_FILE.read_text().strip() or None
    except OSError:
        return None


def roomiest_dir():
    """Pick the writable location with the most free space (SD card or HOME):
    a release download plus its extracted image needs ~25 GB."""
    media = Path('/run/media/steamos')
    candidates = [Path.home()] + ([p for p in media.iterdir() if p.is_dir()] if media.is_dir() else [])
    candidates = [p for p in candidates if os.access(p, os.W_OK)]
    return max(candidates, key=lambda p: shutil.disk_usage(p).free)


PACKAGE_PART = re.compile(r'\.tar\.gz\.\d{3}$')


def is_package(path):
    return path.endswith('.tar.gz') or bool(PACKAGE_PART.search(path))


def package_base(path):
    """`x.tar.gz.001` -> `x.tar.gz`; a plain `x.tar.gz` is returned unchanged."""
    return PACKAGE_PART.sub('.tar.gz', path)


def join_parts(path):
    """Join split package parts (release assets are capped at 2 GiB) into one
    file next to them; konkr-update.py verifies the joined file's checksum."""
    if not PACKAGE_PART.search(path): return path, False
    base = Path(package_base(path))
    parts = sorted(base.parent.glob(base.name + '.[0-9][0-9][0-9]'))
    with base.open('wb') as out:
        for part in parts:
            with part.open('rb') as f: shutil.copyfileobj(f, out, 4 << 20)
    return str(base), True


def sibling_checksum(path):
    try:
        return Path(package_base(path) + '.sha256').read_text().split()[0].lower()
    except (OSError, IndexError):
        return ''


def guess_version(filename):
    m = re.search(r'(v[\w]+(?:[.\-][\w]+)*)', filename)
    return m.group(1) if m else filename


class Window(Gtk.Window):
    def __init__(self):
        super().__init__(title='SteamOS Update')
        self.set_default_size(600, 420)
        self.set_border_width(20)
        self.soc = detect_soc()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14); self.add(box)

        label = Gtk.Label(label='Update SteamOS while keeping your games, saves and login.\n'
                                 'The update runs on the next restart and keeps a recovery backup.')
        label.set_line_wrap(True); label.set_xalign(0); box.pack_start(label, False, False, 0)

        box.pack_start(Gtk.Separator(), False, False, 0)
        online_label = Gtk.Label(label='Check github.com/hashtagbasit/SteamOS-ARM-Port for a new release:')
        online_label.set_xalign(0); box.pack_start(online_label, False, False, 0)
        online_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.check_button = Gtk.Button(label='Check for updates'); self.check_button.connect('clicked', self.check_online)
        online_row.pack_start(self.check_button, False, False, 0)
        self.download_button = Gtk.Button(label='Download and prepare'); self.download_button.set_sensitive(False)
        self.download_button.connect('clicked', self.download_latest)
        online_row.pack_start(self.download_button, False, False, 0)
        box.pack_start(online_row, False, False, 0)
        self.online_status = Gtk.Label(label='' if self.soc else 'Unrecognized device, online checks are unavailable.')
        self.online_status.set_line_wrap(True); self.online_status.set_xalign(0)
        box.pack_start(self.online_status, False, False, 0)
        self._latest_release = None
        box.pack_start(Gtk.Separator(), False, False, 0)

        manual_label = Gtk.Label(label='Or choose a package or release image you already downloaded:')
        manual_label.set_xalign(0); box.pack_start(manual_label, False, False, 0)
        self.file = Gtk.FileChooserButton(title='Choose an official update package or release image')
        for name, patterns in (('SteamOS update package', ('*.tar.gz', '*.tar.gz.001')),
                                ('SteamOS release image', ('*.7z', '*.7z.001', '*.img'))):
            f = Gtk.FileFilter(); f.set_name(name)
            for pattern in patterns: f.add_pattern(pattern)
            self.file.add_filter(f)
        self.file.connect('file-set', self.file_chosen)
        box.pack_start(self.file, False, False, 0)
        self.sha = Gtk.Entry(); self.sha.set_placeholder_text('SHA-256 checksum from the official release (.tar.gz packages only)')
        box.pack_start(self.sha, False, False, 0)
        self.prepare = Gtk.Button(label='Prepare update'); self.prepare.connect('clicked', self.start)
        box.pack_start(self.prepare, False, False, 0)

        self.status = Gtk.Label(label='Pick a downloaded update package (`.tar.gz` or its first `.001` part) '
                                       'and its checksum, or the `.7z` parts (or extracted `.img`) of a full '
                                       'release to install it in place, no reflash needed.')
        self.status.set_line_wrap(True); self.status.set_selectable(True); self.status.set_xalign(0)
        box.pack_start(self.status, True, True, 0)
        self.reboot = Gtk.Button(label='Restart and install'); self.reboot.set_sensitive(False)
        self.reboot.connect('clicked', lambda _: subprocess.run(['systemctl', 'reboot'], check=False))
        box.pack_start(self.reboot, False, False, 0)

    # -- manual package / release image -------------------------------------

    def file_chosen(self, _):
        package = self.file.get_filename()
        if package and not is_package(package):
            self.sha.set_sensitive(False)
            self.sha.set_text('')
            self.sha.set_placeholder_text("Not needed: verified against the release's SHA256SUMS instead")
        else:
            self.sha.set_sensitive(True)
            self.sha.set_placeholder_text('SHA-256 checksum from the official release')
            # Pre-fill from a downloaded `.sha256` next to the package, if any.
            if package and not self.sha.get_text().strip(): self.sha.set_text(sibling_checksum(package))

    def start(self, _):
        package = self.file.get_filename()
        if not package:
            self.status.set_text('Choose a package or release image first.'); return
        if is_package(package):
            sha = self.sha.get_text().strip().lower()
            if len(sha) != 64:
                self.status.set_text('Enter the official release checksum for this package.'); return
            self.run_stage(package, sha)
        else:
            if not self.soc:
                self.status.set_text('Unrecognized device model; cannot tell which SoC package to build.'); return
            version = guess_version(Path(package).name)
            self.run_build_and_stage(package, self.soc, version)

    # -- online check ---------------------------------------------------------

    def check_online(self, _):
        if not self.soc:
            self.online_status.set_text('Unrecognized device, cannot match a release.'); return
        self.check_button.set_sensitive(False)
        self.online_status.set_text('Checking...')
        def worker():
            try:
                req = urllib.request.Request(RELEASES_API, headers={'User-Agent': 'konkr-update'})
                with urllib.request.urlopen(req, timeout=15) as r:
                    releases = json.load(r)
                release = next((rel for rel in releases if any(
                    self.soc in a['name'] for a in rel.get('assets', []))), None)
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                GLib.idle_add(self.online_checked, None, str(e)); return
            GLib.idle_add(self.online_checked, release, None)
        threading.Thread(target=worker, daemon=True).start()

    def online_checked(self, release, error):
        self.check_button.set_sensitive(True)
        if error:
            self.online_status.set_text(f'Could not check for updates: {error}'); return False
        if not release:
            self.online_status.set_text(f'No {self.soc} release found.'); return False
        current = installed_version()
        self._latest_release = release
        tag = release['tag_name']
        if current and current == tag:
            self.online_status.set_text(f'Up to date ({tag}).')
            self.download_button.set_sensitive(False)
        else:
            was = f' (currently {current})' if current else ''
            self.online_status.set_text(f'{tag} is available{was}: {release.get("name", tag)}')
            self.download_button.set_sensitive(True)
        return False

    def download_latest(self, _):
        release = self._latest_release
        if not release: return
        assets = release.get('assets', [])
        # Prefer the prebuilt update package; fall back to building one from the image.
        pkg = sorted((a for a in assets if self.soc in a['name'] and PACKAGE_PART.search(a['name'])),
                     key=lambda a: a['name'])
        pkg_sum = next((a for a in assets if self.soc in a['name'] and a['name'].endswith('.tar.gz.sha256')), None)
        if pkg and pkg_sum:
            parts, sums, is_pkg = pkg, pkg_sum, True
        else:
            parts = sorted((a for a in assets if self.soc in a['name'] and '.7z' in a['name']),
                           key=lambda a: a['name'])
            sums = next((a for a in assets if a['name'] == 'SHA256SUMS'), None)
            is_pkg = False
        if not parts:
            self.online_status.set_text('This release has no update package or .7z image to download.'); return
        self.download_button.set_sensitive(False); self.check_button.set_sensitive(False)
        self.online_status.set_text(f'Downloading {release["tag_name"]} ({len(parts)} part(s))...')
        def worker():
            try:
                tmp = Path(tempfile.mkdtemp(prefix='konkr-download-', dir=roomiest_dir()))
                for asset in ([sums] if sums else []) + parts:
                    dest = tmp / asset['name']
                    GLib.idle_add(self.online_status.set_text, f'Downloading {asset["name"]}...')
                    req = urllib.request.Request(asset['browser_download_url'], headers={'User-Agent': 'konkr-update'})
                    with urllib.request.urlopen(req, timeout=120) as r, dest.open('wb') as f:
                        shutil.copyfileobj(r, f)
                first = tmp / parts[0]['name']
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                GLib.idle_add(self.download_failed, str(e)); return
            GLib.idle_add(self.download_done, str(first), release['tag_name'], is_pkg)
        threading.Thread(target=worker, daemon=True).start()

    def download_failed(self, error):
        self.online_status.set_text(f'Download failed: {error}')
        self.download_button.set_sensitive(True); self.check_button.set_sensitive(True)
        return False

    def download_done(self, first_part, version, is_pkg):
        self.online_status.set_text(f'Downloaded {version}. Preparing the update...')
        self.check_button.set_sensitive(True)
        if is_pkg:
            sha = sibling_checksum(first_part)
            if len(sha) != 64:
                self.download_failed('the release has no valid package checksum'); return False
            self.run_stage(first_part, sha)
        else:
            self.run_build_and_stage(first_part, self.soc, version)
        return False

    # -- shared staging --------------------------------------------------------

    def run_stage(self, package, sha):
        self.prepare.set_sensitive(False); self.file.set_sensitive(False); self.sha.set_sensitive(False)
        self.status.set_text('Checking and preparing the update. This can take several minutes.')
        def worker():
            try:
                joined, temporary = join_parts(package)
            except OSError as e:
                GLib.idle_add(self.finished, subprocess.CompletedProcess([], 1, '', f'Could not join package parts: {e}'))
                return
            try:
                p = subprocess.run(['pkexec', '/usr/bin/python3', '/usr/share/konkr-update/konkr-update.py',
                                    'stage', joined, '--sha256', sha], capture_output=True, text=True)
            finally:
                if temporary: Path(joined).unlink(missing_ok=True)
            GLib.idle_add(self.finished, p)
        threading.Thread(target=worker, daemon=True).start()

    def run_build_and_stage(self, source, soc, version):
        self.prepare.set_sensitive(False); self.file.set_sensitive(False); self.sha.set_sensitive(False)
        self.download_button.set_sensitive(False)
        self.status.set_text('Building the update package from the release image, then preparing it. '
                              'This can take several minutes.')
        def worker():
            script = (
                'set -e; work=$(mktemp -d -p "$(dirname "$1")" .konkr-package-XXXXXX); out="$work/package.tar.gz"; '
                'python3 /usr/share/konkr-update/image-to-package.py "$1" --soc "$2" --version "$3" --output "$out"; '
                'sha=$(sha256sum "$out" | cut -d" " -f1); '
                'rc=0; python3 /usr/share/konkr-update/konkr-update.py stage "$out" --sha256 "$sha" || rc=$?; '
                'rm -rf "$work"; exit $rc')
            p = subprocess.run(['pkexec', 'sh', '-c', script, 'sh', source, soc, version],
                                capture_output=True, text=True)
            GLib.idle_add(self.finished, p)
        threading.Thread(target=worker, daemon=True).start()

    def finished(self, p):
        if p.returncode == 0:
            self.status.set_text('Ready to restart. Keep the device charged and leave it on during the update. Games and saves will be kept.')
            self.reboot.set_sensitive(True)
        else:
            self.status.set_text((p.stderr or p.stdout).strip()[-1000:] or 'The update could not be prepared.')
            self.prepare.set_sensitive(True); self.file.set_sensitive(True)
            if self.file.get_filename() and is_package(self.file.get_filename()):
                self.sha.set_sensitive(True)
        return False

w = Window(); w.connect('destroy', Gtk.main_quit); w.show_all(); Gtk.main()
