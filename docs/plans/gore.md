# Plan: gore system (decapitation, dismemberment, gibbing) on the Box3D ragdoll

Status: **IMPLEMENTED, NOT YET PLAYTESTED** (2026-10-10). All four tiers are
in (`RagdollPkg/Src/RagdollGore.cpp`, gore natives in `RagdollCarcass.cpp`,
script in `RagdollCarcass.uc` / `RagdollMutator.uc`), built, deployed to both
installs and exercised headlessly (`ucc server` + `RagdollTestMutator`:
Dismember / RemovePart / Gib on a Commando corpse, no crash, no script
warnings). See "Implementation notes" at the end for what differs from the
design below. Written after the fifth-pass playtest
("after a headshot there are 2 bodies", "rocket frag makes the corpse just
disappear", "add limbs flying off etc., Soldier of Fortune 1 style").

## 1. Why the two bugs happen (traced through the stock UT99 scripts)

Stock sources were read from the CMU UT99 class dump
(`project-archives.etc.cmu.edu/2001/fall/coyote210/Docs/undox/<Package>.<Class>.html`);
`ucc batchexport` cannot run from this install because the commandlet parses
the raw command line and the game path contains spaces.

### 1a. Headshot -> two bodies

- `TournamentMale.PlayDying` (and the Female/Boss/bot variants) calls
  `PlayDecap()` when `DamageType == 'Decapitated'` and `!bVeryLowGore`.
  `PlayDecap` plays anim `Dead4` and **spawns `UT_HeadMale`** (a head chunk)
  at `Location + 0.8*CollisionHeight` and calls `carc.Initfor(self)`.
- `UT_HeadMale` extends `UTHeads` -> `UTPlayerChunks` -> `UTCreatureChunks`
  -> **`Engine.Carcass`**.
- `RagdollSpawnNotify.SpawnNotification` intercepts **every** `Carcass`
  (`A.IsA('Carcass')`) and replaces it with a `RagdollCarcass`. The head chunk
  therefore becomes a full ragdoll (its `Initfor(pawn)` copies the pawn mesh),
  and 0.3 s later the pawn's `Dying` timer spawns the real carcass: two bodies.
- The same interception would hit every gib chunk class (`UT_MaleTorso`,
  `UT_MaleArm`, `UT_Thigh`, `UTLiver`, ...) if any of them were ever spawned.

**Fix:** intercept only `UTHumanCarcass` subclasses (`TMale1Carcass`,
`TMale2Carcass`, `TFemale1Carcass`, `TFemale2Carcass`, `TBossCarcass`), i.e.
`A.IsA('UTHumanCarcass')`. Chunks/heads keep their stock behaviour.

### 1b. Rocket frag -> corpse disappears

- `Pawn.Died`: `if ( Gibbed(damageType) ) SpawnGibbedCarcass()` where
  `PlayerPawn.Gibbed` = false for `'shot'`/`'decapitated'`, true for
  `Health < -80`, or `Health < -40` with 60 % chance.
- `SpawnGibbedCarcass`: `carc = Spawn(CarcassType); carc.Initfor(self);
  carc.ChunkUp(-1 * Health);`
- `RagdollCarcass extends Carcass`, and **`Carcass.ChunkUp` is `Destroy()`**.
  So we build the whole ragdoll in `Initfor` and destroy it in the same frame.
  (`UTHumanCarcass.ChunkUp` would instead spawn `UT_BigBloodHit` plus its
  `MasterReplacement` chunk, e.g. `TMaleMasterChunk`, whose
  `ClientExtraChunks` spawns `UT_HeadMale` (unless anim Dead4/Dead5),
  `UT_MaleTorso`, `UT_MaleArm`, `UT_Thigh`, 1-2 `UT_MaleFoot`, and one organ
  (`UTLiver`/`UTStomach`/`UTHeart`) on high detail, then hides itself.)
- Also: our `TakeDamage` never calls `ChunkUp`, so a corpse can never be
  gibbed by later hits. Stock thresholds (`Carcass.TakeDamage`): single hit
  `> 0.4*Mass` (72) or `CumulativeDamage > 0.8*Mass` (144); `'shot'` damage is
  scaled by 0.4 first.

**Fix:** override `ChunkUp` in `RagdollCarcass` and route it to the native
gib (section 3, tier 2). Until that exists, the interim override spawns the
stock blood + `MasterReplacement` chunks and destroys the corpse, which is what
UT does today (chunks are no longer intercepted after 1a).

## 2. Reference: Soldier of Fortune (2000) GHOUL

GHOUL splits each character into 26 damage zones with per-zone damage and
location-specific reactions: head shots destroy the head, close shotgun hits
to the torso spill the abdomen, limbs can be shot off (kneecap -> leg gone),
and explosions scatter parts. Damage is rendered on the fly rather than via
canned death animations. Our ragdoll already has 12 rigid bodies that map
cleanly onto "gore zones": head, neck, spine/chest, pelvis, 2x upper arm,
2x forearm(+hand), 2x thigh, 2x shin(+foot). The skin is already partitioned
per body by the bind weights (`FRagdollDatasetEntry::BindBones`), so a
detached body keeps its own skin for free; the only rendering work is the
seam between a detached part and the rest.

## 3. Design

### Tier 0: the two fixes (script only, `ucc make`)

1. `RagdollSpawnNotify`: `if ( !A.IsA('UTHumanCarcass') ) return A;`.
2. `RagdollCarcass.Initfor`: remember
   `MasterChunkClass = class<UTHumanCarcass>(Pawn(Other).CarcassType).default.MasterReplacement`
   (generic, no per-model table) and the pawn's `AnimSequence` (`Dead4` =
   decapitated, used by `TMaleMasterChunk` for the same purpose).
