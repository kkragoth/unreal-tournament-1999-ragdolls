# Handoff (2026-10-11, afternoon) - "too light / floaty", weapon-based damage model

User feedback: bodies feel way too light and floaty; ragdoll motion should
depend on weapon type and damage; rockets should gib more easily (~2 parts);
bullets should only take the limb that was shot, with per-limb damage
accumulating (Soldier of Fortune 1); rocket splash should reach nearby limbs
(right thigh AND right arm); a little randomness at init (sometimes none,
sometimes a full three-axis spin on heavy damage); and the pose mismatch
between the dying pawn's frozen animation frame and the ragdoll's spawn pose.

Everything below is built, deployed to BOTH installs (DLL + .u 12:06) and
verified headless. Not playtested in the client, not committed (the pivot-hip
work from the morning is also still uncommitted).

## Adversarial review: what was actually wrong

1. **Gravity was half the game's.** The Box3D world ran at 9.81 m/s^2 while
   UT's zone gravity is 950 UU/s^2 = 19 m/s^2 at 50 UU/m. Every corpse fell
   at half the rate of the player's own jumps: that IS the floaty look.
2. **Every hit on a corpse hopped the whole body.** `ApplyDamageImpulse`
   gave EVERY body 3.5-8 m/s in the shot direction, a +2 m/s upward pop on
   the near bodies and +0.8 m/s on all the others, regardless of weapon (an
   enforcer round and a rocket both did it). The engine's momentum was only
   used for the direction.
3. **The "hit" body was a guess.** The engine reports hits on the corpse's
   collision CYLINDER, which SyncRagdoll grows to the whole sprawl, so the
   hit point could be 30+ UU from any limb; `NearestBody` by centre
   distance then picked whatever lay closest to the cylinder wall.
4. **Rockets were never explosive.** `IsExplosive` tested `'exploded'`,
   but the retail names are `'RocketDeath'`, `'FlakDeath'`,
   `'GrenadeDeath'` (verified from `ucc batchexport Botpack.u`), while
   flak CHUNKS (`'shredded'`) and pulse (`'zapped'`) were treated as
   explosive. So a rocket on a corpse went through the per-part path with
   its full 75 damage on the "nearest" part, and flak pellets rolled the
   Blast() dice.
5. **Partial gibs were dice rolls.** `Gib()` picked 1/2/3 parts by random
   odds from the damage, nearest-first, ignoring where the blast actually
   was relative to each limb.
6. **Masses were 52 kg with an 18 kg trunk** (real: ~78 kg, trunk ~45%),
   so a limb hit dragged the torso along.
7. **Corpses ignored damage for their first 0.2 s.** Engine `Carcass`'s
   auto state `Dying` has `ignores TakeDamage` until `Sleep(0.2);
   GotoState('Dead')`. The second rocket of a combo arrives inside that
   window.
8. Every corpse got a random tumble of up to 1.2 rad/s; no corpse ever got
   none, none ever got a big one.

## What changed

### Physics
- World gravity = UT default (`RAGDOLL_UT_GRAVITY` 950 x U2M);
  `ZoneGravityUU` (script var copied from `Region.Zone.ZoneGravity` in
  `Initfor`) and the `GravityScale` knob set each body's `gravityScale`
  (fragments copy it), so low-grav zones work.
- `GRagdollBodyMassKg[]` (78.6 kg: pelvis 11.5, spine 21, neck 1.5, head
  5, upper arm 2.6, forearm 1.9, thigh 10.5, shin 4.8) replaces the
  datasets' MassKg column at build time; `BodyMassScale` knob.
- Joint constraints 120 Hz / damping 5 (`ConstraintHertz`,
  `ConstraintDamping`), `SimSubSteps` 4 -> 8. With UT gravity and Box3D's
  default 60 / 2 the knees rested 20 deg past the stop and the hip play
  yielded 8-10 deg; now 1-3 deg.
- `ApplyDeathVelocities`: tumble is now none / mild / full by a roll:
  mild (0.25-0.75 x DeathSpin) 30%; full (1.5-3 x DeathSpin, random 3-axis
  direction biased perpendicular to the shot) with odds `DeathSpinChance`
  (0.2) x (killing damage + overkill) / 80; else none. Log:
  `RagdollPkg: deathspin <corpse> kind=0|1|2 mag= dmg= odds=`.
  `LastHitDamage` (script, = damage + overkill) feeds it.
- The killing-blow kick picks its body with the new ray cast (see below).

