"""Map each live player mesh's vertices onto its Selection-viewer dataset.

The ragdoll datasets (RagdollDatasets.h) were derived from the Selection
viewer meshes (SelectionMale1, ...), whose frame 0 is the arms-down bind
pose. The in-game pawn meshes (Commando, ...) are the SAME model with the
same vertex and face counts, but re-exported: their vertex numbering and UV
wedges differ, so dataset vertex k is NOT live vertex k.

This tool recovers the vertex permutation from the triangle connectivity
alone. Both meshes are the same set of oriented 2-manifold pieces (the main
body plus detached pads / hair / armour plates), so once ONE oriented face
of a piece is matched the mapping propagates across shared edges: the third
vertex of the face on the other side of a matched directed edge must match
too. Pieces are matched largest first; every face of the candidate piece is
tried as the partner of the first face, and the complete, conflict-free
bijections that come out are that piece's automorphisms. Ambiguities are
resolved with geometry: the main body by the dataset's own left/right bone
labels on a standing live frame (Dead1 frame 0), the small pieces (two
identical shoulder pads) by predicting where each candidate's dataset verts
land under the per-body rigid fit of the main body and taking the closest.
A per-body rigid fit (Horn 1987) of that frame finally reports how rigid
each ragdoll body is under the mapping (a wrong mapping scatters the
residuals to 10+ UU).

The result is RagdollPkg/Src/RagdollLiveMaps.h: per live mesh, the dataset
vertex index of every live model vertex (wedge iVertex order, SpecialVerts
excluded). The native pose fit (RagdollCarcass.cpp, ApplyAnimationPose)
uses it to read the dying pawn's current animation frame through the
dataset's per-vertex bone binds.

Usage:
    python tools/build_live_map.py [--pkg <BotPack.u>]
        [--datasets RagdollPkg/Src/RagdollDatasets.h]
        [--header RagdollPkg/Src/RagdollLiveMaps.h]
"""

import argparse
import math
import os
import re
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_mesh import UTPackage, parse_lod_mesh, find_meshes, mesh_to_world  # noqa: E402

DEFAULT_PKG = r"C:\Users\kkragoth\dev\Unreal Tournament\Unreal Tournament\System\BotPack.u"
MODEL_MAP = [
    ("Commando", "SelectionMale1"),
    ("Soldier", "SelectionMale2"),
    ("Boss", "SelectionBoss"),
    ("SGirl", "SelectionFemale2"),
    ("FCommando", "SelectionFemale1"),
]
BONE_NAMES = ["Pelvis", "Spine", "Neck", "Head", "UpArmL", "UpArmR",
              "ForArmL", "ForArmR", "ThighL", "ThighR", "ShinL", "ShinR",
              "FootL", "FootR"]


def face_triples(m):
    """Faces as oriented model-vertex triples (wedge iVertex, SpecialVerts excluded)."""
    return [tuple(m.wedges[w][0] for w in f[:3]) for f in m.faces]


def directed_edges(tris):
    """(u,v) -> third vertex w of the face (u,v,w) in winding order; dropped when ambiguous."""
    d = {}
    dup = set()
    for a, b, c in tris:
        for u, v, w in ((a, b, c), (b, c, a), (c, a, b)):
            if (u, v) in d:
                dup.add((u, v))
            d[(u, v)] = w
    for e in dup:
        d.pop(e, None)
    return d


def components(tris, n):
    """Connected components over face adjacency: label per vertex, faces per label."""
    adj = [set() for _ in range(n)]
    for a, b, c in tris:
        adj[a].update((b, c))
        adj[b].update((a, c))
        adj[c].update((a, b))
    label = [-1] * n
    k = 0
    for s in range(n):
        if label[s] >= 0:
            continue
        stack = [s]
        label[s] = k
        while stack:
            u = stack.pop()
            for v in adj[u]:
                if label[v] < 0:
                    label[v] = k
                    stack.append(v)
        k += 1
    faces = [[] for _ in range(k)]
    for t in tris:
        faces[label[t[0]]].append(t)
    verts = [[] for _ in range(k)]
    for v, l in enumerate(label):
        if l >= 0:
            verts[l].append(v)
    return label, faces, verts


