# T-pose + Blender auto-rig workflow (ACTIVE)

Status date: 2026-10-10. This is the rig source for all five
`RagdollDatasets.h` entries. It supersedes `docs/mixamo-rig-workflow.md`
(Mixamo auto-rig on the arms-down mesh) and the geometric binders in
`tools/build_rig.py` (`derive_bones` bands, `--bind topo`).

## Why

Mixamo was fed the arms-down Selection viewer meshes. Its auto-rigger assumes
normal arm length, so it placed the hand bones at thigh height and bound the
real hands (which hang down to the thigh/knee) to the **leg** bones: 16-30
"wide, low" verts per model were weighted to UpLeg/Leg/Foot. In game that is
the "hands stitched to the legs" artifact (hands follow the thigh, forearms
tear). The earlier in-house binders mis-labelled wide feet as forearms
(`|x| > 11` sweep) or cut arms mid-biceps.

## What the pipeline does (`tools/tpose_rig.py`)

Per Selection mesh, fully offline, deterministic:

1. **Extract + yaw-correct** frame 0 to the front-facing bind space (same
   `find_yaw` / `_pick_facing` math as `build_rig.py`; pelvis near origin,
   Z up, character faces +Y, limbs at +-X).
2. **Segment arms by connectivity, not coordinates.** A 0.14H-tall band is
   slid up from 0.50H; the first band whose induced mesh subgraph splits into
   a central trunk ring plus one lateral ring per side gives the seeds (arm
   rings vs trunk ring). That gap also yields a reliable torso/arm |x|
   threshold used to seed the upper arm up to the shoulder line. Head, feet
   (any width) and the trunk/leg cores are "rest" seeds. **Hands are never
   seeded**: they are reached through the forearm by multi-source Dijkstra on
   the surface, so a hand resting against a thigh still labels as arm. Arm
   labels must connect to an arm ring (Boss chest plates that pass the
   lateral test revert to trunk). If no band separates (arms welded to the
   trunk at every height) a per-slab |x|-gap fallback kicks in.
3. **T-pose.** Each arm swings rigidly about a shoulder pivot (armpit seam
   centroid, or the arm's root cap for detached armor-sheet arms; L/R
   mirror-averaged) so that pivot->hand-tip lies along +-X, with a 5 UU
   geodesic smoothstep falloff from the seam so the armpit stretches instead
   of tearing.
4. **Armature.** A Mixamo-named humanoid skeleton is derived from the T-posed
   segments: trunk joints at proven height fractions (pelvis band
   0.42..0.54H, neck at the shoulder-pivot height, head at 0.855H), leg
   joints from per-side leg slab centroids (knee 0.31H, ankle 0.07H, toe),
   arm joints as slab centroids along the horizontal arm (elbow 0.47,
   wrist 0.76 of the arm length).
5. **Bone-heat skinning in Blender** (`tools/blender_autorig.py`, headless
   Blender 3.6, `ARMATURE_AUTO`). The few verts heat leaves unweighted get
   inverse-distance weights to the two nearest bones of their own segment.
   Weights are capped to top-4 per vertex.
6. **Un-pose.** The arm chain bones (Arm/ForeArm/Hand) are rotated back
   down with the inverse swing; the exported vertices are the ORIGINAL
   arms-down bind verts (exact game mesh), the weights are the T-pose heat
   weights. Output: `tools/rigs/<Sel>.json` in the `fbx_to_json.py` format
   (+ `materials` from the source mesh and a per-face `mat` index).

`tools/build_rig_mixamo.py` consumes those JSONs unchanged except: the
spine-upperarm anchor is now the `Arm` bone head (the real shoulder joint,
not the clavicle root), source materials/texture slots are preserved (so
MultiSkins 1..3 resolve per face) with `PF_TwoSided` OR'd in, and
`--mixamo-dir` defaults to `tools/rigs`.

## Commands

```
# 1. rigs (all five; ~2 min; writes tools/rigs/*.json + render inputs)
python tools/tpose_rig.py "<game>\System\BotPack.u" --scratch tools/rig_verify --out-dir tools/rigs --save-blend

# 2. header
python tools/build_rig_mixamo.py tools/rigs/SelectionMale1.json --live Commando --sel SelectionMale1 --header RagdollPkg/Src/RagdollDatasets.h

# 3. renders (segmentation / T-pose / bone colouring), 5 models side by side
"<blender 3.6>" -b --python-exit-code 1 -P tools/render_groups.py -- out.png 50 front tools/rig_verify/SelectionMale1_bones.obj+tools/rig_verify/SelectionMale1_down_skel.obj ...

# 4. native build + deploy (no ucc make: datasets are header-only)
MSBuild build\RagdollPkg\RagdollPkg.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:BuildProjectReferences=false
copy System\RagdollPkg.dll "<game>\System\"
```

`tools/rig_verify/tpose/` holds the verification renders of this run
(`seg_front.png`, `tpose_front.png`, `tpose_3q.png`, `bones_front.png`,
`bones_back.png`) and the per-model `<Sel>_tpose_rig.blend` files (T-posed
mesh + armature + heat weights; open in Blender and enter Weight Paint to
inspect any bone).

## Results of the 2026-10-10 run

| model            | arm verts L/R | heat unweighted | arm<->leg leaks | dominant Pelvis |
|------------------|---------------|-----------------|-----------------|-----------------|
| SelectionMale1   | 54 / 52       | 0               | 0               | 13              |
| SelectionMale2   | 52 / 52       | 9 (filled)      | 0               | 13              |
| SelectionBoss    | 50 / 50       | 0               | 0               | 11              |
| SelectionFemale2 | 51 / 51       | 9 (filled)      | 0               | 9               |
| SelectionFemale1 | 36 / 33       | 0               | 0               | 5               |

"Leak" = an arm-segment vert with >25% leg weight or a lower-body vert with
>25% arm weight. Zero on all models; with the Mixamo rigs it was 16-30 per
model.

## Physics skeleton sanity (2026-10-10, second pass)

`build_rig_mixamo.py` now caps the half extents in all three axes (spine
hz<=9, head hz<=6 - the Commando's hair spike had made a 27 UU head box),
mirror-averages L/R half extents, enforces pelvis/spine/head minimum sizes,
and runs a non-adjacent overlap resolver (longer box shifted away and
shortened along the tightest axis, 0.4 UU gap). Before this the spine box
reached 1-3 UU into the thigh boxes on four models and the forearms touched
the thighs on SGirl, so those bodies fought each other every step and the
corpses read as stiff. The hip anchor is now the upper-thigh slab centroid
(SGirl's used to sit 1.5 UU from the mid-plane).

Joint friction/limits/damping were retuned at the same time; see the
"Physics tuning knobs" section in AGENTS.md for the console knobs.

## Knobs / known limits

- Arm/trunk seam: shoulder pads of Male1 end up on the upper-arm bones (they
  sit on the deltoid); Female2's pads stay on the trunk. Both are fine for a
  ragdoll. To move the cut, change the `thr` offsets in `_seed_upper`.
- Pelvis band is a fixed fraction window (0.42..0.54H) because a spread
  stance drags the mid-plane low point down the inner thighs; everything
  else is derived from the mesh.
- Elbow/wrist fractions (0.47 / 0.76) are proportions, not detected joints;
  the physics skeleton only needs the elbow hinge roughly mid-arm.
- Feet are still folded into the shins by `build_rig_mixamo.MIXAMO_MAP`
  (ragdoll has no foot bodies).
- Not re-verified in game yet after this run: playtest and watch for
  hands following the forearm (not the thigh) on all five models.
