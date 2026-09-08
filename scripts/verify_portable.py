#!/usr/bin/env python3
"""Verify implementation bodies on older compilers; NOT a module-build test."""
import argparse, os, re, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--only', nargs='*')
p.add_argument('--sanitizer', choices=['address','undefined','thread'])
p.add_argument('--build-only', action='store_true')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
out=root/('.verify-'+(a.sanitizer or 'release'));out.mkdir(exist_ok=True)
def strip(s):
    s=re.sub(r'^(?:export )?module[^\n]*;[ \t]*$', '', s, flags=re.M)
    s=re.sub(r'^import [^\n]+;[ \t]*$', '', s, flags=re.M)
    return s.replace('export namespace ', 'namespace ')
parts=['#pragma once\n']
for name in ['core','memory','signal','runtime','health','output','simd','fft','dsp','acquire','pipeline']:
    path=root/'src/rf'/f'{name}.cppm'
    if path.exists(): parts.append(f'#line 1 "{path}"\n'+strip(path.read_text()))
(out/'rf.hpp').write_text('\n'.join(parts))
files=sorted(x for x in (root/'tests').glob('test_*.cpp') if x.stem!='test_soapy')
if a.only: files=[x for x in files if x.stem in a.only]
else: files.append(root/'src/main.cpp')
flags=['-std=c++23','-O2','-g','-ffp-contract=off','-pthread','-fno-exceptions','-fno-rtti','-Wall','-Wextra','-Wpedantic','-Werror=return-type']
if a.sanitizer:
    flags+=['-O1','-fno-omit-frame-pointer','-fno-pie','-no-pie','-fno-sanitize-recover=all','-fsanitize='+('address,undefined' if a.sanitizer=='address' else a.sanitizer)]
for path in files:
    name='rfdet' if path.name=='main.cpp' else path.stem
    src=out/(name+'.cpp');exe=out/name
    src.write_text('#include "rf.hpp"\n'+f'#line 1 "{path}"\n'+strip(path.read_text()))
    subprocess.run([os.environ.get('CXX','g++'),*flags,str(src),'-o',str(exe)],check=True,timeout=120)
    print('built',name,flush=True)
    if not a.build_only and name!='rfdet':subprocess.run([str(exe)],check=True,timeout=60)
print('Portable body verification completed. Module/import-std/NEON validation requires the target toolchain.',flush=True)
