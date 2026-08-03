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
	native;

var float SimFreq;
var int   SimSubSteps;

native function BuildRagdoll();
native function StepRagdoll(float DeltaSeconds);
native function SyncRagdoll();
native function ShutdownRagdoll();

function PostBeginPlay()
{
	Super.PostBeginPlay();
	SetPhysics(PHYS_None);
	SetCollision(false, false, false);
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
	BuildRagdoll();
}

function Timer()
{
	StepRagdoll(1.0 / SimFreq);
	SyncRagdoll();
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
	bAlwaysTick=true
	RemoteRole=ROLE_SimulatedProxy
	DrawType=DT_Mesh
	Physics=PHYS_None
	Texture=Texture'Engine.S_Corpse'
	CollisionRadius=18.000000
	CollisionHeight=4.000000
	bCollideActors=False
	bCollideWorld=False
	bProjTarget=False
	Mass=180.000000
	Buoyancy=105.000000
}
