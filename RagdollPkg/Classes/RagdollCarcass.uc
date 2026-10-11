// RagdollCarcass: a native Carcass whose corpse is simulated by Box3D.
//
// The Box3D world (bodies + joints) is created by the native BuildRagdoll()
// during Initfor(), right after the pawn's skin/mesh data have been copied
// onto this actor. Stepping and pelvis syncing run at SimFreq Hz from the
// script Timer (including the auto Dying state and the Dead state, whose
// Timer is overridden here so the parent's despawn logic does not shadow it).
//
// The visual is produced by URagdollMesh::GetFrame, which reads the Box3D
// bone transforms each frame and outputs world-space vertices.
//
// Environment (docs/plans/environment.md): the shared Box3D world also holds
// every Mover brush as a kinematic body (lifts carry corpses, doors and
// trains push them), each body applies its own zone's gravity / water /
// current / destructive-zone rules every step, and a body crushed inside a
// mover brush makes the corpse disappear (TakeDamage 'Crushed' +
// RagdollCrushed, or the periodic native check).
//
// Gore (Soldier of Fortune style, see docs/plans/gore.md and
// docs/handoff-2026-10-11-damage.md): the 12 physics bodies double as damage
// zones with a native per-body damage ledger. Bullets and blades only damage
// the body the shot ray actually enters (RagdollHit); blasts damage every
// body by its distance to the explosion (RagdollBlast). A body whose ledger
// passes its threshold is torn off (Dismember: its joint is destroyed, the
// skin is split and capped natively, the part keeps simulating in the same
// world); the head can be removed in favour of the stock rolling UTHeads
// chunk (decapitation), and the whole corpse can be blown into one skinned
// island per body (Gib, redeemer-class damage only).
class RagdollCarcass extends Carcass
	native
	config;

const BONE_Pelvis    = 0;
const BONE_Spine     = 1;
const BONE_Neck      = 2;
const BONE_Head      = 3;
const BONE_UpperArmL = 4;
const BONE_UpperArmR = 5;
const BONE_ForearmL  = 6;
const BONE_ForearmR  = 7;
const BONE_ThighL    = 8;
const BONE_ThighR    = 9;
const BONE_ShinL     = 10;
const BONE_ShinR     = 11;
const BONE_FootL     = 12;
const BONE_FootR     = 13;
const NUM_BONES      = 14;

var float SimFreq;
var int   SimSubSteps;
var float DeathImpulseScale;

// Runtime toggle: when true, corpses use procedural box mannequins (14 bodies,
// no mesh skinning) even for models that have a real-mesh dataset. Persisted to
// UnrealTournament.ini under [RagdollPkg.RagdollCarcass]. Toggle in game with:
//   set RagdollPkg.RagdollCarcass bBoxMannequins 1
// (0 = real skinned mesh). Affects corpses spawned after the change.
var() globalconfig bool bBoxMannequins;

// Live physics tuning (read natively through the script property list, so
// console "set RagdollPkg.RagdollCarcass <Name> <Value>" applies to the next
// corpse without a rebuild; persisted under [RagdollPkg.RagdollCarcass]).
//   JointFrictionScale  multiplies every joint's friction torque (1 = defaults)
//   JointLimitScale     multiplies every joint's cone/hinge range (1 = defaults)
//   AngularDamping      per-body angular damping (1/s); lower = floppier (default 0.4)
//   DeathSpin           random whole-body tumble (rad/s) at death, plus 35% of it
//                       as per-body jitter (default 1.2; before 2026-10-11 every
//                       body got its own spin, which splayed the limbs)
//   DeathHitScale       share of the killing shot's delta-v (Momentum/Mass) given
//                       to the body it hit, off-centre, on top of the whole-body
//                       push (default 0.8, 0 = off, capped at 5 m/s)
//   PoseVariation       spread of the random spawn pose (0 = fixed weapon-aim
//                       pose for every corpse, 1 = default, up to 2)
//   JointSpringHertz    weak spring on every joint toward the neutral pose
//                       (tissue stiffness); 0 = off, default 0.8
//   JointSpringDamping  damping ratio of that spring (default 0.8)
//   GravityScale        multiplies the zone gravity for the corpse's bodies
//                       (1 = the engine's own 950 UU/s^2; 2026-10-11: the sim
//                       used to run at Earth gravity, half of UT's, = floaty)
//   BodyMassScale       multiplies the 78 kg segment mass table (1 = default)
//   HitImpulseMass      coupling mass (kg) turning a shot's engine momentum
//                       into the impulse on the ONE body it hit, its jointed
//                       neighbours taking a third (default 25: an enforcer
//                       round rocks the torso ~0.7 m/s and flicks a forearm
//                       to the 6 m/s cap; 4 was invisible, "minigun has no
//                       effect")
//   BeamPushScale       the pulse rifle's beam ('zapped') pushes as a sustained
//   BeamPushTime        FORCE: each 0.22 s damage packet becomes
//                       momentum x BeamPushScale (through HitImpulseMass, as a
//                       shot) spread over BeamPushTime seconds on the body under
//                       the beam (RagdollPush). Default 18 / 0.25: ~680 N on the
//                       body under the beam, a third per kg on its joint
//                       neighbours: a limb flails and the torso is dragged along
//                       the floor (6 = 230 N left a 21 kg torso at 0.6 UU:
//                       friction 0.6 x 78 kg); 0.5 as an impulse per packet (the
//                       2026-10-11 model) "doesn't push the ragdoll" - friction
//                       ate each 0.2 m/s kick within a frame.
var() globalconfig float JointFrictionScale;
var() globalconfig float JointLimitScale;
var() globalconfig float AngularDamping;
var() globalconfig float DeathSpin;
var() globalconfig float DeathHitScale;
var() globalconfig float PoseVariation;
// bAnimPose (default true): spawn the ragdoll in the dying pawn's CURRENT
// animation frame (fitted natively from the mesh, see ApplyAnimationPose) so
// the corpse continues the pose the player saw instead of jumping to the
// random pose; the random pose (PoseVariation) is the fallback.
var() globalconfig bool  bAnimPose;
var() globalconfig float JointSpringHertz;
var() globalconfig float JointSpringDamping;
//   DeathSpinChance     odds of a FULL whole-body spin at death (1.5-3 x
//                       DeathSpin rad/s about a random 3-axis direction) for
//                       an 80-damage kill; scales with damage + overkill, so
//                       an enforcer kill almost never spins and a rocket
//                       overkill usually does. ~30% of corpses get a mild
//                       tumble, the rest none (default 0.2) - except a corpse
//                       spawned standing (the animation pose 0.2 s into the
//                       death sequence), which always topples away from the
//                       shot (it balanced on its shin boxes otherwise)
var() globalconfig float GravityScale;
var() globalconfig float BodyMassScale;
var() globalconfig float HitImpulseMass;
var() globalconfig float BeamPushScale;
var() globalconfig float BeamPushTime;
var() globalconfig float DeathSpinChance;
//   ConstraintHertz     stiffness of every joint's socket + limits (Box3D soft
//   ConstraintDamping   constraint; default 120 / 5 with 8 substeps: the
//                       engine default 60 / 2 let knees rest 20 deg past the
//                       stop under UT gravity)
var() globalconfig float ConstraintHertz;
var() globalconfig float ConstraintDamping;