def propagate(seed, dA, dB, fwd, rev):
    """Grow fwd/rev from one matched oriented face across shared edges (in place; None on conflict)."""
    def bind(a, b):
        if fwd[a] == -1 and rev[b] == -1:
            fwd[a] = b
            rev[b] = a
            return True
        return fwd[a] == b and rev[b] == a

    (a0, a1, a2), (b0, b1, b2) = seed
    for a, b in ((a0, b0), (a1, b1), (a2, b2)):
        if not bind(a, b):
            return None
    stack = [(a0, a1), (a1, a2), (a2, a0)]
    seen = set()
    while stack:
        u, v = stack.pop()
        if (u, v) in seen:
            continue
        seen.add((u, v))
        for (p, q) in ((u, v), (v, u)):
            w = dA.get((p, q))
            if w is None:
                continue
            w2 = dB.get((fwd[p], fwd[q]))
            if w2 is None:
                continue
            if not bind(w, w2):
                return None
            stack.append((p, w))
            stack.append((w, q))
    return fwd


def complete_piece(facesA, facesB, dA, dB, fwd, rev):
    """Re-seed across edges the directed-edge map dropped (non-manifold spots):
    an A face with two mapped corners and one unmapped corner is matched to the
    B face on the same mapped edge whose third corner is still free."""
    byedgeB = {}
    for t in facesB:
        for k in range(3):
            u, v, w = t[k], t[(k + 1) % 3], t[(k + 2) % 3]
            byedgeB.setdefault((min(u, v), max(u, v)), []).append(w)
    progress = True
    while progress:
        progress = False
        for t in facesA:
            mapped = [c for c in t if fwd[c] >= 0]
            if len(mapped) != 2:
                continue
            w = [c for c in t if fwd[c] < 0][0]
            u, v = fwd[mapped[0]], fwd[mapped[1]]
            cands = [w2 for w2 in byedgeB.get((min(u, v), max(u, v)), []) if rev[w2] < 0]
            if len(cands) != 1:
                continue
            fwd[w] = cands[0]
            rev[cands[0]] = w
            progress = True
            # grow the rest of the way from the newly matched face
            for k in range(3):
                a0, a1 = t[k], t[(k + 1) % 3]
                dummy = propagate(((a0, a1, t[(k + 2) % 3]), (fwd[a0], fwd[a1], fwd[t[(k + 2) % 3]])), dA, dB, fwd, rev)
                if dummy is None:
                    return None
    return fwd


def piece_mappings(facesA, vertsA, facesB, vertsB, nA, nB):
    """All bijections of piece A onto piece B (as {a: b} dicts)."""
    dA = directed_edges(facesA)
    dB = directed_edges(facesB)
    setA = set(vertsA)
    facesetB = set(frozenset(t) for t in facesB)
    out = {}
    for seedA in facesA[:6]:   # a few seeds: a non-manifold edge may block one
        for b in facesB:
            for r in range(3):
                bb = (b[r], b[(r + 1) % 3], b[(r + 2) % 3])
                fwd = [-1] * nA
                rev = [-1] * nB
                if propagate((seedA, bb), dA, dB, fwd, rev) is None:
                    continue
                if any(fwd[a] < 0 for a in vertsA):
                    if complete_piece(facesA, facesB, dA, dB, fwd, rev) is None:
                        continue
                if any(fwd[a] < 0 for a in vertsA):
                    continue
                m = {a: fwd[a] for a in vertsA}
                if set(m.values()) != set(vertsB) or set(m.keys()) != setA:
                    continue
                # every A face must land on a B face
                if any(frozenset(m[c] for c in t) not in facesetB for t in facesA):
                    continue
                out[tuple(sorted(m.items()))] = m
        if out:
            break
    return list(out.values())


def parse_header_arrays(path, sel):
    txt = open(path, encoding="utf-8", errors="replace").read()

    def block(kind, name):
        m = re.search(r"static const %s G%s\[(\d+)\] = \{(.*?)\};" % (kind, re.escape(name)), txt, re.S)
        if not m:
            raise RuntimeError("missing " + name)
        return m.group(2)

    def vec_array(name):
        vals = re.findall(r"FVector\(([-\d.eE+]+), ([-\d.eE+]+), ([-\d.eE+]+)\)", block("FVector", name))
        return [tuple(float(x) for x in v) for v in vals]

    def int_array(name):
        return [int(x) for x in re.findall(r"-?\d+", block("INT", name))]

    def float_array(name):
        return [float(x) for x in re.findall(r"[-\d.eE+]+", block("FLOAT", name).replace("f", ""))]

    def bones(name):
        rows = re.findall(r"\{\s*([^}]*)\}", block("FRagdollBoneDef", name))
        return [[float(x) for x in re.findall(r"[-\d.eE+]+", r.replace("f", ""))] for r in rows]

    return {
        "verts": vec_array(sel + "_Verts"),
        "bones": int_array(sel + "_BindBones"),
        "offs": vec_array(sel + "_BindOffsets"),
        "weights": float_array(sel + "_BindWeights"),
        "defs": bones(sel + "_Bones"),
    }


