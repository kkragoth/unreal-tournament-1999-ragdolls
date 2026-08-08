# SelectionMale2 — foot asymmetry (low priority)

**Status:** noted, not blocking. Pick up later if it becomes a problem in-game.

## Observation
In the exported bind mesh (`tools/rig_verify/SelectionMale2_mesh.obj`), one
foot sits slightly forward and the other slightly behind. The feet are not
bilaterally mirrored in the source Selection viewer mesh frame 0, so this
asymmetry is baked into the bind verts and shows on a standing corpse.

## Likely cause
The Selection viewer mesh itself is asymmetric at the feet (a frame-0 pose
quirk of the source asset), not something introduced by `build_rig.py`. It
propagates into the dataset verts untouched since the rig now exports the
mesh complete.

## Impact
- Cosmetic on the standing corpse; negligible once the ragdoll collapses and
  the corpse falls/rolls.
- No physics impact: bone centers/boxes are mirror-averaged already, so the
  skeleton stays symmetric.

## Possible fix (if ever needed)
In `tools/build_rig.py`, mirror-average the foot geometry before emission so
both feet sit at the same Y: e.g. symmetrize the foot verts' Y about the body
mirror axis (X=0) per side, or snap the foot bone Y to 0 as a fallback. Should
be a small, isolated change in the bind pipeline.