// Gore bookkeeping (script-only; appended after the natively mirrored vars so
// the C++ class layout in RagdollPkgClasses.h stays valid).
var class<Carcass> StockCarcassClass; // the pawn's CarcassType (TMale1Carcass, ...)
var Mesh           SourceMesh;        // the pawn's LodMesh (Mesh is swapped for the ragdoll mesh)
var name           DeathAnim;         // pawn AnimSequence at death (Dead4/Dead6 = decapitated)
var bool           bGibbed;
var vector         ZoneGravityUU;     // Region.Zone.ZoneGravity at spawn (read natively; zero = default)

// Gore config (console "set RagdollPkg.RagdollCarcass GoreLevel 2", persisted):
//   GoreLevel        0 = ragdoll only, 1 = decapitation + gibbing, 2 = + limbs
//                    torn off by the killing blow and by hits on the corpse
//   DismemberScale   multiplies every sever threshold (2 = twice as much
//                    damage needed, 0.5 = limbs come off easily)
//   BulletCutPower   how much of a bullet's ('shot') damage counts toward
//                    tearing the limb it went through (default 0.5: enforcer
//                    8.5 per hit on a 40-point forearm, sniper 22)
//   BlastCutPower    the same for rockets / flak shells / grenades (default
//                    0.9; tearing falls off over HALF the blast radius: a
//                    rocket at a knee takes that shin and often the thigh,
//                    one on the chest the forearms, the far side nothing)
//   BlastImpulseScale share of the blast's engine momentum turned into
//                    radial velocity on the bodies it touches (default 0.6:
//                    rocket ~10 m/s at the impact, 5 m/s 60 UU away)
// The game's bLowGore / bVeryLowGore flags disable all of it.
var() globalconfig int   GoreLevel;
var() globalconfig float DismemberScale;
var() globalconfig float BulletCutPower;
var() globalconfig float BlastCutPower;
var() globalconfig float BlastImpulseScale;
//   bPlanarCuts     true (default since 2026-10-11) = clip the skin along a
//                   plane per cut; false = the 2026-10-10 dominant-bone
//                   split with centroid fans (no cut sites, no template caps).
var() globalconfig bool  bPlanarCuts;
// Cut variation and stump caps (docs/plans/gore-variation.md):
//   CutVariation     0 = every cut at the top of its site range (joint-level,
//                    like before), 1 (default) = the cut lands where the body
//                    was hit, anywhere in the clean range of the bone
//   bBoneStubs       false (default; user 2026-10-11: "bones don't look
//                    anatomically good") = no bone geometry on any cut, both
//                    sides get the torn-meat socket cap; true = the shaft /
//                    flush / head caps below
//   BoneStubChance   odds that the STUMP keeps the broken bone shaft (else
//                    the severed limb carries it; default 1: the body keeps it)
//   BoneStubLength   scale of the protruding shaft (default 1 = about the
//                    limb radius; 0 = flush)
//   bModelledCaps    true (default) = template caps (torn meat ring + bone,
//                    tools/cap_templates/*.obj); false = centroid fans
//   TorsoSeverDamage ledger damage (UT damage x cut power) at which the
//                    spine tears and the upper body comes off at the waist
//                    (default 85: a direct chest rocket books 67, so the
//                    second one tears the waist when the chest is what it
//                    hit; ten enforcer or four sniper rounds; 0 = never)
//   SeverDamage      a SHOT (every hit within 0.15 s) whose CUTTING damage
//                    (UT damage x cut power: rocket 67, 4 flak chunks 64,
//                    grenade 72, hammer 60; sniper 22, shock ball 44) is at
//                    least this and that tore nothing by the ledger always
//                    takes the nearest limb: its forearm/shin, the whole
//                    limb from 90 damage. A rocket or flak burst at the
//                    corpse leaves a mark every time (default 50; 0 = off)
//   MaxSeverPerHit   most parts one shot tears off (default 2): one part,
//                    the second once the shot's cut sums 100+. Bodies over
//                    their ledger that were not torn stay flagged and go
//                    on the next hit, so the corpse comes apart limb by limb.
var() globalconfig float CutVariation;
var() globalconfig bool  bBoneStubs;
var() globalconfig float BoneStubChance;
var() globalconfig float BoneStubLength;
var() globalconfig bool  bModelledCaps;
var() globalconfig float TorsoSeverDamage;
var() globalconfig float SeverDamage;
var() globalconfig int   MaxSeverPerHit;
//   bSelfCollide    true (default) = the upper arms collide with the torso and
//                   the thighs with the pelvis (Box3D joints otherwise let a
//                   body pass through the one body it is jointed to); their
//                   hulls are trimmed at the socket so the pair starts clear.
//                   Read at BuildRagdoll, so it applies to the next corpse.
var() globalconfig bool  bSelfCollide;
//   bFragmentBodies false (default) = a torn-off head/torso/pelvis/thigh
//                   stays in one piece; true = it shatters into random-plane
//                   chunks as well (the "torso collapsing into pieces with
//                   fan-outs" from the 2026-10-10 playtest).
var() globalconfig bool  bFragmentBodies;

// Environment (docs/plans/environment.md; read natively at BuildRagdoll,
// console "set RagdollPkg.RagdollCarcass <Name> <Value>" applies to the next
// corpse). Every body reads ITS OWN zone every tick: zone gravity (low-grav),
// water, currents / wind, destructive and kill zones.
//   WaterBuoyancy    buoyant force / weight of a fully submerged body
//                    (default 1.08: corpses float up slowly; < 1 = they sink)
//   WaterDrag        linear + angular damping (1/s) and the coupling toward
//                    the water's ZoneVelocity, times the submerged fraction
//                    (default 2.0)
//   AirDrag          coupling (1/s) toward a non-water zone's ZoneVelocity
//                    (wind tunnels; default 0.5)
//   ZoneWindScale    multiplier on that wind push (0 = ignore ZoneVelocity
//                    outside water; default 1)
//   bMoverCollision  true (default) = every Mover brush (door / lift /
//                    train) is a kinematic body in the ragdoll world, so
//                    lifts carry corpses and doors push them. Read when the
//                    level world is built (the first corpse of the level).
//   bCrushDestroys   true (default) = a corpse with a body inside a mover
//                    brush simply disappears (no gib, no blood); false = the
//                    stock 'Crushed' damage gibs it.
var() globalconfig float WaterBuoyancy;
var() globalconfig float WaterDrag;
var() globalconfig float AirDrag;
var() globalconfig float ZoneWindScale;
var() globalconfig bool  bMoverCollision;
var() globalconfig bool  bCrushDestroys;