3. `RagdollCarcass.ChunkUp(int Damage)`: interim = spawn `UT_BigBloodHit`
   (green if `bGreenBlood`) + `MasterChunkClass` with `Initfor(self)`, then
   `Destroy()`. Replaced by tier 2.
4. `RagdollCarcass.TakeDamage`: keep `ApplyDamageImpulse`, add the stock
   thresholds (`CumulativeDamage`, 0.4 scale for `'shot'`) and call `ChunkUp`
   when exceeded, so corpses can be gibbed by later hits like stock UT.

### Tier 1: per-part dismemberment (native + script)

State additions (`FRagdollState`): `b3JointId Joints[13]` (`AddJoint`
currently discards the ids), `UBOOL Detached[14]`, `FLOAT PartDamage[14]`,
`INT CapFaceStart` (index of the first generated cap face in the mesh).

`execDismember(int Body, vector Impulse)`:
- `b3DestroyJoint(Joints[parent joint of Body], true)`. Child joints are kept,
  so a forearm leaves with its upper arm, a shin with its thigh, the head
  alone. The bodies stay in the same Box3D world and the same `FRagdollState`
  (no new actors); `GetFrame` keeps skinning them, so the part visibly flies
  off and bounces with no extra plumbing.
- Impulse applied to the subtree (hit direction + up, scaled by damage) plus a
  random spin.
- Mesh seam: the corpse owns its `URagdollMesh` (`CreatePlayerRagdollMesh`
  builds one per corpse), so `Faces`/`Wedges`/`Verts` can be rewritten:
  1. Classify every vertex by dominant bind bone, every face by its three
     vertices: a face whose vertices fall on both sides of the cut is a
     *seam face*.
  2. For each seam face, duplicate the straddling vertices: one copy bound
     100 % to the stump side, one bound 100 % to the limb side (extend
     `S.VertBinds`, `Mesh->Verts`, rewrite the wedge `iVertex`). This keeps
     both silhouettes closed-ish and removes rubber-band triangles.
  3. Cap: fan triangles from the joint anchor (projected onto each side) over
     the seam ring on both the stump and the limb, with a dark-red wound
     texture (candidate: the skin of a stock gib such as `UT_MaleArm`, or a
     tiny runtime-generated solid texture). Faces use `PF_TwoSided`, so a
     missing cap shows a hollow interior; the cap is what sells the cut.
  4. `BuildMeshAdjacency` must be re-run after the rewrite.
- Blood: new `execGetBodyLocation(int Body)` so script can place stock
  effects: `UT_BloodBurst` at the cut, `BloodSpurt`/`BloodSplat` decals along
  the limb path for ~2 s from the Timer, `UTBloodPool2` where a detached part
  comes to rest.

