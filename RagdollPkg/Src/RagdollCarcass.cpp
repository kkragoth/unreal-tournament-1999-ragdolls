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
//    L2 is the spring REST angle (2026-10-10 census: with the spring aimed at
//    exactly straight, ~60% of elbows and half the knees settled ON the
//    hyperextension stop and read as stiff straight limbs); elbows now rest
//    at ~20 deg of flex, knees at ~15 deg.
//  - JK_Ball: spherical joint with a cone limit about frameA Z and a twist
//    limit about frameB Z. L0 = cone half-angle, L1/L2 = twist range. With
//    identity frames these are the world +Z (vertical) cone and the limb's own
//    long-axis twist, which stops limbs from flailing a full 360 degrees.
// EJointKind / FRagdollJointSpec live in RagdollInternal.h (shared with the
// generated per-model table).
#include "RagdollJointTuning.h"

// Global joint table, used by every model without a per-model entry and as
// the fallback for the box mannequins. Ranges sized for a corpse (shoulder
// ~140 deg cone, elbow/knee ~130-140 deg one-sided flexion, hip ~70 deg
// cone); friction in Nm, tiny versus the limbs' gravity torque (upper arm
// ~4 Nm, thigh ~10 Nm) so joints flop. Order = RAGDOLL_TUNED_JOINTS order.
// Cone centres (2026-10-10, user: "legs and arms never going up"): Box3D
// clamps the cone to 90 deg and centres it on frame A's z axis, which with
// identity frames is the hanging limb, so the arm stopped at horizontal and
// the leg at 69 deg. Shoulders now centre on out+forward (slightly down):
// hanging arm at 81 deg inside the cone, overhead reachable, nothing behind
// the back or across the chest. Hips centre 37 deg forward of down: legs
// raise to ~110 deg forward, ~40 deg back, ~90 deg out.
static const FRagdollJointSpec GRagdollDefaultJoints[RAGDOLL_TUNED_JOINTS] =
{
	// 2026-10-10 evening tightening (user: "these ragdolls look kinda
	// weird": knees folded flat, legs in the splits or crossed, heads
	// twisted to the stop): smaller trunk/neck twists, knees to 100 deg,
	// hips a 50 deg cone aimed ~27 deg forward and ~12 deg out (raise ~77,
	// back ~23, out ~62, cross ~38; a leg can no longer come up beside the
	// torso), elbows to 126 deg.
	{ JK_Ball,   0.45f, -0.25f,  0.25f, 0.8f,   0.0f,  0.0f,  0.0f  },  // pelvis-spine
	{ JK_Ball,   0.55f, -0.35f,  0.35f, 0.5f,   0.0f,  0.0f,  0.0f  },  // spine-neck
	{ JK_Ball,   0.50f, -0.35f,  0.35f, 0.3f,   0.0f,  0.0f,  0.0f  },  // neck-head
	{ JK_Ball,   1.57f, -0.60f,  0.60f, 0.3f,  -0.80f, 0.55f, -0.15f },  // spine-upperarm L
	{ JK_Ball,   1.57f, -0.60f,  0.60f, 0.3f,   0.80f, 0.55f, -0.15f },  // spine-upperarm R
	{ JK_Hinge, -0.05f,  2.20f,  0.35f, 0.15f,  0.0f,  0.0f,  0.0f  },  // elbow L
	{ JK_Hinge, -0.05f,  2.20f,  0.35f, 0.15f,  0.0f,  0.0f,  0.0f  },  // elbow R
	{ JK_Ball,   0.87f, -0.35f,  0.35f, 0.5f,  -0.20f, 0.45f, -0.87f },  // hip L
	{ JK_Ball,   0.87f, -0.35f,  0.35f, 0.5f,   0.20f, 0.45f, -0.87f },  // hip R
	{ JK_Hinge, -1.75f,  0.05f, -0.26f, 0.25f,  0.0f,  0.0f,  0.0f  },  // knee L
	{ JK_Hinge, -1.75f,  0.05f, -0.26f, 0.25f,  0.0f,  0.0f,  0.0f  },  // knee R
};

// Shortest rotation taking unit vector A onto unit vector B.
static b3Quat RagdollShortestArc( b3Vec3 A, b3Vec3 B )
{
	const FLOAT D = b3Dot( A, B );
	if( D < -0.9999f )
	{
		// Antiparallel: half turn about any axis perpendicular to A.
		b3Vec3 Perp = ( Abs( A.x ) < 0.9f ) ? b3Cross( A, b3Vec3{ 1.0f, 0.0f, 0.0f } ) : b3Cross( A, b3Vec3{ 0.0f, 1.0f, 0.0f } );
		return b3MakeQuatFromAxisAngle( b3Normalize( Perp ), PI );
	}
	if( D > 0.9999f )
		return b3Quat_identity;
	return b3MakeQuatFromAxisAngle( b3Normalize( b3Cross( A, B ) ), appAcos( D ) );
}

// Per-model table lookup by dataset name (NULL = use the global table).
static const FRagdollJointTuning* FindRagdollJointTuning( const TCHAR* DatasetName )
{
	if( !DatasetName )
		return NULL;
	for( INT i=0; i<GRagdollJointTuningCount; i++ )
		if( appStricmp( GRagdollJointTuning[i].DatasetName, DatasetName ) == 0 )
			return &GRagdollJointTuning[i];
	return NULL;
}

// Anchor offsets are canonical (relative to the pelvis) and are scaled by
// DrawScale here to match the scaled bodies. Defs selects the active skeleton:
// GRagdollBoneDefs (box fallback) or a per-model dataset's derived bones.
// Live tuning knobs (RagdollCarcass.uc globalconfig floats), filled per
// BuildRagdoll from the carcass so console "set" changes apply to new corpses.
static FLOAT GJointFrictionScale = 1.0f;
static FLOAT GJointLimitScale    = 1.0f;
// Weak spring on every joint toward the neutral (bind) pose, standing in
// for tissue stiffness: limbs no longer end up folded flat or splayed to a
// limit by the spawn spin. JointSpringHertz 0 disables it.
static FLOAT GJointSpringHertz   = 0.8f;
static FLOAT GJointSpringDamping = 0.8f;

static FLOAT ClampF( FLOAT V, FLOAT Lo, FLOAT Hi );   // defined with ApplyDeathPose
static FLOAT ClampAngle( FLOAT A )
{
	const FLOAT Lim = 0.98f * PI;
	return A > Lim ? Lim : ( A < -Lim ? -Lim : A );
}