var bool   bDecapitated;
var bool   bLogDamage;         // log every TakeDamage (headless tests)
var bool   bFemale;
var bool   bBoss;
var vector LastHitLocation;    // killing blow (from RagdollMutator) or last corpse hit
var vector LastHitMomentum;
var vector LastBlastOrigin;    // explosion origin of the last blast (zero = none)
var float  LastHitDamage;      // killing blow damage + overkill (read natively: death spin odds)
var bool   bOrganSpawned;
var float  LastWoundTime;      // Wound() ran this tick (the death hook and the engine's ChunkUp arrive together)
// Sever budget per SHOT, not per TakeDamage call: a flak shot is 8-9 chunk
// hits (16 each) inside one or two ticks, a slug is a blast plus 5 chunks,
// and each used to tear its own part. All hits within SEVER_WINDOW of the
// first share one budget (MaxSeverPerHit; 2 only once they sum to 100+).
var float  SeverWindowTime;
var float  SeverWindowDamage;
var int    SeverWindowParts;

// Stock Botpack classes, loaded by name so the package depends on Engine only.
var class<Actor>   BloodBurstClass;   // UT_BloodBurst: sprite burst at a cut
var class<Actor>   BloodHitClass;     // UT_BloodHit: hit splash on the corpse
var class<Actor>   BigBloodClass;     // UT_BigBloodHit: gib splash
var class<Actor>   BloodPoolClass;    // UTBloodPool2: decal under a resting part
var class<Carcass> OrganClass[3];     // UTLiver / UTStomach / UTHeart
var class<Carcass> HeadClass;         // UT_HeadMale / UT_HeadFemale / UT_bosshead
var Sound GibSounds[4];
var Sound CutSounds[4];

// Bleeding parts: blood bursts follow a freshly cut body for a few seconds,
// then a pool decal is dropped where it came to rest.
var int   BleedBody[6];
var float BleedEnd[6];
var float NextBleed;

native function BuildRagdoll();
native function StepRagdoll(float DeltaSeconds);
native function SyncRagdoll();
native function ShutdownRagdoll();
native function WalkOverPush(Pawn Pusher);
native function SetDeathPose(int Yaw, int Pitch);
// Damage model natives (RagdollCarcass.cpp "Damage model"):
//   RagdollHit    casts the shot ray through the skeleton, kicks the body it
//                enters at the entry point (engine momentum through
//                HitImpulseMass, x ImpulseScale) and adds Damage x CutPower to
//                that body's ledger. Returns the body (-1 = clean miss).
//                SeverMask: bit i set = body i's ledger passed its threshold.
//   RagdollBlast  Damage/Momentum as the engine handed them to this corpse
//                (already scaled to it): the body nearest Origin takes them
//                in full, the others fall off with their extra distance
//                ((1 - extra/R)^2 push, over R/2 for the cut). Radial kick
//                (Momentum/100 UU/s x ImpulseScale) at each closest point,
//                Damage x f x CutPower on each ledger. Returns the nearest
//                body (-1 = none within Radius).
//   RagdollLedger   the ledger of one body (diagnostics).
native function int    RagdollHit(vector HitLocation, vector Momentum, int Damage, float CutPower, float ImpulseScale, out int SeverMask);
native function int    RagdollBlast(vector Origin, float Radius, int Damage, float Momentum, float CutPower, float ImpulseScale, out int SeverMask);
//   RagdollPush   RagdollHit's ray + ledger, but the impulse (x ImpulseScale)
//                becomes a force on the hit body for Duration seconds (beams).
native function int    RagdollPush(vector HitLocation, vector Momentum, int Damage, float CutPower, float ImpulseScale, float Duration, out int SeverMask);
native function float  RagdollLedger(int Body);
//   RagdollCrushed  runs the mover crush test now (a body centre inside a
//                mover brush); true = the main island was crushed and the
//                corpse is already hidden and expiring. A crushed detached
//                limb is removed natively. The periodic native check (every
//                4th sync, movers that moved in the last second) does the
//                same without script.
native function bool   RagdollCrushed();
// Gore natives (see RagdollCarcass.cpp / RagdollGore.cpp).
native function bool   Dismember(int Body, vector Impulse);    // tear the subtree off; Impulse in UU/s
native function bool   RemovePart(int Body);                   // tear off AND delete (decapitation)
native function int    Gib(int Damage, vector HitLocation, bool bTotal);    // bTotal: scatter every body; else ONE part nearest the hit; returns joints left
native function vector GetBodyLocation(int Body);
native function int    NearestBody(vector Loc);
native function bool   BodyAttached(int Body);
native function        PaintWound(vector HitLocation, vector Dir, int Damage, name DamageType); // skin mark along the shot, style per damage type

/*-----------------------------------------------------------------------------
	Damage type tables (retail Botpack names, verified 2026-10-11 from the
	exported sources: rocket 'RocketDeath' 75/80000/r220, flak shell
	'FlakDeath' 70/75000/r150, grenade 'GrenadeDeath' 80/50000/r200, shock
	ball/combo 'jolted' 55/70000/r70 and 165/140000/r250, shock beam 'jolted'
	40/60000 hitscan, redeemer 'RedeemerDeath' 1000/r350, flak chunk
	'shredded' 16/10000, ripper blade 'shredded' 30 ('decapitated' on the
	neck), ripper alt 'RipperAltDeath' 34/87000 touch, enforcer 'shot'
	17/3000, minigun 'shot' 9-14/~6000, sniper 'shot' 45/30000 and
	'Decapitated' 100/35000, pulse 'zapped', bio 'Corroded', hammer 'impact').
	Static so RagdollMutator can classify the killing blow the same way.
-----------------------------------------------------------------------------*/

// Radial explosions (HurtRadius): the corpse's TakeDamage gets a point on
// its cylinder and the damage already scaled to the actor location; the
// per-body model needs the explosion origin instead (FindBlastOrigin).
static function bool IsBlastType(name T)
{
	return (T == 'RocketDeath') || (T == 'FlakDeath') || (T == 'GrenadeDeath') || (T == 'RedeemerDeath')
		|| (T == 'mortared') || (T == 'exploded');
}

// Continuous beams: damage arrives in packets while the beam rests on the
// corpse (PBolt.CheckBeam: every 0.22 s, or when the beam leaves). Pushed as
// a sustained force (RagdollPush), not an impulse per packet.
static function bool IsBeamType(name T)
{
	return (T == 'zapped');
}

// HurtRadius radius per type (UU). 'jolted' is the shock ball (70) or the
// combo (250; damage 165 vs 55) - the beam is hitscan and never gets here.
static function float BlastRadiusFor(name T, int Damage)
{
	if ( T == 'RocketDeath' )   return 220;
	if ( T == 'FlakDeath' )     return 150;
	if ( T == 'GrenadeDeath' )  return 200;
	if ( T == 'RedeemerDeath' ) return 350;
	if ( T == 'mortared' )      return 200;
	if ( T == 'jolted' )
	{
		if ( Damage >= 100 )
			return 250;   // combo
		return 70;        // ball
	}
	return 150;
}

