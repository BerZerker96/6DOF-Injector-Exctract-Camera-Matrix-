#!/usr/bin/env bash
# Host-runnable correctness test for the runtime's pure cave logic (insLen + copyRelocated).
# Re-extracts the functions VERBATIM from the runtime so the test always matches shipping code.
set -e
cd "$(dirname "$0")"
python3 - << 'PY'
src=open('../runtime/sixdof_runtime.cpp').read()
def grab(s,e):
    i=src.index(s); j=src.index(e,i)+len(e); return src[i:j]
m=grab('static int modrmLen','\n}\n'); il=grab('static int insLen','\n}\n')
i=src.index('static bool copyRelocated'); j=src.index('    return true;\n}\n',i)+len('    return true;\n}\n')
open('_extracted.inc','w').write('\n'.join([m,il,src[i:j]]))
PY
# -D_WIN64 exercises the x64 rip-relative relocation path (host is x86-64, so the arithmetic is faithful).
g++ -O2 -std=c++17 -D_WIN64 cave_selftest.cpp -o cave_selftest
./cave_selftest