static void AddJoint( FRagdollState& S, INT BodyA, INT BodyB, const FRagdollBoneDef* Defs, const FVector& AnchorCanonical, FLOAT DrawScale, const FRagdollJointSpec& Spec, UBOOL bCollide=0 )
{
	const FRagdollBoneDef& DA = Defs[BodyA];
	const FRagdollBoneDef& DB = Defs[BodyB];
	FVector LA = DrawScale * ( AnchorCanonical - FVector( DA.CX, DA.CY, DA.CZ ) );
	FVector LB = DrawScale * ( AnchorCanonical - FVector( DB.CX, DB.CY, DB.CZ ) );
	b3Transform FrameA = { { LA.X*RAGDOLL_U2M, LA.Y*RAGDOLL_U2M, LA.Z*RAGDOLL_U2M }, b3Quat_identity };
	b3Transform FrameB = { { LB.X*RAGDOLL_U2M, LB.Y*RAGDOLL_U2M, LB.Z*RAGDOLL_U2M }, b3Quat_identity };

	if( Spec.Kind == JK_Hinge )
	{
		// Revolute joints rotate about the local Z of the joint frame. The bind
		// space is FRONT-FACING +Y (Mixamo toe bones point +Y), and the arms/legs
		// hang down at x=±, so the medial-lateral flexion axis is world +X: a
		// right-hand rotation about +X swings a hanging limb forward (+Y) /
		// backward (-Y) in the sagittal plane. (World +Y would swing the limb
		// sideways - that was the "whole arm stiff" bug.) Both frames share the
		// hinge rotation, so the reference (zero) angle is the straight limb pose.
		b3Quat Hinge = b3MakeQuatFromAxisAngle( { 0.0f, 1.0f, 0.0f }, 1.5707963f );
		FrameA.q = Hinge;
		FrameB.q = Hinge;

		b3RevoluteJointDef J = b3DefaultRevoluteJointDef();
		J.base.bodyIdA       = S.Bodies[BodyA];
		J.base.bodyIdB       = S.Bodies[BodyB];
		J.base.localFrameA   = FrameA;
		J.base.localFrameB   = FrameB;
		J.base.collideConnected = bCollide ? true : false;
		J.enableLimit        = true;
		J.lowerAngle         = ClampAngle( Spec.L0 * GJointLimitScale );
		J.upperAngle         = ClampAngle( Spec.L1 * GJointLimitScale );
		// Spring target: the slightly flexed rest angle (Spec.L2), kept inside
		// the limits so the spring never pulls the limb onto a stop.
		J.targetAngle        = ClampF( Spec.L2, J.lowerAngle, J.upperAngle );
		J.enableSpring       = ( GJointSpringHertz > 0.0f );
		J.hertz              = GJointSpringHertz;
		J.dampingRatio       = GJointSpringDamping;
		// Motor at zero speed = Coulomb joint friction. Keep it SMALL: the old
		// 1.5-2.4 Nm brakes were 15-50% of the limbs' gravity torque, so limbs
		// stopped 20-35 deg from vertical and never moved again (stiff corpse).
		J.enableMotor        = true;
		J.maxMotorTorque     = Spec.Friction * GJointFrictionScale;
		J.motorSpeed         = 0.0f;
		S.Joints[BodyB] = b3CreateRevoluteJoint( S.World, &J );
	}
	else
	{
		// Cone centre: frame A's z axis is the cone axis, frame B's z axis the
		// limb. With a spec axis, A.z -> the requested centre direction and
		// B.z -> the hanging limb (-Z at bind) via a pure swing from A, so the
		// bind pose has zero twist and sits inside the cone off-centre.
		// BindRel = frame B relative to frame A in the bind pose: the spring
		// target, so the spring pulls toward the bind pose, not the cone centre.
		b3Quat BindRel = b3Quat_identity;
		if( Spec.AX != 0.0f || Spec.AY != 0.0f || Spec.AZ != 0.0f )
		{
			const b3Vec3 Z     = { 0.0f, 0.0f, 1.0f };
			const b3Vec3 Limb  = { 0.0f, 0.0f, -1.0f };
			const b3Vec3 Axis  = b3Normalize( b3Vec3{ Spec.AX, Spec.AY, Spec.AZ } );
			const b3Quat QA    = RagdollShortestArc( Z, Axis );
			const b3Vec3 LimbA = b3InvRotateVector( QA, Limb );
			const b3Quat Sw    = RagdollShortestArc( Z, LimbA );
			const b3Quat QB    = b3MulQuat( QA, Sw );
			FrameA.q = QA;
			FrameB.q = QB;
			BindRel  = Sw;
		}

		b3SphericalJointDef J = b3DefaultSphericalJointDef();
		J.base.bodyIdA       = S.Bodies[BodyA];
		J.base.bodyIdB       = S.Bodies[BodyB];
		J.base.localFrameA   = FrameA;
		J.base.localFrameB   = FrameB;
		J.base.collideConnected = bCollide ? true : false;
		J.enableConeLimit    = true;
		J.coneAngle          = Min( Spec.L0 * GJointLimitScale, 0.5f * PI );   // Box3D clamps the cone to 90 deg
		J.enableTwistLimit   = true;
		J.lowerTwistAngle    = ClampAngle( Spec.L1 * GJointLimitScale );
		J.upperTwistAngle    = ClampAngle( Spec.L2 * GJointLimitScale );
		J.enableSpring       = ( GJointSpringHertz > 0.0f );
		J.hertz              = GJointSpringHertz;
		J.dampingRatio       = GJointSpringDamping;
		J.targetRotation     = BindRel;
		J.enableMotor        = true;
		J.maxMotorTorque     = Spec.Friction * GJointFrictionScale;
		J.motorVelocity      = b3Vec3_zero;
		S.Joints[BodyB] = b3CreateSphericalJoint( S.World, &J );
	}
}

/*-----------------------------------------------------------------------------
	Self-collision.
-----------------------------------------------------------------------------*/

// Box3D creates joints with collideConnected = false, so every body is free to
// pass through the ONE body it is jointed to. At the shoulders and hips that
// body is the spine box (most of the torso) or the pelvis, which is exactly the
// "arms pass through the torso" the user sees. Those four joints get
// collideConnected = true (bSelfCollide, globalconfig, default on). Elbows,
// knees and the neck keep the default: their boxes overlap by construction
// around the anchor and the limbs cannot fold into their parent anyway.
//
// A colliding pair must NOT overlap at rest or the solver pushes the limb out
// of its socket on the first step. The shoulder/hip anchors sit inside both
// boxes, so the child's hull is trimmed on the face that looks at the parent:
// along the axis with the smallest rest-pose overlap, by that depth plus a
// small margin (datasets 2026-10-10: thigh/pelvis 2.5 UU in Z on every model,
// upper arm/spine clear or <0.5 UU in X on the females). Only the HULL is
// offset (b3MakeOffsetBoxHull); the body origin stays at the dataset centre
// because the skinning bind offsets and the gore centres depend on it.
static UBOOL RagdollJointCollides( INT Child )
{
	return Child == BONE_UpperArmL || Child == BONE_UpperArmR || Child == BONE_ThighL || Child == BONE_ThighR;
}

// Returns the hull offset (UU, DrawScale=1) and shrinks Half in place for the
// child box of a colliding joint; identity when the boxes are already clear.
static FVector RagdollTrimChildBox( const FRagdollBoneDef* Defs, INT Child, INT Parent, FVector& Half, UBOOL bLog )
{
	const FRagdollBoneDef& A = Defs[Child];
	const FRagdollBoneDef& B = Defs[Parent];
	const FLOAT CA[3] = { A.CX, A.CY, A.CZ }, HA[3] = { A.HX, A.HY, A.HZ };
	const FLOAT CB[3] = { B.CX, B.CY, B.CZ }, HB[3] = { B.HX, B.HY, B.HZ };
	FLOAT Ov[3];
	INT   Axis = -1;
	for( INT k=0; k<3; k++ )
	{
		Ov[k] = Min( CA[k]+HA[k], CB[k]+HB[k] ) - Max( CA[k]-HA[k], CB[k]-HB[k] );
		if( Ov[k] <= 0.0f )
			return FVector( 0, 0, 0 );   // separated on this axis: no rest overlap
		if( Axis < 0 || Ov[k] < Ov[Axis] )
			Axis = k;
	}
	const FLOAT Margin = 0.25f;
	FLOAT Depth = Ov[Axis] + Margin;
	Depth = Min( Depth, 0.8f * HA[Axis] );   // never cut more than 40% of the box
	const FLOAT Sign = ( CB[Axis] >= CA[Axis] ) ? 1.0f : -1.0f;   // which face looks at the parent
	FVector Off( 0, 0, 0 );
	FLOAT* H[3] = { &Half.X, &Half.Y, &Half.Z };
	FLOAT* O[3] = { &Off.X,  &Off.Y,  &Off.Z  };
	*H[Axis] -= 0.5f * Depth;
	*O[Axis]  = -Sign * 0.5f * Depth;
	if( bLog )
		debugf( TEXT("RagdollPkg: selfcollide trim body=%i vs %i axis=%i overlap=(%.2f,%.2f,%.2f) cut=%.2f UU"), Child, Parent, Axis, Ov[0], Ov[1], Ov[2], Depth );
	return Off;
}

// Skin that sticks out past the END of a limb box - the feet beyond the
// shins and the hands beyond the forearms (the real-mesh rig has no foot or
// hand bodies; those verts ride the shin/forearm rigidly), the hair/skull
// above the head box - has no collision, so a face-down corpse pushes its
// toes through the floor and the engine clips them ("part of feet is
// missing", 2026-10-10). The dominant-bound verts more than 1.5 UU past a
// box face get a second hull on the same body: their bounding box from
// that face outward, shrunk a little to stay inside the skin shell. The
// toes stick out of the shin box SIDEWAYS (-Y, 40 verts on the Commando),
// not below it (the shin box already reaches the sole), so every face but
// the proximal one (where the joint is) is tried. Axis 0/1/2 = X/Y/Z.
static INT RagdollAddOverhangHull( FRagdollState& S, const FRagdollDatasetEntry* DS, INT Body, const FRagdollBoneDef& D, INT Axis, FLOAT Sign, FLOAT DrawScale, FLOAT Density, UBOOL bLog )
{
	if( !DS || !DS->BindBones || !DS->BindOffsets )
		return 0;
	const FLOAT H[3] = { D.HX, D.HY, D.HZ };
	FVector Mn( 1e9f, 1e9f, 1e9f ), Mx( -1e9f, -1e9f, -1e9f );
	INT N = 0;
	for( INT k=0; k<DS->NumVerts; k++ )
	{
		if( DS->BindBones[k*3] != Body )   // slot 0 is the dominant bind
			continue;
		const FVector& O = DS->BindOffsets[k*3];
		const FLOAT A = ( Axis == 0 ) ? O.X : ( Axis == 1 ) ? O.Y : O.Z;
		if( A * Sign < H[Axis] + 1.5f )
			continue;
		Mn.X = Min( Mn.X, O.X ); Mn.Y = Min( Mn.Y, O.Y ); Mn.Z = Min( Mn.Z, O.Z );
		Mx.X = Max( Mx.X, O.X ); Mx.Y = Max( Mx.Y, O.Y ); Mx.Z = Max( Mx.Z, O.Z );
		N++;
	}
	if( N < 6 )
		return 0;
	// From the box face outward to the farthest vert; lateral extent = the verts'.
	FLOAT* MnA[3] = { &Mn.X, &Mn.Y, &Mn.Z };
	FLOAT* MxA[3] = { &Mx.X, &Mx.Y, &Mx.Z };
	if( Sign > 0.0f ) *MnA[Axis] =  H[Axis]; else *MxA[Axis] = -H[Axis];
	FVector C = ( Mn + Mx ) * 0.5f;
	FVector E = ( Mx - Mn ) * 0.5f * 0.85f;
	E.X = Max( E.X, 0.8f ); E.Y = Max( E.Y, 0.8f ); E.Z = Max( E.Z, 0.8f );
	const FLOAT K = DrawScale * RAGDOLL_U2M;
	b3ShapeDef Sd = b3DefaultShapeDef();
	Sd.density = 0.5f * Density;
	Sd.baseMaterial.friction    = 0.6f;
	Sd.baseMaterial.restitution = 0.05f;
	Sd.filter.categoryBits      = RAGDOLL_BODY_CATEGORY;
	b3BoxHull Hull = b3MakeOffsetBoxHull( E.X * K, E.Y * K, E.Z * K, b3Vec3{ C.X * K, C.Y * K, C.Z * K } );
	b3CreateHullShape( S.Bodies[Body], &Sd, &Hull.base );
	if( bLog )
		debugf( TEXT("RagdollPkg: overhang hull body=%i face=%c%c verts=%i centre=(%.1f,%.1f,%.1f) half=(%.1f,%.1f,%.1f) UU"), Body, Sign > 0 ? '+' : '-', "XYZ"[Axis], N, C.X, C.Y, C.Z, E.X, E.Y, E.Z );
	return 1;
}

