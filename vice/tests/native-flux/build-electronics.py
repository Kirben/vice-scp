"""Compare real modified rotation.c to a pristine upstream reference.

Requires the configured native VICE build's headers. No emulator UI is used.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--configured-build', required=True, type=Path)
p.add_argument('--cc', default=os.environ.get('CC', 'cc'))
p.add_argument('--include-dir', action='append', type=Path, default=[])
p.add_argument('--reference-rotation', type=Path)
a = p.parse_args()
source = Path(__file__).resolve().parents[2]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
if Path(a.cc).is_absolute():
    env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH', '')
if a.reference_rotation:
    reference = a.reference_rotation.resolve()
else:
    reference = out / 'upstream-rotation.c'
    reference.write_bytes(subprocess.check_output(['git', 'show',
        '8e45817287033b36112f6925a89d9f9c953a0cc8:vice/src/drive/rotation.c'], cwd=source))
includes = [a.configured_build.resolve() / 'src'] + [source / ('src/' + n) for n in
    ['', 'drive', 'diskimage', 'core/rtc', 'lib/p64']] + a.include_dir
flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
         '-DHAVE_CONFIG_H', '-DNDEBUG'] + ['-I' + str(d) for d in includes]
if os.name != 'nt':
    flags += ['-fPIC']
libraries = []
with (out / 'compile.log').open('w', encoding='utf-8') as log:
    for variant, rotation in [('reference', reference), ('native', source / 'src/drive/rotation.c')]:
        objects = []
        files = [source / 'tests/native-flux/electronics-hooks.c', source / 'src/lib/p64/p64.c']
        if variant == 'native':
            files += [source / ('src/diskimage/' + n + '.c') for n in ['flux-capture', 'flux-playback', 'flux-logic']]
        for file in files:
            obj = out / (variant + '-' + file.stem + '.o')
            define = ['-DNATIVE_ROTATION_SOURCE="' + rotation.as_posix() + '"']
            if variant == 'reference':
                define += ['-DNATIVE_REFERENCE_ONLY']
            subprocess.run([a.cc] + flags + define + ['-c', str(file), '-o', str(obj)],
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        dll = out / (variant + ('.dll' if os.name == 'nt' else '.so'))
        subprocess.run([a.cc, '-shared'] + objects + ['-lm', '-o', str(dll)],
                       env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        libraries.append(str(dll))
with (out / 'tests.log').open('w', encoding='utf-8') as log:
    result = subprocess.run([sys.executable, str(source / 'tests/native-flux/test_electronics.py')]
                             + libraries, env=env, stdout=log, stderr=subprocess.STDOUT)
print((out / 'tests.log').read_text(encoding='utf-8'))
raise SystemExit(result.returncode)
