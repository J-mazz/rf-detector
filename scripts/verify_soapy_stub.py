#!/usr/bin/env python3
"""Test rf.soapy against a C API shim; this is not hardware or native-module validation."""
import argparse
import os
import re
import subprocess
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitizer',choices=['address','undefined'])
parser.add_argument('--build-only',action='store_true')
args=parser.parse_args()

root=Path(__file__).resolve().parents[1]
out=root/'.verify-soapy-stub'
out.mkdir(exist_ok=True)

def strip_module_syntax(source: str) -> str:
    source=re.sub(r'^(?:export )?module[^\n]*;[ \t]*$','',source,flags=re.M)
    source=re.sub(r'^import [^\n]+;[ \t]*$','',source,flags=re.M)
    return source.replace('export namespace ','namespace ')

parts=[]
for name in ('core','acquire','soapy'):
    path=root/'src'/'rf'/f'{name}.cppm'
    parts.append(f'#line 1 "{path}"\n'+strip_module_syntax(path.read_text()))
test=root/'tests'/'test_soapy.cpp'
parts.append(f'#line 1 "{test}"\n'+strip_module_syntax(test.read_text()))
source=out/'test_soapy.cpp'
source.write_text('\n'.join(parts))

flags=[
    '-std=c++23','-O2','-g','-ffp-contract=off','-pthread','-fno-exceptions','-fno-rtti',
    '-Wall','-Wextra','-Wpedantic','-Werror=return-type','-DRF_WITH_SOAPY=1',
    '-I',str(root/'tests'/'soapy_stub'),
]
if args.sanitizer:
    flags+=['-O1','-fno-omit-frame-pointer','-fno-pie','-no-pie','-fno-sanitize-recover=all']
    flags+=['-fsanitize='+('address,undefined' if args.sanitizer=='address' else 'undefined')]

executable=out/'test_soapy'
subprocess.run([os.environ.get('CXX','g++'),*flags,str(source),'-o',str(executable)],check=True,timeout=120)
print('built test_soapy against test-only SoapySDR C API shim',flush=True)
if not args.build_only:
    environment=os.environ.copy()
    if args.sanitizer=='address' and 'ASAN_OPTIONS' not in environment:
        environment['ASAN_OPTIONS']='detect_leaks=0'
    subprocess.run([str(executable)],check=True,timeout=60,env=environment)
print('Soapy shim body verification completed. Hardware and native-module validation require SoapySDR, a receiver, and GCC >=14.',flush=True)