// Contact census for one corpse (headless diagnostic): how many touching
// manifolds each body has against the level, against its own jointed
// neighbours, against its own non-adjacent bodies, and against other corpses.
// Pairs inside the corpse are counted once (lower body index reports).
static void LogRagdollContacts( FRagdollState& S, ARagdollCarcass* C )
{
	INT Geom = 0, SelfAdj = 0, SelfFar = 0, Other = 0, Asleep = 0;
	TCHAR Pairs[256]; Pairs[0] = 0;
	INT   PairLen = 0;
	for( INT i=0; i<S.NumBodies; i++ )
	{
		if( !S.Alive[i] || !b3Body_IsValid( S.Bodies[i] ) )
			continue;
		if( !b3Body_IsAwake( S.Bodies[i] ) )
			Asleep++;
		b3ContactData Data[48];
		const INT N = b3Body_GetContactData( S.Bodies[i], Data, 48 );
		for( INT n=0; n<N; n++ )
		{
			INT Points = 0;
			for( INT m=0; m<Data[n].manifoldCount; m++ )
				Points += Data[n].manifolds[m].pointCount;
			if( Points == 0 )
				continue;
			b3BodyId BA = b3Shape_GetBody( Data[n].shapeIdA );
			b3BodyId BB = b3Shape_GetBody( Data[n].shapeIdB );
			b3BodyId Ot = B3_ID_EQUALS( BA, S.Bodies[i] ) ? BB : BA;
			void* UD = b3Body_GetUserData( Ot );
			if( !UD )
				Geom++;
			else if( UD != (void*)C )
				Other++;
			else
			{
				INT j = -1;
				for( INT k=0; k<S.NumBodies; k++ )
					if( B3_ID_EQUALS( S.Bodies[k], Ot ) ) { j = k; break; }
				if( j >= 0 && j < i )
					continue;   // already counted from body j
				const UBOOL bAdj = ( j >= 0 ) && ( GRagdollBoneParent[j] == i || GRagdollBoneParent[i] == j );
				if( bAdj ) SelfAdj++; else SelfFar++;
				if( PairLen < 220 )
				{
					if( j >= 0 )
						PairLen += appSprintf( Pairs + PairLen, TEXT(" %i-%i%s"), i, j, bAdj ? TEXT("j") : TEXT("") );
					else
						PairLen += appSprintf( Pairs + PairLen, TEXT(" %i-frag"), i );
				}
			}
		}
	}
	debugf( TEXT("RagdollPkg: contacts %s sync=%i geom=%i self_joint=%i self_other=%i corpses=%i asleep=%i/%i pairs:%s"),
		C->GetName(), S.SyncCount, Geom, SelfAdj, SelfFar, Other, Asleep, S.NumBodies, Pairs );

	// Bodies under the floor: level geometry within 0.5 m straight ABOVE the
	// body centre and none within 0.5 m below it. (User report 2026-10-10:
	// "some parts of ragdolls just go below floor".) Logged with the body
	// index, the surface height above the centre, and the body's speed.
	FRagdollLevelWorld& LW = GetRagdollLevelWorld();
	if( LW.bBuilt )
	{
		b3QueryFilter Filter = b3DefaultQueryFilter();
		Filter.maskBits = RAGDOLL_GEOM_CATEGORY;
		TCHAR Under[256]; Under[0] = 0;
		INT UnderLen = 0, NumUnder = 0;
		for( INT i=0; i<S.NumBodies; i++ )
		{
			if( !S.Alive[i] || !b3Body_IsValid( S.Bodies[i] ) )
				continue;
			b3Vec3 P = b3Body_GetPosition( S.Bodies[i] );
			b3Vec3 Up = { 0.0f, 0.0f, 0.5f }, Down = { 0.0f, 0.0f, -0.5f };
			b3RayResult RU = b3World_CastRayClosest( LW.World, P, Up, Filter );
			b3RayResult RD = b3World_CastRayClosest( LW.World, P, Down, Filter );
			if( RU.hit && !RD.hit )
			{
				NumUnder++;
				b3Vec3 V = b3Body_GetLinearVelocity( S.Bodies[i] );
				if( UnderLen < 200 )
					UnderLen += appSprintf( Under + UnderLen, TEXT(" %i(+%.1fUU,%.1fm/s)"), i, ( RU.point.z - P.z ) * RAGDOLL_M2U, sqrt( V.x*V.x + V.y*V.y + V.z*V.z ) );
			}
		}
		if( NumUnder > 0 )
			debugf( TEXT("RagdollPkg: underfloor %s sync=%i bodies=%i:%s"), C->GetName(), S.SyncCount, NumUnder, Under );
	}

	// Joint angles against their limits (degrees), for tuning the per-model
	// tables: ball joints "c<cone>/<limit> t<twist>[lo,hi]", hinges
	// "<angle>[lo,hi]"; a '!' marks a joint sitting within 2 deg of a limit.
	static const TCHAR* JName[12] = { TEXT(""), TEXT("spine"), TEXT("neck"), TEXT("head"), TEXT("uarmL"), TEXT("uarmR"), TEXT("elbowL"), TEXT("elbowR"), TEXT("hipL"), TEXT("hipR"), TEXT("kneeL"), TEXT("kneeR") };
	TCHAR JL[512]; JL[0] = 0;
	INT JLen = 0;
	const FLOAT R2D = 180.0f / PI;
	for( INT j=1; j<S.NumBodies && j<12; j++ )
	{
		if( !b3Joint_IsValid( S.Joints[j] ) || JLen > 440 )
			continue;
		const UBOOL bHinge = ( j == BONE_ForearmL || j == BONE_ForearmR || j == BONE_ShinL || j == BONE_ShinR );
		if( bHinge )
		{
			const FLOAT A = b3RevoluteJoint_GetAngle( S.Joints[j] ) * R2D;
			const FLOAT Lo = b3RevoluteJoint_GetLowerLimit( S.Joints[j] ) * R2D;
			const FLOAT Hi = b3RevoluteJoint_GetUpperLimit( S.Joints[j] ) * R2D;
			const UBOOL bAt = ( A - Lo < 2.0f ) || ( Hi - A < 2.0f );
			JLen += appSprintf( JL + JLen, TEXT(" %s:%.0f[%.0f,%.0f]%s"), JName[j], A, Lo, Hi, bAt ? TEXT("!") : TEXT("") );
		}
		else
		{
			const FLOAT Cone = b3SphericalJoint_GetConeAngle( S.Joints[j] ) * R2D;
			const FLOAT CLim = b3SphericalJoint_GetConeLimit( S.Joints[j] ) * R2D;
			const FLOAT Tw   = b3SphericalJoint_GetTwistAngle( S.Joints[j] ) * R2D;
			const FLOAT TLo  = b3SphericalJoint_GetLowerTwistLimit( S.Joints[j] ) * R2D;
			const FLOAT THi  = b3SphericalJoint_GetUpperTwistLimit( S.Joints[j] ) * R2D;
			const UBOOL bAt = ( CLim - Cone < 2.0f ) || ( Tw - TLo < 2.0f ) || ( THi - Tw < 2.0f );
			JLen += appSprintf( JL + JLen, TEXT(" %s:c%.0f/%.0f,t%.0f[%.0f,%.0f]%s"), JName[j], Cone, CLim, Tw, TLo, THi, bAt ? TEXT("!") : TEXT("") );
		}
	}
	debugf( TEXT("RagdollPkg: jointangles %s sync=%i%s"), C->GetName(), S.SyncCount, JL );
}

