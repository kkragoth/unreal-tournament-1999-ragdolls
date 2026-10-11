# Handoff (2026-10-11) - "legs spread too wide": pivot hips, coherent death velocities

User screenshot: a Commando corpse on its back, legs in a ~90 deg V, arms
out ("starfish"). Asked for an adversarial review of what the ragdoll
architecture lacks for realism, and a fix. Everything below is built and
deployed to the DEV install only (`C:\Users\kkragoth\dev\Unreal Tournament`,
DLL 10:56 / .u 11:01); D:\old was locked by the running game at the time of
writing (see the end). Not yet committed, not yet playtested by the user.

## Review findings (what was wrong)

1. **Isotropic hip cone.** The hip was a Box3D spherical joint with one
   circular cone (50 deg, aimed 27 deg forward / 12 deg out; 69 deg before
   that). A circular cone cannot express a hip (flexion ~110, extension
   ~20, abduction ~40, adduction ~25): any cone wide enough for flexion
   lets each leg abduct 60+ deg, i.e. the splits. The 2026-10-10 playtest
   census had `hipL:c69/69! hipR:c69/69!` on corpse after corpse.
2. **Spawn pose always spread the legs.** `ApplyDeathPose` drew each leg's
   abduction from 0..23 deg OUTWARD only, independently per leg, and each
   leg's swing independently, so corpses started in a straddle.
3. **Per-body random death spin.** `BuildRagdollImpl` gave EVERY body its
   own random angular velocity of up to `DeathSpin` rad/s (and its own
   linear jitter) at the bind positions, before the pose was applied. The
   joints then fought those incoherent velocities on the first steps and
   the limbs were flung to their limits inside the first half second.
   Floor friction (0.6) then pinned them wherever they landed; the 0.8 Hz
   joint spring is far too weak to drag a leg across the floor.
4. **Killing blow had no local effect.** The pawn's velocity (which already
   includes Momentum/Mass from the engine) was inherited whole-body, but
   the body that was hit got nothing extra: a headshot did not snap the
   head, a chest rocket did not fold the torso. Only the gore code used the
   hit record.

Still open (not fixed, see "Next"): shoulder is still an isotropic 90 deg
cone (acceptable, the shoulder really is a wide ball joint, but there is no
adduction limit past the hanging arm); no pose from the death animation;
soft Box3D limits yield 5-13 deg under contact (see measurements).

## What changed

- **`JK_Pivot` joint kind** (`RagdollInternal.h`, `AddJoint`): a Box3D
  spherical joint whose frames put their z axis on the HINGE axis (spec
  `AX/AY/AZ`, bind +X = lateral, positive = limb swings forward), so the
  twist limit carries the asymmetric flexion range `L1..L2` and the cone
  limit `L0` is the lateral "play" (abduction + long-axis rotation
  combined). New spec field `Rest` (spring rest flexion; the generated
  table now has 9 fields per joint, the ball/hinge rows carry a trailing
  0). Hips: play 32 deg, flexion -20..+110, rest +10, friction 0.5 Nm.
  `tools/build_joint_tuning.py` knows `"type": "pivot"` (`range_deg`,
  `rest_deg`, `play_deg`, `axis`); the five tuning files were regenerated
  from the defaults (they were identical to the defaults before).
- **Pose** (`ApplyDeathPose`): the legs share a base swing (-9..+32 deg)
  and a base spread (-3..+10 deg) with +-14 / +-6 deg per-leg difference;
  knee flexion follows the hip (half the forward swing plus 0..26 deg).
  Non-aim arms abduct 0..40 deg instead of 0..52.