// Share of the damage that counts as cutting (tearing a limb off).
function float CutPowerFor(name T, bool bBlast)
{
	if ( bBlast )               return BlastCutPower;
	if ( T == 'shot' )          return BulletCutPower;
	if ( T == 'Decapitated' )   return 2.0;    // sniper head / ripper neck: the engine already decided
	if ( T == 'shredded' )      return 1.0;    // flak chunks, ripper blades
	if ( T == 'RipperAltDeath' )return 1.2;
	if ( T == 'impact' )        return 1.0;    // hammer
	if ( T == 'jolted' )        return 0.8;    // shock beam
	if ( T == 'zapped' )        return 0.3;    // pulse beam
	if ( T == 'Pulsed' )        return 0.3;    // pulse bolts (PlasmaSphere; the playtest log's name)
	if ( T == 'Corroded' )      return 0.0;    // bio: melts, never cuts
	if ( T == 'burned' )        return 0.2;
	return 0.6;
}

// Share of the engine momentum turned into ragdoll motion. The engine's
// numbers are gameplay knock-back (shock beam 60000 = 12 m/s on a pawn);
// the hit model turns them into the momentum of the projectile itself via
// HitImpulseMass, these scale per weapon on top.
function float ImpulseScaleFor(name T, bool bBlast)
{
	if ( bBlast )               return BlastImpulseScale;
	if ( T == 'shot' )          return 1.0;
	if ( T == 'Decapitated' )   return 1.0;
	if ( T == 'shredded' )      return 1.0;
	if ( T == 'RipperAltDeath' )return 0.4;
	if ( T == 'impact' )        return 0.5;
	if ( T == 'jolted' )        return 0.4;
	if ( T == 'zapped' )        return 0.5;
	if ( T == 'Pulsed' )        return 0.5;
	if ( T == 'Corroded' )      return 0.2;
	return 0.8;
}

// The explosion that is damaging an actor at Loc right now: the projectile
// still exists while its HurtRadius runs (Explode calls HurtRadius before
// Destroy), so the nearest projectile within reach is it. False (and Loc
// returned as the origin) when none is around: HurtRadius reports the hit
// on the victim's near side, in the direction of the blast, which is close.
static function bool FindBlastOrigin(Actor Ctx, vector Loc, float Radius, out vector Origin)
{
	local Projectile P, Best;
	local float D, BestD;

	foreach Ctx.RadiusActors(class'Projectile', P, Radius + 120, Loc)
	{
		// Flak chunks (primary shots, and the 5 a slug spawns AFTER its
		// HurtRadius) lie around for 2.9 s and are not explosions.
		if ( P.IsA('UTChunk') )
			continue;
		D = VSize(P.Location - Loc);
		if ( Best == None || D < BestD )
		{
			Best = P;
			BestD = D;
		}
	}
	if ( Best != None )
	{
		Origin = Best.Location;
		return true;
	}
	Origin = Loc;
	return false;
}

/*-----------------------------------------------------------------------------
	Hits on the corpse.
-----------------------------------------------------------------------------*/

// Projectiles and hitscan call TakeDamage on the corpse. The base
// Carcass/Pawn damage handling is deliberately skipped: our actor has no
// movement physics of its own (PHYS_None), and letting the base physics code
// run would fight the Box3D sync. Everything goes through Wound(), the ONE
// damage model shared with the killing blow (ApplyDeathGore); 250+ in one
// hit (redeemer, crusher, telefrag) scatters the corpse instead (ChunkUp).
function TakeDamage(int Damage, Pawn instigatedBy, Vector hitlocation, Vector momentum, name damageType)
{
	local bool bBlast;
	local vector Origin;

	if ( bLogDamage )
		Log("RagdollCarcass: TakeDamage " $ self $ " state=" $ GetStateName() $ " dmg=" $ Damage $ " type=" $ damageType $ " decorative=" $ bDecorative $ " gibbed=" $ bGibbed $ " mass=" $ Mass);
	if ( bDecorative || bGibbed )
		return;
	// Mover encroachment (Engine Mover.EncroachingOn gives carcasses 10000
	// 'Crushed'): the engine tested our collision CYLINDER, which covers the
	// whole sprawled body, so verify per body before doing anything. Crushed
	// = the corpse disappears (bCrushDestroys), nothing else happens.
	if ( damageType == 'Crushed' )
	{
		if ( !bCrushDestroys )
		{
			ChunkUp(Damage);
			return;
		}
		if ( RagdollCrushed() )
		{
			Log("RagdollCarcass: crushed " $ self $ " by " $ instigatedBy $ ", destroyed");
			Destroy();
		}
		return;
	}
	LastHitLocation = hitlocation;
	LastHitMomentum = momentum;
	if ( GoreAllowed() )
	{
		if ( BloodHitClass != None )
			Spawn(BloodHitClass,,, hitlocation, rotator(momentum));
		PaintWound(hitlocation, momentum, Damage, damageType);   // every hit on the corpse, not only the kill
	}
	if ( Damage >= 250 )
	{
		ChunkUp(Damage);
		return;
	}
	// Radial damage: the per-body model wants the explosion itself, which is
	// the projectile still sitting at its impact point. 'jolted' is a blast
	// only when a shock ball / combo is around (the beam has no projectile).
	bBlast = IsBlastType(damageType);
	if ( damageType == 'jolted' )
		bBlast = FindBlastOrigin(self, hitlocation, 300, Origin);
	else if ( bBlast )
		FindBlastOrigin(self, hitlocation, BlastRadiusFor(damageType, Damage), Origin);
	Wound(Damage, damageType, hitlocation, momentum, bBlast, Origin, false);
}

// THE damage model. The killing blow (ApplyDeathGore) and every later hit on
// the corpse come through here with the damage the ENGINE charged (HurtRadius
// has already scaled a blast by its distance to the corpse):
//  1. physics + ledger (native): a blast kicks every body by its extra
//     distance beyond the nearest one and books Damage x CutPower x falloff
//     on each ledger (RagdollBlast); anything else kicks the ONE body the
//     shot ray goes through, or passes nearest, and books Damage x CutPower
//     on it (RagdollHit; a beam pushes with a force instead, RagdollPush).
//     bKill: no kick (ApplyDeathVelocities kicked the hit body at build).
//  2. Sever(): bodies over their threshold come off, nearest the hit first,
//     at most one per shot (two once the shot sums 100+), and a shot of
//     SeverDamage+ that tore nothing takes the nearest limb's forearm/shin.
// Per-limb damage accumulates (Soldier of Fortune): ~5 enforcer rounds or 2
// sniper rounds into the same forearm take it off; a torso full of bullets
// takes nothing until TorsoSeverDamage. Returns the body hit / nearest the
// blast, -1 when nothing was reached.
function int Wound(int Damage, name Type, vector HitLoc, vector Mom, bool bBlast, vector Origin, bool bKill)
{
	local int Body, Mask;
	local float Scale;
	local vector At;

	LastWoundTime = Level.TimeSeconds;
	if ( bBlast )
	{
		LastBlastOrigin = Origin;
		if ( !bKill )
			Scale = ImpulseScaleFor(Type, true);
		Body = RagdollBlast(Origin, BlastRadiusFor(Type, Damage), Damage, VSize(Mom), CutPowerFor(Type, true), Scale, Mask);
		At = Origin;
	}
	else
	{
		if ( !bKill )
			Scale = ImpulseScaleFor(Type, false);
		if ( IsBeamType(Type) )
		{
			if ( !bKill )
				Scale = BeamPushScale;
			Body = RagdollPush(HitLoc, Mom, Damage, CutPowerFor(Type, false), Scale, BeamPushTime, Mask);
		}
		else
			Body = RagdollHit(HitLoc, Mom, Damage, CutPowerFor(Type, false), Scale, Mask);
		if ( Body >= 0 )
			At = GetBodyLocation(Body);
	}
	if ( Body < 0 )
		return -1;
	if ( GoreLevel >= 2 && GoreAllowed() )
		Sever(Mask, Damage, Damage * CutPowerFor(Type, bBlast), At, Mom, bBlast);
	return Body;
}