// Pose the freshly-created ragdoll so the corpse resembles the dead pawn
// instead of a frozen arms-down bind statue:
//  - yaw: rotate the whole bind skeleton (bind front = +Y) to the pawn's facing;
//  - pose: every joint gets a per-corpse random local rotation (arms swung
//    forward/out and flexed, legs spread/bent, spine and head tilted), scaled
//    by the PoseVariation config knob. With PoseVariation=0 every corpse gets
//    the old fixed weapon-aim pose (arms ~70 deg forward, elbows flexed, head
//    nodded); at 1 the right arm keeps the aim pose on ~45% of corpses and
//    everything else is randomized so no two corpses spawn alike.
// Transforms are built by forward kinematics down the bone chain, so each body
// is placed by its parent's posed anchor: the joint frames still coincide at
// their anchors, hinge bodies rotate only about their parent's local hinge
// axis, and the ranges stay inside the cone/hinge limits. The joints therefore
// hold the pose and the corpse flops naturally from there. Runs before spawn
// clearance so embedded-body checks see the posed pose.
struct FPoseLink { INT Parent; INT Anchor; };
static const FPoseLink GPoseChain[RAGDOLL_BONE_COUNT] =
{
	{ -1,             -1 },  // Pelvis (root)
	{ BONE_Pelvis,     0 },  // Spine
	{ BONE_Spine,      1 },  // Neck
	{ BONE_Neck,       2 },  // Head
	{ BONE_Spine,      3 },  // UpperArmL
	{ BONE_Spine,      4 },  // UpperArmR
	{ BONE_UpperArmL,  5 },  // ForearmL
	{ BONE_UpperArmR,  6 },  // ForearmR
	{ BONE_Pelvis,     7 },  // ThighL
	{ BONE_Pelvis,     8 },  // ThighR
	{ BONE_ThighL,     9 },  // ShinL
	{ BONE_ThighR,    10 },  // ShinR
	{ BONE_ShinL,     11 },  // FootL (box skeleton only)
	{ BONE_ShinR,     12 },  // FootR
};

static FLOAT RandRange( FLOAT A, FLOAT B )
{
	return A + ( B - A ) * appFrand();
}
static FLOAT ClampF( FLOAT V, FLOAT Lo, FLOAT Hi )
{
	return V < Lo ? Lo : ( V > Hi ? Hi : V );
}

static void ApplyDeathPose( FRagdollState& S, ARagdollCarcass* C, const FVector* Anch )
{
	if( !S.bHasDeathPose )
		return;

	const FLOAT DrawScale = C->DrawScale > 0.0f ? C->DrawScale : 1.0f;
	const FVector Origin = C->Location;
	const FRagdollBoneDef* Defs = S.BoneDefs ? S.BoneDefs : GRagdollBoneDefs;

	// Yaw: pawn forward is (cos,sin) of its Yaw (Yaw=0 -> +X); mapping bind +Y
	// onto that direction is a rotation about Z of (Yaw - 90 deg).
	const FLOAT YawRad = ( S.SpawnYaw & 65535 ) * ( 2.0f * PI / 65536.0f );
	const FLOAT Theta  = YawRad - PI / 2.0f;
	b3Quat QYaw = b3MakeQuatFromAxisAngle( { 0.0f, 0.0f, 1.0f }, Theta );

	// Aim: base arm raise ~70 deg from vertical; Unreal pitch (positive = up)
	// adds a little more so high shots pose the arms higher.
	const FLOAT PitchRad = ( S.SpawnPitch & 65535 ) * ( 2.0f * PI / 65536.0f );
	FLOAT AimTilt = ClampF( 1.22f + PitchRad * 0.25f, 0.8f, 1.45f );

	// Variation amount (0 = fixed aim pose, 1 = default spread, up to 2).
	const FLOAT Var = ClampF( RagdollFloatProp( C, TEXT("PoseVariation"), 1.0f ), 0.0f, 2.0f );

	// Local joint rotations in the bind frame: X = sagittal swing (positive
	// swings a hanging limb forward, +Y), Y = lateral swing (positive moves a
	// hanging limb toward -X, i.e. outward for the LEFT limb), Z = twist. The
	// cone limits measure the combined X/Y swing, so keep tilt+abduct well
	// inside each joint's cone (shoulder 2.4, hip 1.2, spine 0.6, head 0.7).
	auto RotX = []( FLOAT a ) -> b3Quat { return b3MakeQuatFromAxisAngle( { 1.0f, 0.0f, 0.0f }, a ); };
	auto RotY = []( FLOAT a ) -> b3Quat { return b3MakeQuatFromAxisAngle( { 0.0f, 1.0f, 0.0f }, a ); };
	auto Swing = [&]( FLOAT Tilt, FLOAT Abduct ) -> b3Quat { return b3MulQuat( RotY( Abduct ), RotX( Tilt ) ); };

	b3Quat Local[RAGDOLL_BONE_COUNT];
	for( INT i=0; i<RAGDOLL_BONE_COUNT; i++ )
		Local[i] = b3Quat_identity;

	// Trunk: slight random bend/lean; neck and head nod down-range.
	Local[BONE_Spine] = Swing( Var * RandRange( -0.20f, 0.30f ), Var * RandRange( -0.20f, 0.20f ) );
	Local[BONE_Neck]  = Swing( Var * RandRange( -0.10f, 0.20f ), Var * RandRange( -0.10f, 0.10f ) );
	Local[BONE_Head]  = Swing( ClampF( 0.35f + Var * RandRange( -0.40f, 0.20f ), -0.10f, 0.55f ),
	                           Var * RandRange( -0.20f, 0.20f ) );

	// Arms. Outward abduction is +Y rotation for the left arm, -Y for the right.
	FLOAT ArmTilt[2], ArmAbd[2], ArmFlex[2];
	for( INT s=0; s<2; s++ )
	{
		const UBOOL bAimArm = ( s == 1 ) && ( appFrand() < 0.45f );   // right = weapon arm
		if( bAimArm )
		{
			ArmTilt[s] = ClampF( AimTilt + Var * RandRange( -0.15f, 0.15f ), -0.4f, 1.45f );
			ArmAbd[s]  = Var * RandRange( 0.0f, 0.20f );
			ArmFlex[s] = 0.35f + Var * RandRange( 0.0f, 0.30f );
		}
		else
		{
			ArmTilt[s] = ClampF( AimTilt + Var * RandRange( -1.50f, 0.30f ), -0.4f, 1.45f );
			ArmAbd[s]  = Var * RandRange( 0.0f, 0.90f );
			ArmFlex[s] = ClampF( 0.35f + Var * RandRange( -0.25f, 1.20f ), 0.05f, 1.80f );
		}
	}
	Local[BONE_UpperArmL] = Swing( ArmTilt[0],  ArmAbd[0] );
	Local[BONE_UpperArmR] = Swing( ArmTilt[1], -ArmAbd[1] );
	Local[BONE_ForearmL]  = RotX( ArmFlex[0] );   // elbow hinge: flex is +X
	Local[BONE_ForearmR]  = RotX( ArmFlex[1] );

	// Legs: hips swing/spread a little, knees flex (hinge range is -2.0..0.05,
	// so flexion is a NEGATIVE X rotation: the shin folds backward).
	FLOAT LegTilt[2], LegAbd[2], KneeFlex[2];
	for( INT s=0; s<2; s++ )
	{
		LegTilt[s]  = Var * RandRange( -0.35f, 0.70f );
		LegAbd[s]   = Var * RandRange(  0.00f, 0.40f );
		KneeFlex[s] = -Var * RandRange( 0.00f, 0.70f );
	}
	Local[BONE_ThighL] = Swing( LegTilt[0],  LegAbd[0] );
	Local[BONE_ThighR] = Swing( LegTilt[1], -LegAbd[1] );
	Local[BONE_ShinL]  = RotX( KneeFlex[0] );
	Local[BONE_ShinR]  = RotX( KneeFlex[1] );

	static INT GPoseDbg = 0;
	if( GPoseDbg < 8 )
	{
		GPoseDbg++;
		debugf( TEXT("RagdollPkg: pose %s var=%.2f armL=(%.2f,%.2f,%.2f) armR=(%.2f,%.2f,%.2f) legL=(%.2f,%.2f,%.2f) legR=(%.2f,%.2f,%.2f)"),
			C->GetName(), Var,
			ArmTilt[0], ArmAbd[0], ArmFlex[0], ArmTilt[1], ArmAbd[1], ArmFlex[1],
			LegTilt[0], LegAbd[0], KneeFlex[0], LegTilt[1], LegAbd[1], KneeFlex[1] );
	}

	// Forward kinematics over the chain (parents always precede children in
	// the bone enum). Positions are bind-space offsets from the corpse origin
	// in UU; WorldQ is the accumulated rotation.
	b3Vec3 WP[RAGDOLL_BONE_COUNT];
	b3Quat WQ[RAGDOLL_BONE_COUNT];
	for( INT i=0; i<S.NumBodies; i++ )
	{
		const FRagdollBoneDef& D = Defs[i];
		const b3Vec3 Ci = { DrawScale * D.CX, DrawScale * D.CY, DrawScale * D.CZ };
		const FPoseLink& L = GPoseChain[i];
		if( L.Parent < 0 )
		{
			WP[i] = Ci;
			WQ[i] = b3Quat_identity;
		}
		else
		{
			const FRagdollBoneDef& DP = Defs[L.Parent];
			const b3Vec3 Cp = { DrawScale * DP.CX, DrawScale * DP.CY, DrawScale * DP.CZ };
			const b3Vec3 A  = { DrawScale * Anch[L.Anchor].X, DrawScale * Anch[L.Anchor].Y, DrawScale * Anch[L.Anchor].Z };
			WQ[i] = b3MulQuat( WQ[L.Parent], Local[i] );
			// posed anchor = parent center + parent rotation * (anchor - parent center)
			const b3Vec3 PA = b3Add( WP[L.Parent], b3RotateVector( WQ[L.Parent], b3Sub( A, Cp ) ) );
			// body center hangs off the posed anchor by its own rotation
			WP[i] = b3Add( PA, b3RotateVector( WQ[i], b3Sub( Ci, A ) ) );
		}

		// Yaw the posed skeleton about the corpse origin (pose first, then yaw).
		const b3Vec3 Py = b3RotateVector( QYaw, WP[i] );
		const b3Quat Q  = b3MulQuat( QYaw, WQ[i] );
		b3Body_SetTransform( S.Bodies[i],
			{ ( Origin.X + Py.x ) * RAGDOLL_U2M, ( Origin.Y + Py.y ) * RAGDOLL_U2M, ( Origin.Z + Py.z ) * RAGDOLL_U2M }, Q );
	}
}

