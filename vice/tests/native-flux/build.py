"""Build and run standalone production native-flux tests, without VICE/P64.

Windows: python tests/native-flux/build.py OUTPUT --cc PATH_TO_MINGW_GCC
Linux:   python3 tests/native-flux/build.py OUTPUT [--sanitize]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source = Path(__file__).resolve().parents[2]
env = os.environ.copy()
if Path(args.cc).is_absolute():
    env['PATH'] = str(Path(args.cc).parent) + os.pathsep + env.get('PATH', '')
windows = os.name == 'nt'
library = out / ('native-flux.dll' if windows else 'native-flux.so')
flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(source / 'src/diskimage')]
if not windows:
    flags += ['-fPIC']
if args.sanitize:
    flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
hooks = ['-Dmalloc=native_test_malloc', '-Dcalloc=native_test_calloc', '-Dfree=native_test_free',
         '-Dfread=native_test_fread', '-Dfseek=native_test_fseek', '-Dftell=native_test_ftell']
objects = []
commands = []
with (out / 'compile.log').open('w') as log:
    for name in ['flux-capture', 'flux-playback', 'scp-reader', 'test-hooks']:
        file = source / ('tests/native-flux/test-hooks.c' if name == 'test-hooks'
                         else 'src/diskimage/' + name + '.c')
        obj = out / (name + '.o')
        command = [args.cc] + flags + ([] if name == 'test-hooks' else hooks) + ['-c', str(file), '-o', str(obj)]
        commands.append(command)
        subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects.append(str(obj))
    command = [args.cc, '-shared'] + objects + ['-o', str(library)]
    if windows:
        command += ['-Wl,--export-all-symbols']
    if args.sanitize:
        command += ['-fsanitize=undefined']
    commands.append(command)
    subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    writer_test = out / ('test-write.exe' if windows else 'test-write')
    writer_sources = [source / 'tests/native-flux/test-write.c']
    writer_sources += [source / 'src/diskimage' / (name + '.c')
                       for name in ('flux-capture', 'scp-reader', 'scp-writer')]
    command = [args.cc] + flags + [str(file) for file in writer_sources] + ['-o', str(writer_test)]
    commands.append(command)
    subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
version = subprocess.check_output([args.cc, '--version'], env=env, text=True).splitlines()[0]
(out / 'compiler.txt').write_text(version + '\n' + repr(commands) + '\n')
with (out / 'tests.log').open('w', encoding='utf-8') as log:
    subprocess.run([str(writer_test)], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    p = subprocess.run([sys.executable, str(source / 'tests/native-flux/test_native_flux.py'),
                        str(library), str(out)], env=env, stdout=log, stderr=subprocess.STDOUT)
print((out / 'tests.log').read_text(encoding='utf-8'))
raise SystemExit(p.returncode)