// Tear parts off after a hit. Mask = the bodies over their ledger (native),
// Cut = this hit's cutting damage (Damage x cut power, what the ledger got).
// The budget is per SHOT: every call within 0.15 s of the first shares it (a
// flak shot is 8-9 chunk hits, a slug a blast plus 5 chunks): one part, two
// once the shot's cut sums 100+, capped by MaxSeverPerHit. Flagged bodies go
// nearest to the hit first (what you hit is what comes off; a flagged shin
// under a torn thigh leaves with it); one that did not fit the budget stays
// over its ledger and goes on the next hit. A shot whose cut reaches
// SeverDamage and tore nothing takes the nearest limb (forearm/shin; the
// whole limb from 90 damage), so a rocket or a flak burst at the body always
// leaves a mark while bullets (enforcer 8.5 cut, sniper 22) only ever tear by
// their ledger. Blast parts fly away from the origin, shot-off parts leave
// with the bullet's push. Returns the number torn off.
function int Sever(int Mask, int Damage, float Cut, vector At, vector Mom, bool bBlast)
{
	local int i, n, Best, Budget;
	local float D, BestD;

	if ( SeverWindowTime == 0 || Level.TimeSeconds - SeverWindowTime > 0.15 )
	{
		SeverWindowTime = Level.TimeSeconds;
		SeverWindowDamage = 0;
		SeverWindowParts = 0;
	}
	SeverWindowDamage += Cut;
	Budget = 1;
	if ( SeverWindowDamage >= 100 )
		Budget = 2;
	if ( MaxSeverPerHit > 0 )
		Budget = Min(Budget, MaxSeverPerHit);
	Budget -= SeverWindowParts;
	if ( Budget <= 0 )
		return 0;

	while ( n < Budget )
	{
		Best = -1;
		for ( i = BONE_Spine; i < NUM_BONES; i++ )
		{
			if ( (Mask & (1 << i)) == 0 || !BodyAttached(i) )
				continue;
			if ( (i == BONE_Head || i == BONE_Neck) && bDecapitated )
				continue;
			D = VSize(GetBodyLocation(i) - At);
			if ( Best < 0 || D < BestD )
			{
				Best = i;
				BestD = D;
			}
		}
		if ( Best < 0 )
			break;
		Mask = Mask & ~(1 << Best);
		if ( TearOff(Best, SeverImpulse(Best, At, Mom, Damage, bBlast)) )
			n++;
	}
	if ( n == 0 && SeverWindowParts == 0 && SeverDamage > 0 && SeverWindowDamage >= SeverDamage )
	{
		Best = NearestLimbPart(At, Damage >= 90);
		if ( Best >= 0 && TearOff(Best, SeverImpulse(Best, At, Mom, Damage, bBlast)) )
			n++;
	}
	SeverWindowParts += n;
	return n;
}

// Velocity change (UU/s) for a part torn off by a hit at At.
function vector SeverImpulse(int Body, vector At, vector Mom, int Damage, bool bBlast)
{
	local vector Loc, Impulse;

	Loc = GetBodyLocation(Body);
	if ( bBlast )
	{
		if ( VSize(Loc - At) < 4 )
			Impulse = VRand() * 200;
		else
			Impulse = Normal(Loc - At) * (150 + 2 * Clamp(Damage, 0, 150));
		Impulse.Z += 150;
	}
	else
	{
		if ( VSize(Mom) > 1 )
			Impulse = Normal(Mom) * (60 + 2 * Clamp(Damage, 0, 100));
		else
			Impulse = VRand() * 80;
		Impulse.Z += 60;
	}
	return Impulse;
}

// The limb (arm L/R, leg L/R) with an attached part nearest At: its
// forearm/shin, or the whole upper arm/thigh when bWhole (or when the distal
// part is already gone). -1 when no limb is left.
function int NearestLimbPart(vector At, bool bWhole)
{
	local int g, Best, Groups[4], Distal[4];
	local float D, BestD;

	Groups[0] = BONE_UpperArmL; Distal[0] = BONE_ForearmL;
	Groups[1] = BONE_UpperArmR; Distal[1] = BONE_ForearmR;
	Groups[2] = BONE_ThighL;    Distal[2] = BONE_ShinL;
	Groups[3] = BONE_ThighR;    Distal[3] = BONE_ShinR;
	Best = -1;
	for ( g = 0; g < 4; g++ )
	{
		if ( !BodyAttached(Groups[g]) )
			continue;
		D = VSize(GetBodyLocation(Groups[g]) - At);
		if ( BodyAttached(Distal[g]) )
			D = FMin(D, VSize(GetBodyLocation(Distal[g]) - At));
		if ( Best < 0 || D < BestD )
		{
			Best = g;
			BestD = D;
		}
	}
	if ( Best < 0 )
		return -1;
	if ( !bWhole && BodyAttached(Distal[Best]) )
		return Distal[Best];
	return Groups[Best];
}

// Native Gib: bTotal scatters whatever is still attached, else ONE part
// nearest the hit comes off. Blood and a sound per part. Returns the number
// of parts cut.
function int GibCorpse(int Damage, vector HitLoc, bool bTotal)
{
	local int i, Before, After, Cut;

	for ( i=1; i<NUM_BONES; i++ )
		if ( BodyAttached(i) )
			Before++;
	if ( HitLoc == vect(0,0,0) )
		HitLoc = Location;
	Gib(Damage, HitLoc, bTotal);
	for ( i=1; i<NUM_BONES; i++ )
		if ( BodyAttached(i) )
			After++;
	Cut = Before - After;
	if ( Cut <= 0 )
		return 0;
	if ( BloodBurstClass != None )
		for ( i=0; i<Min(Cut + 1, 4); i++ )
			Spawn(BloodBurstClass,,, HitLoc + VRand() * 8, rot(16384,0,0));
	for ( i=0; i<Min(Cut, 3); i++ )
		AddBleed(BONE_Spine + Rand(NUM_BONES - 1), 2.0);
	if ( Cut >= 3 )
		PlayGibSound();
	else
		PlayCutSound();
	return Cut;
}

