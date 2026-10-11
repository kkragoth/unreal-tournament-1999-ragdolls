import sys, os, re, math
sys.path.insert(0, 'tools')
from plane_cut import load_dataset, PARENT, NAMES
hdr = 'RagdollPkg/Src/RagdollDatasets.h'
text = open(hdr, encoding='utf-8', errors='replace').read()
sels = sorted(set(re.findall(r'static const FVector G(\w+)_Verts', text)))
ARM = {4,5,6,7}; LEG = {8,9,10,11,12,13}
def adjacent(a, b):
    return a == b or PARENT[a] == b or PARENT[b] == a
for sel in sels:
    ds = load_dataset(hdr, sel)
    V, F, W, binds = ds['verts'], ds['faces'], ds['wedges'], ds['binds']
    pairs = {}
    bad = []
    for fi, (w0, w1, w2, m) in enumerate(F):
        vs = [W[w][0] for w in (w0, w1, w2)]
        dom = [binds[v][0][0] for v in vs]
        for i in range(3):
            for j in range(i+1, 3):
                a, b = dom[i], dom[j]
                if not adjacent(a, b):
                    key = tuple(sorted((a, b)))
                    pairs[key] = pairs.get(key, 0) + 1
                    bad.append((fi, vs, dom))
                    break
            else:
                continue
            break
    # verts blending arm+leg
    mix = [i for i, (bb, ww) in enumerate(binds) if any(b in ARM and w > 0.05 for b, w in zip(bb, ww)) and any(b in LEG and w > 0.05 for b, w in zip(bb, ww))]
    print(f'== {sel}: verts={len(V)} faces={len(F)} spanning_faces={len(bad)} arm+leg blended verts={len(mix)}')
    for (a, b), n in sorted(pairs.items(), key=lambda kv: -kv[1]):
        print(f'   {NAMES[a]}-{NAMES[b]}: {n}')
    for fi, vs, dom in bad[:6]:
        print('   face', fi, [(v, NAMES[d], tuple(round(c,1) for c in V[v])) for v, d in zip(vs, dom)])
    if mix:
        print('   mixed verts sample:', [(i, binds[i]) for i in mix[:4]])
print('==== non-adjacent vertex blends')
for sel in sels:
    ds = load_dataset(hdr, sel)
    n = 0; ex = []
    for i, (bb, ww) in enumerate(ds['binds']):
        bs = [b for b, w in zip(bb, ww) if w > 0.02 and b >= 0]
        for a in bs:
            for b in bs:
                if a < b and not adjacent(a, b):
                    n += 1; ex.append((i, NAMES[a], NAMES[b], tuple(round(c,1) for c in ds['verts'][i]), ww))
    print(sel, n, ex[:5])
