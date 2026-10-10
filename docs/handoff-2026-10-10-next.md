# Handoff (2026-10-10, end of day) — next: per-model joints, self-collision

You are continuing work on `C:\Users\kkragoth\dev\unreal-tournament-1999-ragdolls`
(UT99 Box3D ragdoll mod, native `RagdollPkg`). Read `AGENTS.md` first (build /
deploy loop, gore section, tuning knobs). Earlier handoffs:
`docs/handoff-2026-10-10-gore.md` (gore system + playtest feedback, now
mostly resolved) and `docs/handoff-2026-10-10.md` (rig pipeline).

## State right now

Everything is committed on `main` and DEPLOYED to both installs (`.dll` and
`.u`). Commits since the gore handoff:

| commit | what |
|---|---|
| `4f41de8` | planar dismemberment cuts (plane fitted to the heat-weight seam ring, seam faces clipped, flat dark caps, fragments by random planes); `PaintWound` takes the shot direction; `tools/plane_cut.py` offline prototype |
| `a761afe` | GPF fix: dangling reference into `VertBinds` while growing it (crashed the client on the test mutator's first sweep) |
| `dfff2db` | planar cuts gated behind `bPlanarCuts` (globalconfig bool on `RagdollCarcass`, DEFAULT OFF) |

**Why the planar cuts are off by default.** The user playtested `a761afe`
(planar always on) and said it is "a bit broken visually" and asked to hide
it behind a flag. Exactly what looked broken was NOT described; candidates to
check in the next playtest with `set RagdollPkg.RagdollCarcass bPlanarCuts 1`:
the armpit fallback patch on torn-off arms, caps on fragment seams (planes
through the torso make large flat cross-sections), the intersection vertices'
skinning (weights copied from the inside endpoint), cap colour. With the
flag off the split is bit-for-bit the morning behaviour (dominant-bone
islands, duplicated skirts, centroid fans; headless forearm 22 seam / 22
caps, neck 42 / 42). `docs/gore-planar-cut-findings.md` has the measurements
behind the planar version, including the important one: the shoulders sit
ABOVE the spine-neck joint anchor on every model, so cut planes must come
from the mesh seam, never from the skeleton anchors.

Wound marks now follow the shot direction (hit momentum) onto the skin, so
shooting a corpse leaves marks (user request); verify in a playtest via the
`RagdollPkg: wound ... ray=1` log lines (cap raised to 60 lines).

Headless check for all of this: `subst U: "<install>"`, run
`U:\System\UCC.exe server DM-Deck16][?game=Botpack.DeathMatchPlus?mutator=RagdollPkg.RagdollMutator,RagdollPkg.RagdollTestMutator -log=ragdolltest.log`
for ~45 s, grep `SplitIslands|cut plane|wound|Critical`, `subst U: /D`.
It does NOT catch everything (the GPF above only showed in the client).

## NEXT WORK 1: per-model joint configuration

User (2026-10-10): "each model should have their own joint configuration",
following the earlier "I don't feel single rigging for female/male different
models is doing best job".

What exists: ONE joint table for every model, hard-coded in
`BuildRagdollImpl` (`RagdollPkg/Src/RagdollCarcass.cpp`, the `AddJoint`
calls: `{Kind, L0, L1, L2, Friction}` = cone half-angle / twist range or
hinge range, friction Nm), scaled globally by `JointLimitScale` /
`JointFrictionScale`. Per model the dataset (`RagdollDatasets.h`,
`FRagdollDatasetEntry`) already carries the 14 box defs (`Bones`: half
extents, centres, mass) and the 13 joint anchors (`Anchors`), derived by
`tools/build_rig_mixamo.py` from `tools/rigs/<Sel>.json`.

Suggested shape:

1. Add an optional per-model joint table to `FRagdollDatasetEntry`
   (`const FRagdollJointSpec* Joints; // 13 or NULL`) plus per-joint anchor
   offsets if wanted. Emit it from `build_rig_mixamo.py` out of a hand-edited
   override file `tools/rigs/<Sel>.tuning.json` (joint ranges in degrees,
   friction, box scale per bone, mass); missing entries fall back to the
   global table so the five models keep working while tuning one at a time.
   `FRagdollJointSpec` is defined in `RagdollCarcass.cpp`; move it to
   `RagdollInternal.h` for the dataset header.
2. Keep the console knobs as multipliers on top (they are read per corpse via
   `RagdollFloatProp`). Consider per-model console overrides too:
   `set RagdollPkg.RagdollCarcass JointLimitScaleFemale 1.2` is cheap to add
   with the same property walk (`RagdollFloatProp` by name).
3. Ask the user which model is worst and start with that one. Render
   `tools/render_groups.py` bone colours (`tools/rig_verify/`) side by side
   male vs female to show the bind differences; `tools/plane_cut.py` can
   also dump per-bone vertex counts.
4. Longer term (still wanted): hand-authored rigs per model in Blender
   (`tools/tpose_rig.py --save-blend` writes the `.blend`; an exporter back to
   `tools/rigs/<Sel>.json` is missing). See the "Feedback 2" section of
   `docs/handoff-2026-10-10-gore.md`.

## NEXT WORK 2: self-collision and corpse-corpse collision

User (2026-10-10): "currently I see arms just pass through torso. The ragdoll
should interact with its own body parts, and maybe other ragdolls, e.g.
possible to stack them".

What the code does today (checked 2026-10-10 evening):

- All corpses live in ONE Box3D world per level (`FRagdollLevelWorld`,
  `UnRagdollGeometry.cpp`). Every body shape has
  `filter.categoryBits = RAGDOLL_BODY_CATEGORY` and the DEFAULT `maskBits`
  (all bits), so by the filter alone bodies of the same corpse and of other
  corpses should collide with each other and with the level mesh.
- Joints are created with the default `collideConnected = false`
  (`b3JointDef.base`, `box3d/types.h:637`), so each body is allowed to pass
  through the ONE body it is jointed to: upper arm through the spine box,
  thigh through the pelvis box, forearm through the upper arm, etc. That is
  the most likely reason "arms pass through the torso": the upper arm is
  jointed to the spine, and the spine box (8 x 6 x 10.5 UU half extents on
  the Commando) is most of the torso. The forearm is not jointed to the spine
  and should already collide with it; verify.
- Bodies are boxes (`b3MakeBoxHull`) sized from the dataset
  (`FRagdollBoneDef`), fragments 80% of their chunk bounds. The 2026-10-10
  dataset fix capped box heights and removed non-adjacent overlaps, so
  non-adjacent boxes should not start interpenetrating. Jointed neighbours DO
  overlap at the joint by design (anchor inside both boxes).

Plan:

1. Measure first, headlessly: in `StepRagdoll`/`SyncRagdoll` (or a test
   native) call `b3Body_GetContactData` per body for a few seconds after
   spawn and log how many body-body contacts exist, split into same-corpse
   vs other-corpse. If same-corpse non-adjacent contacts are zero, something
   beyond `collideConnected` is off (sensor flag, filter, or the solver
   treating the whole ragdoll as one island - check `b3ShapeDef.isSensor`
   and `enableContactEvents`).
2. `collideConnected = true` on the shoulder and hip joints only (upper arm
   vs spine, thigh vs pelvis), with the box ends trimmed so the two hulls do
   not overlap at rest (shrink the child box along its axis by the overlap
   depth, or switch limbs to capsules: `b3MakeCapsule` if Box3D exposes it,
   which also stops boxes catching on edges). Keep elbows/knees/neck as
   non-colliding pairs (their boxes overlap by construction) or trim them
   too and enable one by one; log joint stability (no explosions) with
   `ClearCorpseSpawn` still in place.
3. Corpse-corpse: should already work physically (same world, same
   category). Test by spawning two test corpses on top of each other in
   `RagdollTestMutator` and reading the pelvis Z of the upper one. If they
   sink through, look at `b3WorldDef` (contact hertz / speculative margin)
   and at sleeping: a sleeping corpse must wake when another lands on it
   (`b3Body_SetAwake` / `enableSleep`). The script-side hit cylinder only
   tracks bodies attached to the pelvis; it does not affect Box3D.
4. Cost: self-collision roughly doubles the broad-phase pairs per corpse
   (12 bodies). `SimFreq` 50 / `SimSubSteps` 4 should cope; watch the
   `sync ... count=` cadence in the log with 8+ corpses.

## Other notes

- `ucc make` prompts to regenerate `RagdollPkgClasses.h`: answer N (piped
  `"N`nN`n"`), keep the hand-maintained header. The PaintWound native
  signature changed today; it binds by export name, so no header change was
  needed.
- The PowerShell tool's pre-screen blocks commands that combine
  `Remove-Item` with long quoted exe paths or `U:` paths; split build /
  deploy / subst into separate calls.
- `tools/render_groups.py` needs ABSOLUTE paths for the OBJ and PNG
  (Blender's cwd differs); `RG_RES=1400` env for close-ups; object names
  `Bone_<i>_...` colour by bone, `cap*` dark red.
- Package still depends on Engine only (Botpack classes are
  `DynamicLoadObject`ed). Keep it that way.
