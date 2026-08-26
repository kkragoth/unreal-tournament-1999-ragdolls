# Plan: topology-based mesh segmentation + rigging (replaces range-based binding)

Status: **PLANNED** (audited 2026-08-25). Supersedes the "proximity-to-bone-box
binding" rework sketched in `docs/handoff.md` — proximity alone cannot tell a
hand from a foot when both rest against the legs; only connectivity can.
Read `docs/handoff.md` + `docs/skinning-findings.md` before implementing.

## Why range-based binding fails (the diagnosis)

`tools/build_rig.py` `derive_bones` classifies verts with absolute coordinate
bands (`arm_thr=11`, elbow=`0.52*H`, ...). Every observed bug traces to a band
cutting through the wrong geometry:

- Feet wider than the arm threshold (`|x|>11`) got labeled "arm column" and
  bound to forearms (SelectionMale2 sole verts x=+17.2/-17.8).
- Elbow/shoulder/pelvis fractions of total height H land differently per model
  (male vs female proportions, Boss armor bulk), so bands cut through limbs.
- Faces whose verts straddle a band ("weld faces") triggered the whole
  drop-and-cap surgery saga — chasing ghosts, see below.

Root cause: **coordinate ranges know nothing about which surface a vertex
belongs to**. Two verts at the same height on different limbs are identical to
a range classifier. Connectivity is not.

## Measured facts (2026-08-25 audits, throwaway scripts, repro in M1)

Ran adjacency/Dijkstra/FPS audits over all 5 Selection meshes frame 0,
yaw-corrected (`topo_audit.py` results):

1. **The meshes are NOT single sheets.** Connected components:
   - SelectionMale2: 3 comps (288 + two 9-v neck/collar islands)
   - SelectionMale1: 4 comps (297 + 16-v back piece + two 8-v hip details)
   - SelectionBoss: **9 comps** — chest(50), pelvis(32), head(36), whole
     L/R arms(50 each), L/R thighs(26), L/R shins(20): armor-piece construction
   - SelectionFemale1: 5 comps (256 + **whole L/R arm sheets, 38 v each**
     + 12-v collar + 4-v hair)
   - SelectionFemale2: 3 comps (290 + two 9-v collar islands)
2. **Zero coincident-position vertices** across all models.
3. **Zero faces bridge a foot patch to any arm-zone patch** (geodesic patch
   test, r=8..10 UU). The 58 "weld faces" in the handoff were artifacts of the
   broken classifier, not real fused triangles. The weld surgery was solving a
   non-existent problem.
4. Geodesic hand->foot path (where both are in the main sheet) runs up over
   the shoulder (97 UU, maxZ=head height): limbs are cleanly separable by
   surface distance.
5. Small floating islands (collars, packs, pouches, hair) cannot be reached by
   connectivity at all — they need proximity binding as a fallback.

## Design

Offline-only change in `tools/build_rig.py`; dataset format, native loader,
`GetFrame` skinning, physics all stay as-is. Rigid 1-bone binding stays.

### New segmentation pipeline (replaces region bands)

1. **Adjacency**: build the vertex graph from faces (wedge-deduped dense
   verts; remap already exists).
2. **Component census**: label connected components. Components >= ~15 verts
   are "body parts"; smaller ones are "accessories".
3. **Landmark seeding per body-part component** (fully automatic, no ranges):
   geodesic farthest-point sampling (FPS) starting from the component's top
   vert yields the extremities (feet, hands, head) as FPS chain points.
4. **Seed naming** (the only coordinate use, and only to NAME seeds):
   highest seed = head-top; lowest pair = L/R foot; remaining outer pair =
   L/R hand. Naming mistakes are visible in renders and fixable via overrides.
5. **Limb Voronoi**: multi-source Dijkstra from all seeds of a component;
   every vert inherits the nearest seed's label (surface distance, edge cost =
   length, optionally x (1+w*crease) dihedral penalty so labels don't creep
   across the armpit fold). Limb labels: armL, armR, legL, legR, torso, head.