- **`ApplyDeathVelocities`** (new, runs after pose + clearance): one random
  tumble `w` (|w| <= DeathSpin per axis, half on Z) about the pelvis, every
  body gets `V + w x r` plus 0.25 m/s jitter and `w` plus 35% jitter. Then
  the killing blow: `RecordKillingBlow` (new script function, called from
  `Initfor` before `BuildRagdoll`) copies the mutator's last hit into
  `LastHitLocation` / `LastHitMomentum`; the native reads them through a
  new `RagdollVectorProp` and applies `DeathHitScale` (globalconfig, 0.8)
  x Momentum/Mass (capped 5 m/s) as an impulse at the hit point (lever
  capped at 0.25 m) on the nearest body, a third of it at the centres of
  its chain neighbours. Log: `RagdollPkg: deathhit <corpse> body= dv= mom=`.
- **Census**: `jointangles` now starts with `legspread=`/`armspread=`
  (angle between the two thigh / upper-arm axes, deg) and prints pivots as
  `hipL:f<flex>[lo,hi],p<play>/<limit>`; `FRagdollState::JointKind[]`
  records each joint's kind.
- **Test mutator**: sweep 1 spawns up to six lone corpses on floor-checked
  ring spots (`spread corpse` lines) BEFORE the gore corpse so they take
  the first census slots; test corpses now call `SetDeathPose(random yaw)`
  so the spawn pose runs headless too.

## Headless measurements (dev install, `ucc server` 95 s, Commando)

Three ring spots passed the floor check on DM-Deck16][. Leg spread (deg)
at sync 50 / 150 / 300 (~3 / 9 / 18 s at the server's ~17 Hz):

| corpse | 50 | 150 | 300 | note |
|---|---|---|---|---|
| RagdollCarcass0 | 47 | 46 | 25 | face-down, hips at -20 extension, knees fully folded |
| RagdollCarcass1 | 18 | 22 | 20 | |
| RagdollCarcass2 | 50 | 28 | 27 | one hip flexed 42 deg at first |

Old build for comparison: both hips parked at the 69 deg cone (100+ deg
spread) in most sampled corpses. The two stacking-test corpses (clean, not
in the pile) read 24-26 deg.

Caveats seen in the same log:
- Box3D limits are soft: hip play reads 34-45 over the 32 limit and knees
  -102..-112 over the -100 stop while the corpse settles, converging to
  within ~3 deg by sync 300. Corpse 0 (face down, legs folded under the
  body, thigh hull pressed into the pelvis hull by the self-collision
  contact `0-8j 0-9j`) is the worst case. Not addressed.
- The gore-pile corpses (RagdollCarcass3+) sit at every limit; ignore them.
- The settle from ~50 to ~25 deg takes 10+ s: the 0.8 Hz spring slowly
  drags the legs together against floor friction. If that reads as
  "crawling legs" in the client, lower `JointSpringHertz` or raise the
  play cone a little (`play_deg`), do not widen the pose.

## Deploy state / next

- DEV install has DLL + .u. D:\old is STILL ON THE OCT 10 BUILD: the copy
  failed with "Device or resource busy" because UnrealTournament.exe was
  running from D:\old. After the user quits the game copy
  `System\RagdollPkg.dll` (repo) and the dev install's `RagdollPkg.u` to
  `D:\old\...\System\` (a playtest log from D:\old before that shows the
  old behaviour, see the deploy-check note in AGENTS.md). The D:\old ini already carries
  AngularDamping=0.4 / DeathSpin=1.2; `DeathHitScale` is new and defaults
  to 0.8 without an ini line.
- Playtest: watch `legspread=` in the first 8 corpses' `jointangles` lines,
  the `deathhit` lines (does the head snap on headshots, does dv look sane
  for rockets ~5 m/s cap vs enforcer ~0.5), and whether the whole-body
  tumble (DeathSpin 1.2) is too much / too little.
- Candidates after that: shoulder as a pivot-plus-cone pair is NOT
  possible with one Box3D joint (needs flexion 180 / adduction 40); an
  adductor-style distance joint knee-to-knee would be the next tool if the
  play cone alone is not enough. Pose seeded by the death animation name
  (Dead1..DeadN) + shot direction instead of random would be the big
  realism step.
