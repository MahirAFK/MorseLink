import sys, struct, re
def tris(path):
    d = open(path,'rb').read()
    if d[:5] == b'solid' and b'facet' in d[:300]:
        v = [tuple(map(float, m.groups())) for m in re.finditer(rb'vertex\s+(\S+)\s+(\S+)\s+(\S+)', d)]
        return [v[i:i+3] for i in range(0, len(v), 3)]
    n = struct.unpack('<I', d[80:84])[0]; out = []
    for i in range(n):
        f = struct.unpack('<12f', d[84+50*i:84+50*i+48]); out.append([f[3:6], f[6:9], f[9:12]])
    return out
for p in sys.argv[1:]:
    T = tris(p); vol = 0; zs = []; xs=[]; ys=[]
    for a, b, c in T:
        vol += (a[0]*(b[1]*c[2]-b[2]*c[1]) - a[1]*(b[0]*c[2]-b[2]*c[0]) + a[2]*(b[0]*c[1]-b[1]*c[0]))/6
        for q in (a,b,c): xs.append(q[0]); ys.append(q[1]); zs.append(q[2])
    print(f"{p}: volume {abs(vol):.3f} mm3, x {min(xs):.2f}..{max(xs):.2f}, y {min(ys):.2f}..{max(ys):.2f}, z {min(zs):.2f}..{max(zs):.2f}")