def jacobi_eigen4(A):
    """Eigenvector of the largest eigenvalue of a symmetric 4x4 (cyclic Jacobi)."""
    n = 4
    V = [[1.0 if i == j else 0.0 for j in range(n)] for i in range(n)]
    A = [row[:] for row in A]
    for _ in range(60):
        off = sum(A[i][j] ** 2 for i in range(n) for j in range(n) if i != j)
        if off < 1e-18:
            break
        for p in range(n):
            for q in range(p + 1, n):
                if abs(A[p][q]) < 1e-15:
                    continue
                theta = (A[q][q] - A[p][p]) / (2.0 * A[p][q])
                t = (1.0 if theta >= 0 else -1.0) / (abs(theta) + math.sqrt(theta * theta + 1.0))
                c = 1.0 / math.sqrt(t * t + 1.0)
                s = t * c
                for k in range(n):
                    akp, akq = A[k][p], A[k][q]
                    A[k][p] = c * akp - s * akq
                    A[k][q] = s * akp + c * akq
                for k in range(n):
                    apk, aqk = A[p][k], A[q][k]
                    A[p][k] = c * apk - s * aqk
                    A[q][k] = s * apk + c * aqk
                for k in range(n):
                    vkp, vkq = V[k][p], V[k][q]
                    V[k][p] = c * vkp - s * vkq
                    V[k][q] = s * vkp + c * vkq
    best = max(range(n), key=lambda i: A[i][i])
    return [V[k][best] for k in range(n)]


def quat_rotate(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty),
            v[1] + w * ty + (z * tx - x * tz),
            v[2] + w * tz + (x * ty - y * tx))


def horn_fit(P, Q, W):
    """Weighted rigid fit P->Q (Horn 1987, unit quaternion). Returns (rms, q(x,y,z,w), t) or None."""
    sw = sum(W)
    if len(P) < 3 or sw <= 0:
        return None
    cp = [sum(p[i] * w for p, w in zip(P, W)) / sw for i in range(3)]
    cq = [sum(q[i] * w for q, w in zip(Q, W)) / sw for i in range(3)]
    S = [[0.0] * 3 for _ in range(3)]
    for p, q, w in zip(P, Q, W):
        dp = [p[i] - cp[i] for i in range(3)]
        dq = [q[i] - cq[i] for i in range(3)]
        for i in range(3):
            for j in range(3):
                S[i][j] += w * dp[i] * dq[j]
    Sxx, Sxy, Sxz = S[0]
    Syx, Syy, Syz = S[1]
    Szx, Szy, Szz = S[2]
    N = [
        [Sxx + Syy + Szz, Syz - Szy, Szx - Sxz, Sxy - Syx],
        [Syz - Szy, Sxx - Syy - Szz, Sxy + Syx, Szx + Sxz],
        [Szx - Sxz, Sxy + Syx, -Sxx + Syy - Szz, Syz + Szy],
        [Sxy - Syx, Szx + Sxz, Syz + Szy, -Sxx - Syy + Szz],
    ]
    w, x, y, z = jacobi_eigen4(N)
    nrm = math.sqrt(w * w + x * x + y * y + z * z)
    q = (x / nrm, y / nrm, z / nrm, w / nrm)
    rcp = quat_rotate(q, cp)
    t = [cq[i] - rcp[i] for i in range(3)]
    err = 0.0
    for p, qq, wt in zip(P, Q, W):
        r = quat_rotate(q, p)
        err += wt * sum((r[i] + t[i] - qq[i]) ** 2 for i in range(3))
    return math.sqrt(err / sw), q, t


def frame_world(m, f):
    base = f * m.frame_verts + m.special_verts
    return [mesh_to_world(m.verts[base + k], m) for k in range(m.frame_verts - m.special_verts)]


def seq_start(m, pkg, name):
    for s in m.anim_seqs:
        if pkg.name(s[0]) == name:
            return s[2]
    return None


def bind_pos(ds, d, slot):
    b = ds["bones"][d * 3 + slot]
    c = ds["defs"][b][3:6]
    o = ds["offs"][d * 3 + slot]
    return (c[0] + o[0], c[1] + o[1], c[2] + o[2])


def fit_bodies(fwd_items, ds, W):
    """Per-body rigid fit of mapped live verts (a -> dataset d). Returns {b: (n, rms, q, t)}."""
    out = {}
    for b in range(12):
        P, Q, Wt = [], [], []
        for a, d in fwd_items:
            for slot in range(3):
                if ds["bones"][d * 3 + slot] == b and ds["weights"][d * 3 + slot] >= 0.25:
                    P.append(bind_pos(ds, d, slot))
                    Q.append(W[a])
                    Wt.append(ds["weights"][d * 3 + slot])
        r = horn_fit(P, Q, Wt)
        out[b] = (len(P), r[0], r[1], r[2]) if r else (len(P), None, None, None)
    return out


