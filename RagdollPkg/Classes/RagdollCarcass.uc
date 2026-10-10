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
// Gore (Soldier of Fortune style, see docs/plans/gore.md): the 12 physics
// bodies double as damage zones. A body can be torn off (Dismember: its joint
// is destroyed, the skin is split and capped natively, the part keeps
// simulating in the same world), the head can be removed in favour of the
// stock rolling UTHeads chunk (decapitation), and the whole corpse can be
// blown into one skinned island per body (Gib). Triggers: the killing blow
// (looked up in RagdollMutator), hits on the corpse accumulating per part,
// and the stock Carcass gib thresholds.
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
//   AngularDamping      per-body angular damping (1/s); lower = floppier
//   DeathSpin           random angular velocity (rad/s) given to each body at death
//   PoseVariation       spread of the random spawn pose (0 = fixed weapon-aim
//                       pose for every corpse, 1 = default, up to 2)
var() globalconfig float JointFrictionScale;
var() globalconfig float JointLimitScale;
var() globalconfig float AngularDamping;
var() globalconfig float DeathSpin;
var() globalconfig float PoseVariation;

// Gore bookkeeping (script-only; appended after the natively mirrored vars so
// the C++ class layout in RagdollPkgClasses.h stays valid).
var class<Carcass> StockCarcassClass; // the pawn's CarcassType (TMale1Carcass, ...)
var Mesh           SourceMesh;        // the pawn's LodMesh (Mesh is swapped for the ragdoll mesh)
var name           DeathAnim;         // pawn AnimSequence at death (Dead4/Dead6 = decapitated)
var bool           bGibbed;

// Gore config (console "set RagdollPkg.RagdollCarcass GoreLevel 2", persisted):
//   GoreLevel       0 = ragdoll only, 1 = decapitation + gibbing, 2 = + limbs
//                   torn off by the killing blow and by hits on the corpse
//   DismemberScale  multiplies every dismemberment threshold (2 = twice as
//                   much damage needed, 0.5 = limbs come off easily)
// The game's bLowGore / bVeryLowGore flags disable all of it.
var() globalconfig int   GoreLevel;
var() globalconfig float DismemberScale;

var float  PartDamage[14];     // damage accumulated per body by hits on the corpse
var bool   bDecapitated;
var bool   bFemale;
var bool   bBoss;
var vector LastHitLocation;    // killing blow (from RagdollMutator) or last corpse hit
var vector LastHitMomentum;
var bool   bOrganSpawned;

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
native function int ApplyDamageImpulse(vector HitLocation, vector Momentum, int Damage); // returns nearest body
native function WalkOverPush(Pawn Pusher);
native function SetDeathPose(int Yaw, int Pitch);
// Gore natives (see RagdollCarcass.cpp / RagdollGore.cpp).
native function bool   Dismember(int Body, vector Impulse);    // tear the subtree off; Impulse in UU/s
native function bool   RemovePart(int Body);                   // tear off AND delete (decapitation)
native function int    Gib(int Damage, vector HitLocation);    // partial/total blast; returns joints left
native function vector GetBodyLocation(int Body);
native function int    NearestBody(vector Loc);
native function bool   BodyAttached(int Body);
native function        PaintWound(vector HitLocation, vector Dir, int Damage, name DamageType); // skin mark along the shot, style per damage type

// Projectiles and hitscan call TakeDamage on the corpse. We translate the
// hit into a Box3D impulse on the bodies near the impact point. The base
// Carcass/Pawn damage handling is deliberately skipped: our actor has no
// movement physics of its own (PHYS_None), and letting the base physics code
// run would fight the Box3D sync. The stock Carcass gib thresholds are kept
// so a corpse under sustained fire still gibs: a single hit above 40% of
// Mass, or accumulated damage above 80% of Mass ('shot' damage counts 40%).
// Below that, damage accumulates per body part and tears it off past the
// part's own threshold (head 40, forearm/shin 60, upper arm 70, thigh 90).
function TakeDamage(int Damage, Pawn instigatedBy, Vector hitlocation, Vector momentum, name damageType)
{
	local int Body;
	local float Scaled;

	if ( bDecorative || bGibbed )
		return;
	Body = ApplyDamageImpulse(hitlocation, momentum, Damage);
	LastHitLocation = hitlocation;
	LastHitMomentum = momentum;
	if ( GoreAllowed() )
	{
		if ( BloodHitClass != None )
			Spawn(BloodHitClass,,, hitlocation, rotator(momentum));
		PaintWound(hitlocation, momentum, Damage, damageType);   // every hit on the corpse, not only the kill
	}

	Scaled = Damage;
	if ( damageType == 'shot' )
		Scaled *= 0.4;
	CumulativeDamage += Scaled;
	if ( (Scaled > 0.4 * Mass) || (CumulativeDamage > 0.8 * Mass) )
	{
		ChunkUp(Damage);
		return;
	}
	if ( GoreLevel < 2 || !GoreAllowed() || Body < 0 || Body >= NUM_BONES || !BodyAttached(Body) )
		return;
	PartDamage[Body] += Scaled;
	if ( PartDamage[Body] > PartThreshold(Body) * DismemberScale )
		TearOff(Body, momentum, Damage);
}

