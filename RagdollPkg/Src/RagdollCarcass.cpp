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
	{ 3.0f,  3.0f,  4.5f,    0.0f,   0.0f,  25.0f, 2.0f },   // BONE_Neck
	{ 4.5f,  4.5f,  5.0f,    0.0f,   0.0f,  33.0f, 4.0f },   // BONE_Head
	{ 2.0f,  2.0f,  7.0f,  -13.0f,   6.0f,  24.0f, 3.0f },   // BONE_UpperArmL
	{ 2.0f,  2.0f,  7.0f,   13.0f,   6.0f,  24.0f, 3.0f },   // BONE_UpperArmR
	{ 1.5f,  1.5f,  6.0f,  -13.0f,   8.0f,   6.0f, 2.0f },   // BONE_ForearmL
	{ 1.5f,  1.5f,  6.0f,   13.0f,   8.0f,   6.0f, 2.0f },   // BONE_ForearmR
	{ 3.0f,  3.0f,  8.0f,   -7.0f,   0.0f, -12.0f, 6.0f },   // BONE_ThighL
	{ 3.0f,  3.0f,  8.0f,    7.0f,   0.0f, -12.0f, 6.0f },   // BONE_ThighR
	{ 2.5f,  2.5f,  8.0f,   -7.0f,   0.0f, -30.0f, 3.0f },   // BONE_ShinL
	{ 2.5f,  2.5f,  8.0f,    7.0f,   0.0f, -30.0f, 3.0f },   // BONE_ShinR
	{ 2.0f,  4.0f,  2.5f,   -6.5f,   2.0f, -37.0f, 1.0f },   // BONE_FootL
	{ 2.0f,  4.0f,  2.5f,    6.5f,   2.0f, -37.0f, 1.0f }    // BONE_FootR
};

TMap<AActor*, FRagdollState>& GetRagdollStates()
{
	static TMap<AActor*, FRagdollState> States;
	return States;
}

/*-----------------------------------------------------------------------------
	Box3D world construction.
-----------------------------------------------------------------------------*/

// Joint kind and per-joint limits. Anatomical ranges of motion:
//  - JK_Hinge: one-axis revolute joint. L0/L1 are the lower/upper angle in
//    radians (positive = right-hand rotation about the frame Z axis, which the
//    hinge frame orients along world +Y). Knees/elbows get a one-sided range so
//    they can only flex the natural way (knee bends back, elbow bends forward)
//    instead of the old symmetric +/-51 degrees that let both hyperextend.
//  - JK_Ball: spherical joint with a cone limit about frameA Z and a twist
//    limit about frameB Z. L0 = cone half-angle, L1/L2 = twist range. With
//    identity frames these are the world +Z (vertical) cone and the limb's own
//    long-axis twist, which stops limbs from flailing a full 360 degrees.
enum EJointKind
{
	JK_Ball,
	JK_Hinge
};

struct FRagdollJointSpec
{
	EJointKind Kind;
	FLOAT L0, L1, L2;
	FLOAT Friction; // motor friction torque multiplier (x5 Nm)
};