def lr_sign(fwd_items, ds, W):
    """Dataset left arm minus right arm mean world Y (bind -X -> world +Y at yaw 0)."""
    def mean_y(bone):
        ys = [W[a][1] for a, d in fwd_items if ds["bones"][d * 3] == bone]
        return sum(ys) / len(ys) if ys else 0.0
    return mean_y(4) - mean_y(5)


def predict_error(m, ds, fits, W):
    """Mean distance between live verts and their dataset verts pushed through the body fits."""
    tot, n = 0.0, 0
    for a, d in m.items():
        b = ds["bones"][d * 3]
        _, rms, q, t = fits.get(b, (0, None, None, None))
        if q is None:
            continue
        p = quat_rotate(q, bind_pos(ds, d, 0))
        p = [p[i] + t[i] for i in range(3)]
        tot += math.dist(p, W[a])
        n += 1
    return tot / n if n else 1e9


def emit_header(entries, path):
    out = []
    out.append("// Generated by tools/build_live_map.py. DO NOT EDIT.")
    out.append("// Live player mesh vertex -> dataset vertex (RagdollDatasets.h) for the")
    out.append("// death-animation pose fit. Index = live model vertex (wedge iVertex,")
    out.append("// SpecialVerts excluded), value = dataset vertex index.")
    out.append("#pragma once")
    out.append("")
    out.append("#include \"RagdollInternal.h\"")
    out.append("")
    for live, sel, fwd in entries:
        out.append("static const INT G%s_LiveMap[%d] = {" % (live, len(fwd)))
        for i in range(0, len(fwd), 20):
            out.append("\t" + ", ".join(str(v) for v in fwd[i:i + 20]) + ",")
        out.append("};")
        out.append("")
    out.append("static const FRagdollLiveMap GRagdollLiveMapTable[] = {")
    for live, sel, fwd in entries:
        out.append("\t{ TEXT(\"%s\"), TEXT(\"%s\"), %d, G%s_LiveMap }," % (live, sel, len(fwd), live))
    out.append("};")
    out.append("static const INT GRagdollLiveMapCount = %d;" % len(entries))
    out.append("")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out))