static void BuildRagdollImpl( ARagdollCarcass* C )
{
	guard(BuildRagdollImpl);
	TMap<AActor*,FRagdollState>& Map = GetRagdollStates();
	FRagdollState* Pre = Map.Find( C );
	if( Pre && Pre->bBuilt )
		return;
	// The death pose was stored by SetDeathPose (from Initfor) on a fresh state
	// entry; Map.Set below overwrites the value, so carry the pose across.
	const INT   PoseYaw   = Pre ? Pre->SpawnYaw   : 0;
	const INT   PosePitch = Pre ? Pre->SpawnPitch : 0;
	const UBOOL bPose     = Pre ? Pre->bHasDeathPose : false;
	FRagdollState& S = Map.Set( C, FRagdollState() );
	S.SpawnYaw      = PoseYaw;
	S.SpawnPitch    = PosePitch;
	S.bHasDeathPose = bPose;
	S.Owner         = C;

	FLOAT DrawScale = C->DrawScale > 0.0f ? C->DrawScale : 1.0f;
	FVector Origin  = C->Location;

	// Per-model dataset (derived skeleton + anchors) or canonical box skeleton.
	// The Carcass carries the dead pawn's ULodMesh (set by Initfor before
	// BuildRagdoll), so its name resolves the dataset for real-mesh corpses.
	// bBoxMannequins (class config) forces the box fallback even when a
	// dataset exists - toggle via ini or the console "set" command.
	const FRagdollDatasetEntry* DS = NULL;
	ULodMesh* LiveMesh = Cast<ULodMesh>( C->Mesh );
	if( LiveMesh && !RagdollForceBoxes( C ) )
		DS = FindRagdollDataset( LiveMesh->GetName() );
	const FRagdollBoneDef* Defs = DS ? DS->Bones : GRagdollBoneDefs;
	S.BoneDefs = Defs;
	S.Dataset  = DS;
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
		DeathVel.X += ( appFrand() * 2.0f - 1.0f ) * 1.4f;
		DeathVel.Y += ( appFrand() * 2.0f - 1.0f ) * 1.4f;
		DeathVel.Z += 0.5f;
		BaseSpeed = DeathVel.Size();
	}
	// Clamp so a fast fall or hit doesn't blast the body out of the world or
	// off the ground slab. 8 m/s keeps the killing shot's push visible.
	const FLOAT MaxImpulse = 8.0f;
	if( BaseSpeed > MaxImpulse )
		DeathVel *= MaxImpulse / BaseSpeed;

	// Ragdoll bodies.
	const FLOAT AngDamp   = RagdollFloatProp( C, TEXT("AngularDamping"), 0.4f );
	const FLOAT DeathSpin = RagdollFloatProp( C, TEXT("DeathSpin"), 1.2f );
	const UBOOL bSelfCollide = RagdollBoolProp( C, TEXT("bSelfCollide"), 1 );
	static INT GTrimDbg = 0;
	const UBOOL bTrimLog = ( GTrimDbg++ < 2 );
	for( INT i=0; i<S.NumBodies; i++ )
	{
		const FRagdollBoneDef& D = Defs[i];
		FVector Center = Origin + DrawScale * FVector( D.CX, D.CY, D.CZ );

		b3BodyDef Bd = b3DefaultBodyDef();
		Bd.type           = b3_dynamicBody;
		Bd.position       = { Center.X*RAGDOLL_U2M, Center.Y*RAGDOLL_U2M, Center.Z*RAGDOLL_U2M };
		Bd.rotation       = b3Quat_identity;
		Bd.linearDamping  = 0.05f;
		Bd.angularDamping = AngDamp;   // low: limbs keep swinging (tune: AngularDamping)
		Bd.gravityScale   = 1.0f;
		Bd.userData       = C;
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
		W.x = ( appFrand() * 2.0f - 1.0f ) * DeathSpin;
		W.y = ( appFrand() * 2.0f - 1.0f ) * DeathSpin;
		W.z = ( appFrand() * 2.0f - 1.0f ) * DeathSpin * 0.66f;
		b3Body_SetAngularVelocity( S.Bodies[i], W );

		// Hull: the dataset box, trimmed at the parent end for the bodies whose
		// joint collides with the parent (see RagdollTrimChildBox).
		FVector Half( D.HX, D.HY, D.HZ );
		FVector HullOff( 0, 0, 0 );
		if( bSelfCollide && RagdollJointCollides( i ) )
			HullOff = RagdollTrimChildBox( Defs, i, GRagdollBoneParent[i], Half, bTrimLog );
		FLOAT HX = Half.X * DrawScale * RAGDOLL_U2M;
		FLOAT HY = Half.Y * DrawScale * RAGDOLL_U2M;
		FLOAT HZ = Half.Z * DrawScale * RAGDOLL_U2M;
		const b3Vec3 Offset = { HullOff.X * DrawScale * RAGDOLL_U2M, HullOff.Y * DrawScale * RAGDOLL_U2M, HullOff.Z * DrawScale * RAGDOLL_U2M };

		b3ShapeDef Sd = b3DefaultShapeDef();
		Sd.density = D.MassKg / ( 8.0f * HX * HY * HZ );
		Sd.baseMaterial.friction     = 0.6f;
		Sd.baseMaterial.restitution  = 0.05f;
		// b3DefaultShapeDef sets filter.categoryBits = UINT64_MAX, which
		// overlaps EVERY bit including the geometry category. The clearance ray
		// masks on the geometry category, so without an explicit body category
		// it would hit the corpse's own bodies and false-lift every spawn.
		Sd.filter.categoryBits = RAGDOLL_BODY_CATEGORY;
		b3BoxHull Hull = b3MakeOffsetBoxHull( HX, HY, HZ, Offset );
		b3CreateHullShape( S.Bodies[i], &Sd, &Hull.base );
		// Feet / hands / skull: extra hulls for the skin past the limb box on
		// every face except the one holding the joint to the parent.
		if( i == BONE_ShinL || i == BONE_ShinR || i == BONE_ForearmL || i == BONE_ForearmR || i == BONE_Head )
		{
			const FLOAT ProxSign = ( i == BONE_Head ) ? -1.0f : 1.0f;   // joint side on Z
			for( INT Face=0; Face<6; Face++ )
			{
				const INT   Axis = Face / 2;
				const FLOAT Sign = ( Face & 1 ) ? -1.0f : 1.0f;
				if( Axis == 2 && Sign == ProxSign )
					continue;
				RagdollAddOverhangHull( S, DS, i, D, Axis, Sign, DrawScale, Sd.density, bTrimLog );
			}
		}

		S.RestPositions[i] = Bd.position;
		S.RestRotations[i] = b3Quat_identity;
		S.Alive[i] = 1;
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
	// Working copy of the anchors so a per-model table can nudge them.
	FVector Anch[13];
	for( INT k=0; k<13; k++ )
		Anch[k] = DS ? DS->Anchors[k] : CanonicalAnchors[k];

	// Joint table: the global defaults, replaced by the model's own table when
	// tools/rigs/<Sel>.tuning.json provided one (RagdollJointTuning.h). The
	// console multipliers JointLimitScale / JointFrictionScale apply on top.
	const FRagdollJointSpec* Spec = GRagdollDefaultJoints;
	const FRagdollJointTuning* Tune = DS ? FindRagdollJointTuning( DS->DatasetName ) : NULL;
	if( Tune )
	{
		Spec = Tune->Joints;
		for( INT k=0; k<RAGDOLL_TUNED_JOINTS; k++ )
			Anch[k] += Tune->AnchorOffsets[k];
	}
	static INT GJointDbg = 0;
	if( GJointDbg < 6 )
	{
		GJointDbg++;
		debugf( TEXT("RagdollPkg: joints %s table=%s shoulder=%.2f/%.2f hip=%.2f/%.2f elbow=%.2f..%.2f knee=%.2f..%.2f"),
			C->GetName(), Tune ? Tune->DatasetName : TEXT("global"),
			Spec[3].L0, Spec[3].Friction, Spec[7].L0, Spec[7].Friction, Spec[5].L0, Spec[5].L1, Spec[9].L0, Spec[9].L1 );
	}

	S.bPlanarCuts       = RagdollBoolProp( C, TEXT("bPlanarCuts"), 0 );
	S.bFragmentBodies   = RagdollBoolProp( C, TEXT("bFragmentBodies"), 0 );
	GJointFrictionScale = RagdollFloatProp( C, TEXT("JointFrictionScale"), 1.0f );
	GJointLimitScale    = RagdollFloatProp( C, TEXT("JointLimitScale"), 1.0f );
	GJointSpringHertz   = ClampF( RagdollFloatProp( C, TEXT("JointSpringHertz"), 0.8f ), 0.0f, 5.0f );
	GJointSpringDamping = ClampF( RagdollFloatProp( C, TEXT("JointSpringDamping"), 0.8f ), 0.0f, 5.0f );
	AddJoint( S, BONE_Pelvis,    BONE_Spine,     Defs, Anch[0],  DrawScale, Spec[0] );
	AddJoint( S, BONE_Spine,     BONE_Neck,      Defs, Anch[1],  DrawScale, Spec[1] );
	AddJoint( S, BONE_Neck,      BONE_Head,      Defs, Anch[2],  DrawScale, Spec[2] );
	AddJoint( S, BONE_Spine,     BONE_UpperArmL, Defs, Anch[3],  DrawScale, Spec[3], bSelfCollide );
	AddJoint( S, BONE_Spine,     BONE_UpperArmR, Defs, Anch[4],  DrawScale, Spec[4], bSelfCollide );
	AddJoint( S, BONE_UpperArmL, BONE_ForearmL,  Defs, Anch[5],  DrawScale, Spec[5] );
	AddJoint( S, BONE_UpperArmR, BONE_ForearmR,  Defs, Anch[6],  DrawScale, Spec[6] );
	AddJoint( S, BONE_Pelvis,    BONE_ThighL,    Defs, Anch[7],  DrawScale, Spec[7], bSelfCollide );
	AddJoint( S, BONE_Pelvis,    BONE_ThighR,    Defs, Anch[8],  DrawScale, Spec[8], bSelfCollide );
	AddJoint( S, BONE_ThighL,    BONE_ShinL,     Defs, Anch[9],  DrawScale, Spec[9] );
	AddJoint( S, BONE_ThighR,    BONE_ShinR,     Defs, Anch[10], DrawScale, Spec[10] );
	// Feet are skipped from real-mesh ragdolls; only the box skeleton keeps the
	// ankle joints (bodies 12/13 only exist in the box path).
	if( !DS )
	{
		AddJoint( S, BONE_ShinL, BONE_FootL, Defs, Anch[11], DrawScale, { JK_Ball, 0.60f, -0.35f,  0.35f, 0.1f, 0.0f, 0.0f, 0.0f } );
		AddJoint( S, BONE_ShinR, BONE_FootR, Defs, Anch[12], DrawScale, { JK_Ball, 0.60f, -0.35f,  0.35f, 0.1f, 0.0f, 0.0f, 0.0f } );
	}

	// Lift bodies that spawned inside level geometry (ramp/stairs deaths) so the
	// first step doesn't explode them. Must run before the first StepRagdoll.
	// Pose first (arms raised, corpse yawed to the pawn) so clearance sees the
	// final spawn configuration.
	ApplyDeathPose( S, C, Anch );
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

	// Contact census (self-collision / stacking diagnostics) for the first 8
	// corpses at ~1 s, 3 s and 6 s after spawn (SimFreq 50).
	static INT GContactDbg = 0;
	if( S->SyncCount == 50 && GContactDbg < 8 )
	{
		GContactDbg++;
		S->bContactLog = 1;
	}
	if( S->bContactLog && ( S->SyncCount == 50 || S->SyncCount == 150 || S->SyncCount == 300 ) )
		LogRagdollContacts( *S, C );

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

	// The visual is driven entirely by URagdollMesh::GetFrame (world space).
	// Keep the actor transform on the pelvis for culling/replication/3rd-person
	// camera heuristics. Move it through the engine (un-hash, move, re-hash) so
	// the collision hash tracks the corpse: a raw Location write leaves stale
	// hash buckets, and SetCollisionSize's un-hash then computes the wrong
	// bucket and double-hashes the corpse, which eventually corrupts the bucket
	// chain and crashes FCollisionHash::ActorLineCheck during pawn physics.
	// bNoFail so the ragdoll's own physics owns world collision (the engine
	// move is a follow-along teleport, not a push).
	FCheckResult MoveHit( 1.0f );
	C->GetLevel()->MoveActor( C, NewLoc - C->Location, NewRot, MoveHit,
		false, false, false, true );

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
			// Only bodies still attached to the pelvis island: a limb or gib
			// flung across the room must not inflate the hit cylinder to cover
			// the whole room (projectiles would detonate in mid-air).
			if( !S->Alive[i] || RagdollIslandRoot( *S, i ) != BONE_Pelvis )
				continue;
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
				if( S->Alive[i] && b3Body_IsValid( S->Bodies[i] ) )
					b3DestroyBody( S->Bodies[i] );
			for( INT e=0; e<S->NumExtra; e++ )
				if( b3Body_IsValid( S->ExtraBodies[e] ) )
					b3DestroyBody( S->ExtraBodies[e] );
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

// Nearest live body (by centre distance) to a world point; -1 without state.
static INT NearestBodyImpl( const FRagdollState* S, const FVector& Loc )
{
	if( !S || !S->bBuilt )
		return -1;
	INT Closest = -1;
	FLOAT ClosestD = 1e30f;
	for( INT i=0; i<S->NumBodies; i++ )
	{
		if( !S->Alive[i] )
			continue;
		b3WorldTransform T = b3Body_GetTransform( S->Bodies[i] );
		FLOAT dx = T.p.x * RAGDOLL_M2U - Loc.X, dy = T.p.y * RAGDOLL_M2U - Loc.Y, dz = T.p.z * RAGDOLL_M2U - Loc.Z;
		FLOAT D = dx*dx + dy*dy + dz*dz;
		if( D < ClosestD ) { ClosestD = D; Closest = i; }
	}
	return Closest;
}

void ARagdollCarcass::execApplyDamageImpulse( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execApplyDamageImpulse);
	P_GET_VECTOR( HitLocation );
	P_GET_VECTOR( Momentum );
	P_GET_INT( Damage );
	P_FINISH;

	FRagdollState* S = GetRagdollStates().Find( this );
	*(INT*)Result = NearestBodyImpl( S, HitLocation );
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
		INT Closest = *(INT*)Result;
		FLOAT ClosestD = 0.0f;
		if( Closest >= 0 )
		{
			b3WorldTransform T = b3Body_GetTransform( S->Bodies[Closest] );
			FLOAT dx = T.p.x - Hit.x, dy = T.p.y - Hit.y, dz = T.p.z - Hit.z;
			ClosestD = sqrt( dx*dx + dy*dy + dz*dz );
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
		if( !S->Alive[i] )
			continue;
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

// Walk-over push: a living pawn moving over the corpse shoves the body parts
// its collision cylinder actually touches. Per body, the closest point of the
// body's box to the pawn's cylinder axis is found; only bodies whose box comes
// within the pawn radius (plus a short soft band) react, so stepping on a hand
// moves the hand and forearm, not the whole corpse. The reaction is a velocity
// match at the CONTACT POINT (eased toward the pawn's horizontal motion plus
// a radial shove away from the pawn axis, applied as an off-centre impulse),
// so a trodden limb rotates and splays out of the way instead of the corpse
// translating as one block. Deficit-based easing can't stack over the 50 Hz
// ticks: once the limb moves with the pawn the correction goes to zero.
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

	// Pawn collision cylinder in meters.
	const FLOAT Px    = P->Location.X * RAGDOLL_U2M;
	const FLOAT Py    = P->Location.Y * RAGDOLL_U2M;
	const FLOAT FootZ = ( P->Location.Z - P->CollisionHeight ) * RAGDOLL_U2M;
	const FLOAT TopZ  = ( P->Location.Z + P->CollisionHeight ) * RAGDOLL_U2M;
	const FLOAT PawnR = Max( P->CollisionRadius, 10.0f ) * RAGDOLL_U2M;
	const FLOAT Band  = 0.18f;   // soft contact band outside the cylinder (m)
	const FLOAT Gain  = 0.35f;   // per-tick easing toward the target velocity
	const FLOAT Radial = 1.2f;   // m/s shove away from the pawn axis
	const FLOAT LiftV  = 0.6f;   // m/s: trodden parts hop slightly so they roll

	const FLOAT Scale = ( this->DrawScale > 0.0f ) ? this->DrawScale : 1.0f;
	const FRagdollBoneDef* Defs = S->BoneDefs ? S->BoneDefs : GRagdollBoneDefs;

	for( INT i=0; i<S->NumBodies; i++ )
	{
		if( !S->Alive[i] )
			continue;
		const FRagdollBoneDef& D = Defs[i];
		const FLOAT HX = D.HX * Scale * RAGDOLL_U2M;
		const FLOAT HY = D.HY * Scale * RAGDOLL_U2M;
		const FLOAT HZ = D.HZ * Scale * RAGDOLL_U2M;
		b3WorldTransform T = b3Body_GetTransform( S->Bodies[i] );

		// Cheap reject on the body's bounding sphere.
		const FLOAT Reach = sqrt( HX*HX + HY*HY + HZ*HZ ) + PawnR + Band;
		FLOAT dx = T.p.x - Px, dy = T.p.y - Py;
		if( dx*dx + dy*dy > Reach*Reach )
			continue;
		if( T.p.z - HZ - HX > TopZ || T.p.z + HZ + HX < FootZ - 0.45f )
			continue;

		// Closest point of the box to the pawn axis, sampled at the body's own
		// height clamped into the cylinder (a limb lying under the feet probes
		// at foot level, a part beside the shins at its own level).
		const FLOAT Sz = ClampF( T.p.z, FootZ - 0.3f, TopZ );
		const b3Vec3 Sample = { Px, Py, Sz };
		b3Vec3 Lp = b3InvRotateVector( T.q, b3Sub( Sample, T.p ) );
		Lp.x = ClampF( Lp.x, -HX, HX );
		Lp.y = ClampF( Lp.y, -HY, HY );
		Lp.z = ClampF( Lp.z, -HZ, HZ );
		const b3Vec3 Cw = b3Add( b3RotateVector( T.q, Lp ), T.p );
		const FLOAT cdx = Cw.x - Px, cdy = Cw.y - Py;
		const FLOAT d = sqrt( cdx*cdx + cdy*cdy );
		if( Cw.z > TopZ || Cw.z < FootZ - 0.45f )
			continue;
		const FLOAT W = ClampF( 1.0f - ( d - PawnR ) / Band, 0.0f, 1.0f );
		if( W <= 0.0f )
			continue;

		// Target velocity at the contact point: the pawn's motion plus a shove
		// outward from the pawn axis so limbs splay rather than march together.
		b3Vec3 Target = Push;
		if( d > 0.01f )
		{
			Target.x += cdx / d * Radial;
			Target.y += cdy / d * Radial;
		}
		const b3Vec3 Vc = b3Body_GetWorldPointVelocity( S->Bodies[i], Cw );
		b3Vec3 dV;
		dV.x = ( Target.x - Vc.x ) * Gain * W;
		dV.y = ( Target.y - Vc.y ) * Gain * W;
		dV.z = ( Vc.z < LiftV ) ? ( LiftV - Vc.z ) * Gain * W * 0.5f : 0.0f;
		const FLOAT Mass = b3Body_GetMass( S->Bodies[i] );
		const b3Vec3 Imp = { dV.x * Mass, dV.y * Mass, dV.z * Mass };
		b3Body_ApplyLinearImpulse( S->Bodies[i], Imp, Cw, true );   // off-centre -> limb rotates
	}
	unguard;
}

void ARagdollCarcass::execSetDeathPose( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execSetDeathPose);
	P_GET_INT( Yaw );
	P_GET_INT( Pitch );
	P_FINISH;
	FRagdollState& S = GetRagdollStates().Set( this, FRagdollState() );
	S.SpawnYaw      = Yaw;
	S.SpawnPitch    = Pitch;
	S.bHasDeathPose = true;
	unguard;
}