// Anchor offsets are canonical (relative to the pelvis) and are scaled by
// DrawScale here to match the scaled bodies. Defs selects the active skeleton:
// GRagdollBoneDefs (box fallback) or a per-model dataset's derived bones.
static void AddJoint( FRagdollState& S, INT BodyA, INT BodyB, const FRagdollBoneDef* Defs, const FVector& AnchorCanonical, FLOAT DrawScale, const FRagdollJointSpec& Spec )
{
	const FRagdollBoneDef& DA = Defs[BodyA];
	const FRagdollBoneDef& DB = Defs[BodyB];
	FVector LA = DrawScale * ( AnchorCanonical - FVector( DA.CX, DA.CY, DA.CZ ) );
	FVector LB = DrawScale * ( AnchorCanonical - FVector( DB.CX, DB.CY, DB.CZ ) );
	b3Transform FrameA = { { LA.X*RAGDOLL_U2M, LA.Y*RAGDOLL_U2M, LA.Z*RAGDOLL_U2M }, b3Quat_identity };
	b3Transform FrameB = { { LB.X*RAGDOLL_U2M, LB.Y*RAGDOLL_U2M, LB.Z*RAGDOLL_U2M }, b3Quat_identity };

	if( Spec.Kind == JK_Hinge )
	{
		// Revolute joints rotate about the local Z of the joint frame. Orient
		// the frame so that local Z points along world +Y (the medial-lateral
		// flexion axis for knees and elbows). Both frames share the hinge
		// rotation, so the reference (zero) angle is the straight limb pose.
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
		J.lowerAngle         = Spec.L0;
		J.upperAngle         = Spec.L1;
		J.enableSpring       = false;
		J.enableMotor        = true;
		J.maxMotorTorque     = Spec.Friction * 5.0f;
		J.motorSpeed         = 0.0f;
		b3CreateRevoluteJoint( S.World, &J );
	}
	else
	{
		b3SphericalJointDef J = b3DefaultSphericalJointDef();
		J.base.bodyIdA       = S.Bodies[BodyA];
		J.base.bodyIdB       = S.Bodies[BodyB];
		J.base.localFrameA   = FrameA;
		J.base.localFrameB   = FrameB;
		J.enableConeLimit    = true;
		J.coneAngle          = Spec.L0;
		J.enableTwistLimit   = true;
		J.lowerTwistAngle    = Spec.L1;
		J.upperTwistAngle    = Spec.L2;
		J.enableSpring       = false;
		J.enableMotor        = true;
		J.maxMotorTorque     = Spec.Friction * 5.0f;
		J.motorVelocity      = b3Vec3_zero;
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

	// Per-model dataset (derived skeleton + anchors) or canonical box skeleton.
	// The Carcass carries the dead pawn's ULodMesh (set by Initfor before
	// BuildRagdoll), so its name resolves the dataset for real-mesh corpses.
	// bBoxMannequins (class config) forces the box fallback even when a
	// dataset exists - toggle via ini or the console "set" command.
	const FRagdollDatasetEntry* DS = NULL;
	ULodMesh* LiveMesh = Cast<ULodMesh>( C->Mesh );
	if( LiveMesh && !RagdollForceBoxes() )
		DS = FindRagdollDataset( LiveMesh->GetName() );
	const FRagdollBoneDef* Defs = DS ? DS->Bones : GRagdollBoneDefs;
	S.BoneDefs = Defs;
	// Real-mesh skeletons skip the feet (foot verts are bound rigidly to the
	// shins; a separate foot body is what scrambles the ankles). Box fallback
	// keeps all 14.
	S.NumBodies = DS ? 12 : RAGDOLL_BONE_COUNT;
	if( DS )
		debugf( TEXT("RagdollPkg: BuildRagdoll %s dataset=%s"), C->GetName(), DS->DatasetName );

	// Shared level world: one Box3D world holding the level BSP as a static
	// triangle mesh plus every corpse's bodies. (Re)built when the level changes.
	FRagdollLevelWorld& LW = GetRagdollLevelWorldFor( C->GetLevel() );
	if( !LW.bBuilt )
	{
		debugf( TEXT("RagdollPkg: BuildRagdoll skipped actor=%s (no level geometry)"), C->GetName() );
		return;
	}
	S.World = LW.World;

	// Death impulse: the corpse inherits the dying pawn's velocity (copied into
	// C->Velocity by Initfor, in UU/s) as a base, then each body gets a random
	// jitter plus angular velocity so the chain flexes and no two bodies settle
	// in the same pose. A stationary victim still receives a minimum randomized
	// kick so it flops instead of dropping straight down.
	FLOAT ImpulseScale = C->DeathImpulseScale > 0.0f ? C->DeathImpulseScale : 1.0f;
	FVector DeathVel = C->Velocity * RAGDOLL_U2M * ImpulseScale;   // m/s
	FLOAT BaseSpeed  = DeathVel.Size();
	if( BaseSpeed < 0.5f )
	{
		// Mild randomized kick so a stationary corpse still flops, but gentle
		// enough that the long real-mesh limbs don't flail apart on the drop.
		DeathVel.X += ( appFrand() * 2.0f - 1.0f ) * 1.0f;
		DeathVel.Y += ( appFrand() * 2.0f - 1.0f ) * 1.0f;
		DeathVel.Z += 0.4f;
		BaseSpeed = DeathVel.Size();
	}
	// Clamp so a fast fall or hit doesn't blast the body out of the world or
	// off the ground slab.
	const FLOAT MaxImpulse = 6.0f;
	if( BaseSpeed > MaxImpulse )
		DeathVel *= MaxImpulse / BaseSpeed;

	// Ragdoll bodies.
	for( INT i=0; i<S.NumBodies; i++ )
	{
		const FRagdollBoneDef& D = Defs[i];
		FVector Center = Origin + DrawScale * FVector( D.CX, D.CY, D.CZ );

		b3BodyDef Bd = b3DefaultBodyDef();
		Bd.type           = b3_dynamicBody;
		Bd.position       = { Center.X*RAGDOLL_U2M, Center.Y*RAGDOLL_U2M, Center.Z*RAGDOLL_U2M };
		Bd.rotation       = b3Quat_identity;
		Bd.linearDamping  = 0.05f;
		Bd.angularDamping = 0.6f;
		Bd.gravityScale   = 1.0f;
		S.Bodies[i] = b3CreateBody( S.World, &Bd );

		// Small per-body jitter so the corpse lands differently every time but
		// doesn't explode into a flailing tangle on spawn (the real-mesh limbs
		// are long; big random spins stretch the skin and look like tearing).
		b3Vec3 V;
		V.x = DeathVel.X + ( appFrand() * 2.0f - 1.0f ) * 0.4f;
		V.y = DeathVel.Y + ( appFrand() * 2.0f - 1.0f ) * 0.4f;
		V.z = DeathVel.Z + ( appFrand() * 2.0f - 1.0f ) * 0.3f;
		b3Body_SetLinearVelocity( S.Bodies[i], V );
		b3Vec3 W;
		W.x = ( appFrand() * 2.0f - 1.0f ) * 1.8f;
		W.y = ( appFrand() * 2.0f - 1.0f ) * 1.8f;
		W.z = ( appFrand() * 2.0f - 1.0f ) * 1.2f;
		b3Body_SetAngularVelocity( S.Bodies[i], W );

		FLOAT HX = D.HX * DrawScale * RAGDOLL_U2M;
		FLOAT HY = D.HY * DrawScale * RAGDOLL_U2M;
		FLOAT HZ = D.HZ * DrawScale * RAGDOLL_U2M;

		b3ShapeDef Sd = b3DefaultShapeDef();
		Sd.density = D.MassKg / ( 8.0f * HX * HY * HZ );
		Sd.baseMaterial.friction     = 0.6f;
		Sd.baseMaterial.restitution  = 0.05f;
		// b3DefaultShapeDef sets filter.categoryBits = UINT64_MAX, which
		// overlaps EVERY bit including the geometry category. The clearance ray
		// masks on the geometry category, so without an explicit body category
		// it would hit the corpse's own bodies and false-lift every spawn.
		Sd.filter.categoryBits = RAGDOLL_BODY_CATEGORY;
		b3BoxHull Hull = b3MakeBoxHull( HX, HY, HZ );
		b3CreateHullShape( S.Bodies[i], &Sd, &Hull.base );

		S.RestPositions[i] = Bd.position;
		S.RestRotations[i] = b3Quat_identity;
	}

	// Joints. All ranges in radians. Spherical joints: cone half-angle about the
	// vertical + twist range about the limb's own axis. Hinges: one-sided so
	// knees/elbows flex the anatomically correct direction only. Anchor points
	// come from the canonical skeleton or the per-model dataset.
	static const FVector CanonicalAnchors[13] =
	{
		FVector(  0.0f, 0.0f,  6.0f), // pelvis-spine
		FVector(  0.0f, 0.0f, 22.0f), // spine-neck
		FVector(  0.0f, 0.0f, 30.0f), // neck-head
		FVector(-13.0f, 6.0f, 24.0f), // spine-upperarmL
		FVector( 13.0f, 6.0f, 24.0f), // spine-upperarmR
		FVector(-13.0f, 8.0f, 12.0f), // upperarm-forearmL
		FVector( 13.0f, 8.0f, 12.0f), // upperarm-forearmR
		FVector( -7.0f, 0.0f, -6.0f), // pelvis-thighL
		FVector(  7.0f, 0.0f, -6.0f), // pelvis-thighR
		FVector( -7.0f, 0.0f,-22.0f), // thigh-shinL
		FVector(  7.0f, 0.0f,-22.0f), // thigh-shinR
		FVector( -7.0f, 2.0f,-34.0f), // shin-footL
		FVector(  7.0f, 2.0f,-34.0f), // shin-footR
	};
	const FVector* Anch = DS ? DS->Anchors : CanonicalAnchors;

	AddJoint( S, BONE_Pelvis,    BONE_Spine,     Defs, Anch[0], DrawScale, { JK_Ball, 0.35f, -0.26f,  0.26f, 0.6f } );
	AddJoint( S, BONE_Spine,     BONE_Neck,      Defs, Anch[1], DrawScale, { JK_Ball, 0.61f, -0.35f,  0.35f, 0.8f } );
	AddJoint( S, BONE_Neck,      BONE_Head,      Defs, Anch[2], DrawScale, { JK_Ball, 0.44f, -0.26f,  0.26f, 0.4f } );
	AddJoint( S, BONE_Spine,     BONE_UpperArmL, Defs, Anch[3], DrawScale, { JK_Ball, 1.40f, -0.44f,  0.44f, 0.7f } );
	AddJoint( S, BONE_Spine,     BONE_UpperArmR, Defs, Anch[4], DrawScale, { JK_Ball, 1.40f, -0.44f,  0.44f, 0.7f } );
	AddJoint( S, BONE_UpperArmL, BONE_ForearmL,  Defs, Anch[5], DrawScale, { JK_Hinge, -0.12f, 1.10f, 0.0f, 0.5f } );
	AddJoint( S, BONE_UpperArmR, BONE_ForearmR,  Defs, Anch[6], DrawScale, { JK_Hinge, -0.12f, 1.10f, 0.0f, 0.5f } );
	AddJoint( S, BONE_Pelvis,    BONE_ThighL,    Defs, Anch[7], DrawScale, { JK_Ball, 1.22f, -0.52f,  0.52f, 0.8f } );
	AddJoint( S, BONE_Pelvis,    BONE_ThighR,    Defs, Anch[8], DrawScale, { JK_Ball, 1.22f, -0.52f,  0.52f, 0.8f } );
	AddJoint( S, BONE_ThighL,    BONE_ShinL,     Defs, Anch[9], DrawScale, { JK_Hinge, -1.30f, 0.12f, 0.0f, 0.6f } );
	AddJoint( S, BONE_ThighR,    BONE_ShinR,     Defs, Anch[10], DrawScale, { JK_Hinge, -1.30f, 0.12f, 0.0f, 0.6f } );
	// Feet are skipped from real-mesh ragdolls; only the box skeleton keeps the
	// ankle joints (bodies 12/13 only exist in the box path).
	if( !DS )
	{
		AddJoint( S, BONE_ShinL, BONE_FootL, Defs, Anch[11], DrawScale, { JK_Ball, 0.44f, -0.35f,  0.35f, 0.3f } );
		AddJoint( S, BONE_ShinR, BONE_FootR, Defs, Anch[12], DrawScale, { JK_Ball, 0.44f, -0.35f,  0.35f, 0.3f } );
	}

	// Lift bodies that spawned inside level geometry (ramp/stairs deaths) so the
	// first step doesn't explode them. Must run before the first StepRagdoll.
	ClearCorpseSpawn( S, LW );

	// Player mesh (preferred): capture the dead pawn's ULodMesh and attach it to
	// the bones. Falls back to the procedural box mannequin when the source mesh
	// isn't a usable vertex-mesh (e.g. skeletal/animated-only content).
	S.Mesh = CreatePlayerRagdollMesh( C, S );
	if( !S.Mesh )
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

	// The world is shared by every corpse, so only one corpse actually steps it
	// per frame. We advance by the real elapsed time (the engine's monotonic
	// appSeconds() clock) accumulated since the last step: the second corpse in
	// a frame sees dt==0 and skips, and if a corpse's Timer skips a frame
	// (timer catch-up at high FPS) the next step absorbs the full delta so the
	// sim keeps real time instead of stuttering or running slow.
	FRagdollLevelWorld& LW = GetRagdollLevelWorld();
	if( !LW.bBuilt || LW.World.index1 != S->World.index1 || LW.World.generation != S->World.generation )
		return;
	FLOAT Now = appSeconds().GetFloat();
	FLOAT dt  = Now - LW.LastStepTime;
	if( dt <= 0.0f )
		return;
	LW.LastStepTime = Now;

	// Clamp so a hitch (level load, big stall) doesn't shove the corpse through
	// geometry in one giant step; anything longer is simply dropped.
	if( dt > 0.05f )
		dt = 0.05f;

	INT SubSteps = Max( C->SimSubSteps, 1 );
	static INT GStepDbg = 0;
	if( GStepDbg < 5 )
	{
		debugf( TEXT("RagdollPkg: step actor=%s dt=%.3f now=%.2f sub=%i"), C->GetName(), dt, Now, SubSteps );
		GStepDbg++;
	}
	b3World_Step( LW.World, dt, SubSteps );
}

static void SyncRagdollImpl( ARagdollCarcass* C )
{
	FRagdollState* S = GetRagdollStates().Find( C );
	if( !S || !S->bBuilt )
		return;

	// Diagnostic: sample the pelvis position/velocity over the corpse's first
	// few syncs to confirm the sim is actually moving the bodies.
	if( S->SyncCount < 4 || S->SyncCount == 100 )
	{
		b3WorldTransform T = b3Body_GetTransform( S->Bodies[BONE_Pelvis] );
		b3Vec3 V = b3Body_GetLinearVelocity( S->Bodies[BONE_Pelvis] );
		FLOAT Spd = sqrt( V.x*V.x + V.y*V.y + V.z*V.z );
		debugf( TEXT("RagdollPkg: sync %s count=%i pelvis=(%.1f,%.1f,%.1f) vel=%.2f m/s"),
			C->GetName(), S->SyncCount, T.p.x*RAGDOLL_M2U, T.p.y*RAGDOLL_M2U, T.p.z*RAGDOLL_M2U, Spd );
	}
	S->SyncCount++;

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

	// Keep the collision hitbox covering the whole body. The default Carcass
	// cylinder (radius 18, height 4) is a dot at the pelvis, so shots that
	// visually hit the sprawled body miss entirely. Track the bone extents and
	// grow/shrink the cylinder so the corpse is actually hittable.
	{
		FLOAT DrawScale = ( C->DrawScale > 0.0f ) ? C->DrawScale : 1.0f;
		const FRagdollBoneDef* Defs = S->BoneDefs ? S->BoneDefs : GRagdollBoneDefs;
		FLOAT MaxR = 20.0f, MaxUp = 20.0f, MaxDown = 20.0f;
		for( INT i=0; i<S->NumBodies; i++ )
		{
			b3WorldTransform BT = b3Body_GetTransform( S->Bodies[i] );
			const FRagdollBoneDef& D = Defs[i];
			FLOAT E  = Max( Max( D.HX, D.HY ), D.HZ ) * DrawScale;
			FLOAT dx = BT.p.x * RAGDOLL_M2U - NewLoc.X;
			FLOAT dy = BT.p.y * RAGDOLL_M2U - NewLoc.Y;
			FLOAT dz = BT.p.z * RAGDOLL_M2U - NewLoc.Z;
			MaxR = Max( MaxR, (FLOAT)sqrt( dx*dx + dy*dy ) + E );
			if( dz > 0.0f )
				MaxUp   = Max( MaxUp,   dz + E );
			else
				MaxDown = Max( MaxDown, -dz + E );
		}
		FLOAT NewRadius = MaxR;
		FLOAT NewHeight = Max( MaxUp, MaxDown );
		if( Abs( NewRadius - C->CollisionRadius ) > 8.0f || Abs( NewHeight - C->CollisionHeight ) > 8.0f )
			C->SetCollisionSize( NewRadius, NewHeight );
	}

	// Out-of-world despawn: without the old fake floor slab, a corpse flung off
	// a ledge or through geometry can fall forever. If it drops well below the
	// lowest solid BSP vertex, hand it a short LifeSpan so the engine destroys
	// it (and ShutdownRagdoll frees its bodies).
	FRagdollLevelWorld& LW = GetRagdollLevelWorld();
	if( LW.bBuilt && NewLoc.Z < LW.MinZ - 150.0f )
	{
		if( !S->bOutOfWorldLogged )
		{
			S->bOutOfWorldLogged = true;
			debugf( TEXT("RagdollPkg: corpse %s out of world z=%.0f minZ=%.0f, despawning"),
				C->GetName(), NewLoc.Z, LW.MinZ );
		}
		if( C->LifeSpan == 0.0f || C->LifeSpan > 1.0f )
			C->LifeSpan = 1.0f;
	}
}

static void ShutdownRagdollImpl( ARagdollCarcass* C )
{
	TMap<AActor*,FRagdollState>& Map = GetRagdollStates();
	FRagdollState* S = Map.Find( C );
	if( S && S->bBuilt )
	{
		// Destroy only this corpse's bodies (Box3D auto-destroys attached
		// joints). The shared level world stays alive for the other corpses.
		FRagdollLevelWorld& LW = GetRagdollLevelWorld();
		const UBOOL WorldStillValid = LW.bBuilt
			&& LW.World.index1 == S->World.index1 && LW.World.generation == S->World.generation;
		if( WorldStillValid )
		{
			for( INT i=0; i<S->NumBodies; i++ )
				if( b3Body_IsValid( S->Bodies[i] ) )
					b3DestroyBody( S->Bodies[i] );
		}
		S->bBuilt = false;
		debugf( TEXT("RagdollPkg: ShutdownRagdoll actor=%s"), C->GetName() );
	}
	Map.Remove( C );

	// With the last corpse gone the level world has nothing left to simulate,
	// so free it (mesh + world). The next corpse on this or any other level
	// rebuilds it lazily.
	if( Map.Num() == 0 )
		TeardownLevelWorld();
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

void ARagdollCarcass::execApplyDamageImpulse( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execApplyDamageImpulse);
	P_GET_VECTOR( HitLocation );
	P_GET_VECTOR( Momentum );
	P_GET_INT( Damage );
	P_FINISH;

	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt )
	{
		debugf( TEXT("RagdollPkg: dmg %s NO-STATE hit=(%.0f,%.0f,%.0f) dmg=%i"), GetName(), HitLocation.X, HitLocation.Y, HitLocation.Z, Damage );
		return;
	}

	b3Vec3 Hit;
	Hit.x = HitLocation.X * RAGDOLL_U2M;
	Hit.y = HitLocation.Y * RAGDOLL_U2M;
	Hit.z = HitLocation.Z * RAGDOLL_U2M;

	// Shot direction: prefer the hit momentum; fall back to a random upward kick.
	b3Vec3 Dir;
	FLOAT MLen = sqrt( Momentum.X*Momentum.X + Momentum.Y*Momentum.Y + Momentum.Z*Momentum.Z );
	if( MLen > 1.0f )
	{
		Dir.x = Momentum.X / MLen;
		Dir.y = Momentum.Y / MLen;
		Dir.z = Momentum.Z / MLen;
	}
	else
	{
		FLOAT A = appFrand() * 2.0f * PI;
		Dir.x = appCos( A );
		Dir.y = appSin( A );
		Dir.z = 0.6f;
	}

	// Diagnostic: report where the shot landed relative to the nearest body.
	{
		INT Closest = -1;
		FLOAT ClosestD = 1e9f;
		for( INT i=0; i<S->NumBodies; i++ )
		{
			b3WorldTransform T = b3Body_GetTransform( S->Bodies[i] );
			FLOAT dx = T.p.x - Hit.x, dy = T.p.y - Hit.y, dz = T.p.z - Hit.z;
			FLOAT D = sqrt( dx*dx + dy*dy + dz*dz );
			if( D < ClosestD ) { ClosestD = D; Closest = i; }
		}
		b3WorldTransform T0 = b3Body_GetTransform( S->Bodies[BONE_Pelvis] );
		b3Vec3 V0 = b3Body_GetLinearVelocity( S->Bodies[BONE_Pelvis] );
		FLOAT V0s = sqrt( V0.x*V0.x + V0.y*V0.y + V0.z*V0.z );
		debugf( TEXT("RagdollPkg: dmg %s hit=(%.0f,%.0f,%.0f) nearest=b%i d=%.2f mom=%.0f dmg=%i pelvisVel=%.2f"),
			GetName(), HitLocation.X, HitLocation.Y, HitLocation.Z, Closest, ClosestD, MLen, Damage, V0s );
	}

	// Whole-body push: every body gets shoved in the shot direction so the
	// entire corpse reacts, not just the limb that happened to be hit. Bodies
	// close to the impact take the brunt (and get a lever-arm spin since the
	// impulse is applied at the hit point); distant bodies still get pushed
	// toward the shot direction so the whole ragdoll is knocked. The velocity
	// kick is deliberately strong so a single pistol shot visibly flinches the
	// hit limb instead of a subtle nudge.
	const FLOAT CloseRadius = 0.40f; // full-strength zone (m, ~20 UU)
	const FLOAT MaxRadius   = 1.50f; // beyond this only the min nudge remains
	FLOAT BaseSpeed = 3.5f + Min( Damage, 120 ) * 0.05f;
	if( BaseSpeed > 8.0f )
		BaseSpeed = 8.0f;

	for( INT i=0; i<S->NumBodies; i++ )
	{
		b3WorldTransform T = b3Body_GetTransform( S->Bodies[i] );
		FLOAT dx = T.p.x - Hit.x, dy = T.p.y - Hit.y, dz = T.p.z - Hit.z;
		FLOAT Dist  = sqrt( dx*dx + dy*dy + dz*dz );
		FLOAT Scale = 1.0f - ( Dist - CloseRadius ) / Max( MaxRadius - CloseRadius, 0.001f );
		if( Scale < 0.25f )
			Scale = 0.25f; // minimum nudge keeps every body awake and reacting
		FLOAT Speed = BaseSpeed * Scale;
		FLOAT Mass  = b3Body_GetMass( S->Bodies[i] );
		b3Vec3 Imp;
		Imp.x = Dir.x * Speed * Mass;
		Imp.y = Dir.y * Speed * Mass;
		if( Dist <= CloseRadius )
			Imp.z = ( Dir.z * Speed + 2.0f ) * Mass;   // hit limb pops up visibly
		else
			Imp.z = ( Dir.z * Speed + 0.8f ) * Mass;
		if( Dist <= CloseRadius )
			b3Body_ApplyLinearImpulse( S->Bodies[i], Imp, Hit, true );      // at hit point -> torque
		else
			b3Body_ApplyLinearImpulseToCenter( S->Bodies[i], Imp, true );   // pure push
	}
	{
		b3Vec3 V0 = b3Body_GetLinearVelocity( S->Bodies[BONE_Pelvis] );
		FLOAT V0s = sqrt( V0.x*V0.x + V0.y*V0.y + V0.z*V0.z );
		debugf( TEXT("RagdollPkg: dmg-after %s pelvisVel=%.2f"), GetName(), V0s );
	}
	unguard;
}