def map_model(A, B, ds, frame, log):
    nA = A.frame_verts - A.special_verts
    nB = B.frame_verts - B.special_verts
    trisA, trisB = face_triples(A), face_triples(B)
    if nA != nB or len(trisA) != len(trisB):
        log("  SKIP: different topology")
        return None
    labA, facesA, vertsA = components(trisA, nA)
    labB, facesB, vertsB = components(trisB, nB)
    log(f"  pieces live={sorted((len(v) for v in vertsA), reverse=True)} sel={sorted((len(v) for v in vertsB), reverse=True)}")
    W = frame_world(A, frame)

    # Every piece's options: (B piece, mapping) over same-size candidates.
    options = {}
    for ci in range(len(vertsA)):
        opts = []
        for cj in range(len(vertsB)):
            if len(vertsB[cj]) != len(vertsA[ci]) or len(facesB[cj]) != len(facesA[ci]):
                continue
            for m in piece_mappings(facesA[ci], vertsA[ci], facesB[cj], vertsB[cj], nA, nB):
                opts.append((cj, m))
        if not opts:
            log(f"  piece of {len(vertsA[ci])} verts / {len(facesA[ci])} faces: NO MATCH")
            return None
        options[ci] = opts

    fwd = {}
    usedB = set()
    pending = sorted(options, key=lambda ci: -len(vertsA[ci]))
    # 1) pieces with a single option are certain.
    for ci in list(pending):
        if len(options[ci]) == 1:
            cj, m = options[ci][0]
            fwd.update(m)
            usedB.add(cj)
            pending.remove(ci)
    log(f"  unique pieces: {len(options) - len(pending)}/{len(options)} ({len(fwd)} verts)")
    # 2) no certain piece: the largest piece decides by the left/right test.
    if not fwd:
        ci = pending[0]
        scored = []
        for cj, m in options[ci]:
            items = list(m.items())
            lr = lr_sign(items, ds, W)
            f = fit_bodies(items, ds, W)
            tot_n = sum(n for n, r, q, t in f.values())
            tot = sum((r or 0) * n for n, r, q, t in f.values()) / max(1, tot_n)
            scored.append(((0 if lr > 0 else 1, tot), cj, m, lr))
        scored.sort(key=lambda o: o[0])
        _, cj, m, lr = scored[0]
        log(f"  largest piece {len(vertsA[ci])} verts: {len(scored)} option(s), LR={lr:+.1f}")
        if lr <= 0:
            log("  WARNING: no mapping puts the dataset's left arm on the live mesh's left")
        fwd.update(m)
        usedB.add(cj)
        pending.remove(ci)
    lr = lr_sign(list(fwd.items()), ds, W)
    if lr <= 0:
        log(f"  WARNING: left/right check failed on the certain pieces (LR={lr:+.1f})")
    # 3) ambiguous pieces: predict with the body fit of what is mapped so far.
    while pending:
        fits = fit_bodies(list(fwd.items()), ds, W)
        ci = pending.pop(0)
        scored = []
        for cj, m in options[ci]:
            if cj in usedB:
                continue
            scored.append((predict_error(m, ds, fits, W), cj, m))
        if not scored:
            log(f"  piece {len(vertsA[ci])} verts: all candidates taken")
            return None
        scored.sort(key=lambda o: o[0])
        alt = f" (next {scored[1][0]:.1f})" if len(scored) > 1 else ""
        log(f"  piece {len(vertsA[ci])} verts: {len(scored)} option(s), predicted err {scored[0][0]:.1f} UU{alt}")
        fwd.update(scored[0][2])
        usedB.add(scored[0][1])
    if len(fwd) != nA or len(set(fwd.values())) != nA:
        log("  ERROR: incomplete mapping")
        return None
    # Whole-mesh check: every live face maps onto a dataset face.
    facesetB = set(frozenset(t) for t in trisB)
    bad = sum(1 for t in trisA if frozenset(fwd[c] for c in t) not in facesetB)
    if bad:
        log(f"  ERROR: {bad} live faces do not map onto dataset faces")
        return None
    return [fwd[a] for a in range(nA)]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pkg", default=DEFAULT_PKG)
    ap.add_argument("--datasets", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "RagdollPkg", "Src", "RagdollDatasets.h"))
    ap.add_argument("--header", default=None)
    ap.add_argument("--frame-seq", default="Dead1", help="standing live sequence whose frame 0 verifies the fit")
    args = ap.parse_args(argv)

    pkg = UTPackage(args.pkg)
    meshes = {}
    for i, e, cn in find_meshes(pkg):
        meshes[pkg.name(e.object_name)] = i
    entries = []
    ok = True
    for live, sel in MODEL_MAP:
        A = parse_lod_mesh(pkg, meshes[live])
        B = parse_lod_mesh(pkg, meshes[sel])
        print(f"{live} -> {sel}: verts {A.frame_verts - A.special_verts}/{B.frame_verts - B.special_verts} faces {len(A.faces)}/{len(B.faces)}")
        ds = parse_header_arrays(args.datasets, sel)
        frame = seq_start(A, pkg, args.frame_seq) or 0
        fwd = map_model(A, B, ds, frame, print)
        if fwd is None:
            ok = False
            continue
        W = frame_world(A, frame)
        fits = fit_bodies(list(enumerate(fwd)), ds, W)
        cells = []
        worst = 0.0
        for b in range(12):
            n, r, q, t = fits[b]
            cells.append(f"{BONE_NAMES[b]}:{n}v/{r:.1f}" if r is not None else f"{BONE_NAMES[b]}:{n}v/-")
            worst = max(worst, r or 0)
        print(f"  {args.frame_seq}+0 per-body rigid rms (UU): " + " ".join(cells))
        # Facing check: the fitted pelvis/spine rotation carries the dataset's
        # front (+Y) onto the live pawn's facing, which at yaw 0 is world +X
        # (0 deg). ~180 deg means that dataset was built facing backwards
        # (its left/right bone labels are swapped as well).
        for b in (0, 1):
            n, r, q, t = fits[b]
            if q is not None:
                f = quat_rotate(q, (0.0, 1.0, 0.0))
                deg = math.degrees(math.atan2(f[1], f[0]))
                flag = "  <-- dataset faces BACKWARDS" if abs(deg) > 135 else ""
                print(f"  {BONE_NAMES[b]} fitted facing of the dataset front: {deg:+.0f} deg (0 = pawn forward){flag}")
        if worst > 8.0:
            print("  WARNING: a body's residual is above 8 UU; mapping suspect")
            ok = False
        entries.append((live, sel, fwd))
    if args.header:
        emit_header(entries, args.header)
        print(f"wrote {args.header} ({len(entries)} maps)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
