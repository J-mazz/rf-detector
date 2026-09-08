#!/usr/bin/env python3
"""Test rf.soapy against a C API shim; --native also validates GCC 16+ modules.

The shim does not exercise the real SoapySDR SDK or receiver hardware.
"""
import argparse
import os
import re
import subprocess
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitizer',choices=['address','undefined'])
parser.add_argument('--build-only',action='store_true')
parser.add_argument('--compiler',default=os.environ.get('CXX','g++'))
parser.add_argument('--native',action='store_true',help='compile actual C++26/import-std modules with GCC >=16')
parser.add_argument('--out',type=Path,help='isolated build directory (default: .verify-soapy-stub[-native])')
args=parser.parse_args()

root=Path(__file__).resolve().parents[1]
out=args.out.resolve() if args.out else root/('.verify-soapy-stub-native' if args.native else '.verify-soapy-stub')
out.mkdir(parents=True,exist_ok=True)

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
objects=[]
if args.native:
    version=subprocess.run([args.compiler,'-dumpfullversion','-dumpversion'],check=True,capture_output=True,text=True)
    if int(version.stdout.split('.')[0])<16:
        parser.error('--native requires GCC >=16; omit it for portable body verification')
    flags[0]='-std=c++26'
    flags+=['-fmodules','-DRF_IMPORT_STD=1']
    for index,name in enumerate(('core','acquire','soapy')):
        obj=out/f'rf.{name}.o'
        step=['--compile-std-module'] if index==0 else []
        subprocess.run([args.compiler,*flags,*step,'-x','c++','-c',str(root/'src'/'rf'/f'{name}.cppm'),'-o',str(obj)],
                       cwd=out,check=True,timeout=120)
        objects.append(str(obj))
    source=test
subprocess.run([args.compiler,*flags,str(source),*objects,'-o',str(executable)],cwd=out,check=True,timeout=120)
print('built test_soapy against test-only SoapySDR C API shim',flush=True)
if not args.build_only:
    environment=os.environ.copy()
    if args.sanitizer=='address' and 'ASAN_OPTIONS' not in environment:
        environment['ASAN_OPTIONS']='detect_leaks=0'
    subprocess.run([str(executable)],check=True,timeout=60,env=environment)
mode='native module' if args.native else 'body'
print(f'Soapy shim {mode} verification completed. Real SDK and hardware behavior are not exercised.',flush=True)
