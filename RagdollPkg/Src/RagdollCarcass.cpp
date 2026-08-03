/*=============================================================================
	RagdollCarcass.cpp
	Native implementation of the Box3D ragdoll carcass.
=============================================================================*/

#include "RagdollPkg.h"
#include "RagdollInternal.h"
#include "UnRagdollMesh.h"
#include "box3d/box3d.h"

/*-----------------------------------------------------------------------------
	The following must be done once per package (.dll).
-----------------------------------------------------------------------------*/

// C++/UnrealScript glue logic.
#define NAMES_ONLY
#define AUTOGENERATE_NAME(name) RAGDOLLPKG_API FName RAGDOLLPKG_##name;
#define AUTOGENERATE_FUNCTION(cls,idx,name) IMPLEMENT_FUNCTION(cls,idx,name)
#include "RagdollPkgClasses.h"
#undef AUTOGENERATE_FUNCTION
#undef AUTOGENERATE_NAME
#undef NAMES_ONLY
void RegisterNames()
{
	static INT Registered=0;
	if(!Registered++)
	{
		#define NAMES_ONLY
		#define AUTOGENERATE_NAME(name) extern RAGDOLLPKG_API FName RAGDOLLPKG_##name; RAGDOLLPKG_##name=FName(TEXT(#name),FNAME_Intrinsic);
		#define AUTOGENERATE_FUNCTION(cls,idx,name)
		#include "RagdollPkgClasses.h"
		#undef DECLARE_NAME
		#undef NAMES_ONLY
	}
}

// Package implementation.
IMPLEMENT_PACKAGE(RagdollPkg);

/*-----------------------------------------------------------------------------
	Bone layout and process-global state.
-----------------------------------------------------------------------------*/

const FRagdollBoneDef GRagdollBoneDefs[RAGDOLL_BONE_COUNT] =
{
	{ 7.0f,  7.0f,  5.0f,    0.0f,   0.0f,   0.0f, 8.0f },   // BONE_Pelvis
	{ 8.0f,  6.0f, 10.0f,    0.0f,   0.0f,  16.0f, 10.0f },  // BONE_Spine
	{ 4.5f,  4.5f,  4.5f,    0.0f,   0.0f,  33.0f, 4.0f },   // BONE_Head
	{ 2.0f,  2.0f,  7.0f,  -13.0f,   6.0f,  24.0f, 3.0f },   // BONE_UpperArmL
	{ 2.0f,  2.0f,  7.0f,   13.0f,   6.0f,  24.0f, 3.0f },   // BONE_UpperArmR
	{ 1.5f,  1.5f,  6.0f,  -13.0f,   8.0f,   6.0f, 2.0f },   // BONE_ForearmL
	{ 1.5f,  1.5f,  6.0f,   13.0f,   8.0f,   6.0f, 2.0f },   // BONE_ForearmR
	{ 3.0f,  3.0f,  8.0f,   -7.0f,   0.0f, -12.0f, 6.0f },   // BONE_ThighL
	{ 3.0f,  3.0f,  8.0f,    7.0f,   0.0f, -12.0f, 6.0f },   // BONE_ThighR
	{ 2.5f,  2.5f,  8.0f,   -7.0f,   0.0f, -30.0f, 3.0f },   // BONE_ShinL
	{ 2.5f,  2.5f,  8.0f,    7.0f,   0.0f, -30.0f, 3.0f }    // BONE_ShinR
};

TMap<AActor*, FRagdollState>& GetRagdollStates()
{
	static TMap<AActor*, FRagdollState> States;
	return States;
}

/*-----------------------------------------------------------------------------
	Box3D world construction.
-----------------------------------------------------------------------------*/

// Anchor offsets are canonical (relative to the pelvis) and are scaled by
// DrawScale here to match the scaled bodies.
static void AddJoint( FRagdollState& S, INT BodyA, INT BodyB, const FVector& AnchorCanonical, FLOAT DrawScale, UBOOL bRevolute )
{
	const FRagdollBoneDef& DA = GRagdollBoneDefs[BodyA];
	const FRagdollBoneDef& DB = GRagdollBoneDefs[BodyB];
	FVector LA = DrawScale * ( AnchorCanonical - FVector( DA.CX, DA.CY, DA.CZ ) );
	FVector LB = DrawScale * ( AnchorCanonical - FVector( DB.CX, DB.CY, DB.CZ ) );
	b3Transform FrameA = { { LA.X*RAGDOLL_U2M, LA.Y*RAGDOLL_U2M, LA.Z*RAGDOLL_U2M }, b3Quat_identity };
	b3Transform FrameB = { { LB.X*RAGDOLL_U2M, LB.Y*RAGDOLL_U2M, LB.Z*RAGDOLL_U2M }, b3Quat_identity };

	if( bRevolute )
	{
		// Revolute joints rotate about the local Z of the joint frame. Orient
		// the frame so that local Z points along world Y (the flexion axis).
		b3Quat Hinge = b3MakeQuatFromAxisAngle( { 1.0f, 0.0f, 0.0f }, 1.5707963f );
		FrameA.q = Hinge;
		FrameB.q = Hinge;

		b3RevoluteJointDef J = b3DefaultRevoluteJointDef();
		J.base.bodyIdA       = S.Bodies[BodyA];
		J.base.bodyIdB       = S.Bodies[BodyB];
		J.base.localFrameA   = FrameA;
		J.base.localFrameB   = FrameB;
		J.targetAngle        = 0.0f;
		J.enableLimit        = true;
		J.lowerAngle         = -0.9f;
		J.upperAngle         = 0.9f;
		J.enableSpring       = false;
		b3CreateRevoluteJoint( S.World, &J );
	}
	else
	{
		b3SphericalJointDef J = b3DefaultSphericalJointDef();
		J.base.bodyIdA       = S.Bodies[BodyA];
		J.base.bodyIdB       = S.Bodies[BodyB];
		J.base.localFrameA   = FrameA;
		J.base.localFrameB   = FrameB;
		J.enableSpring       = false;
		J.enableConeLimit    = false;
		J.enableTwistLimit   = false;
		b3CreateSphericalJoint( S.World, &J );
	}
}