function bool GoreAllowed()
{
	return GoreLevel > 0 && !class'GameInfo'.default.bLowGore && !class'GameInfo'.default.bVeryLowGore;
}

// Tear a body part off the corpse with the given velocity change (UU/s).
// Limbs leave with their children (forearm + hand, thigh + shin).
function bool TearOff(int Body, vector Impulse)
{
	local vector Cut;

	if ( Body == BONE_Head || Body == BONE_Neck )
	{
		Decapitate(true);
		return true;
	}
	// Limbs, and the spine (torso torn in two at the waist: the upper body
	// with arms and head leaves, the pelvis and legs stay the actor's island).
	if ( Body < BONE_Spine || Body >= NUM_BONES || !BodyAttached(Body) )
		return false;
	Cut = GetBodyLocation(Body);
	if ( !Dismember(Body, Impulse) )
		return false;
	if ( BloodBurstClass != None )
	{
		Spawn(BloodBurstClass,,, Cut, rot(16384,0,0));
		Spawn(BloodBurstClass,,, Cut + VRand() * 6, rotator(Impulse));
	}
	AddBleed(Body, 2.5);
	PlayCutSound();
	return true;
}

// Remove the head. The stock UTHeads chunk is the visible head: on a stock
// decapitation (PlayDecap) it was spawned before this corpse existed and
// carries the dead player's camera, so it is left alone; for our own
// decapitations we spawn the matching head chunk ourselves.
function Decapitate(bool bSpawnHead)
{
	local vector HeadLoc, NeckLoc;
	local Carcass Head;

	if ( bDecapitated )
		return;
	bDecapitated = true;
	HeadLoc = GetBodyLocation(BONE_Head);
	if ( !RemovePart(BONE_Head) )
		return;
	NeckLoc = GetBodyLocation(BONE_Neck);
	if ( bSpawnHead && HeadClass != None )
	{
		Head = Spawn(HeadClass, self,, HeadLoc, Rotation);
		if ( Head != None )
		{
			Head.Initfor(self);
			Head.Velocity = Velocity + VRand() * 200 + vect(0,0,220);
		}
	}
	if ( BloodBurstClass != None )
	{
		Spawn(BloodBurstClass,,, NeckLoc, rot(16384,0,0));
		Spawn(BloodBurstClass,,, NeckLoc + vect(0,0,6), rot(16384,0,0));
	}
	AddBleed(BONE_Neck, 3.0);
	PlayCutSound();
}

// Gib the corpse. Carcass.ChunkUp is Destroy(), which is why a gibbing kill
// (Pawn.SpawnGibbedCarcass: Initfor + ChunkUp(-Health)) used to make the
// fresh ragdoll vanish. 250+ in one hit (redeemer, crushers, telefrags)
// scatters every body as closed, capped skinned islands ("tiny bits").
// Anything smaller is the engine's own gib verdict on a kill (80+ overkill,
// or 40+ with a 60% roll): when the killing blow already went through Wound()
// this tick it has done its tearing and only the blood is added; otherwise
// (no hit record) ONE part nearest the hit comes off. Only a fully scattered
// corpse is marked bGibbed and gets the 20 s stock gib lifetime.
function ChunkUp(int Damage)
{
	local vector Loc;
	local Carcass Organ;
	local int i, n, Remaining;

	if ( bGibbed )
		return;
	if ( !GoreAllowed() )
	{
		bGibbed = true;
		Destroy();
		return;
	}
	if ( LastHitLocation == vect(0,0,0) )
		LastHitLocation = Location;
	if ( Damage >= 250 )
	{
		Log("RagdollCarcass: total gib " $ self $ " dmg=" $ Damage);
		GibCorpse(Damage, LastHitLocation, true);
	}
	else if ( LastWoundTime == 0 || Level.TimeSeconds - LastWoundTime > 0.05 )
		GibCorpse(Damage, LastHitLocation, false);   // the engine gibbed it: one part goes
	Remaining = 0;
	for ( i=1; i<NUM_BONES; i++ )
		if ( BodyAttached(i) )
			Remaining++;
	bGibbed = (Remaining == 0);

	if ( BigBloodClass != None )
		Spawn(BigBloodClass,,, Location, rot(-16384,0,0));
	if ( BloodBurstClass != None )
	{
		n = 2 + Rand(2);
		for ( i=0; i<n; i++ )
		{
			Loc = GetBodyLocation(Rand(NUM_BONES));
			Spawn(BloodBurstClass,,, Loc, rot(16384,0,0));
		}
	}
	if ( Level.bHighDetailMode && !Level.bDropDetail && !bOrganSpawned )
	{
		bOrganSpawned = true;
		if ( FRand() < 0.3 )
			i = 0;
		else if ( FRand() < 0.5 )
			i = 1;
		else
			i = 2;
		if ( OrganClass[i] != None )
		{
			Organ = Spawn(OrganClass[i], self,, GetBodyLocation(BONE_Spine), Rotation);
			if ( Organ != None )
				Organ.Initfor(self);
		}
	}
	for ( i=0; i<3; i++ )
		AddBleed(BONE_Spine + Rand(NUM_BONES - 1), 2.0);
	PlayGibSound();
	if ( bGibbed )
		LifeSpan = 20;
}

function PlayGibSound()
{
	local int i;

	i = Rand(4);
	if ( GibSounds[i] != None )
	{
		PlaySound(GibSounds[i], SLOT_Interact, 16);
		PlaySound(GibSounds[i], SLOT_Misc, 12);
	}
}

function PlayCutSound()
{
	local int i;

	i = Rand(4);
	if ( CutSounds[i] != None )
		PlaySound(CutSounds[i], SLOT_Interact, 10);
}

function AddBleed(int Body, float Duration)
{
	local int i, Slot;

	Slot = -1;
	for ( i=0; i<ArrayCount(BleedBody); i++ )
		if ( BleedEnd[i] <= Level.TimeSeconds )
		{
			Slot = i;
			break;
		}
	if ( Slot < 0 )
		return;
	BleedBody[Slot] = Body;
	BleedEnd[Slot]  = Level.TimeSeconds + Duration;
}

// Called from the sim Timer: bursts along the detached parts, a pool at rest.
function Bleed()
{
	local int i;
	local vector Loc;

	if ( Level.TimeSeconds < NextBleed )
		return;
	NextBleed = Level.TimeSeconds + 0.15;
	for ( i=0; i<ArrayCount(BleedBody); i++ )
	{
		if ( BleedEnd[i] <= 0 )
			continue;
		Loc = GetBodyLocation(BleedBody[i]);
		if ( Level.TimeSeconds < BleedEnd[i] )
		{
			if ( BloodBurstClass != None && FRand() < 0.6 && !Level.bDropDetail )
				Spawn(BloodBurstClass,,, Loc + VRand() * 4, rot(16384,0,0));
		}
		else
		{
			if ( BloodPoolClass != None )
				Spawn(BloodPoolClass,,, Loc, rot(16384,0,0));
			BleedEnd[i] = 0;
		}
	}
}

