# Planar dismemberment cuts: findings (2026-10-10)

Measured offline with `tools/plane_cut.py` on the five datasets in
`RagdollPkg/Src/RagdollDatasets.h` (the exact arrays the DLL sees). Read this
before changing how cut planes are chosen.

## 1. The shoulders sit ABOVE the neck joint anchor (important)

The handoff plan said: "define the cut plane at the joint anchor
(`DS->Anchors[...]`) with the normal along the limb axis". That does not work
for the neck, and only partly for the shoulders:

- The spine-neck anchor is at the TOP OF THE SPINE, i.e. at the back and
  low: Male1 `(0, -3.3, 24.8)`. The shoulder anchors are at `z = 22.6` and
  the trapezius / shoulder tops rise to `z ~ 26-27`. So a plane through the
  neck anchor passes through the shoulders, not through the neck. Counted
  spine-dominated vertices on the *neck side* of such a plane (Male1):
  95 with the anchor->neck-centre normal, 60 with a vertical normal, with
  perpendicular distances 4-13 UU from the axis (the whole shoulder slope).
- The neck box centre is forward of the anchor (`y = -1.0` vs `-3.3`), so the
  anchor->centre normal tilts the neck plane 37 deg forward (Male1), 19 deg
  (Female1), 12 deg (Male2/Female2). The real neck cylinder is near vertical.
- The same holds for every model (Male2, Boss, Female1, Female2): the neck
  anchor is below the shoulder tops in all five.

Consequence: the plane point must come from the MESH, not the skeleton. The
implemented rule fits the plane to the heat-weight seam ring: point = centroid
of the vertices on faces straddling the child island and the parent island,
normal = from that point toward the centroid of the child subtree's vertices.
For the neck this lands the plane at `z ~ 26.5`, through the actual neck
base; for the thigh at the hip crease; for the spine at the waist.

## 2. Where a plane can never be clean: fused regions

With the fitted planes most seams clip perfectly (limb/knee/spine/hip cuts:
0 non-planar seam faces on Male1). What is left is where two body parts are
welded in the Selection mesh:

- **Armpit**: the hanging upper arm is fused to the flank. The heat boundary
  runs *down the armpit* for ~10 UU below the shoulder, so the torso verts
  there are on the arm side of any shoulder plane (Male1: spine verts with
  d = 2..10 UU, 11-14 UU from the arm axis). ~20-38 seam faces per shoulder
  stay non-planar on every model.
- **Neck/head** on Male2 and Female1: the head-dominated region reaches into
  the collar; 20-30 faces per cut.
- **Crotch** (hip cuts): a couple of three-island faces.

Those faces use the old per-island copy + duplicated foreign vertices, and
their rim is fan-capped from the centroid only for edges within
`1.5 * limb radius + 2` of it, so the fan cannot reach through the skin.

## 3. Numbers (seam faces: planar / fallback)

| model | gib (all 11 joints) | shoulders+neck | head | hips | knees | spine |
|---|---|---|---|---|---|---|
| Male1 | 336 / 80 | 118 / 69 | 33 / 12 | 49 / 0 | 44 / 0 | 57 / 0 |
| Male2 | 290 / 88 | 104 / 85 | 21 / 29 | 43 / 11 | 32 / 0 | 32 / 22 |
| Boss | 380 / 87 | 103 / 53 | 41 / 13 | 75 / 2 | 93 / 0 | 46 / 0 |
| Female1 | 317 / 36 | 103 / 29 | 36 / 21 | 56 / 2 | 76 / 0 | 45 / 0 |
| Female2 | 296 / 48 | 109 / 45 | 43 / 0 | 55 / 3 | 42 / 0 | 53 / 0 |

Before this change every seam face was in the "fallback" column.

## 4. In-game (headless `ucc server`, Commando, 2026-10-10)

The DLL reproduces the prototype numbers exactly: forearm cut 22/22 planar,
thigh 24/24, decapitation 33 planar / 12 fallback, full gib at 199 damage
279 / 59 before fragmentation and 389 / 23 after (7 fragment bodies, 1292
-> 2963 verts, 2568 -> 5348 faces). Fitted planes (bind space):

| joint | A | N | W | R |
|---|---|---|---|---|
| spine (1) | (0.3 -0.3 3.9) | (0.03 -0.10 0.99) | 4.0 | 8.3 |
| neck (2) | (-0.2 -1.4 27.6) | (-0.04 0.80 0.60) | 7.0 | 7.4 |
| head (3) | (-0.4 0.6 30.7) | (0.22 0.43 0.87) | 2.7 | 4.3 |
| shoulder L (4) | (-10.5 -4.0 22.3) | (-0.46 -0.41 -0.78) | 5.1 | 7.4 |
| elbow L (6) | (-15.1 -7.5 9.8) | (-0.10 0.20 -0.97) | 2.9 | 4.1 |
| hip R (9) | (4.6 1.6 -1.2) | (0.25 0.08 -0.96) | 8.1 | 6.5 |
| knee L (10) | (-10.0 -1.4 -14.6) | (-0.14 -0.17 -0.98) | 4.7 | 5.0 |

Note the neck normal still tilts 37 deg forward (the Commando's neck+head
centroid is forward of the neck base); it reads fine because the plane
point is now at the neck base, not at the shoulders.

The plane NORMAL is taken from the whole limb below the joint by the bone
tree, not by the current island: with the island, a thigh plane in a full
gib (shin cut too) came out 70 deg tilted because the thigh-dominated
vertices alone are concentrated at the hip.

## 5. Wound marks on corpse hits

Script always painted on every `TakeDamage`, but the hit location of a
hitscan shot is on the corpse's collision cylinder, which for a lying
corpse can be 20-40 UU from the skin; the nearest-face lookup then marked
the wrong place or missed (45 UU cutoff). `PaintWound` now takes the shot
direction (hit momentum) and picks the face nearest the ray, entry side
first. Headless: a ray from 80 UU away lands 1.1 UU off the ray on the
chest, from 60 UU above the head 0.9 UU off. Log lines carry `ray=1`.
