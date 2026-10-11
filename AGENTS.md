# AGENTS.md

Notes for AI agents working in this repo.

## Build/deploy loop (native + script)

> **ALWAYS deploy to `D:\old\Unreal Tournament\Unreal Tournament\System\`**
> (the user's playtest install) on EVERY build, in addition to the dev
> install: the DLL after a native build, the `.u` after `ucc make`. A deploy
> that skips D:\old means the user playtests a stale build. Check the file
> timestamps afterwards; the game locks the files while it runs, so if the
> copy fails, say so instead of claiming it shipped.

- First checkout only: provision gitignored deps with
  `.\setup.ps1 -Ut99Src "<UT99 public source tree>"` (adds `thirdparty\ut99sdk`
  headers/libs and `thirdparty\box3d`; add `-Configure` to configure CMake too).
- Native-only edits (`.cpp`/`.h`): build `build\RagdollPkg\RagdollPkg.vcxproj`
  with MSBuild `/p:Configuration=Release /p:Platform=Win32
  /p:BuildProjectReferences=false`, then copy `System\RagdollPkg.dll` to the
  retail `System\`.
- `.uc` (UnrealScript) edits: **you MUST delete the retail
  `System\RagdollPkg.u` first**, then run `ucc make RagdollPkg`. ucc make skips
  recompilation when the `.u` exists, even if the `.uc` sources are newer.
  - Retail game dirs (2026-10-10; the old `Downloads\Unreal Tournament`
    path is gone): `D:\old\Unreal Tournament\Unreal Tournament\` (the copy
    the playtest logs come from) and
    `C:\Users\kkragoth\dev\Unreal Tournament\Unreal Tournament\`. Deploy
    `RagdollPkg.dll` (and `.u` after `ucc make`) to BOTH `System\` folders.
    Both installs' `RagdollPkg\` folders are junctions into THIS repo's
    `RagdollPkg\` (recreated 2026-10-10 with `mklink /J`), so `ucc make` run
    from either `System\` compiles the repo sources
    (`...\Ut99PubSrc` is the retired monorepo - do not edit or deploy from it).
  - Non-interactive `ucc make`: pipe the header answer, e.g. in PowerShell
    `"N`nN`n" | .\UCC.exe make` (answer N keeps the hand-maintained header).
  - `ucc make` binds native functions by the DLL export name
    (`intARagdollCarcassexec<Name>`). New native exec functions must be
    built/deployed in the DLL **before** running `ucc make`, or make fails with
    "Can't find ... in RagdollPkg.dll".
  - When `ucc make` prompts "RagdollPkgClasses.h needs to be updated (Y/N)":
    answer **N** and keep the hand-maintained
    `RagdollPkg\Inc\RagdollPkgClasses.h` (uses `-1` native indices, matched by
    name at runtime). If you do answer Y, rebuild the DLL against the
    regenerated header.
- Playtest: `UnrealTournament.exe
  DM-Deck16][?game=Botpack.DeathMatchPlus?MinPlayers=8?mutator=RagdollPkg.RagdollMutator,RagdollPkg.RagdollTestMutator
  -log` from the retail `System\`. Logs land in `System\UnrealTournament.log`.

## Physics tuning knobs (no rebuild)

`RagdollCarcass` has globalconfig floats read natively by name
(`RagdollFloatProp`, same property-walk as `bBoxMannequins`), so they can be
tuned in the console and apply to the next corpse:
`set RagdollPkg.RagdollCarcass JointFrictionScale 0.5` (joint friction,
1 = defaults of 0.15-0.8 Nm), `JointLimitScale` (cone/hinge ranges),
`AngularDamping` (per-body, default 0.4), `DeathSpin` (random WHOLE-BODY
tumble in rad/s at death plus 35% of it as per-body jitter, default 1.2),
`DeathHitScale` (share of the killing shot's delta-v given to the body it
hit, off-centre, default 0.8, 0 = off), `PoseVariation` (spread of the
random spawn pose built by `ApplyDeathPose`; 0 = every corpse gets the
fixed weapon-aim pose, 1 = default, up to 2). Persisted under
`[RagdollPkg.RagdollCarcass]` in UnrealTournament.ini. The spawn pose is
forward kinematics down the bone chain with a random local rotation per
joint (arms forward/out + elbow flex, legs swung/spread TOGETHER with a
small per-leg difference, knees flexing with their hip, spine/head tilt),
so it always satisfies the joint limits. Death velocities are assigned
after the pose (`ApplyDeathVelocities`): one rigid-body velocity field
(V + w x r) plus the killing-blow impulse at the hit body
(`RecordKillingBlow` in the script stores the mutator's hit record before
`BuildRagdoll`); the pre-2026-10-11 per-body random spins were what flung
limbs to their limits. Walk-over pushes (`execWalkOverPush`) act per body on the closest
point of the body box to the pawn cylinder, as an off-centre impulse plus a
radial shove, so trodden limbs rotate and splay instead of the corpse
sliding as one block. The 2026-10-10 defaults replaced joint brakes of
1.5-2.4 Nm (15-50% of the limbs' gravity torque, which froze limbs 20-35 deg
from vertical) and ~doubled every joint range. Dataset body boxes are capped
in height and checked for non-adjacent overlap in `build_rig_mixamo.py`
(the spine box used to reach into the thigh boxes and jam the hips).

## One world step per engine tick; zones, movers, crushing (2026-10-11 night)

See `docs/handoff-2026-10-11-hits.md` (this session) and
`docs/plans/environment.md` (zones/movers design + Status). Headless
verified on DM-Deck16][, NOT yet playtested.

- **The shared Box3D world steps ONCE per engine tick** (`StepRagdollImpl`
  gates on `Level->TimeSeconds` via `FRagdollLevelWorld::LastTickTime`).
  Before, `appSeconds()` advanced between two corpses' Timers inside one
  tick, so every corpse after the first stepped the world again by ~1 ms,
  and Box3D clears applied forces at the end of EVERY step: buoyancy /
  current / wind forces of every corpse but the first acted for a
  millisecond (the pool corpse sank at full gravity while its ledger read
  lift 1651 N vs weight 1529 N), and N corpses cost N solver passes per
  tick. `RagdollPkg: step` lines must show one dt per tick, never 0.001.
- Water: `RagdollPkg: wet <corpse> sync= lift= weight= pelvisZ= vz= dry=`
  (first 40 lines, every 25th sync of a corpse with a wet pelvis) sums
  buoyancy vs weight over the bodies and lists bodies not fully wet. A
  corpse in the Deck16 slime pool now rises at ~0.75 m/s to the surface.
  Samples inside solid BSP (no leaf) count like the body centre.
- Movers: the kinematic target leads the engine pose by
  `Vel x max(CATCHUP - dt, 0)` (velocity from the last two engine poses)
  so the body sits ON the engine brush right after each step
  (`RagdollPkg: ride ... engZ= kinZ=` within ~1 UU; it trailed 20-30 UU
  before = the corpse rode inside the lift). Slabs are 16 UU thick,
  clamped to the brush depth per face; `contactHertz` 60. The script
  `envtest mover` lines read the body BEFORE the tick's physics step, so
  on a 20 Hz `ucc server` their `lowestAboveTop` goes negative by one
  engine tick of lift travel (16 UU) while the lift moves: use the native
  `ride` lines for the real offset.
- Crush: a body counts as crushed only when its centre AND the six points
  `RAGDOLL_CRUSH_DEPTH` (6 UU) away along the world axes are inside the
  mover (convex brushes: plane test at that depth; the 26-faced Deck16
  lifts are non-convex and use the slab probe 7 times). A corpse that
  falls back onto a lift that outran it downward lands at ~14 m/s and dips
  a shin 5 UU in; that used to delete it.
- `bPlanarCuts=False` had been persisted in the D:\old install's
  UnrealTournament.ini from the first gore playtest and silently overrode
  the new default (modelled caps / bone stubs never showed there); the line
  was removed 2026-10-11. Check `[RagdollPkg.RagdollCarcass]` in BOTH inis
  when a default change does not show up in play.
- Gore: bone geometry on cuts is behind `bBoneStubs` (globalconfig bool,
  default FALSE since 2026-10-11: the user found the bones "not
  anatomically good"); off = both sides of every cut get the torn-meat
  socket cap, `cut site` lines read `stub=none`. With it on, a mid-bone cut
  leaves the BODY the protruding bone shaft and the severed piece a flush
  cross-section (`BoneStubChance` 1.0 default; < 1 lets the limb carry it
  sometimes). Toggle: `set RagdollPkg.RagdollCarcass bBoneStubs 1`. Only the dataset's original
  vertices (`NumOrigVerts`) are re-placed by the cut planes: cap/clip
  vertices of earlier cuts stay with their bone, which stopped the mesh
  growing to 375k verts after ten cuts (now ~1.3k -> 5k).

## Spawn pose from the death animation (2026-10-11 evening)

See `docs/handoff-2026-10-11-pose.md`. Headless verified, NOT yet playtested.

- `ApplyAnimationPose` (RagdollCarcass.cpp) spawns the ragdoll in the dying
  pawn's CURRENT animation frame: `Initfor` copies `AnimSequence` /
  `AnimFrame` from the pawn (Engine `Carcass.Initfor` is empty), the native
  blends the two keyframes of the pawn's ULodMesh (`Verts.Load()` first),
  transforms them with the renderer's chain and fits one rigid transform
  per body (Horn) through the dataset's bone binds, then runs the shared
  `RagdollPoseFK` from the fitted pelvis with the fitted relative
  rotations (hinges projected onto their axis and clamped). Fallback = the
  random `ApplyDeathPose`. Knob `bAnimPose` (globalconfig, default True).
  Log `RagdollPkg: animpose <corpse> seq= frame= rms= bodies=12/12 ...
  fails=`; expect rms 1-3 UU.
- Vertex correspondence: the datasets come from the Selection meshes, the
  pawns use Commando/Soldier/..., SAME model but re-numbered vertices.
  `tools/build_live_map.py` recovers the permutation from the triangle
  connectivity (piece by piece, left/right and identical pads resolved
  geometrically) and writes `RagdollPkg/Src/RagdollLiveMaps.h`
  (`FindRagdollLiveMap`). Rerun it after regenerating `RagdollDatasets.h`.
  Its "fitted facing" line must read ~0 deg per model; the SelectionBoss
  dataset reads 174 (built facing BACKWARDS: Boss corpses spawned reversed
  and their shoulder cones aim backward) - regenerate it.
- Headless: `RagdollTestMutator`'s direct corpse carries a real Dead*
  sequence per sweep so `animpose` lines appear on a `ucc server`; test
  corpses without Initfor have the Engine default `AnimSequence='Dead'`
  (no such sequence) and log a skip. Note the ucc log is block-buffered
  (4 KB): a killed server loses its last lines, run it long enough.

## Damage model, gravity, masses (2026-10-11 afternoon)

See `docs/handoff-2026-10-11-damage.md` (review + measurements). Headless
verified, NOT yet playtested in the client.

- Gravity is UT's 950 UU/s^2 (`RAGDOLL_UT_GRAVITY` x U2M = 19 m/s^2; it
  was 9.81 = floaty), scaled per body by the zone (`ZoneGravityUU`, set in
  `Initfor`) and `GravityScale`. Masses come from `GRagdollBodyMassKg[]`
  (78.6 kg) x `BodyMassScale`, NOT the datasets' MassKg column. Joint
  constraints are 120 Hz / 5 (`ConstraintHertz` / `ConstraintDamping`)
  with `SimSubSteps` 8; Box3D's default let knees sit 20 deg past the
  stop under this gravity.
- Hits on a corpse (REWRITTEN 2026-10-11 late night, "rockets from area
  damage: no limbs flying off; six flak shots on a ragdoll: nothing"): ONE
  script path, `RagdollCarcass.Wound(Damage, Type, HitLoc, Mom, bBlast,
  Origin, bKill)`, used by `TakeDamage` and by the killing blow
  (`ApplyDeathGore`, bKill = no kick, damage WITHOUT overkill). Natives:
  `RagdollHit` (bullets/blades: ray through the hulls; when the ray clears
  every hull the body whose centre passes nearest the ray takes the hit
  anyway - the engine already consumed the shot on the sprawl-sized
  cylinder; the old ~12 UU graze cutoff dropped most flak chunks),
  `RagdollPush` (beams), `RagdollBlast` (the Damage/Momentum the ENGINE
  handed the corpse are already distance-scaled by HurtRadius, so the body
  NEAREST the origin takes them in full and the others fall off with their
  EXTRA distance: push over R, cut over R/2; the old code scaled by the
  origin distance a second time, so a rocket 65 UU from the nearest shin
  booked 8 cut). Then `Sever(Mask, Damage, Cut, At, Mom, bBlast)`: ledger
  bodies over `GRagdollSeverThreshold[]` (spine: `TorsoSeverDamage` 85) come
  off NEAREST THE HIT FIRST (what you hit is what comes off; a flagged shin
  under a torn thigh leaves with it), budget per 0.15 s shot window
  (`SeverWindow*`): 1 part, 2 once the window's CUT sums 100+, capped by
  `MaxSeverPerHit`; a shot whose cut reaches `SeverDamage` (50: rocket 67,
  4 flak chunks 64, grenade 72; never a sniper 22 or enforcer 8.5) and tore
  nothing takes the nearest limb's forearm/shin (whole limb from 90
  damage) via `NearestLimbPart`. Gone: `RollDeathGore` (random 0-4 parts),
  the native forced-limb block in RagdollBlast, `GibDamage` /
  `CumulativeDamage` scatter, `BlastSeverDamage` (now `SeverDamage`),
  `LastBlastTime` (now `LastWoundTime`). `ChunkUp` only scatters at 250+
  (or one part when the engine gibs a kill that had no hit record).
  `RagdollMutator` folds a flak shot's chunks by searching the victim's
  NEWEST record (not the ring's last entry). Hit/blast/push log lines are
  capped at 400 per process (40 ran out inside the headless test). Log
  `RagdollCarcass: death wound <corpse> type= blast= dmg= overkill= body=`,
  `RagdollPkg: blast ... nearest=b dmin=UU ledger= sever=0x`. Headless
  (`dmgtest`, each step in its own shot window via `NewShot`): spine x6
  enforcer tears nothing, forearm off at round 5, knee rocket takes the
  shin (thigh 64.7 of 65) and no arm, chest rocket #1 one limb part, #2
  the waist when the spine is nearest else an arm, #3 never gibs.
  Retail damage type names: rocket `RocketDeath`, flak shell
  `FlakDeath`, grenade `GrenadeDeath`, flak chunk / ripper `shredded`,
  shock `jolted` (ball+combo radial, beam hitscan), pulse bolts `Pulsed`
  (the playtest log's name; the beam is `zapped`), bio `Corroded`, hammer
  `impact`; `'exploded'` is NOT a retail name. Blast
  origin = the nearest `Projectile` (still alive during its HurtRadius),
  recorded for kills in `RagdollMutator.HitRecord`.
- **Momentum gotcha (2026-10-11 evening):** Engine `Pawn.TakeDamage`
  divides the weapon momentum by the pawn's `Mass` (100) and raises its Z
  to 0.4 x size on a walking pawn BEFORE calling the damage mutator, so
  `MutatorTakeDamage` sees a VELOCITY (enforcer 30, rocket 800), while a
  hit on the corpse (`Carcass.TakeDamage`, called by the weapon directly)
  carries the raw momentum (3000 / 80000). `RagdollMutator` multiplies the
  record back by `Victim.Mass`; without that every killing blow read
  `deathhit dv=0.00` (the natives divide by the pawn mass again). Retail
  hitscan momenta (Botpack.u text): enforcer 3000, minigun 500 x damage,
  sniper 30000 (head 35000), shock beam 60000.
- **Pulse beam = sustained push (2026-10-11 night):** the pulse rifle's
  secondary (`PBolt.CheckBeam`, damage type `zapped`) hands a corpse one
  packet every 0.22 s (72 dmg/s x 0.22 = 16 dmg, ~8600 x 0.22 = 1900
  momentum); as a `RagdollHit` impulse at scale 0.5 each packet kicked the
  torso 0.2 m/s and floor friction ate it within a frame ("doesn't push the
  ragdoll"). `IsBeamType` types go through the native `RagdollPush`
  instead: same ray/graze/ledger as `RagdollHit`, but the impulse
  (x `BeamPushScale`, default 18) becomes a force for `BeamPushTime` (0.25 s)
  stored in `FRagdollState::Push*` and applied by `RagdollApplyPush` before
  every world step (hit body at its body-space point, jointed neighbours a
  third per kg, the rest of the corpse a quarter per kg so the beam beats
  the floor friction of the WHOLE corpse, nothing once a body moves 6 m/s
  along the push). Headless: one packet on the pelvis slides the corpse
  ~30 UU along the beam; scale 6 (230 N) left a 21 kg torso at 0.6 UU and
  684 N on the spine without the island share moved it 3 UU. Log
  `RagdollPkg: push <corpse> body= graze= force=<N> (<m/s^2>) for <s>`; the
  headless `dmgtest beam moved=` line reads the forearm displacement one
  sweep after a packet.
- Knobs: `BulletCutPower` 0.5, `BlastCutPower` 0.9, `BlastImpulseScale`
  0.6, `HitImpulseMass` 25 (was 4: "minigun has no effect"; an enforcer
  round now rocks the torso ~0.7 m/s, a forearm hits the 6 m/s cap, the
  jointed neighbours take a third), `DeathSpinChance` 0.2 (full 3-axis
  spin odds, scaled by damage + overkill; 30% mild tumble, rest none -
  EXCEPT a corpse spawned upright: pelvis > 0.5 m above its lowest body,
  which is every anim-pose corpse 0.2 s into a Dead* sequence; it always
  gets a mild tumble about a horizontal axis, head moving along the shot,
  because a ragdoll standing on its shin-box ends with the joint springs
  pulling toward the bind pose just stood there with one bent knee).
  `deathspin` lines carry `upright= rise=`.
- Log lines: `RagdollPkg: hit <corpse> body= graze= cut= ledger=a/b dv=`,
  `RagdollPkg: blast <corpse> origin= r= dmg= bodies= nearest= f= sever=0x`,
  `RagdollPkg: deathspin <corpse> kind= mag=`, `deathhit ... ray=1`.
- The corpse goes to state `Dead` in `Initfor` (and on the first Timer):
  Engine `Carcass`'s auto state `Dying` ignores TakeDamage for 0.2 s;
  `GotoState` in PostBeginPlay does not stick and a subclass state cannot
  un-ignore a probe.
- **Native naming:** UE1 names are case-insensitive and the first
  registration wins the spelling; Botpack registers `hitdamage`
  (Enforcer property), so a native `HitDamage` failed to bind
  ("Can't find intARagdollCarcassexechitdamage"). Prefix new natives with
  `Ragdoll`.
- Retail sources: `ucc batchexport Botpack.u class .uc <dir>` from the
  subst drive (Engine.u's export crashes; read its script text straight
  from the .u). UnrealScript: no `?:`, no bool `out` params.
- Headless damage test: `RagdollTestMutator.DamageModelTest` (sweeps
  4/8/12/16) logs `dmgtest spine x6 enforcer`, `dmgtest forearmR rounds=`
  (expect 5), `dmgtest rocket@kneeL lost=` (expect shin L, thigh L on the
  edge, arms/head intact). Test corpses must `GotoState('Dead')` and clear
  `bDecorative` first.

## Self-collision, corpse stacking, per-model joints

Implemented 2026-10-10 (evening), see `docs/handoff-2026-10-10-next.md`
"Status" for the headless measurements. NOT yet playtested in the client.

- Self-collision: Box3D joints default to `collideConnected = false`, so a
  body could pass through the one body it is jointed to (upper arm through
  the spine box = "arms pass through the torso"). The shoulder and hip
  joints now collide (`bSelfCollide`, globalconfig bool on `RagdollCarcass`,
  default true, read at `BuildRagdoll`); their child hulls are trimmed at
  the socket by the rest-pose overlap (`RagdollTrimChildBox`, offset hull
  via `b3MakeOffsetBoxHull`, body origin unchanged so skinning is
  unaffected). Elbows/knees/neck stay non-colliding (their boxes overlap by
  construction). Non-adjacent bodies and other corpses always collided
  (same world, same category); corpses rest on each other (headless pile:
  10-12 of 16 pelvises >15 UU above the floor).
- Diagnostics: `RagdollPkg: contacts <corpse> sync=N geom= self_joint=
  self_other= corpses= asleep= pairs:` for the first 8 corpses at sync
  50/150/300 (`LogRagdollContacts`; `Nj` = jointed pair, `N-frag` =
  fragment body). Every body carries its `ARagdollCarcass*` as Box3D user
  data (`FRagdollState::Owner`). `RagdollTestMutator` also runs a
  two-corpse stacking test (`stack lowerTopZ=... pelvisXYdist=`) and logs
  the gore pile (`pileTopZ`, `raisedPelvises`).
- Per-model joints: `tools/rigs/<Sel>.tuning.json` (degrees / Nm / UU
  anchor offsets, one line per joint) -> `python tools/build_joint_tuning.py`
  -> `RagdollPkg/Src/RagdollJointTuning.h` (`GRagdollJointTuning[]`, looked
  up by dataset name in `BuildRagdollImpl`; global table
  `GRagdollDefaultJoints` for models without a file). Rebuild the DLL only
  (no `ucc make`). The five files currently equal the global defaults
  (`--init` writes them), so behaviour is unchanged until someone edits one.
  Console multipliers still apply on top. Log line:
  `RagdollPkg: joints <corpse> table=<dataset|global> ...`. Hinges also carry
  `rest_deg` (spec slot `L2`): the joint spring's rest angle, elbow +20 /
  knee -15 by default, because a spring aimed at exactly straight parked
  most elbows and knees ON the hyperextension stop (stiff straight limbs).
- Deploy check before reading a playtest log: compare the install's
  `System\RagdollPkg.dll` / `.u` timestamps with the repo's `System\` and
  the other install. The 2026-10-10 "ragdolls dont look ok" screenshot came
  from D:\old running a build two deploys behind (the game had held the
  files locked), so every limit in its `jointangles` lines was the old one.
- Joint geometry facts (2026-10-10 playtest): Box3D CLAMPS every spherical
  cone to 90 deg and centres it on frame A's z axis. With identity frames
  that is the hanging limb, so arms could never pass horizontal and legs
  raised at most the hip cone. Ball joints therefore carry a `cone_axis`
  (bind space, left = -X, front = +Y, up = +Z): shoulders aim out+forward
  (whole front/outer hemisphere). The bind
  pose sits off-centre inside the cone with zero twist (`RagdollShortestArc`
  frames in `AddJoint`).
- Hips are `JK_Pivot` since 2026-10-11 ("legs spread too wide"): an
  isotropic cone cannot express flexion 110 / abduction 40, and every
  corpse could sit with both legs at the 50-69 deg cone edge (the splits).
  A pivot puts both joint frames' z on the hinge axis (`axis`, bind +X =
  lateral, positive = limb swings forward), so Box3D's TWIST limit carries
  the asymmetric flexion `range_deg` (-20..+110) and its CONE limit is the
  `play_deg` (32: abduction + long-axis rotation combined); `rest_deg` is
  the spring target (10 deg flexion). Tuning JSON `"type": "pivot"`. The
  census prints pivots as `hipL:f<flex>[lo,hi],p<play>/<limit>` and the
  `jointangles` line starts with `legspread=`/`armspread=` (angle between
  the two thigh / upper-arm axes, deg; a relaxed corpse ~30 or less).
  Headless check: `RagdollTestMutator` spawns six lone corpses around the
  start on sweep 1 (`spread corpse` lines) so they fill the census slots;
  the gore pile corpses are crushed under 16 bodies and sit at every limit,
  ignore their numbers. Every joint also has a weak spring toward the bind
  pose (`JointSpringHertz` 0.8 / `JointSpringDamping` 0.8, globalconfig; 0
  = off) so limbs stop settling at a limit. `RagdollPkg: jointangles`
  (first 8 corpses, sync 50/150/300) prints every joint's angle against its
  limits with `!` at a limit; `RagdollPkg: underfloor` lists bodies with
  level geometry just above them and none below.
- Gib history (superseded 2026-10-11 late night by the single `Wound` /
  `Sever` model above): the 2026-10-10 "gibbing way too easily" roll, the
  2026-10-11 night tiers (native direct-hit guarantee, waist tear 130 ->
  85, cumulative `GibDamage`), the per-hit cap + per-shot window, and the
  random 0-4 part `RollDeathGore` for flak/rocket kills. Kept from them:
  `TorsoSeverDamage` 85, `MaxSeverPerHit` 2, the per-shot window, "limb by
  limb, never tiny bits" (only 250+ in one hit scatters), the mutator
  shifting a kill's hit/blast origin by the victim's flight (`GetLastHit`),
  `FindBlastOrigin` skipping `UTChunk`s. Body fragmentation (random-plane
  chunks) stays behind `bFragmentBodies`, default off. Headless: the
  `dmgtest` steps 1-2 of an anim-posed corpse may land in the upper arm /
  spine instead of the spine / forearm; read the `hit ... body=` lines
  before trusting their counts.
- Shoulder pads: `build_rig_mixamo.py` routes clavicle weights on verts
  lateral of the shoulder anchor to the upper arm (binds only; boxes and
  anchors unchanged). Regenerate with the documented header command.
- Leg stance: the selection meshes stand in a contrapposto (one hip ~5 UU
  ahead, shins tilted back 5-18 deg) while the physics skeleton is
  mirror-averaged, so every shin's skin sat ~3.5 UU off its box (boots
  into the floor, legs bent wrong). `unpose_legs` in `build_rig_mixamo.py`
  now straightens both legs with weight-blended rigid transforms before
  anything is derived. Check with the offline vertex-cloud-vs-box means
  (should be ~0) after any rig regeneration.
- Overhang hulls: skin past the distal faces of the shins, forearms and
  head (toes, heel, hands, skull) gets a second hull on the same body
  (`RagdollAddOverhangHull`, logged as `RagdollPkg: overhang hull`), so
  feet no longer clip into the floor on a face-down corpse.

## Gore (dismemberment / decapitation / gibbing)

Planar cuts are ON by default (`bPlanarCuts=true` since 2026-10-11, with
`bModelledCaps=true`: every closed rim loop gets a Blender-editable template
cap from `tools/cap_templates/*.obj` -> `RagdollCapTemplates.h`, the
"stumps"; `bBoneStubs=false` = no bone geometry, both sides get the torn-meat
socket cap). The 2026-10-10 "a bit broken visually" verdict was about the
old centroid fans. Reviewed 2026-10-11 night (this is what the review found
and fixed in `RagdollSplitIslands` step 3): the severed piece's template was
extruded INTO the piece whenever the cut lay distal of the bone's box centre
(the outer half of every site range, t >= ~0.55 on arms/legs; the old
"away from the island root's box centre" test), so the stump looked right
and the limb showed an open rim with a crater. Now a joint plane's child
island always caps toward -N (N is distal), the parent toward +N; only
fragment planes keep the centre test. Rims under `RAGDOLL_CAP_MIN_RADIUS`
(1.5 UU: straps, antenna, pouch tubes the plane also crosses, 9-16 verts at
0.2-1.4 UU) get a flat fan instead of a template scaled to max(R, 1) UU.
`SplitIslands ... modelled=N` counts template caps (4 or 6 on a cut that
crosses an accessory = one per loop per side). Offline mirror:
`tools/gore_cuts.py` (caps only the largest loop per side; its cap direction
was always right, which is why the DLL bug never showed in the renders).

Implemented 2026-10-10 per `docs/plans/gore.md` (read its "Implementation
notes" first). Natives `Dismember` / `RemovePart` / `Gib` /
`GetBodyLocation` / `NearestBody` / `BodyAttached` live in
`RagdollCarcass.cpp`; the skin split + stump caps in
`RagdollPkg/Src/RagdollGore.cpp`. With `bPlanarCuts` on, every cut is a
PLANE fitted to the heat-weight seam ring (NOT the skeleton anchor: the
shoulders sit above the neck anchor on every model, see
`docs/gore-planar-cut-findings.md`), seam faces are clipped against it and
the planar rim is fan-capped flat with a dark cap texture (material slot 7);
fragments are random planes; fused regions (armpit, collar) fall back to
duplicated skirts with small caps. With it off (default) every seam face
takes the duplicated-skirt path with full centroid fans.
Verify seam changes offline first: `tools/plane_cut.py` (parses
`RagdollDatasets.h`, same algorithm) + `tools/render_groups.py` with
ABSOLUTE paths (`RG_RES=1400` for close-ups). Wound marks (`PaintWound`)
take the shot direction and mark the skin face along that ray (corpse hits
land on the collision cylinder, far from a lying corpse's skin). Script
triggers are in `RagdollCarcass.uc` (`ApplyDeathGore`, `TakeDamage`,
`ChunkUp`) with the killing blow recorded by
`RagdollMutator.MutatorTakeDamage`. Knobs
(globalconfig, console-settable): `GoreLevel` (0 off, 1 decap+gib, 2 limbs;
default 2), `DismemberScale` (threshold multiplier), `bPlanarCuts` (planar
clipped cuts, default false). The package still
depends on Engine only: Botpack classes are `DynamicLoadObject`ed.
`RagdollSpawnNotify` intercepts ONLY `UTHumanCarcass` (chunks/heads keep
stock behaviour). Headless check: `RagdollTestMutator` drives the gore
natives on a `ucc server`; `ucc server` cannot run from a path with spaces,
so `subst U: "<install>"` first and run `U:\System\UCC.exe server ...
-log=ragdolltest.log` (remove with `subst U: /D`).

## Ragdoll real-mesh skinning (ON; runtime-toggleable)

Real-mesh skinning is the default, gated at runtime by dataset availability
(`bRealMesh` when `S->VertBinds.Num() == FrameVerts` in
`RagdollPkg\Src\UnRagdollMesh.cpp`). Box mannequins appear when a model has no
dataset OR when the class config flag `bBoxMannequins` is true — toggle with
`set RagdollPkg.RagdollCarcass bBoxMannequins 1|0` (console or ini), no
recompile. The gate lives in one place per side: `RagdollForceBoxes()` wrapping
`FindRagdollDataset` in both `RagdollCarcass.cpp` (physics skeleton) and
`UnRagdollMesh.cpp` (mesh build). Read `docs/handoff.md`,
`docs/skinning-findings.md`, and `docs/plans/real-mesh.md` before touching this
feature.

An earlier "weld surgery" experiment (dropping hand<->leg fused faces +
fan-capping the rims to fix "missing parts in legs") was investigated and
**reverted**: the caps overlapped kept faces because the seam loops are
mis-traced. Do not resume that surgery as-is; see "Re-enable path" in
`docs/handoff.md`.

## Regenerating datasets (T-pose + Blender heat rig)

`RagdollPkg/Src/RagdollDatasets.h` is committed; regenerate only when rig
logic changes. The ACTIVE source of truth is `docs/tpose-rig-workflow.md`:
`tools/tpose_rig.py` (segment arms by mesh connectivity -> swing them into a
T-pose -> derive a Mixamo-named armature -> Blender 3.6 bone-heat weights via
`tools/blender_autorig.py` -> un-pose) writes `tools/rigs/<Sel>.json`, then
`tools/build_rig_mixamo.py ... --header RagdollPkg/Src/RagdollDatasets.h`
emits the header (its `--mixamo-dir` defaults to `tools/rigs`). Verify with
`tools/render_groups.py` (OBJ import must be `forward_axis=Y, up_axis=Z`).
`tools/mixamo_rigs/` (Mixamo FBX dumps) and `build_rig.py`'s own binders are
legacy fallbacks only: Mixamo bound the hanging hands to the LEG bones and
the range-based binders mislabelled feet/arms. Renders and `.blend` files
land in `tools/rig_verify/` (gitignored).