/*-----------------------------------------------------------------------------
	Gore: dismemberment / gibbing.
-----------------------------------------------------------------------------*/

// Destroy the joint holding 'Body' to its parent. Returns false when the body
// is the pelvis, dead, or already detached.
static UBOOL DetachBody( FRagdollState& S, INT Body )
{
	if( Body <= 0 || Body >= S.NumBodies || !S.Alive[Body] || S.Cut[Body] )
		return 0;
	if( b3Joint_IsValid( S.Joints[Body] ) )
		b3DestroyJoint( S.Joints[Body], true );
	S.Joints[Body] = b3_nullJointId;
	S.Cut[Body]    = 1;
	return 1;
}

// Kick every live body of the island rooted at 'Root': add DeltaV (m/s, with a
// little per-body scatter) and a random spin so the part tumbles.
static void KickIsland( FRagdollState& S, INT Root, const b3Vec3& DeltaV, FLOAT Spin )
{
	for( INT b=0; b<S.NumBodies; b++ )
	{
		if( !S.Alive[b] || RagdollIslandRoot( S, b ) != Root )
			continue;
		b3Vec3 V = b3Body_GetLinearVelocity( S.Bodies[b] );
		const FLOAT Scatter = 0.85f + appFrand() * 0.3f;
		V.x += DeltaV.x * Scatter;
		V.y += DeltaV.y * Scatter;
		V.z += DeltaV.z * Scatter;
		b3Body_SetLinearVelocity( S.Bodies[b], V );
		b3Vec3 W;
		W.x = ( appFrand() * 2.0f - 1.0f ) * Spin;
		W.y = ( appFrand() * 2.0f - 1.0f ) * Spin;
		W.z = ( appFrand() * 2.0f - 1.0f ) * Spin;
		b3Body_SetAngularVelocity( S.Bodies[b], W );
	}
}

