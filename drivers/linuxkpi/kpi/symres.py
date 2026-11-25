#!/usr/bin/env python3
"""symres.py <serial.log> <modules.list> addr...  : map addresses inside loaded Linux modules to function+offset."""
import re,subprocess,sys
log,lst=sys.argv[1],sys.argv[2]; addrs=[int(a,16) for a in sys.argv[3:]]
mods=open(lst).read().strip().split(';')
secs=[[]]
for l in open(log,errors='ignore'):
    m=re.search(r'LKPI:   sec (\d+) (\S+) at ([0-9A-Fa-f]+) size ([0-9a-f]+)',l)
    if m: secs[-1].append((int(m.group(1)),m.group(2),int(m.group(3),16),int(m.group(4),16)))
    elif 'LKPI: loaded module' in l: secs.append([])
def syms(path):
    out=subprocess.run(['readelf','-sW',path],capture_output=True,text=True).stdout
    r=[]
    for ln in out.splitlines():
        p=ln.split()
        if len(p)>=8 and p[3] in('FUNC','OBJECT','NOTYPE') and p[6].isdigit():
            r.append((int(p[6]),int(p[1],16),int(p[2]) if p[2].isdigit() else 0,p[7]))
    return r
for a in addrs:
    for mi,ss in enumerate(secs[:len(mods)]):
        for idx,name,base,size in ss:
            if base<=a<base+size:
                off=a-base; best=None
                for si,val,sz,nm in syms(mods[mi]):
                    if si==idx and val<=off and (best is None or val>best[0]): best=(val,nm)
                print(hex(a),mods[mi].split('/')[-1],name,hex(off),best[1]+'+'+hex(off-best[0]) if best else '?')
