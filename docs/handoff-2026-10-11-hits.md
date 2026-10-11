# Handoff 2026-10-11 night: corpse hits, standing corpses, one step per tick

Headless verified (`ucc server` DM-Deck16][, dev install, 4 runs), deployed
to BOTH installs (DLL 20:44, .u 20:14), NOT yet playtested, NOT committed.

## User feedback that started this

- "I shoot the ragdoll and it doesn't move at all, only the rocket launcher
  moves it; minigun no effect."
- "If I plasma someone they only bend a knee and stay upright with the bent
  knee. So stiff."
- (From the previous, interrupted session) "bones should stick out of the
  more important part", a lift ride that clipped a shin into the platform,
  a pool corpse that sank.

## What was actually wrong (playtest log D:\old 13:22, DM-Morpheus/Gothic)

1. `deathhit ... dv=0.00 mom=30..392` on every kill: Engine
   `Pawn.TakeDamage` divides the momentum by `Mass` BEFORE calling the
   damage mutator (Engine.u script text, verified), so the mutator's hit
   record held a velocity and `ApplyDeathVelocities` divided by the pawn
   mass again: the killing blow kicked nothing. Rockets still flung the
   corpse only because the pawn's own knock-back velocity is inherited.
2. Corpse hits used a 4 kg coupling mass: an enforcer round moved the
   torso 0.11 m/s (5 UU/s) and a forearm 0.9 m/s - invisible.
3. The animation spawn pose is taken 0.2 s into the Dead* sequence, where
   the pawn still stands. With no death spin (70-80% of kills) and no
   killing-blow kick (1.), a ragdoll standing on the flat ends of its
   shin boxes, joint springs pulling toward the standing bind pose, just
   stood there - the "upright with a bent knee" the user saw.
4. Pulse bolts arrive as damage type `Pulsed`, not `zapped` (the beam).

## Changes

- `RagdollMutator.MutatorTakeDamage`: record `Momentum * Victim.Mass`.
- `RagdollHit`: `HitImpulseMass` default 25 (globalconfig), per-body cap
  6 m/s, jointed neighbours in the same island get a third. Headless:
  enforcer on a forearm `dv=5.77`, on the spine `0.71`.
- `ApplyDeathVelocities`: a corpse whose pelvis is > 0.5 m above its
  lowest body ("upright") always gets at least a mild tumble (0.4-0.8 x
  `DeathSpin`) about a horizontal axis = up x shot direction (head moves
  along the shot; random horizontal when there was no shot).
  `deathspin ... upright=1 rise=0.55` on the standing test corpses.
- `CutPowerFor` / `ImpulseScaleFor`: `'Pulsed'` = pulse.
- `StepRagdollImpl`: the shared world steps once per engine tick
  (`LastTickTime`); fixes buoyancy/current/wind (forces were cleared by
  the extra 1 ms steps) and removes N-1 solver passes per tick.
- Movers: kinematic target leads by `Vel x max(CATCHUP - dt, 0)`; slabs
  16 UU clamped to the brush depth; `contactHertz` 60; crush needs the
  body centre and six +-6 UU axis points inside (`RAGDOLL_CRUSH_DEPTH`).
- Test mutator: `envtest mover` lines add `lowestBody= lowestAboveTop=`.
- Gore (finished from the interrupted session, already in the 13:12 DLL,
  now verified): bone shaft stays with the body; original-vertex gating
  (`NumOrigVerts`) stops caps being re-split (375k -> 5k verts / 10 cuts).
- D:\old ini: removed the persisted `bPlanarCuts=False`.

## Playtest checklist

- Minigun / enforcer into a lying corpse: the hit limb flicks, a torso hit
  rocks the upper body and slides it a little under sustained fire; it
  must not hop as one block. `RagdollPkg: hit ... dv=` 0.7 (torso) to 6
  (forearm) for mom=3000. Dial: `set RagdollPkg.RagdollCarcass
  HitImpulseMass 15` (gentler) / `40` (wilder).
- Pulse / enforcer kills: `deathhit ... mom=3000+ dv=0.4+`, the corpse
  topples over away from the shooter instead of standing; `deathspin ...
  upright=1`. No corpse standing on its feet after ~1 s.
- Rocket kill: `deathhit ... mom=80000 dv=5.00` (capped) + the pawn's
  knock-back; should read as before, slightly more violent at the hit body.
- Lift ride (Deck16 lifts, Morpheus elevators): corpse rides on the
  platform, not inside it, not deleted when the lift stops.
- Slime/water: corpse floats up and bobs (then the slime destroys it).
- Gore: a torn forearm leaves the bone sticking out of the upper arm, the
  forearm shows a flat cut; repeated hits on a corpse must not slow the game
  (mesh growth).
- If anything stands still or looks weak: `RagdollPkg: step` lines must
  show one dt per tick (0.016-0.05), never 0.001.