Trigger rules:
- **Decapitation on death.** `PlayDecap` already spawned the stock
  `UT_HeadMale` before our carcass exists (it also retargets the killer's
  victim camera to the head, which we keep). In `Initfor`, if the pawn's
  `AnimSequence == 'Dead4'` (verify the Female/Boss anim names; fallback:
  match a `UTHeads` spawned within the last 0.5 s whose `PlayerOwner` equals
  ours), call `Dismember(BONE_Head, up+random)` **and hide the head faces**
  (the stock head chunk is the visible head), capping the neck. Result: a
  headless ragdoll plus the rolling stock head.
- **Location-based death gore.** `Initfor` has no hit info. Add
  `MutatorTakeDamage` to `RagdollMutator` to record the last hit per victim
  (`HitLocation`, `Momentum`, `DamageType`, `Damage`); `Initfor` looks it up
  by pawn. A killing blow with overkill (`-Health`) above a threshold tears
  off the body nearest the hit (flak `'shredded'`, rocket/`'exploded'`,
  ripper/shock `'shot'` >= 45 damage), SoF kneecap-style.
- **Hits on the corpse.** `ApplyDamageImpulse` already finds the nearest body
  and distance. Accumulate `PartDamage[body]`; detach at per-part thresholds
  (head 40, forearm/shin 60, upper arm 70, thigh 90; `'shot'` scaled 0.4 like
  stock). Damage type names to verify against the retail weapons: `'shot'`,
  `'Decapitated'`, `'shredded'`, `'exploded'`, `'jolted'`, `'zapped'`,
  `'corroded'`, `'burned'`, `'pounded'`, `'RedeemerDeath'`, `'Gibbed'`.

### Tier 2: gibbing (replaces the interim ChunkUp)

`execGib(int Damage, vector HitLocation)`:
- Destroy all 11 joints; every body gets a radial impulse from the hit point
  scaled by `Damage`, plus random spin; mesh split into 12 islands (drop or
  cap all seam faces as in tier 1).
- Spawn stock `UT_BigBloodHit`, 2-3 `UT_BloodBurst`, and one organ
  (`UTLiver`/`UTStomach`/`UTHeart`, stock classes, now not intercepted). Do
  not spawn the stock `UT_MaleTorso`/`UT_MaleArm`/`UT_Thigh`: our skinned
  parts replace them.
- Partial gib for mid-range damage: detach 2-4 bodies nearest the hit.
- `LifeSpan` 20 s for a gibbed corpse (stock chunk lifetime) and respect the
  zone carcass limits; 12 free bodies per corpse adds up.

### Tier 3: polish

- Gib sounds (`UnrealShare.Gibs` set used by `UTHumanCarcass.GibSounds`).
- `bLowGore`/`bVeryLowGore` (`GameInfo`): no dismemberment, ragdoll only.
- Config: `GoreLevel` (0 none, 1 decap+gib, 2 limbs) and
  `DismemberScale` (threshold multiplier), both `globalconfig` like the
  existing knobs.

## 4. Implementation order

1. Tier 0, items 1-4 (`RagdollSpawnNotify.uc`, `RagdollCarcass.uc`; delete
   the retail `.u`, `ucc make`, deploy). Playtest: one body per headshot,
   rocket frags produce stock gibs.
2. Native plumbing: keep joint ids, `execDismember`, `execGetBodyLocation`,
   `execGib` stubs. New native exec functions must be built and deployed in
   the DLL **before** `ucc make` (AGENTS.md).
3. Mesh seam split + caps (`UnRagdollMesh.cpp`). Verify offline first: a
   `tools/` script that performs the same face classification on
   `tools/rigs/<Sel>.json` and renders the limb/stump islands with
   `render_groups.py`.
4. Decapitation on `Dead4` + head-face hiding.
5. `MutatorTakeDamage` recording + location-based death dismemberment +
   corpse-hit thresholds.
6. Gib explosion replacing the interim `ChunkUp`.
7. Tier 3.

## 5. Risks / open questions

- Seam quality on ~1200-vert meshes: the seam ring may be ragged where heat
  weights blend across a joint; duplicating verts by *dominant* bone is the
  simplest cut, a per-joint precomputed ring (emitted by
  `build_rig_mixamo.py` into `RagdollDatasets.h`) is the fallback.
- Cap texture: no stock "wound" texture; a runtime solid dark-red `UTexture`
  is the safest default.
- The stock `UT_HeadMale` camera retarget (`UTHeads.Dying.Tick`) is wanted;
  do not intercept `UTHeads`.
