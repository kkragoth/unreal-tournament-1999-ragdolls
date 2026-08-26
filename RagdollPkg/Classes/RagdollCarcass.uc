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
// run would fight the Box3D sync. Gibbing can be layered on later.
function TakeDamage(int Damage, Pawn instigatedBy, Vector hitlocation, Vector momentum, name damageType)
{
	if ( bDecorative )
		return;
	ApplyDamageImpulse(hitlocation, momentum, Damage);
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
