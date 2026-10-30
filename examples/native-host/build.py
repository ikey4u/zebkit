#!/usr/bin/env python3
"""Build a toolkit-owned web/PDF window against an existing zebkit package."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('toolkit', choices=['gtk', 'qt'])
parser.add_argument('--package', type=Path, default=Path('out/blink/slim/package'))
parser.add_argument('--output', type=Path)
parser.add_argument('--toolkit-root', type=Path,
                    help='Optional extracted development-package root (with usr/include and usr/lib)')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
package = args.package.resolve()
output = (args.output or repo / 'out/blink/examples' / (args.toolkit + '-host')).resolve()
output.parent.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
extra = []
if args.toolkit_root:
    toolkit = args.toolkit_root.resolve()
    lib = toolkit / 'usr/lib/x86_64-linux-gnu'
    env['PKG_CONFIG_PATH'] = str(lib / 'pkgconfig') + os.pathsep + env.get('PKG_CONFIG_PATH', '')
    extra.append('-L' + str(lib))
flags = shlex.split(subprocess.check_output(
    ['pkg-config', '--cflags', '--libs', 'gtk+-3.0' if args.toolkit == 'gtk' else 'Qt6Widgets'],
    env=env, text=True))
if args.toolkit_root:
    for i, flag in enumerate(flags):
        if flag.startswith(('-I/usr/', '-L/usr/')):
            local = toolkit / flag[2:].lstrip('/')
            if local.exists():
                flags[i] = flag[:2] + str(local)
if sys.platform == 'darwin':
    link = ['-F' + str(package), '-framework', 'Zebkit', '-Wl,-rpath,' + str(package)]
else:
    link = ['-L' + str(package), '-l:libzebkit.so.1', '-Wl,-rpath,' + str(package)]
command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
           '-Wno-deprecated-declarations', '-fPIC', '-I' + str(package / 'include'),
           str(Path(__file__).with_name(args.toolkit + '.cc')), '-o', str(output), *extra, *flags, *link]
subprocess.run(command, check=True)
print(output)
