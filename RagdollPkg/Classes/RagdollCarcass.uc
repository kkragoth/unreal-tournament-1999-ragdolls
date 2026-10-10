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
class RagdollCarcass extends Carcass
	native
	config;

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

native function BuildRagdoll();
native function StepRagdoll(float DeltaSeconds);
native function SyncRagdoll();
native function ShutdownRagdoll();
native function ApplyDamageImpulse(vector HitLocation, vector Momentum, int Damage);
native function WalkOverPush(Pawn Pusher);
native function SetDeathPose(int Yaw, int Pitch);

// Projectiles and hitscan call TakeDamage on the corpse. We translate the
// hit into a Box3D impulse on the bodies near the impact point. The base
// Carcass/Pawn damage handling is deliberately skipped: our actor has no
// movement physics of its own (PHYS_None), and letting the base physics code
// run would fight the Box3D sync. The stock Carcass gib thresholds are kept
// so a corpse under sustained fire still gibs: a single hit above 40% of
// Mass, or accumulated damage above 80% of Mass ('shot' damage counts 40%).
function TakeDamage(int Damage, Pawn instigatedBy, Vector hitlocation, Vector momentum, name damageType)
{
	if ( bDecorative || bGibbed )
		return;
	ApplyDamageImpulse(hitlocation, momentum, Damage);
	if ( damageType == 'shot' )
		Damage *= 0.4;
	CumulativeDamage += Damage;
	if ( (Damage > 0.4 * Mass) || (CumulativeDamage > 0.8 * Mass) )
		ChunkUp(Damage);
}

// Gib the corpse. Carcass.ChunkUp is Destroy(), which is why a gibbing kill
// (Pawn.SpawnGibbedCarcass: Initfor + ChunkUp(-Health)) used to make the
// fresh ragdoll vanish. Interim implementation: spawn the pawn's stock carcass
// class (owned by us, so the spawn notify lets it through), hand it our
// appearance and let ITS ChunkUp spawn the stock UT_BigBloodHit + the
// MasterReplacement chunk (torso, arm, thighs, feet, organ, head unless the
// death anim is a decapitation), then remove the ragdoll.
function ChunkUp(int Damage)
{
	local Carcass Stock;
	local int i;

	if ( bGibbed )
		return;
	bGibbed = true;
	if ( StockCarcassClass != None )
	{
		Stock = Spawn(StockCarcassClass, self,, Location, Rotation);
		if ( Stock != None )
		{
			Stock.PlayerOwner = PlayerOwner;
			Stock.bDecorative = false;
			Stock.Mesh = SourceMesh;
			Stock.Skin = Skin;
			for ( i=0; i<4; i++ )
				Stock.MultiSkins[i] = MultiSkins[i];
			Stock.DrawScale = DrawScale;
			Stock.Fatness = Fatness;
			Stock.AnimSequence = DeathAnim;
			Stock.Velocity = Velocity;
			Stock.Mass = Mass;
			Stock.ChunkUp(Damage);
		}
		else
			Log("RagdollCarcass: ChunkUp could not spawn " $ StockCarcassClass, 'RagdollTest');
	}
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

function Timer()
{
	StepRagdoll(1.0 / SimFreq);
	SyncRagdoll();
	CheckWalkPush();
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