// Dismember(Body, Impulse): tear the subtree rooted at Body off the corpse.
// Impulse is a velocity change in UU/s given to the detached part (child
// joints are kept, so a forearm leaves with its hand, a thigh with its shin).
// Returns true when a joint was actually cut.
void ARagdollCarcass::execDismember( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execDismember);
	P_GET_INT( Body );
	P_GET_VECTOR( Impulse );
	P_FINISH;
	*(DWORD*)Result = 0;

	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt )
		return;
	if( !DetachBody( *S, Body ) )
		return;
	RagdollSplitIslands( *S );

	b3Vec3 DeltaV = { Impulse.X * RAGDOLL_U2M, Impulse.Y * RAGDOLL_U2M, Impulse.Z * RAGDOLL_U2M };
	const FLOAT Spd = sqrt( DeltaV.x*DeltaV.x + DeltaV.y*DeltaV.y + DeltaV.z*DeltaV.z );
	const FLOAT MaxSpd = 12.0f;
	if( Spd > MaxSpd )
	{
		DeltaV.x *= MaxSpd / Spd;
		DeltaV.y *= MaxSpd / Spd;
		DeltaV.z *= MaxSpd / Spd;
	}
	KickIsland( *S, Body, DeltaV, 6.0f );
	// The stump recoils a little the other way.
	b3Vec3 Recoil = { -DeltaV.x * 0.15f, -DeltaV.y * 0.15f, 0.3f };
	KickIsland( *S, RagdollIslandRoot( *S, GRagdollBoneParent[Body] ), Recoil, 1.5f );

	debugf( TEXT("RagdollPkg: dismember %s body=%i dv=%.1f m/s"), GetName(), Body, Min( Spd, MaxSpd ) );
	*(DWORD*)Result = 1;
	unguard;
}

// RemovePart(Body): cut the subtree off AND delete it (faces + bodies). Used
// for decapitation, where the stock UTHeads chunk is the visible head.
void ARagdollCarcass::execRemovePart( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execRemovePart);
	P_GET_INT( Body );
	P_FINISH;
	*(DWORD*)Result = 0;

	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt || Body <= 0 || Body >= S->NumBodies || !S->Alive[Body] )
		return;
	DetachBody( *S, Body );
	RagdollSplitIslands( *S );
	RagdollRemoveIsland( *S, Body );
	debugf( TEXT("RagdollPkg: removed part %s body=%i"), GetName(), Body );
	*(DWORD*)Result = 1;
	unguard;
}