- `ucc batchexport` on this install: run it from a game copy whose path has
  no spaces if the stock scripts are needed again locally.

## 6. Implementation notes (2026-10-10)

What was built, where it deviates from sections 3-4, and what to look at in
the first playtest.

- **Package stays Engine-only.** No Botpack compile-time references:
  `CarcassType` is read with `GetPropertyText` (it lives on PlayerPawn/Bot,
  not Pawn), the blood/organ/head classes and the gib sounds are
  `DynamicLoadObject`ed by name in `LoadGoreClasses`, and the notify filter is
  `IsA('UTHumanCarcass')`. Green blood is not supported (would need
  `UT_Blood2.GreenBlood()`).
- **Mesh seam = generic island closure** (`RagdollSplitIslands`): islands are
  computed from the `Cut[]` joints (root of each body after removing cut
  joints), every vertex is re-bound to its own island, each straddling face
  is copied once per island present with the foreign vertices duplicated, and
  the rim (foreign-foreign edges seen in exactly one seam face) is fan-capped
  from the centroid of the duplicated vertices. One routine serves limb cuts,
  repeated cuts and the full gib; no seam-loop tracing, so the old
  weld-surgery pitfalls do not apply. Cap texture: a 16x16 palettised
  dark-red noise texture created at runtime (`RagdollCapTexture`), material
  slot 7 (`Textures(7)`; no UT model sets `MultiSkins[7]`).
- **Decapitation removes the head** (`RemovePart`: faces deleted, body
  destroyed, orphan verts parked on the neck) instead of keeping an invisible
  head body. Stock `PlayDecap` deaths (anim `Dead4` male/boss, `Dead6`
  female, bots included) keep the stock head chunk; our own decapitations
  (head/neck nearest a strong hit, head part threshold) spawn
  `UT_HeadMale` / `UT_HeadFemale` / `UT_bosshead` with `Initfor(self)`.
- **Hit cylinder** only tracks bodies still attached to the pelvis island, so
  a limb flung across the room cannot inflate the corpse's collision.
  Detached parts are therefore not hittable (fine for now).
- **Killing-blow rules** (`ApplyDeathGore`, `GoreLevel >= 2`): hit looked up
  in `RagdollMutator` (registered via `RegisterDamageMutator`, 16-entry ring,
  2 s max age); `'Decapitated'` -> decap; explosive types (`exploded`,
  `shredded`, `RedeemerDeath`, `pounded`, `jolted`, `zapped`) with overkill
  >= 15*DismemberScale, or `'shot'` with Damage >= 45 and overkill >= 5 ->
  tear off the body nearest the hit (torso hits do nothing). Corpse hits:
  stock gib thresholds first, else per-part accumulation (head/neck 40,
  forearm/shin/foot 60, upper arm 70, thigh 90, `'shot'` at 40%).
- **Gib**: `Gib()` cuts all joints and blasts radially (4..11.5 m/s with
  damage), spawns `UT_BigBloodHit`, 2-3 `UT_BloodBurst`, one organ on high
  detail, `LifeSpan=20`. Stock `UT_MaleTorso`/`UT_MaleArm`/`UT_Thigh` are not
  spawned. With low gore the corpse is simply destroyed on ChunkUp.
- **Blood**: `UT_BloodHit` per corpse hit, bursts at the cut, bursts
  following a cut part for 2-3 s (`Bleed()` from the sim Timer, 0.15 s
  cadence, 60%), then a `UTBloodPool2` decal at the part's position with
  `rot(16384,0,0)` (the orientation stock uses for floor landings; if the
  decal never shows, the native trace direction is the other way).
- **Config**: `GoreLevel` (default 2) and `DismemberScale` (default 1),
  globalconfig on `RagdollCarcass`, console-settable like the physics knobs.
- Headless numbers (Commando): forearm cut 22 seam faces / 22 caps
  (1292 -> 1316 verts), neck 42/42, full gib 315 seam / 311 caps
  (1292 -> 1633 verts, 2568 -> 3196 faces).

Playtest checklist: cap visibility/colour (slot-7 texture through the
renderer), seam raggedness on arms vs legs, decapitated corpses show no head
and one rolling stock head, rocket/flak kills gib into skinned parts, a
corpse under minigun fire loses limbs then gibs, blood pool decals appear,
no "Accessed None" in `UnrealTournament.log`.