// The actor (pelvis) crossed into another zone. The per-body physics (water
// drag, buoyancy, zone gravity, destructive zones) is native and runs for
// every body every tick; this adds the stock splash on a water entry and
// the immediate despawn in a destructive zone (lava / slime), with the
// zone's own entry effect so the corpse does not just blink out.
simulated function ZoneChange(ZoneInfo NewZone)
{
	local actor Splash;
	local float SplashSize;

	if ( NewZone == None )
		return;
	Log("RagdollCarcass: ZoneChange " $ self $ " -> " $ NewZone $ " water=" $ NewZone.bWaterZone $ " destructive=" $ NewZone.bDestructive $ " gravity=" $ NewZone.ZoneGravity.Z);
	if ( NewZone.bWaterZone || NewZone.bDestructive )
	{
		SplashSize = FClamp(0.0005 * (250 + 0.5 * VSize(Velocity)), 0.1, 0.5);
		if ( Level.NetMode != NM_DedicatedServer )
		{
			if ( NewZone.EntrySound != None )
				PlaySound(NewZone.EntrySound, SLOT_Interact, SplashSize * 2);
			if ( NewZone.EntryActor != None )
			{
				Splash = Spawn(NewZone.EntryActor);
				if ( Splash != None )
					Splash.DrawScale = 4 * SplashSize;
			}
		}
	}
	if ( NewZone.bDestructive )
		Destroy();
}

function PostBeginPlay()
{
	Super.PostBeginPlay();
	SetPhysics(PHYS_None);
	// NOTE: do NOT disable collision here. The defaultproperties already set
	// bCollideActors=True / bBlockActors=False / bProjTarget=True so projectiles
	// and hitscan can hit the corpse; SetCollision(false,false,false) previously
	// made the corpse completely unhittable (shooting did nothing).
	SetTimer(1.0 / SimFreq, true);
}

function Initfor(actor Other)
{
	local int i;

	Super.Initfor(Other);
	PlayerOwner = Pawn(Other).PlayerReplicationInfo;
	StockCarcassClass = FindCarcassClass(Other);
	SourceMesh = Other.Mesh;
	DeathAnim = Other.AnimSequence;
	bFemale = Other.IsA('TournamentFemale') || Other.IsA('FemaleBotPlus');
	bBoss = Other.IsA('TBoss');
	PrePivot = vect(0,0,0);
	for ( i=0; i<4; i++ )
		Multiskins[i] = Pawn(Other).MultiSkins[i];
	bDecorative = false;
	bMeshCurvy = Other.bMeshCurvy;
	bMeshEnviroMap = Other.bMeshEnviroMap;
	Mesh = Other.Mesh;
	Skin = Other.Skin;
	Texture = Other.Texture;
	Fatness = Other.Fatness;
	DrawScale = Other.DrawScale;
	SetCollisionSize(Other.CollisionRadius + 4, Other.CollisionHeight);
	Velocity = Other.Velocity;
	Mass = Other.Mass;
	if ( Region.Zone != None )
		ZoneGravityUU = Region.Zone.ZoneGravity;   // read natively at BuildRagdoll (low-grav zones)
	// Freeze the pawn's animation state on the carcass (Engine Carcass.Initfor
	// is empty, so nothing copied it): the native build fits the ragdoll's
	// spawn pose to this frame of the pawn mesh (ApplyAnimationPose, bAnimPose).
	// The pawn is ~0.2 s into its Dead* animation here (Dying: Sleep(0.2);
	// SpawnCarcass()). Rates zeroed so the engine never advances it.
	AnimSequence = Other.AnimSequence;
	AnimFrame = Other.AnimFrame;
	AnimRate = 0;
	TweenRate = 0;
	SetDeathPose(Pawn(Other).Rotation.Yaw, Pawn(Other).Rotation.Pitch);
	RecordKillingBlow(Pawn(Other));
	BuildRagdoll();
	LoadGoreClasses();
	// The engine Carcass's auto state Dying "ignores TakeDamage" for 0.2 s
	// (Begin: Sleep(0.2); GotoState('Dead')), so stock corpses are not
	// hittable while they start to fall. A ragdoll must be hittable from its
	// first tick (a second rocket right after the kill is the common case),
	// so go to Dead now. GotoState in PostBeginPlay did not stick (the
	// engine applies the auto state after it); a subclass state cannot
	// un-ignore a probe either (both tried 2026-10-11 headless). Timer()
	// below covers corpses that never get Initfor (tests).
	GotoState('Dead');
	ApplyDeathGore(Pawn(Other));
}

// Killing blow (RagdollMutator's last hit on the pawn within 2 s) into
// LastHitLocation / LastHitMomentum BEFORE BuildRagdoll, so the native build
// can kick the body the shot hit (ApplyDeathVelocities, DeathHitScale).
// ApplyDeathGore reads the same record again for the gore decision.
function RecordKillingBlow(Pawn P)
{
	local RagdollMutator M;
	local vector HitLoc, Mom, Origin;
	local name Type;
	local int Dmg, BlastKind;

	LastHitLocation = vect(0,0,0);
	LastHitMomentum = vect(0,0,0);
	LastBlastOrigin = vect(0,0,0);
	LastHitDamage = 0;
	if ( P == None )
		return;
	foreach AllActors(class'RagdollMutator', M)
		break;
	if ( M != None && M.GetLastHit(P, 2.0, HitLoc, Mom, Type, Dmg, Origin, BlastKind) )
	{
		LastHitLocation = HitLoc;
		LastHitMomentum = Mom;
		LastHitDamage = Dmg + Max(0, -P.Health);
		if ( BlastKind != 0 )
			LastBlastOrigin = Origin;
	}
}

// CarcassType is declared on PlayerPawn and Bot (not Pawn), so read it by
// name; the text form is Class'Botpack.TMale1Carcass'.
function class<Carcass> FindCarcassClass(Actor Other)
{
	local string S;
	local int i;

	S = Other.GetPropertyText("CarcassType");
	i = InStr(S, "'");
	if ( i >= 0 )
	{
		S = Mid(S, i + 1);
		i = InStr(S, "'");
		if ( i >= 0 )
			S = Left(S, i);
	}
	if ( S == "" || S ~= "None" )
		return None;
	return class<Carcass>(DynamicLoadObject(S, class'Class'));
}