function float PartThreshold(int Body)
{
	switch ( Body )
	{
		case BONE_Head:
		case BONE_Neck:      return 40;
		case BONE_ForearmL:
		case BONE_ForearmR:
		case BONE_ShinL:
		case BONE_ShinR:
		case BONE_FootL:
		case BONE_FootR:     return 60;
		case BONE_UpperArmL:
		case BONE_UpperArmR: return 70;
		case BONE_ThighL:
		case BONE_ThighR:    return 90;
	}
	return 100000; // pelvis / spine never come off (that is a gib)
}

function bool GoreAllowed()
{
	return GoreLevel > 0 && !class'GameInfo'.default.bLowGore && !class'GameInfo'.default.bVeryLowGore;
}

// Tear the body part nearest a hit off the corpse. Limbs leave with their
// children (forearm + hand, thigh + shin); a head or neck hit decapitates.
function TearOff(int Body, vector Momentum, int Damage)
{
	local vector Impulse, Cut;

	if ( Body == BONE_Head || Body == BONE_Neck )
	{
		Decapitate(true);
		return;
	}
	if ( Body < BONE_UpperArmL || Body >= NUM_BONES || !BodyAttached(Body) )
		return;
	if ( VSize(Momentum) > 1 )
		Impulse = Normal(Momentum) * (250 + 3 * Clamp(Damage, 0, 150));
	else
		Impulse = VRand() * 250;
	Impulse.Z += 180;
	Cut = GetBodyLocation(Body);
	if ( !Dismember(Body, Impulse) )
		return;
	if ( BloodBurstClass != None )
	{
		Spawn(BloodBurstClass,,, Cut, rot(16384,0,0));
		Spawn(BloodBurstClass,,, Cut + VRand() * 6, rotator(Impulse));
	}
	AddBleed(Body, 2.5);
	PlayCutSound();
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
// fresh ragdoll vanish. Now the joints are destroyed natively and the skinned
// bodies fly apart as closed, capped islands; the stock blood splashes and
// one organ chunk are spawned on top (the stock torso/arm/thigh meshes are
// replaced by our skinned parts). How much comes apart scales with Damage
// (plus chance): a glancing rocket tears a few parts off and leaves a trunk
// that later hits can keep gibbing; only a fully scattered corpse is marked
// bGibbed and gets the 20 s stock gib lifetime.
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
	Remaining = Gib(Damage, LastHitLocation);
	bGibbed = (Remaining == 0);
	CumulativeDamage = 0;
	for ( i=0; i<NUM_BONES; i++ )
		PartDamage[i] = 0;

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
	SetDeathPose(Pawn(Other).Rotation.Yaw, Pawn(Other).Rotation.Pitch);
	BuildRagdoll();
	LoadGoreClasses();
	ApplyDeathGore(Pawn(Other));
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
//  - Otherwise look the killing hit up in RagdollMutator and, with enough
//    overkill, tear off the body nearest the shot (flak/rocket/ripper...).
function ApplyDeathGore(Pawn P)
{
	local RagdollMutator M;
	local vector HitLoc, Mom;
	local name Type;
	local int Dmg, Body, Overkill;
	local bool bExplosive;

	if ( P == None || !GoreAllowed() )
		return;
	if ( IsDecapAnim(DeathAnim) )
	{
		Decapitate(false);
		return;
	}
	foreach AllActors(class'RagdollMutator', M)
		break;
	if ( M == None || !M.GetLastHit(P, 2.0, HitLoc, Mom, Type, Dmg) )
		return;
	LastHitLocation = HitLoc;
	LastHitMomentum = Mom;
	PaintWound(HitLoc, Mom, Dmg, Type);
	if ( GoreLevel < 2 )
		return;
	Overkill = -P.Health;
	Body = NearestBody(HitLoc);
	bExplosive = (Type == 'exploded') || (Type == 'shredded') || (Type == 'RedeemerDeath')
		|| (Type == 'pounded') || (Type == 'jolted') || (Type == 'zapped');
	if ( Type == 'Decapitated' )
		TearOff(BONE_Head, Mom, Dmg);
	else if ( bExplosive && Overkill >= 15 * DismemberScale )
		TearOff(Body, Mom, Dmg);
	else if ( Type == 'shot' && Dmg >= 45 && Overkill >= 5 * DismemberScale )
		TearOff(Body, Mom, Dmg);
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
	SimFreq=50.0
	SimSubSteps=4
	DeathImpulseScale=1.0
	bBoxMannequins=false
	JointFrictionScale=1.0
	JointLimitScale=1.0
	AngularDamping=0.15
	DeathSpin=2.0
	PoseVariation=1.0
	GoreLevel=2
	DismemberScale=1.0
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
