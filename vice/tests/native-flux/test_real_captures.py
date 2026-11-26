"""Copy, hash and read-only smoke-test a directory of user-authorized SCPs.

Only verified workspace copies are passed to c1541/x64sc. A boot screen is not
proof of full gameplay, every sector, or every copy-protection check.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
import threading
from concurrent.futures import ThreadPoolExecutor


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--cycles', type=int, default=180000000)
    parser.add_argument('--model', default='c64')
    parser.add_argument('--drive', type=int, default=1542)
    parser.add_argument('--jobs', type=int, choices=[1, 2, 3], default=1)
    parser.add_argument('--image', action='append', help='Limit testing to this relative filename (repeatable)')
    args = parser.parse_args()
    source, out, app = args.source.resolve(), args.output.resolve(), args.runtime.resolve()
    assert not out.is_relative_to(source), 'Copies/output must be outside the original-image directory'
    assert args.cycles > 0
    originals = sorted(p for p in source.rglob('*') if p.is_file() and p.suffix.lower() == '.scp')
    if args.image:
        selected = set(args.image)
        originals = [p for p in originals if p.relative_to(source).as_posix() in selected]
        assert len(originals) == len(selected), 'One or more requested SCP files were not found'
    assert originals, 'No SCP images found'
    out.mkdir(parents=True, exist_ok=False)
    copies = out / 'copies'
    copies.mkdir()
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', SDL_RENDER_DRIVER='software')
    env['PATH'] = str(app) + os.pathsep + env.get('SystemRoot', 'C:/Windows') + '/System32'
    for key in ['TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA']:
        folder = out / key
        folder.mkdir()
        env[key] = str(folder)
    results = {'runtime': str(app), 'model': args.model, 'drive': args.drive,
               'cycles': args.cycles, 'read_only': True, 'images': [], 'complete': False,
               'executable_sha256': {name: digest(app / name) for name in ['x64sc.exe', 'c1541.exe']}}

    def save():
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')

    # Copy every input before starting any emulator/utility test.
    for original in originals:
        before = digest(original)
        target = copies / original.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, target)
        target.chmod(0o444)
        assert digest(target) == digest(original) == before
        header = target.read_bytes()[:16]
        item = {'name': str(original.relative_to(source)), 'original': str(original),
                'copy': str(target), 'sha256': before, 'size': target.stat().st_size,
                'copy_verified_before_testing': True, 'header_hex': header.hex()}
        if len(header) == 16 and header[:3] == b'SCP':
            item.update(revolutions=header[5], version=hex(header[3]), flags=hex(header[8]),
                        start_halftrack=header[6], end_halftrack=header[7], heads=header[10])
        results['images'].append(item)
        save()
    print('Copied and SHA-256 verified all %d images before testing.' % len(originals), flush=True)
    lock = threading.Lock()

    def check(entry):
        index, result_item = entry
        item = dict(result_item)
        original, target = Path(item['original']), Path(item['copy'])
        directory = out / ('image-%02d' % index)
        directory.mkdir()
        try:
            started = time.monotonic()
            read = subprocess.run([str(app / 'c1541.exe'), '-attach', str(target), '-list'],
                                  cwd=directory, env=env, capture_output=True, timeout=90,
                                  creationflags=subprocess.CREATE_NO_WINDOW)
            (directory / 'directory.txt').write_bytes(read.stdout + read.stderr)
            entries = re.findall(rb'(?im)^\s*\d+\s+"[^"\r\n]+"\s+\*?(?:prg|seq|usr|rel|del)\b', read.stdout)
            item.update(directory_exit=read.returncode,
                        directory_bam_listed=b'blocks free.' in read.stdout.lower(),
                        directory_entries=len(entries), directory_listed=bool(entries),
                        directory_seconds=round(time.monotonic() - started, 2))
            print(item['name'], 'directory listed', item['directory_listed'], flush=True)
        except subprocess.TimeoutExpired:
            item['directory_timeout'] = True
        try:
            started = time.monotonic()
            log, screen = directory / 'vice.log', directory / 'screen.png'
            command = [app / 'x64sc.exe', '-default', '-console', '-warp', '+sound',
                       '-model', args.model, '-drive8type', str(args.drive), '-drive8truedrive',
                       '+trapdevice8', '-attach8ro', '-autostart-handle-tde', '-autostart-delay', '0',
                       '+autostart-delay-random', '-limitcycles', str(args.cycles),
                       '-logfile', log, '-exitscreenshot', screen, '-autostart', target]
            process = subprocess.run([str(v) for v in command], cwd=directory, env=env,
                                     capture_output=True, timeout=180, creationflags=subprocess.CREATE_NO_WINDOW)
            (directory / 'console.txt').write_bytes(process.stdout + process.stderr)
            text = log.read_text(errors='replace') if log.exists() else ''
            item.update(emulator_exit=process.returncode, emulator_seconds=round(time.monotonic() - started, 2),
                        autostart_done='Autostart: Done.' in text,
                        autostart_loading='Autostart: Loading' in text,
                        screenshot=str(screen) if screen.exists() else None,
                        errors=[line for line in text.splitlines() if 'Error' in line or 'Autostart:' in line])
            print(item['name'], 'emulator', process.returncode, 'autostart done', item['autostart_done'], flush=True)
        except subprocess.TimeoutExpired:
            item['emulator_timeout'] = True
        finally:
            item['original_unchanged'] = digest(original) == item['sha256']
            item['copy_unchanged'] = digest(target) == item['sha256']
            with lock:
                result_item.update(item)
                save()
            assert item['original_unchanged'] and item['copy_unchanged']
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        list(executor.map(check, enumerate(results['images'])))
    results['complete'] = True
    save()
    print('All original/copy hashes unchanged; native smoke tests complete.', flush=True)


if __name__ == '__main__':
    main()