// Gib(Damage, HitLocation): blow the corpse apart. How MANY joints go scales
// with Damage (plus a random factor), and the joints nearest the hit point go
// first, so a glancing rocket tears two or three parts off while a point-blank
// overkill scatters all twelve bodies. Every island is blasted radially away
// from the hit (the trunk, being heavy, only a little), speed growing with
// Damage, and the skin is split into one closed island per rigid group.
// Returns the number of joints still intact (0 = fully gibbed), so script can
// let later hits keep gibbing a partially destroyed corpse.
void ARagdollCarcass::execGib( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execGib);
	P_GET_INT( Damage );
	P_GET_VECTOR( HitLocation );
	P_FINISH;
	*(INT*)Result = 0;

	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt )
		return;

	// Hit point: the given location when it lies near the corpse, else the
	// pelvis (gibbing kills pass the carcass location).
	b3Vec3 Hit = { HitLocation.X * RAGDOLL_U2M, HitLocation.Y * RAGDOLL_U2M, HitLocation.Z * RAGDOLL_U2M };
	{
		b3WorldTransform T0 = b3Body_GetTransform( S->Bodies[BONE_Pelvis] );
		FLOAT dx = Hit.x - T0.p.x, dy = Hit.y - T0.p.y, dz = Hit.z - T0.p.z;
		if( HitLocation.IsZero() || dx*dx + dy*dy + dz*dz > 4.0f )
			Hit = T0.p;
	}

	// Blast size (2026-10-10 user feedback: "gibbing way too easily"):
	//  - a TOTAL gib (everything comes apart, torso halves included) only at
	//    GibTotalDamage (250: redeemer, crushers, telefrags);
	//  - otherwise ONE part mostly, two rarely, three very rarely, the odds
	//    growing with damage (at 80: 25% / 5%; at 160: 50% / 10%);
	//  - below GibWholeLimbDamage (90) only the distal parts come off
	//    (forearm/shin/head stand in for "hand/foot": the rig has no hand or
	//    foot bodies), above it a whole arm or leg may go;
	//  - the spine and neck joints never split short of a total gib.
	const UBOOL bTotal = ( Damage >= 250 );

	// Candidates: live bodies whose joint is still intact, ordered by their
	// (jittered) distance to the hit so the blast eats the corpse from the
	// impact outward rather than popping random far limbs.
	INT   Cand[RAGDOLL_BONE_COUNT];
	FLOAT Score[RAGDOLL_BONE_COUNT];
	INT   NumCand = 0;
	for( INT b=1; b<S->NumBodies; b++ )
	{
		if( !S->Alive[b] || S->Cut[b] )
			continue;
		if( !bTotal && ( b == BONE_Spine || b == BONE_Neck ) )
			continue;
		b3WorldTransform T = b3Body_GetTransform( S->Bodies[b] );
		FLOAT dx = T.p.x - Hit.x, dy = T.p.y - Hit.y, dz = T.p.z - Hit.z;
		Cand[NumCand]  = b;
		Score[NumCand] = sqrt( dx*dx + dy*dy + dz*dz ) * ( 0.6f + appFrand() * 0.8f );
		NumCand++;
	}
	if( NumCand == 0 )
		return;
	for( INT i=1; i<NumCand; i++ )          // insertion sort, 11 entries
		for( INT j=i; j>0 && Score[j] < Score[j-1]; j-- )
		{
			Exchange( Score[j], Score[j-1] );
			Exchange( Cand[j],  Cand[j-1] );
		}

	INT Want = 1;
	if( bTotal )
		Want = NumCand;
	else
	{
		const FLOAT K = ClampF( Damage / 80.0f, 0.4f, 2.5f );
		const FLOAT R = appFrand();
		if( R < 0.05f * K )
			Want = 3;
		else if( R < 0.30f * K )
			Want = 2;
	}
	Want = Min( Want, NumCand );
	const UBOOL bWholeLimb = bTotal || ( Damage >= 90 );

	INT NumCut = 0;
	UBOOL CutNow[RAGDOLL_BONE_COUNT];
	for( INT b=0; b<RAGDOLL_BONE_COUNT; b++ )
		CutNow[b] = 0;
	for( INT i=0; i<NumCand && NumCut<Want; i++ )
	{
		INT b = Cand[i];
		if( !bWholeLimb )
		{
			// Light blast: take the forearm/shin instead of the whole limb.
			INT Child = -1;
			if( b == BONE_UpperArmL ) Child = BONE_ForearmL;
			if( b == BONE_UpperArmR ) Child = BONE_ForearmR;
			if( b == BONE_ThighL )    Child = BONE_ShinL;
			if( b == BONE_ThighR )    Child = BONE_ShinR;
			if( Child >= 0 && Child < S->NumBodies && S->Alive[Child] && !S->Cut[Child] )
				b = Child;
			else if( Child >= 0 )
				continue;   // distal part already gone: the limb stays
		}
		if( CutNow[b] )
			continue;
		if( DetachBody( *S, b ) )
		{
			CutNow[b] = 1;
			NumCut++;
		}
	}
	RagdollSplitIslands( *S );

	// Fragmentation (bFragmentBodies, default OFF): bodies torn off by THIS
	// blast shatter further - the head into 3 chunks, the torso into 3 (4 on
	// heavy damage), a thigh into 2 on heavy damage - and the pelvis splits
	// in two once most of the corpse is gone. Chunk 0 keeps the original
	// body, so joints and the actor location (pelvis) stay valid.
	if( S->bFragmentBodies )
	{
		const FLOAT DS = ( DrawScale > 0.0f ) ? DrawScale : 1.0f;
		INT Frag = 0;
		if( CutNow[BONE_Head] && S->Alive[BONE_Head] )
			Frag += RagdollFragmentBody( *S, BONE_Head, 3, DS );
		if( CutNow[BONE_Spine] )
			Frag += RagdollFragmentBody( *S, BONE_Spine, Damage >= 150 ? 4 : 3, DS );
		if( bTotal && NumCut >= NumCand / 2 )
			Frag += RagdollFragmentBody( *S, BONE_Pelvis, 2, DS );
		if( Damage >= 120 )
		{
			if( CutNow[BONE_ThighL] && appFrand() < 0.5f )
				Frag += RagdollFragmentBody( *S, BONE_ThighL, 2, DS );
			if( CutNow[BONE_ThighR] && appFrand() < 0.5f )
				Frag += RagdollFragmentBody( *S, BONE_ThighR, 2, DS );
		}
		if( Frag > 0 )
			RagdollSplitIslands( *S );
	}

	const FLOAT Base = 4.0f + Min( Max( Damage, 0 ), 250 ) * 0.03f;   // 4..11.5 m/s
	for( INT b=0; b<RagdollNumBones( *S ); b++ )
	{
		if( !RagdollBoneAlive( *S, b ) )
			continue;
		// The trunk (whatever is still joined to the pelvis) is heavy and
		// stays roughly put; detached islands and fragments fly.
		const FLOAT Weight = ( RagdollIslandRoot( *S, b ) == BONE_Pelvis ) ? 0.35f : 1.0f;
		const b3BodyId Bid = RagdollBody( *S, b );
		b3WorldTransform T = b3Body_GetTransform( Bid );
		b3Vec3 Dir = { T.p.x - Hit.x, T.p.y - Hit.y, T.p.z - Hit.z };
		FLOAT L = sqrt( Dir.x*Dir.x + Dir.y*Dir.y + Dir.z*Dir.z );
		if( L < 0.05f )
		{
			const FLOAT A = appFrand() * 2.0f * PI;
			Dir.x = appCos( A ); Dir.y = appSin( A ); Dir.z = 0.3f;
			L = sqrt( Dir.x*Dir.x + Dir.y*Dir.y + Dir.z*Dir.z );
		}
		Dir.x /= L; Dir.y /= L; Dir.z /= L;
		Dir.z += 0.45f;   // bias upward so parts arc instead of skidding
		const FLOAT Spd = Base * ( 0.7f + appFrand() * 0.6f ) * Weight;
		b3Vec3 DeltaV = { Dir.x * Spd, Dir.y * Spd, Dir.z * Spd };
		b3Vec3 V = b3Body_GetLinearVelocity( Bid );
		V.x += DeltaV.x; V.y += DeltaV.y; V.z += DeltaV.z;
		b3Body_SetLinearVelocity( Bid, V );
		b3Vec3 W;
		W.x = ( appFrand() * 2.0f - 1.0f ) * 9.0f * Weight;
		W.y = ( appFrand() * 2.0f - 1.0f ) * 9.0f * Weight;
		W.z = ( appFrand() * 2.0f - 1.0f ) * 9.0f * Weight;
		b3Body_SetAngularVelocity( Bid, W );
	}
	const INT Remaining = NumCand - NumCut;
	*(INT*)Result = Remaining;
	debugf( TEXT("RagdollPkg: gib %s dmg=%i want=%i cut=%i/%i wholelimb=%i total=%i remaining=%i fragments=%i base=%.1f m/s"), GetName(), Damage, Want, NumCut, NumCand, bWholeLimb, bTotal, Remaining, S->NumExtra, Base );
	unguard;
}

// GetBodyLocation(Body): world position (UU) of a body's centre; the actor
// location when the body does not exist.
void ARagdollCarcass::execGetBodyLocation( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execGetBodyLocation);
	P_GET_INT( Body );
	P_FINISH;
	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt || Body < 0 || Body >= S->NumBodies || !S->Alive[Body] )
	{
		*(FVector*)Result = Location;
		return;
	}
	b3WorldTransform T = b3Body_GetTransform( S->Bodies[Body] );
	*(FVector*)Result = FVector( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
	unguard;
}

// NearestBody(Loc): index of the live body closest to a world point, -1 none.
void ARagdollCarcass::execNearestBody( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execNearestBody);
	P_GET_VECTOR( Loc );
	P_FINISH;
	*(INT*)Result = NearestBodyImpl( GetRagdollStates().Find( this ), Loc );
	unguard;
}

// BodyAttached(Body): true while the body exists and is still joined (through
// uncut joints) to the pelvis.
void ARagdollCarcass::execBodyAttached( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execBodyAttached);
	P_GET_INT( Body );
	P_FINISH;
	FRagdollState* S = GetRagdollStates().Find( this );
	*(DWORD*)Result = ( S && S->bBuilt && Body >= 0 && Body < S->NumBodies && S->Alive[Body]
		&& RagdollIslandRoot( *S, Body ) == BONE_Pelvis ) ? 1 : 0;
	unguard;
}

// PaintWound(HitLocation, Dir, Damage, DamageType): persistent wound mark in
// the corpse's skin where the shot (HitLocation along Dir, usually the hit
// momentum) meets the skin; Dir = 0 marks the face nearest HitLocation.
void ARagdollCarcass::execPaintWound( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execPaintWound);
	P_GET_VECTOR( HitLocation );
	P_GET_VECTOR( Dir );
	P_GET_INT( Damage );
	P_GET_NAME( DamageType );
	P_FINISH;
	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt )
		return;
	RagdollPaintWound( *S, this, HitLocation, Dir, Damage, *DamageType );
	unguard;
}

// Register ARagdollCarcass.
IMPLEMENT_CLASS( ARagdollCarcass );

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