// Walk-over push: a living pawn moving over the corpse drags the bodies under
// its feet along with it. Uses velocity matching (eased toward the pawn's
// horizontal motion, capped) instead of impulses, so the corpse slides aside
// at the pawn's pace and stops when the pawn stops. Impulses here would stack
// up over the 50 Hz ticks and launch the ragdoll.
void ARagdollCarcass::execWalkOverPush( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execWalkOverPush);
	P_GET_ACTOR( Pusher );
	P_FINISH;

	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt || !Pusher )
		return;
	APawn* P = Cast<APawn>( Pusher );
	if( !P )
		return;

	// Pawn's horizontal velocity in m/s; stationary/too slow pawns do nothing.
	b3Vec3 Push;
	Push.x = P->Velocity.X * RAGDOLL_U2M;
	Push.y = P->Velocity.Y * RAGDOLL_U2M;
	Push.z = 0.0f;
	FLOAT Speed = sqrt( Push.x*Push.x + Push.y*Push.y );
	if( Speed < 0.8f )
		return;
	const FLOAT MaxPushSpeed = 4.5f;
	if( Speed > MaxPushSpeed )
	{
		FLOAT Scl = MaxPushSpeed / Speed;
		Push.x *= Scl;
		Push.y *= Scl;
	}

	// Pawn feet position in meters; only bodies roughly under the feet react.
	FLOAT Px = P->Location.X * RAGDOLL_U2M;
	FLOAT Py = P->Location.Y * RAGDOLL_U2M;
	FLOAT FootZ = ( P->Location.Z - P->CollisionHeight ) * RAGDOLL_U2M;

	for( INT i=0; i<S->NumBodies; i++ )
	{
		b3WorldTransform T = b3Body_GetTransform( S->Bodies[i] );
		FLOAT dx = T.p.x - Px, dy = T.p.y - Py, dz = T.p.z - FootZ;
		if( dx*dx + dy*dy > 0.85f*0.85f )
			continue;
		if( dz > 0.7f || dz < -1.3f )
			continue;
		b3Vec3 V = b3Body_GetLinearVelocity( S->Bodies[i] );
		V.x += ( Push.x - V.x ) * 0.35f;   // ease toward the pawn's motion
		V.y += ( Push.y - V.y ) * 0.35f;
		b3Body_SetLinearVelocity( S->Bodies[i], V );
	}
	unguard;
}

// Register ARagdollCarcass.
IMPLEMENT_CLASS( ARagdollCarcass );

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
