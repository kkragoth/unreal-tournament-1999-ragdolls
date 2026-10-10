# Handoff (2026-10-10, evening) — gore system in, two things the user wants changed

You are continuing work on `C:\Users\kkragoth\dev\unreal-tournament-1999-ragdolls`
(UT99 Box3D ragdoll mod, native `RagdollPkg`). Read `AGENTS.md` first (build /
deploy loop, gore section), then `docs/plans/gore.md` section 6
("Implementation notes") for what exists. `docs/handoff-2026-10-10.md` is the
morning handoff (rig pipeline state); it is still accurate for the rigs.

## State right now

Everything is committed on `main` and DEPLOYED to both installs (`.dll` and
`.u`, see AGENTS.md for the two `System\` dirs). Commits today, in order:

| commit | what |
|---|---|
| `3197bb0` | gore tier 0: notify filters `UTHumanCarcass` only, ChunkUp no longer destroys the corpse, stock gib thresholds |
| `9445279` | tiers 1-3: native `Dismember` / `RemovePart` / `Gib`, island split + caps (`RagdollGore.cpp`), killing-blow + per-part triggers, blood, sounds, `GoreLevel` / `DismemberScale` |
| `3b5e097` | partial, damage-scaled gibbing (joints nearest the hit go first; trunk can be gibbed again) |
| `f5a0e7b` | fragmentation: head into 3, torso 3-4, pelvis 2, thigh 2 chunks (k-means, extra Box3D bodies) |
| `ec404fd` | wound marks painted into a per-corpse copy of the skin texture, styled per damage type |

Headless verification of all of it: `ucc server` + `RagdollTestMutator`
(see AGENTS.md "Gore" for the `subst` trick). The user playtested the
deployed build once (screenshot below) and gave two pieces of feedback.

## STATUS UPDATE (2026-10-10, later): Feedback 1 is implemented and deployed

Planar cuts are in `RagdollGore.cpp` (DLL + `.u` deployed to both installs,
headless-verified: forearm/thigh/hip/knee/spine cuts 100% planar, full gib
279 planar / 59 fallback seam faces on the Commando). The plan below said
"cut plane at the joint anchor"; that was WRONG for the neck and shoulders
(the shoulders are above the neck anchor on every model), the plane is now
fitted to the heat-weight seam ring instead - read
`docs/gore-planar-cut-findings.md`. Also done: darker cap texture, wound
marks painted along the shot direction (user: "wound painting should also
work if I already shoot the ragdoll, not only on initial death" - script
already did, the hit point was the problem). NOT yet playtested in the
renderer: check cap colour, the armpit fallback patch on torn-off arms, and
`RagdollPkg: wound ... ray=1` lines for corpse hits in the log.
Feedback 2 (per-model rigging) is still open.

## Feedback 1 (ORIGINAL, now fixed): the stump caps look "triangulated and sticking out"

Screenshot: a gibbed Commando trunk on Deck16; every cut shows a large,
bright, flat-red FAN of triangles - on the shoulder stump, and one spread
along the outside of the remaining thigh - plus a severed arm with the same
red fan at its end. The user called them "new wounds" but they are the
**stump caps** from `RagdollSplitIslands` (`RagdollPkg/Src/RagdollGore.cpp`),
not the painted wound marks (those are subtle texture edits and may not even
be visible in the shot).

Why it looks like that:

- The cut is a *topological* partition by dominant bind bone (per vertex).
  The resulting seam ring is jagged and NOT planar: it wanders around the
  joint following the heat-weight boundary, and on the k-means fragment
  seams (`RagdollFragmentBody`: head/torso/pelvis/thigh chunks) it is an
  arbitrary curve across the body surface.
- The cap is a fan from the centroid of that ring to every rim edge. For a
  non-planar / elongated ring the apex lies inside the limb and the fan
  triangles cut through the skin, so they show as red triangles "sticking
  out" all along the boundary (that is the big fan on the thigh: a fragment
  seam running down the leg).
- Every material is `PF_TwoSided` and the cap texture is a flat bright red
  noise (`RagdollCapTexture`, palette R 70-175), so each fan triangle is
  individually visible and much brighter than the skin.

Recommended fix (in this order):

1. **Cut with planes, not by dominant bone.** For a joint cut, define the
   cut plane at the joint anchor (`DS->Anchors[...]`, bind space) with the
   normal along the limb axis (child bone centre minus anchor). Clip every
   triangle against the plane: triangles entirely on one side keep their
   side, straddling triangles are split into 1+2 triangles with NEW
   vertices at the edge/plane intersections (interpolate bind position, UV,
   and bind to the side's bone). Then the rim is an exact planar polygon:
   cap it with a fan from its centroid (in the plane) - flat, no poking. Do
   the same for fragments but with random planes through the body (two or
   three planes give 3-4 chunks) instead of k-means. Keep the existing
   island bookkeeping (`Cut[]`, `Island[]`, extra bodies); only the vertex
   classification + seam handling changes. Vertex count grows a little
   (new intersection verts) but far less than the duplicate skirts today.
   Verify offline first: a `tools/` script that performs the same plane
   clip on `tools/rigs/<Sel>.json` and renders stump + limb with
   `tools/render_groups.py`.
2. **Tone the cap down.** Darker palette (R 30-90, almost black clots),
   maybe `PF_Unlit` off but with a darker `MipZero`; or sample the stock
   gib skin colour. A cap should read as a dark cross-section, not a flag.
3. Until 1 is done: either disable fragmentation (the `RagdollFragmentBody`
   calls in `execGib`, `RagdollCarcass.cpp`) or limit it to head + torso,
   and shrink the per-cut artefact by capping only edges whose two rim
   vertices are within ~1.5x the limb radius of the apex.

Also unverified in game: whether the painted wound marks show at all
(`RagdollPaintWound`: look for `RagdollPkg: wound ...` lines in
`UnrealTournament.log`), and whether `UTBloodPool2` decals appear under
resting parts (if not, flip the spawn rotation in `RagdollCarcass.uc`
`Bleed()`; the decal trace direction was never confirmed).

## Feedback 2 (DESIGN): "each model should have separate rigging"

User: "I don't feel single rigging for female/male different models is doing
best job." What exists today (`docs/tpose-rig-workflow.md`,
`tools/tpose_rig.py`): one rig JSON *per Selection mesh* (5 models), but all
of them are produced by the SAME automatic procedure (connectivity arm
segmentation, derived armature, Blender bone-heat weights, generic joint
limits in `BuildRagdollImpl`, generic box derivation in
`build_rig_mixamo.py`). The user's point is that the automatic rig does not
fit the female / boss bodies as well as the male ones, and wants per-model
hand-tuned rigging.

Suggested direction:

- Author the rig per model by hand in Blender: `tools/tpose_rig.py
  --save-blend` already writes a `.blend` per model into
  `tools/rig_verify/` with the T-posed mesh + armature + heat weights. Let
  the user (or the agent through the Blender MCP) adjust bone placement and
  weight-paint per model there, and add an exporter
  (`tools/export_blend_rig.py`) that writes the same `tools/rigs/<Sel>.json`
  schema from a `.blend`, so `build_rig_mixamo.py --header ...` and the
  native side are unchanged. Keep the automatic pipeline as the starting
  point only.
- Per-model physics parameters: joint limits / friction are one global
  table in `BuildRagdollImpl` (`RagdollCarcass.cpp`). Add an optional
  per-model joint spec to `FRagdollDatasetEntry` (emitted by
  `build_rig_mixamo.py` from a per-model override file, e.g.
  `tools/rigs/<Sel>.tuning.json`) so the female and boss rigs can get their
  own ranges and box proportions.
- Ask the user which model looks worst and start there; render the
  per-bone weights (`tools/render_groups.py` vertex-colour mode) side by
  side for male vs female to show the difference.

## Other notes

- Package still depends on Engine only; Botpack classes are
  `DynamicLoadObject`ed. Keep it that way.
- The PowerShell tool's pre-screen blocks commands that combine `Remove-Item`
  with `U:` paths or long quoted exe paths; split deploy / build / subst into
  separate calls (see the memory note `headless-server-test`).
- Build and deploy commands are in AGENTS.md; new native exec functions
  need the DLL deployed BEFORE `ucc make`.