static void BuildRagdollImpl( ARagdollCarcass* C )
{
	guard(BuildRagdollImpl);
	TMap<AActor*,FRagdollState>& Map = GetRagdollStates();
	FRagdollState& S = Map.Set( C, FRagdollState() );
	if( S.bBuilt )
		return;

	FLOAT DrawScale = C->DrawScale > 0.0f ? C->DrawScale : 1.0f;
	FVector Origin  = C->Location;

	// World.
	b3WorldDef Wd = b3DefaultWorldDef();
	Wd.gravity      = { 0.0f, 0.0f, -9.81f };
	Wd.enableSleep  = true;
	Wd.workerCount  = 0;
	S.World = b3CreateWorld( &Wd );

	// Ground: a large static slab whose top sits just under the corpse's feet.
	// NOTE: the slab must be placed at the corpse's XY, not at the box3d world
	// origin (0,0), or the body falls forever under gravity without landing.
	FLOAT FeetZ = Origin.Z - 38.0f * DrawScale;
	{
		b3BodyDef Bd = b3DefaultBodyDef();
		Bd.type     = b3_staticBody;
		Bd.position = { Origin.X*RAGDOLL_U2M, Origin.Y*RAGDOLL_U2M, ( FeetZ - 0.5f ) * RAGDOLL_U2M };
		S.Ground = b3CreateBody( S.World, &Bd );

		b3ShapeDef Sd = b3DefaultShapeDef();
		Sd.baseMaterial.friction     = 0.9f;
		Sd.baseMaterial.restitution  = 0.0f;
		b3BoxHull Hull = b3MakeBoxHull( 25.0f, 25.0f, 0.5f );
		b3CreateHullShape( S.Ground, &Sd, &Hull.base );
	}

	// Ragdoll bodies.
	for( INT i=0; i<RAGDOLL_BONE_COUNT; i++ )
	{
		const FRagdollBoneDef& D = GRagdollBoneDefs[i];
		FVector Center = Origin + DrawScale * FVector( D.CX, D.CY, D.CZ );

		b3BodyDef Bd = b3DefaultBodyDef();
		Bd.type           = b3_dynamicBody;
		Bd.position       = { Center.X*RAGDOLL_U2M, Center.Y*RAGDOLL_U2M, Center.Z*RAGDOLL_U2M };
		Bd.rotation       = b3Quat_identity;
		Bd.linearDamping  = 0.05f;
		Bd.angularDamping = 0.6f;
		Bd.gravityScale   = 1.0f;
		S.Bodies[i] = b3CreateBody( S.World, &Bd );

		FLOAT HX = D.HX * DrawScale * RAGDOLL_U2M;
		FLOAT HY = D.HY * DrawScale * RAGDOLL_U2M;
		FLOAT HZ = D.HZ * DrawScale * RAGDOLL_U2M;

		b3ShapeDef Sd = b3DefaultShapeDef();
		Sd.density = D.MassKg / ( 8.0f * HX * HY * HZ );
		Sd.baseMaterial.friction     = 0.8f;
		Sd.baseMaterial.restitution  = 0.05f;
		b3BoxHull Hull = b3MakeBoxHull( HX, HY, HZ );
		b3CreateHullShape( S.Bodies[i], &Sd, &Hull.base );

		S.RestPositions[i] = Bd.position;
		S.RestRotations[i] = b3Quat_identity;
	}

	// Joints.
	AddJoint( S, BONE_Pelvis,    BONE_Spine,     FVector(  0.0f, 0.0f,  6.0f), DrawScale, false );
	AddJoint( S, BONE_Spine,     BONE_Head,      FVector(  0.0f, 0.0f, 26.0f), DrawScale, false );
	AddJoint( S, BONE_Spine,     BONE_UpperArmL, FVector(-13.0f, 6.0f, 24.0f), DrawScale, false );
	AddJoint( S, BONE_Spine,     BONE_UpperArmR, FVector( 13.0f, 6.0f, 24.0f), DrawScale, false );
	AddJoint( S, BONE_UpperArmL, BONE_ForearmL,  FVector(-13.0f, 8.0f, 12.0f), DrawScale, true );
	AddJoint( S, BONE_UpperArmR, BONE_ForearmR,  FVector( 13.0f, 8.0f, 12.0f), DrawScale, true );
	AddJoint( S, BONE_Pelvis,    BONE_ThighL,    FVector( -7.0f, 0.0f, -6.0f), DrawScale, false );
	AddJoint( S, BONE_Pelvis,    BONE_ThighR,    FVector(  7.0f, 0.0f, -6.0f), DrawScale, false );
	AddJoint( S, BONE_ThighL,    BONE_ShinL,     FVector( -7.0f, 0.0f,-22.0f), DrawScale, true );
	AddJoint( S, BONE_ThighR,    BONE_ShinR,     FVector(  7.0f, 0.0f,-22.0f), DrawScale, true );

	// Procedural mannequin mesh.
	S.Mesh = CreateRagdollMesh( C );
	if( S.Mesh )
		C->Mesh = S.Mesh;

	S.bBuilt = true;
	debugf( TEXT("RagdollPkg: BuildRagdoll actor=%s loc=(%.0f,%.0f,%.0f) scale=%.2f"), C->GetName(), C->Location.X, C->Location.Y, C->Location.Z, DrawScale );
	unguard;
}