### Damage model (RagdollCarcass.cpp "Damage model", RagdollCarcass.uc)
- Native per-body ledger `FRagdollState::PartDamage[]`, thresholds
  `GRagdollSeverThreshold[]` (neck/head 50, upper arm 60, forearm 40,
  thigh 65, shin 50, feet 30, pelvis/spine never) x `DismemberScale`.
- `RagdollHit(HitLocation, Momentum, Damage, CutPower, ImpulseScale, out
  SeverMask)`: casts the shot ray (hit point along the momentum, started
  80 UU before it) through every live body hull with `b3Body_CastRay`;
  only the body it ENTERS takes the impulse at the entry point and
  `Damage x CutPower` on its ledger. Impulse = pawn delta-v (Momentum /
  100) x `HitImpulseMass` (4 kg) x ImpulseScale, capped 8 m/s per body:
  enforcer 1.3 m/s on a forearm and 0.11 m/s on the torso, sniper ~8 m/s
  on a forearm. A ray that clears every box but passes within 12 UU of a
  body centre is a graze (half impulse, half ledger); farther = miss.
- `RagdollBlast(Origin, Radius, Damage, Momentum, CutPower, ImpulseScale,
  out SeverMask)`: for every body the closest point of its hull to the
  origin (`b3Body_GetClosestPoint`); push falloff (1-d/R)^2 as a radial
  impulse at that point (pawn delta-v x f x ImpulseScale, cap 10 m/s);
  TEARING falloff (1-d/(R/2))^2 on the ledger. Half radius because with
  the full 220 UU a knee rocket took both legs whole plus a forearm.
