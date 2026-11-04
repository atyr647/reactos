#!/usr/bin/env python3
"""List imports of PE files that the ReactOS build (amd64) does not export.
usage: rosimp.py BUILD_DIR file.dll|exe ..."""
import subprocess,sys,os,collections,functools
build=sys.argv[1]
@functools.lru_cache(None)
def exports(dll):
    for root,_,fs in os.walk(build):
        for f in fs:
            if f.lower()==dll.lower():
                p=os.path.join(root,f)
                o=subprocess.run(['x86_64-w64-mingw32-objdump','-p',p],capture_output=True,text=True).stdout
                names=set(); inexp=False
                for l in o.split('\n'):
                    if 'Ordinal/Name Pointer' in l: inexp=True; continue
                    if inexp:
                        t=l.split()
                        if len(t)>=2 and t[0].startswith('['): names.add(t[-1])
                        elif not l.strip(): inexp=False
                if names: return names
    return None
def imports(p):
    out=subprocess.run(['x86_64-w64-mingw32-objdump','-p',p],capture_output=True,text=True).stdout.split('\n')
    d=collections.defaultdict(set); cur=None
    for l in out:
        s=l.strip()
        if s.startswith('DLL Name:'): cur=s.split()[-1]
        elif cur and s and s[0] in '0123456789abcdef' and len(s.split())>=3 and s.split()[1].isdigit(): d[cur].add(s.split()[2])
        elif s.startswith('The Import Address Table'): cur=None
    return d
tot=0
for p in sys.argv[2:]:
    for dll,syms in sorted(imports(p).items()):
        ex=exports(dll)
        if ex is None: print(f"{os.path.basename(p)}: {dll}: NOT BUILT/UNKNOWN ({len(syms)} symbols)"); continue
        miss=sorted(s for s in syms if s not in ex and s!='' and s!='<none>' and not (len(s)>=8 and all(c in '0123456789abcdef' for c in s)))
        if miss: print(f"{os.path.basename(p)}: {dll}: missing {len(miss)}: {' '.join(miss)}"); tot+=len(miss)
print("total missing:",tot)