6. **Proportional bone splits along each limb's own axis**: order each limb's
   verts by geodesic distance from its TIP seed (hand/foot); split upper/fore
   at the midpoint of the tip->root span; same for thigh/shin. Torso: split
   pelvis/spine/neck/head by geodesic bands from the crotch-most vert.
   Fractions along a segment replace absolute coordinates — they survive
   proportion changes between models.
7. **Accessories**: each accessory component binds rigidly to the nearest
   bone box (sign(x) disambiguates L/R). No connectivity needed — collars sit
   on necks, pouches on hips.
8. **Label smoothing**: one majority-vote pass over the 1-ring kills
   salt-and-pepper singletons at boundaries.
9. **Skeleton from clusters** (replaces `derive_bones` bands): centers =
   per-label centroids (mirror-average L/R pairs as today); halves sized to
   cluster extents; `compute_anchors` reused on the new centers;
   `_enforce_clearance` unchanged.
10. **Bind**: rigid 1-bone = smoothed label + offset (unchanged emit format).
    **Delete `_is_arm_leg_face` face dropping entirely** — fact #3 shows there
    is nothing to drop; boundary faces between adjacent bones are legitimate
    skin (dropping them caused the hole-filled Unity import once already).

### Boss-style fully-split bodies (risk handled by construction)

When NO big main component exists (Boss), steps 3-6 run per component: each
limb-shaped component IS a limb (its own FPS gives tip/root directly), and
torso-stack pieces are recognized by their z-order among components. No
per-model special-casing expected; if a component is ambiguous it degrades to
accessory rules (nearest bone), which is correct for armor plates.

### Verification gates (before any native rebuild)

- Existing: per-bone counts (no starvation), mean offsets, non-adjacent box
  interpenetration, Blender bone-color renders front/3q/back per model.
- New: every body-part label must be geodesically contiguous (exactly 1
  connected run per label per source component); L/R label counts within
  tolerance; accessory assignment report (which bone got which island).
- Visual review by user via the existing PNG/OBJ overlays (no Blender editing
  required, per handoff constraint).

## Milestones

1. **M1 — commit the audit tooling**: formalize the throwaway scripts into
   `tools/topology_audit.py` (component census + FPS extremities + weld-face
   counter) so future model questions get answered by running it.
2. **M2 — implement segmentation** in `build_rig.py` behind `--bind topo`
   (default legacy until verified): steps 1-10 above; keep old path runnable
   for A/B renders.
3. **M3 — verify all 5 models**: assertions + renders; iterate seeds/weights
   until every render reads correctly; flip default to topo; delete the face
   dropper.
4. **M4 — regenerate `RagdollDatasets.h`**, rebuild DLL, deploy to retail
   `System\`, playtest DM-Deck16][ (native-only; no `ucc make` needed).
5. **M5 — docs**: update `docs/handoff.md` (binding rework section),
   `docs/skinning-findings.md` (weld-face correction), AGENTS.md if needed.

## Risks / answers

- **FPS finds feet but misses hands** (hands hang mid-body, never extreme):
  seed candidates = union of several FPS chains (from head-top AND from each
  foot); hands are consistently far-from-feet-but-not-head points. If a seed
  still lands wrong, per-model override JSON (seed vert ids) reviewed via
  renders.
- **Arms-vs-torso leak at the shoulder/armpit**: the Voronoi boundary sits
  where it should (mid-deltoid), because torso seeds pull the chest while the
  hand seed pulls the arm; dihedral crease weight tightens the armpit line.
  Verify visually; tune `w` once globally, not per model.
- **Accessory binds to the wrong side bone**: sign(x) fails only for
  perfectly centered pieces (collars) where either side is fine.
- **Female1's detached 38-v arm sheets**: whole-limb components; step 3-6 per
  component handles them natively (tip = FPS end, root = wrist end near
  torso), no proximity fallback needed.
- **Dataset churn**: emit format unchanged; native code untouched; worst case
  a full regenerate of the header (already scripted).
