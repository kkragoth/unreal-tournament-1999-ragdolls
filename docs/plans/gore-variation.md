# Plan: cut variation, torso tears, modelled stump caps, bone stubs

Status: IMPLEMENTED 2026-10-11 (headless-verified, see the bottom). Written
after the user's feedback on the first gore playtests: "it only tears limbs
at specific points and all looks the same", the planar cuts "have weird
artifacts, triangles fanning out", wants "hand polished" caps, torso tears,
"maybe bones sticking out".

## What was wrong

1. Every cut was AT the joint (the heat-weight seam ring), so a forearm
   always came off at the elbow, a leg at the hip: no variation.
2. The stump cap was a FAN from the rim centroid: a flat disc of bright red
   noise, each fan triangle individually visible under Gouraud lighting, and
   on non-planar rims the fan poked through the skin.
3. Fused regions (armpit, collar) gave non-planar seams because the plane
   was fitted to the heat seam, which runs down the armpit.
4. Spine/pelvis never severed short of a total gib; no torso tears.

## Design

### Cut sites (offline, per model, per bone)

`tools/gore_cuts.py` reads `RagdollDatasets.h` (same loader as
`plane_cut.py`) and, for every severable bone (spine, neck, upper arms,
forearms, thighs, shins), sweeps a cut plane along the bone axis
(`A(t) = Anchor + t * (DistalEnd - Anchor)`, normal = axis) and runs the
SAME classification + clip the DLL runs. A `t` is "clean" when no seam face
needs the duplicated-skirt fallback and each side has exactly one rim loop.
The clean `t` range, the capture band / radius that made it clean, and the
rim radius are written to `RagdollPkg/Src/RagdollCutSites.h`
(`GRagdollCutSites[]`, looked up by dataset name). Hand overrides live in
`tools/rigs/<Sel>.cuts.json` (per bone: `t_min`, `t_max`, `band`,
`radius`, `stub`), so a site can be polished by hand and re-emitted.
Verification: `--obj` writes exploded OBJs (islands + `cap_*` + `bone_*`)
rendered with `tools/render_groups.py` (headless Blender 3.6).

### Runtime

- `Dismember(Body)` picks `t` inside the site's clean range: near where the
  body was last hit (`HitT[body]`, recorded by `RagdollHit`/`RagdollBlast`
  as the hit point projected onto the bone axis in body space), jittered by
  `CutVariation` (globalconfig, 0 = always at the joint like before, 1 =
  default). The plane `(A(t), N)` goes into `JointPlanes[c]`; the rest of
  the clipping pipeline (`RagdollSplitIslands`) is unchanged. Skin above the
  plane that was limb-dominated stays with the parent island as a stump
  (no new physics body); the limb's box hull is left as is.
- **Modelled caps** replace the fan: the planar rim of each island is traced
  into an ordered loop and capped with a recessed meat dish (rim -> inner
  ring quad strip, inner ring -> bone-base ring bridge), and a bone
  cylinder with a jagged broken tip. Protrusion per side: the stump side
  gets the broken shaft sticking out 3-6 UU with `BoneStubChance`
  (globalconfig, default 0.7), the limb side a flush cross-section (or the
  rounded bone head on joint-level cuts). Rims that are not a single loop
  (fallback skirts) keep the small centroid fan.
- **Wound atlas**: `tools/build_wound_texture.py` paints a 64x64 palettised
  atlas (meat band / bone band / clot band, tileable in U) with mips into
  `RagdollPkg/Src/RagdollWoundTexture.h` and a PNG preview
  (`tools/rig_verify/wound_atlas.png`). `--from <png>` quantises a
  hand-painted replacement. The runtime texture (`RagdollCapTexture`) is
  built from the header instead of 16x16 noise.
- **Torso**: the spine's sever threshold is a knob (`TorsoSeverDamage`,
  default 130 ledger units: two chest rockets, one point-blank flak shell).
  `ApplySever`/`TearOff` accept `BONE_Spine`: the upper body (spine, neck,
  head, arms) comes off at a waist/lower-rib site, legs + pelvis stay as
  the actor's island. The detached torso half is no longer hittable (the
  hit cylinder tracks the pelvis island only).
- `Gib` goes through the same site selection, so gibs vary too.
- `bPlanarCuts` default flips to true (the modelled caps need planar rims).

### Not done (follow-ups)

- Distal chops that need a NEW body (hand off at the wrist, foot at the
  ankle, top of the skull): the fragment-body path exists
  (`RagdollFragmentBody`), needs authored planes + hull sizing.
- Diagonal chest cuts carrying a shoulder + arm: need re-jointing the arm
  onto the chunk body (destroy + recreate the ball joint).
- Hand-painted wound atlas (the tool accepts a PNG).
- Blender MCP was not connected during this work; the OBJ/PNG verification
  path is headless Blender.

## Status (2026-10-11 night)

- "Bones should stick out of the more important part": the BODY (chest,
  pelvis, upper arm, thigh) keeps the protruding shaft, the severed piece
  gets the flush cross-section (`BuildSplitMesh`, `StubOnParent` default
  on via `BoneStubChance` 1.0; lower it to let the limb carry the bone
  sometimes). Joint-level cuts: body = short shaft, limb = bone head.
- Repeated cuts no longer re-split earlier caps: only the dataset's
  original vertices (`FRagdollState::NumOrigVerts`) are placed by the cut
  planes; clip points, skirt copies and cap geometry stay with the bone
  they were bound to. Headless: ten `Dismember` calls grow the Commando
  mesh 1292 -> ~4800-5000 verts (it reached 375k before the fix).
- The D:\old install's ini had `bPlanarCuts=False` persisted from the
  first gore playtest, so the modelled caps never showed there; removed.
- Still unverified visually in the client (headless OBJ renders only).
- 2026-10-11 night review of the planar split + template caps
  (RagdollGore.cpp step 3): the DLL chose the cap's outward direction as
  "away from the island root's box centre", which for the SEVERED piece
  (island root = the cut bone, box centre mid-bone) points the wrong way
  on every cut distal of the box centre: forearm/shin t >= 0.8, upper
  arm t >= 0.55, thigh t >= 0.75, spine t = 0.6 on all five models
  (the playtest log had ~40% of its cuts there). The meat dish was built
  inside the limb and the rim stayed an open hole. Fixed: joint planes cap
  the child island toward -N and the parent toward +N (N is distal);
  fragment planes keep the centre test. Rims with a mean radius under
  1.5 UU (straps, antenna, pouch tubes crossed by the same plane) get a
  centroid fan instead of a template inflated to a 1 UU radius. The
  offline mirror (`gore_cuts.py build_caps`) always used the correct
  sign, so the Blender renders never showed the bug.