- `RagdollLedger(Body)` reads the ledger (tests).
- Script `TakeDamage`: 250+ or 3x Mass cumulative -> `ChunkUp` (total
  `Gib`); blast types (`IsBlastType`: RocketDeath, FlakDeath,
  GrenadeDeath, RedeemerDeath, mortared, exploded; `'jolted'` only when a
  ShockProj is nearby) -> `FindBlastOrigin` (nearest `Projectile` within
  radius+120 UU of the hit: it still exists while its HurtRadius runs;
  fallback = the engine's near-side hit point) -> `RagdollBlast` with
  `BlastRadiusFor` (rocket 220, flak shell 150, grenade 200, shock ball
  70 / combo 250, redeemer 350); everything else -> `RagdollHit`. Cut
  powers: `BulletCutPower` 0.5 ('shot'), `BlastCutPower` 0.9,
  Decapitated 2, shredded 1, RipperAltDeath 1.2, impact 1, jolted beam
  0.8, zapped 0.3, Corroded 0. Impulse scales per type
  (`ImpulseScaleFor`; `BlastImpulseScale` 0.6).
- `ApplySever(Mask, ...)`: distal first (shin before thigh so both can
  leave as two pieces), head/neck -> `Decapitate`; shot-off parts drop with
  the bullet's push (60 + 2 x dmg UU/s), blast parts fly from the origin
  (150 + 2 x dmg). `TearOff(Body, Impulse)` no longer invents the impulse.
- `ApplyDeathGore`: the killing blow goes through the same two natives
  with damage + overkill (no impulse: the build already kicked the hit
  body); `ChunkUp(<250)` (the engine's gib at 80+ overkill) is a
  `RagdollBlast` at the last blast origin / hit, falling back to the old
  random `Gib` so an engine gib always visibly takes something; the
  once-per-tick `LastBlastTime` guard covers ApplyDeathGore + ChunkUp in
  the same tick.
- `RagdollMutator.HitRecord` carries `bBlast` + `BlastOrigin` (projectile
  search at MutatorTakeDamage time, while the pawn is alive and the
  projectile exists); `GetLastHit` gained `out vector BlastOrigin, out int
  BlastKind` (UnrealScript forbids bool out params).
- `Initfor` -> `GotoState('Dead')` right after `BuildRagdoll`, and
  `Timer()` does it for corpses without Initfor. `GotoState` in
  PostBeginPlay did NOT stick (the engine applies the auto state after
  it) and a subclass `state Dying { function TakeDamage }` cannot
  un-ignore the probe.
- Wound styles know the retail blast names.

### Gotchas found
- A native named `HitDamage` failed to bind at runtime ("Can't find
  intARagdollCarcassexechitdamage in RagdollPkg.dll"): Botpack registers
  the lowercase name `hitdamage` (Enforcer property) first and UE1 names
  are case-insensitive, so the export lookup was lowercased. Natives are
  now `RagdollHit` / `RagdollBlast` / `RagdollLedger`. Prefix new natives
  with `Ragdoll`.
- `ucc batchexport Botpack.u class .uc <dir>` (from a subst drive) dumps
  the retail sources; Engine.u's export crashes after one class, but the
  script text can be read straight out of Engine.u (`class Carcass
  extends` ... plain text).

## Headless measurements (dev install, DM-Deck16][, Commando, 4 runs)

`RagdollTestMutator.DamageModelTest` (sweeps 4/8/12/16), aimed at body
centres from 100 UU, enforcer = 17 dmg / 3000 momentum 'shot', rocket = 75 /
80000 'RocketDeath':

| test | result |
|---|---|
| 6 enforcer rounds into the spine | ledger 51, nothing comes off, torso dv 0.11 m/s per round |
| enforcer rounds into the right forearm | off on the 5th (ledger 42.5 / 40), 1.26 m/s per round, no bleed into upper arm / shin |
| rocket at the left knee | shin L off (67.5), thigh L 62.9-63.6 / 65 (just stays), shin R 42-49 / 50, upper arm L 30-37, head 20 |

So a rocket takes 1-2 parts around the impact and nothing on the far side;
`BlastCutPower` 1.0 makes the thigh go too (whole leg).

Joint census at sync 50 / 150 / 300 with UT gravity and the stiffer
constraints: knees within 1-2 deg of the -100 stop, hip play 33-37 vs 32
(was -122 and 40-54 with the default softness). Lone-corpse leg spread 26 /
26 / 37 / 40 at sync 50; corpse 0 (face down, both hips at -20 extension)
reads 66-68 throughout: extension + play on both sides is a legal splay for
a prone body. Stacking test unchanged (upper corpse lands on the lower, slides
off to 24 UU).

## Knobs (all `set RagdollPkg.RagdollCarcass <Name> <Value>`, next corpse)

GravityScale 1, BodyMassScale 1, HitImpulseMass 4, DeathSpinChance 0.2,
ConstraintHertz 120, ConstraintDamping 5, BulletCutPower 0.5, BlastCutPower
0.9, BlastImpulseScale 0.6, DismemberScale 1 (+ the earlier ones).

## Playtest checklist

- Do corpses drop like the player does now? If still floaty, `GravityScale`
  1.2-1.5 before touching damping.
- Enforcer into a lying corpse: only the hit limb twitches
  (`RagdollPkg: hit ... body= dv=`); torso shots nudge, never hop.
- Rocket on a corpse: `RagdollPkg: blast ... nearest=b f= sever=0x..`;
  parts near the blast go, far side stays. Too many -> BlastCutPower 0.8;
  too few -> 1.0.
- Kill with a rocket: `deathhit ... ray=1` (the ray found the body) and
  `deathspin kind=2` on heavy overkill only.
- Watch the joint census under real play; `ConstraintHertz` 120 with 8
  substeps is the first thing to back off if anything jitters.

## Next: pose continuity - DONE the same evening, see
## `docs/handoff-2026-10-11-pose.md` (plan kept below for the record)

## Next: pose continuity (user: "the pawn has a frozen pose, then the
ragdoll spawns in a different position")

Today `ApplyDeathPose` builds a RANDOM pose from scratch (arms forward,
legs swung) and only takes the pawn's yaw/pitch; the pawn's last animation
frame (its run/aim pose at the instant of death) is thrown away. Plan:
- In `Initfor`, before `BuildRagdoll`, the pawn still exists with its
  vertex-animated `ULodMesh` at `AnimSequence`/`AnimFrame`. Call the mesh's
  `GetFrame` for the pawn (world-space vertices of the current frame).
- For each ragdoll body, take the dataset vertices dominantly bound to it
  (`VertBinds` slot 0 + bind offsets) and fit the rigid transform that maps
  bind offsets onto those world vertices (Kabsch / least squares, 12 small
  3x3 problems). That is the body's spawn transform; joints hold whatever
  they hold (they are soft, a few degrees of violation settle).
- Fall back to the current random pose when the fit residual is bad (the
  stock death animations start from the standing pose, so the first frame
  fits well; `Dead*` frames later in the animation are already lying down).
- Blend: keep the pawn's velocity field (already done) and apply the hit
  kick at the ray body (already done). Variation then comes from the
  animation + shot, not from dice.
This also removes the `pose` randomness that puts corpses in the splits in
the first place.