static void StepRagdollImpl( ARagdollCarcass* C, FLOAT DeltaSeconds )
{
	FRagdollState* S = GetRagdollStates().Find( C );
	if( !S || !S->bBuilt )
		return;
	if( DeltaSeconds <= 0.0f )
		return;
	FLOAT TimeStep = 1.0f / Max( C->SimFreq, 1.0f );
	INT   SubSteps = Max( C->SimSubSteps, 1 );
	b3World_Step( S->World, TimeStep, SubSteps );
	static INT LogCount = 0;
	if( LogCount < 200 )
	{
		LogCount++;
		debugf( TEXT("RagdollPkg: StepRagdoll actor=%s dt=%.4f steps=%i"), C->GetName(), DeltaSeconds, SubSteps );
	}
}

static void SyncRagdollImpl( ARagdollCarcass* C )
{
	FRagdollState* S = GetRagdollStates().Find( C );
	if( !S || !S->bBuilt )
		return;

	// The visual is driven entirely by URagdollMesh::GetFrame (world space).
	// Keep the actor transform on the pelvis for culling/replication/3rd-person
	// camera heuristics.
	b3WorldTransform T = b3Body_GetTransform( S->Bodies[BONE_Pelvis] );
	FVector NewLoc( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );

	b3Vec3 X = b3RotateVector( T.q, { 1.0f, 0.0f, 0.0f } );
	b3Vec3 Y = b3RotateVector( T.q, { 0.0f, 1.0f, 0.0f } );
	b3Vec3 Z = b3RotateVector( T.q, { 0.0f, 0.0f, 1.0f } );
	FCoords Coords;
	Coords.Origin = NewLoc;
	Coords.XAxis  = FVector( X.x, X.y, X.z );
	Coords.YAxis  = FVector( Y.x, Y.y, Y.z );
	Coords.ZAxis  = FVector( Z.x, Z.y, Z.z );
	FRotator NewRot = Coords.OrthoRotation();

	C->Location = NewLoc;
	C->Rotation = NewRot;
	static INT LogCount = 0;
	if( LogCount < 100 )
	{
		LogCount++;
		debugf( TEXT("RagdollPkg: SyncRagdoll actor=%s loc=(%.0f,%.0f,%.0f)"), C->GetName(), C->Location.X, C->Location.Y, C->Location.Z );
	}
}

static void ShutdownRagdollImpl( ARagdollCarcass* C )
{
	TMap<AActor*,FRagdollState>& Map = GetRagdollStates();
	FRagdollState* S = Map.Find( C );
	if( S && S->bBuilt )
	{
	if( b3World_IsValid( S->World ) )
		b3DestroyWorld( S->World );
	S->World = { 0, 0 };
	S->bBuilt = false;
	debugf( TEXT("RagdollPkg: ShutdownRagdoll actor=%s"), C->GetName() );
	}
	Map.Remove( C );
}

/*-----------------------------------------------------------------------------
	ARagdollCarcass class implementation.
-----------------------------------------------------------------------------*/

void ARagdollCarcass::execBuildRagdoll( FFrame& Stack, RESULT_DECL )
{
	P_FINISH;
	BuildRagdollImpl( this );
}

void ARagdollCarcass::execStepRagdoll( FFrame& Stack, RESULT_DECL )
{
	P_GET_FLOAT( DeltaSeconds );
	P_FINISH;
	StepRagdollImpl( this, DeltaSeconds );
}

void ARagdollCarcass::execSyncRagdoll( FFrame& Stack, RESULT_DECL )
{
	P_FINISH;
	SyncRagdollImpl( this );
}

void ARagdollCarcass::execShutdownRagdoll( FFrame& Stack, RESULT_DECL )
{
	P_FINISH;
	ShutdownRagdollImpl( this );
}

// Register ARagdollCarcass.
IMPLEMENT_CLASS( ARagdollCarcass );

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