function LoadGoreClasses()
{
	BloodBurstClass = class<Actor>(DynamicLoadObject("Botpack.UT_BloodBurst", class'Class'));
	BloodHitClass   = class<Actor>(DynamicLoadObject("Botpack.UT_BloodHit", class'Class'));
	BigBloodClass   = class<Actor>(DynamicLoadObject("Botpack.UT_BigBloodHit", class'Class'));
	BloodPoolClass  = class<Actor>(DynamicLoadObject("Botpack.UTBloodPool2", class'Class'));
	OrganClass[0]   = class<Carcass>(DynamicLoadObject("Botpack.UTLiver", class'Class'));
	OrganClass[1]   = class<Carcass>(DynamicLoadObject("Botpack.UTStomach", class'Class'));
	OrganClass[2]   = class<Carcass>(DynamicLoadObject("Botpack.UTHeart", class'Class'));
	if ( bBoss )
		HeadClass = class<Carcass>(DynamicLoadObject("Botpack.UT_bosshead", class'Class'));
	else if ( bFemale )
		HeadClass = class<Carcass>(DynamicLoadObject("Botpack.UT_HeadFemale", class'Class'));
	else
		HeadClass = class<Carcass>(DynamicLoadObject("Botpack.UT_HeadMale", class'Class'));
	GibSounds[0] = Sound(DynamicLoadObject("UnrealShare.Gibs.Gib1", class'Sound'));
	GibSounds[1] = Sound(DynamicLoadObject("Botpack.Male.NewGib", class'Sound'));
	GibSounds[2] = Sound(DynamicLoadObject("UnrealShare.Gibs.Gib4", class'Sound'));
	GibSounds[3] = Sound(DynamicLoadObject("UnrealShare.Gibs.Gib5", class'Sound'));
	CutSounds[0] = Sound(DynamicLoadObject("UnrealShare.Gibs.gibP1", class'Sound'));
	CutSounds[1] = Sound(DynamicLoadObject("UnrealShare.Gibs.gibP4", class'Sound'));
	CutSounds[2] = Sound(DynamicLoadObject("UnrealShare.Gibs.gibP5", class'Sound'));
	CutSounds[3] = Sound(DynamicLoadObject("UnrealShare.Gibs.gibP6", class'Sound'));
}

// Gore driven by the killing blow, applied right after the ragdoll is built.
//  - A stock decapitation (PlayDecap played Dead4 / Dead6 and already spawned
//    the rolling UTHeads chunk): remove our head and cap the neck.
//  - Otherwise the killing hit (looked up in RagdollMutator) goes through
//    Wound(), the SAME model as a hit on the corpse: a rocket kill books the
//    bodies around the explosion and always takes the nearest limb part, a
//    sniper kill books the body the bullet went through. The damage is what
//    the pawn was charged (the mutator folded a flak shot's chunks into one
//    record); the overkill only feeds the death spin and the engine's gib
//    verdict. No kick here - ApplyDeathVelocities already kicked the hit
//    body at build time.
function ApplyDeathGore(Pawn P)
{
	local RagdollMutator M;
	local vector HitLoc, Mom, Origin;
	local name Type;
	local int Dmg, BlastKind, Body;

	if ( P == None || !GoreAllowed() )
		return;
	if ( IsDecapAnim(DeathAnim) )
	{
		Decapitate(false);
		return;
	}
	foreach AllActors(class'RagdollMutator', M)
		break;
	if ( M == None || !M.GetLastHit(P, 2.0, HitLoc, Mom, Type, Dmg, Origin, BlastKind) )
		return;
	LastHitLocation = HitLoc;
	LastHitMomentum = Mom;
	PaintWound(HitLoc, Mom, Dmg, Type);
	if ( GoreLevel < 2 )
		return;
	if ( Type == 'Decapitated' )
	{
		Decapitate(true);
		LastWoundTime = Level.TimeSeconds;
		return;
	}
	Body = Wound(Dmg, Type, HitLoc, Mom, BlastKind != 0, Origin, true);
	Log("RagdollCarcass: death wound " $ self $ " type=" $ Type $ " blast=" $ (BlastKind != 0) $ " dmg=" $ Dmg $ " overkill=" $ Max(0, -P.Health) $ " body=" $ Body);
}

// PlayDecap plays Dead4 on the male/boss models and Dead6 on the female ones;
// neither is used by any other death.
function bool IsDecapAnim(name Anim)
{
	if ( bFemale )
		return Anim == 'Dead6';
	return Anim == 'Dead4';
}

function Timer()
{
	if ( !IsInState('Dead') )
		GotoState('Dead');   // see Initfor: Dying ignores TakeDamage
	StepRagdoll(1.0 / SimFreq);
	SyncRagdoll();
	CheckWalkPush();
	Bleed();
}

// A living pawn walking/running over the corpse shoves it out of the way.
// WalkOverPush is a native velocity-matching push (bodies under the pawn are
// eased toward the pawn's horizontal motion, capped) so it slides the corpse
// aside at the pawn's pace instead of launching it: impulses applied every
// tick would stack and rocket the ragdoll into the sky.
function CheckWalkPush()
{
	local Pawn P;

	foreach AllActors(class'Pawn', P)
	{
		if ( P.Health > 0 && VSize(P.Velocity) > 80 && VSize(P.Location - Location) < 240 )
			WalkOverPush(P);
	}
}

simulated function Destroyed()
{
	ShutdownRagdoll();
	Super.Destroyed();
}

state Dead
{
	function Timer()
	{
		StepRagdoll(1.0 / SimFreq);
		SyncRagdoll();
		CheckWalkPush();
		Bleed();
	}
	function BeginState()
	{
		SetTimer(1.0 / SimFreq, true);
	}
}

defaultproperties
{
	WaterBuoyancy=1.08
	WaterDrag=2.0
	AirDrag=0.5
	ZoneWindScale=1.0
	bMoverCollision=True
	bCrushDestroys=True
	SimFreq=50.0
	SimSubSteps=8
	bSelfCollide=True
	bFragmentBodies=False
	JointSpringHertz=0.8
	JointSpringDamping=0.8
	DeathImpulseScale=1.0
	bBoxMannequins=false
	JointFrictionScale=1.0
	JointLimitScale=1.0
	AngularDamping=0.4
	DeathSpin=1.2
	DeathHitScale=0.8
	PoseVariation=1.0
	bAnimPose=True
	GravityScale=1.0
	BodyMassScale=1.0
	HitImpulseMass=25.0
	BeamPushScale=18.0
	BeamPushTime=0.25
	DeathSpinChance=0.2
	ConstraintHertz=120.0
	ConstraintDamping=5.0
	GoreLevel=2
	DismemberScale=1.0
	BulletCutPower=0.5
	BlastCutPower=0.9
	BlastImpulseScale=0.6
	bPlanarCuts=true
	CutVariation=1.0
	bBoneStubs=false
	BoneStubChance=1.0
	BoneStubLength=1.0
	bModelledCaps=true
	TorsoSeverDamage=85
	SeverDamage=50
	MaxSeverPerHit=2
	bAlwaysTick=true
	RemoteRole=ROLE_SimulatedProxy
	DrawType=DT_Mesh
	Physics=PHYS_None
	Texture=Texture'Engine.S_Corpse'
	CollisionRadius=18.000000
	CollisionHeight=4.000000
	bCollideActors=True
	bBlockActors=False
	bCollideWorld=False
	bProjTarget=True
	Mass=180.000000
	Buoyancy=105.000000
}
