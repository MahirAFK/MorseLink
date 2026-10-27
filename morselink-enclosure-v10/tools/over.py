import sys, struct, re, math
from collections import defaultdict
def tris(path):
    d = open(path,'rb').read()
    if d[:5] == b'solid' and b'facet' in d[:300]:
        v = [tuple(map(float, m.groups())) for m in re.finditer(rb'vertex\s+(\S+)\s+(\S+)\s+(\S+)', d)]
        return [v[i:i+3] for i in range(0, len(v), 3)]
    n = struct.unpack('<I', d[80:84])[0]
    return [[struct.unpack('<3f', d[84+50*i+12*k:84+50*i+12*k+12]) for k in (1,2,3)] for i in range(n)]
for p in sys.argv[1:]:
    T = tris(p)
    zmin = min(q[2] for t in T for q in t)
    bed = 0; over = defaultdict(float); total_over = 0
    for a,b,c in T:
        ux,uy,uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]; vx,vy,vz = c[0]-a[0], c[1]-a[1], c[2]-a[2]
        nx,ny,nz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx; ar = math.sqrt(nx*nx+ny*ny+nz*nz)/2
        if ar == 0: continue
        nz /= (2*ar)
        zc = (a[2]+b[2]+c[2])/3
        if nz < -0.999 and max(a[2],b[2],c[2]) - zmin < 0.01: bed += ar; continue
        if nz < -0.72:   # steeper than ~46 deg overhang
            total_over += ar
            key = (round((a[0]+b[0]+c[0])/3/5)*5, round((a[1]+b[1]+c[1])/3/5)*5, round(zc,1))
            over[key] += ar
    print(f"\n{p}: z from {zmin:.3f}; on-bed area {bed:.0f} mm2; overhang area (>45 deg, off the bed) {total_over:.1f} mm2")
    for k,v in sorted(over.items(), key=lambda kv: -kv[1])[:8]:
        if v > 0.5: print(f"   near x~{k[0]} y~{k[1]} z={k[2]}: {v:.1f} mm2")
