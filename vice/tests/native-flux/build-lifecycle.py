"""Compile the real diskimage/fsimage lifecycle with instrumented platform services.

Requires a configured VICE build for its config.h; does not alter that build.
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
a = p.parse_args()
source = Path(__file__).resolve().parents[2]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
if Path(a.cc).is_absolute():
    env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH', '')
includes = [a.configured_build.resolve() / 'src'] + [source / ('src/' + n) for n in
    ['', 'diskimage', 'drive', 'lib/p64', 'arch/sdl', 'arch/shared']]
includes += a.include_dir
flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-Wno-sign-compare', '-DHAVE_CONFIG_H', '-DNDEBUG'] + ['-I' + str(d) for d in includes]
if os.name != 'nt':
    flags += ['-fPIC']
names = ['diskimage', 'fsimage', 'fsimage-check', 'fsimage-probe', 'fsimage-scp', 'flux-capture', 'flux-playback', 'flux-logic', 'flux-view', 'scp-reader']
objects = []
hooks = ['-Dmalloc=native_test_malloc', '-Dcalloc=native_test_calloc', '-Dfree=native_test_free',
         '-Dfread=native_test_fread', '-Dfseek=native_test_fseek', '-Dftell=native_test_ftell']
with (out / 'compile.log').open('w', encoding='utf-8') as log:
    for name in names + ['lifecycle-hooks', 'test-hooks']:
        file = source / ('tests/native-flux/' + name + '.c' if name.endswith('hooks')
                         else 'src/diskimage/' + name + '.c')
        obj = out / (name + '.o')
        injected = hooks if name in ['flux-capture', 'flux-playback', 'flux-logic', 'flux-view', 'scp-reader'] else []
        subprocess.run([a.cc] + flags + injected + ['-c', str(file), '-o', str(obj)],
                       env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects.append(str(obj))
    dll = out / ('lifecycle.dll' if os.name == 'nt' else 'lifecycle.so')
    subprocess.run([a.cc, '-shared'] + objects + ['-o', str(dll)],
                   env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
with (out / 'tests.log').open('w', encoding='utf-8') as log:
    result = subprocess.run([sys.executable, str(source / 'tests/native-flux/test_lifecycle.py'),
                             str(dll), str(out)], env=env, stdout=log, stderr=subprocess.STDOUT)
print((out / 'tests.log').read_text(encoding='utf-8'))
if result.returncode:
    raise SystemExit(result.returncode)
with (out / 'view-tests.log').open('w', encoding='utf-8') as log:
    result = subprocess.run([sys.executable, str(source / 'tests/native-flux/test_view.py'),
                             str(dll), str(out)], env=env, stdout=log, stderr=subprocess.STDOUT)
print((out / 'view-tests.log').read_text(encoding='utf-8'))
raise SystemExit(result.returncode)
