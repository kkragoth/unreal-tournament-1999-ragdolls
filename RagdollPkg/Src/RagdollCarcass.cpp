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

// Segment masses of a ~78 kg adult (Dempster fractions: trunk 50%, thigh
// 10%, shank+foot 6%, upper arm 3%, forearm+hand 2.5%, head+neck 8%). The
// 2026-10-10 datasets carried 52 kg with an 18 kg trunk, so a limb hit
// dragged the torso along and corpse-on-corpse stacks were springy. Used
// for every skeleton (dataset MassKg ignored); scaled by BodyMassScale.
const FLOAT GRagdollBodyMassKg[RAGDOLL_BONE_COUNT] =
{
	11.5f,   // BONE_Pelvis
	21.0f,   // BONE_Spine (thorax + abdomen)
	 1.5f,   // BONE_Neck
	 5.0f,   // BONE_Head
	 2.6f,   // BONE_UpperArmL
	 2.6f,   // BONE_UpperArmR
	 1.9f,   // BONE_ForearmL (+ hand)
	 1.9f,   // BONE_ForearmR
	10.5f,   // BONE_ThighL
	10.5f,   // BONE_ThighR
	 4.8f,   // BONE_ShinL (+ foot on real meshes)
	 4.8f,   // BONE_ShinR
	 1.0f,   // BONE_FootL (box mannequins only)
	 1.0f    // BONE_FootR
};

// Cutting damage (UT damage x the type's cut power) at which a body comes
// off. A direct rocket (75 at the blast, cut power 1) takes a whole leg; an
// enforcer bullet (17 x 0.5) needs ~5 hits on the same forearm; the sniper
// (45 x 0.5) two. Pelvis and spine only go in a total gib.
const FLOAT GRagdollSeverThreshold[RAGDOLL_BONE_COUNT] =
{
	1e9f,   // BONE_Pelvis
	1e9f,   // BONE_Spine
	50.0f,  // BONE_Neck  (decapitation)
	50.0f,  // BONE_Head  (decapitation)
	60.0f,  // BONE_UpperArmL
	60.0f,  // BONE_UpperArmR
	40.0f,  // BONE_ForearmL
	40.0f,  // BONE_ForearmR
	65.0f,  // BONE_ThighL
	65.0f,  // BONE_ThighR
	50.0f,  // BONE_ShinL
	50.0f,  // BONE_ShinR
	30.0f,  // BONE_FootL
	30.0f   // BONE_FootR
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
	// 2026-10-11 (user: "not a fan how wide the legs are spread"): the hips
	// are JK_Pivot, a hinge with a cone of play. Flexion -20..+110 deg about
	// the lateral axis, 32 deg of play (abduction + rotation combined), spring
	// rest at 10 deg of flexion. The old isotropic 50 deg cone let each leg
	// abduct ~62 deg (= the splits) because a circular cone cannot express
	// flexion 110 / abduction 40 at the same time.
	{ JK_Pivot,  0.56f, -0.35f,  1.92f, 0.5f,   1.00f, 0.00f,  0.00f, 0.17f },  // hip L
	{ JK_Pivot,  0.56f, -0.35f,  1.92f, 0.5f,   1.00f, 0.00f,  0.00f, 0.17f },  // hip R
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
// Joint CONSTRAINT softness (Box3D soft step, per joint): how hard the
// limits and the socket hold. Box3D's default (60 Hz, ratio 2) let knees sit
// 20 deg past their stop and the hip play cone yield 8-10 deg once gravity
// was raised to UT's 950 UU/s^2 (2026-10-11); 120 Hz / 5 needs the
// 8-substep solve (SimSubSteps) to stay stable.
static FLOAT GJointConstraintHertz   = 120.0f;
static FLOAT GJointConstraintDamping = 5.0f;

static FLOAT ClampF( FLOAT V, FLOAT Lo, FLOAT Hi );   // defined with ApplyDeathPose
static FLOAT ClampAngle( FLOAT A )
{
	const FLOAT Lim = 0.98f * PI;
	return A > Lim ? Lim : ( A < -Lim ? -Lim : A );
}

static void AddJoint( FRagdollState& S, INT BodyA, INT BodyB, const FRagdollBoneDef* Defs, const FVector& AnchorCanonical, FLOAT DrawScale, const FRagdollJointSpec& Spec, UBOOL bCollide=0 )
{
	S.JointKind[BodyB] = Spec.Kind;
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
		J.base.constraintHertz        = GJointConstraintHertz;
		J.base.constraintDampingRatio = GJointConstraintDamping;
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
	else if( Spec.Kind == JK_Pivot )
	{
		// Hinge with a cone of play (hips). Both joint frames put their z axis
		// on the hinge axis (bind space, +X lateral), so Box3D's TWIST limit
		// carries the asymmetric flexion range L1..L2 and its CONE limit L0
		// bounds the lateral play (abduction and long-axis rotation combined).
		// A positive twist about bind +X swings the hanging limb forward (+Y)
		// on both sides. The bind pose is the straight limb (relative rotation
		// identity); the spring rests at Spec.Rest of flexion.
		const b3Vec3 Z    = { 0.0f, 0.0f, 1.0f };
		const b3Vec3 Axis = b3Normalize( b3Vec3{ Spec.AX, Spec.AY, Spec.AZ } );
		const b3Quat QA   = RagdollShortestArc( Z, Axis );
		FrameA.q = QA;
		FrameB.q = QA;

		b3SphericalJointDef J = b3DefaultSphericalJointDef();
		J.base.bodyIdA       = S.Bodies[BodyA];
		J.base.bodyIdB       = S.Bodies[BodyB];
		J.base.localFrameA   = FrameA;
		J.base.localFrameB   = FrameB;
		J.base.collideConnected = bCollide ? true : false;
		J.base.constraintHertz        = GJointConstraintHertz;
		J.base.constraintDampingRatio = GJointConstraintDamping;
		J.enableConeLimit    = true;
		J.coneAngle          = Min( Spec.L0 * GJointLimitScale, 0.5f * PI );
		J.enableTwistLimit   = true;
		J.lowerTwistAngle    = ClampAngle( Spec.L1 * GJointLimitScale );
		J.upperTwistAngle    = ClampAngle( Spec.L2 * GJointLimitScale );
		J.enableSpring       = ( GJointSpringHertz > 0.0f );
		J.hertz              = GJointSpringHertz;
		J.dampingRatio       = GJointSpringDamping;
		J.targetRotation     = b3MakeQuatFromAxisAngle( Z, ClampF( Spec.Rest, J.lowerTwistAngle, J.upperTwistAngle ) );
		J.enableMotor        = true;
		J.maxMotorTorque     = Spec.Friction * GJointFrictionScale;
		J.motorVelocity      = b3Vec3_zero;
		S.Joints[BodyB] = b3CreateSphericalJoint( S.World, &J );
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
		J.base.constraintHertz        = GJointConstraintHertz;
		J.base.constraintDampingRatio = GJointConstraintDamping;
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
	b3ShapeId Sid = b3CreateHullShape( S.Bodies[Body], &Sd, &Hull.base );
	RagdollRecordHull( S, Body, Sid, C, E, Sd.density );
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
		if( !b3Joint_IsValid( S.Joints[j] ) || JLen > 400 )
			continue;
		const UBOOL bHinge = ( S.JointKind[j] == JK_Hinge );
		if( S.JointKind[j] == JK_Pivot )
		{
			// Pivot: "f<flexion>[lo,hi],p<play>/<limit>" (flexion rides the
			// twist limit, the play cone bounds abduction + rotation).
			const FLOAT Fl   = b3SphericalJoint_GetTwistAngle( S.Joints[j] ) * R2D;
			const FLOAT FLo  = b3SphericalJoint_GetLowerTwistLimit( S.Joints[j] ) * R2D;
			const FLOAT FHi  = b3SphericalJoint_GetUpperTwistLimit( S.Joints[j] ) * R2D;
			const FLOAT Play = b3SphericalJoint_GetConeAngle( S.Joints[j] ) * R2D;
			const FLOAT PLim = b3SphericalJoint_GetConeLimit( S.Joints[j] ) * R2D;
			const UBOOL bAt = ( Fl - FLo < 2.0f ) || ( FHi - Fl < 2.0f ) || ( PLim - Play < 2.0f );
			JLen += appSprintf( JL + JLen, TEXT(" %s:f%.0f[%.0f,%.0f],p%.0f/%.0f%s"), JName[j], Fl, FLo, FHi, Play, PLim, bAt ? TEXT("!") : TEXT("") );
		}
		else if( bHinge )
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
	// Limb spread: the angle between the two thighs' long axes and between
	// the two upper arms' (degrees). The user's "legs spread too wide"
	// complaint in one number: a relaxed corpse keeps the legs within ~30 deg.
	FLOAT LegSpread = -1.0f, ArmSpread = -1.0f;
	for( INT p=0; p<2; p++ )
	{
		const INT A = p ? BONE_UpperArmL : BONE_ThighL;
		const INT B = p ? BONE_UpperArmR : BONE_ThighR;
		if( !S.Alive[A] || !S.Alive[B] || !b3Body_IsValid( S.Bodies[A] ) || !b3Body_IsValid( S.Bodies[B] ) )
			continue;
		const b3Vec3 DA = b3RotateVector( b3Body_GetRotation( S.Bodies[A] ), b3Vec3{ 0.0f, 0.0f, -1.0f } );
		const b3Vec3 DB = b3RotateVector( b3Body_GetRotation( S.Bodies[B] ), b3Vec3{ 0.0f, 0.0f, -1.0f } );
		const FLOAT Deg = appAcos( ClampF( b3Dot( DA, DB ), -1.0f, 1.0f ) ) * R2D;
		if( p ) ArmSpread = Deg; else LegSpread = Deg;
	}
	debugf( TEXT("RagdollPkg: jointangles %s sync=%i legspread=%.0f armspread=%.0f%s"), C->GetName(), S.SyncCount, LegSpread, ArmSpread, JL );
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

// Forward kinematics over the bone chain (parents always precede children in
// the bone enum): place every body from the root (pelvis) world transform and
// the per-joint LOCAL rotations (child relative to its parent, bind frame),
// so the joint frames coincide exactly at their anchors whatever the pose.
// Positions in UU; shared by the random spawn pose and the animation fit.
static void RagdollPoseFK( FRagdollState& S, const FRagdollBoneDef* Defs, const FVector* Anch, FLOAT DrawScale, const b3Quat* Local, const b3Quat& RootQ, const b3Vec3& RootCenterUU )
{
	b3Vec3 WP[RAGDOLL_BONE_COUNT];
	b3Quat WQ[RAGDOLL_BONE_COUNT];
	for( INT i=0; i<S.NumBodies; i++ )
	{
		const FRagdollBoneDef& D = Defs[i];
		const b3Vec3 Ci = { DrawScale * D.CX, DrawScale * D.CY, DrawScale * D.CZ };
		const FPoseLink& L = GPoseChain[i];
		if( L.Parent < 0 )
		{
			WP[i] = RootCenterUU;
			WQ[i] = RootQ;
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
		b3Body_SetTransform( S.Bodies[i], { WP[i].x * RAGDOLL_U2M, WP[i].y * RAGDOLL_U2M, WP[i].z * RAGDOLL_U2M }, WQ[i] );
	}
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
			ArmAbd[s]  = Var * RandRange( 0.0f, 0.70f );
			ArmFlex[s] = ClampF( 0.35f + Var * RandRange( -0.25f, 1.20f ), 0.05f, 1.80f );
		}
	}
	Local[BONE_UpperArmL] = Swing( ArmTilt[0],  ArmAbd[0] );
	Local[BONE_UpperArmR] = Swing( ArmTilt[1], -ArmAbd[1] );
	Local[BONE_ForearmL]  = RotX( ArmFlex[0] );   // elbow hinge: flex is +X
	Local[BONE_ForearmR]  = RotX( ArmFlex[1] );

	// Legs. The two legs move TOGETHER: a shared swing and a shared (small,
	// mostly outward) spread with a little per-leg difference, and a knee that
	// flexes more when its hip does. Up to 2026-10-11 each leg drew its own
	// 0..23 deg outward spread and its own swing, so the corpse started in a
	// straddle and the spin took it the rest of the way to the splits. Knees:
	// hinge range is -1.75..0.05, flexion is a NEGATIVE X rotation (the shin
	// folds backward). Abduction is bounded by the hip's play cone anyway.
	FLOAT LegTilt[2], LegAbd[2], KneeFlex[2];
	const FLOAT TiltBase = Var * RandRange( -0.15f, 0.55f );
	const FLOAT AbdBase  = Var * RandRange( -0.05f, 0.18f );
	for( INT s=0; s<2; s++ )
	{
		LegTilt[s]  = ClampF( TiltBase + Var * RandRange( -0.25f, 0.25f ), -0.30f, 0.90f );
		LegAbd[s]   = ClampF( AbdBase  + Var * RandRange( -0.10f, 0.10f ), -0.12f, 0.30f );
		KneeFlex[s] = -ClampF( 0.5f * Max( LegTilt[s], 0.0f ) + Var * RandRange( 0.00f, 0.45f ), 0.0f, 0.90f );
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

	// Forward kinematics from the pelvis, yawed to the pawn: the root is the
	// pelvis centre rotated by QYaw about the corpse origin.
	const FRagdollBoneDef& D0 = Defs[BONE_Pelvis];
	const b3Vec3 C0 = b3RotateVector( QYaw, b3Vec3{ DrawScale * D0.CX, DrawScale * D0.CY, DrawScale * D0.CZ } );
	RagdollPoseFK( S, Defs, Anch, DrawScale, Local, QYaw, b3Vec3{ Origin.X + C0.x, Origin.Y + C0.y, Origin.Z + C0.z } );
}

/*-----------------------------------------------------------------------------
	Spawn pose from the dying pawn's animation frame.
-----------------------------------------------------------------------------*/

// Largest eigenpair of a symmetric 4x4 (cyclic Jacobi rotations). A is
// destroyed; Out receives the unit eigenvector, the eigenvalue is returned.
static FLOAT RagdollJacobi4Max( FLOAT A[4][4], FLOAT Out[4] )
{
	FLOAT V[4][4];
	for( INT i=0; i<4; i++ )
		for( INT j=0; j<4; j++ )
			V[i][j] = ( i == j ) ? 1.0f : 0.0f;
	for( INT Sweep=0; Sweep<60; Sweep++ )
	{
		FLOAT Off = 0.0f;
		for( INT i=0; i<4; i++ )
			for( INT j=0; j<4; j++ )
				if( i != j )
					Off += A[i][j] * A[i][j];
		if( Off < 1e-10f )
			break;
		for( INT p=0; p<4; p++ )
		{
			for( INT q=p+1; q<4; q++ )
			{
				if( Abs( A[p][q] ) < 1e-9f )
					continue;
				const FLOAT Theta = ( A[q][q] - A[p][p] ) / ( 2.0f * A[p][q] );
				const FLOAT t = ( Theta >= 0.0f ? 1.0f : -1.0f ) / ( Abs( Theta ) + appSqrt( Theta * Theta + 1.0f ) );
				const FLOAT c = 1.0f / appSqrt( t * t + 1.0f );
				const FLOAT s = t * c;
				for( INT k=0; k<4; k++ ) { const FLOAT akp = A[k][p], akq = A[k][q]; A[k][p] = c*akp - s*akq; A[k][q] = s*akp + c*akq; }
				for( INT k=0; k<4; k++ ) { const FLOAT apk = A[p][k], aqk = A[q][k]; A[p][k] = c*apk - s*aqk; A[q][k] = s*apk + c*aqk; }
				for( INT k=0; k<4; k++ ) { const FLOAT vkp = V[k][p], vkq = V[k][q]; V[k][p] = c*vkp - s*vkq; V[k][q] = s*vkp + c*vkq; }
			}
		}
	}
	INT Best = 0;
	for( INT i=1; i<4; i++ )
		if( A[i][i] > A[Best][Best] )
			Best = i;
	for( INT k=0; k<4; k++ )
		Out[k] = V[k][Best];
	return A[Best][Best];
}

// Weighted rigid fit of point pairs (Horn 1987, unit quaternion): the rotation
// Q and translation T with Q*P + T ~ X. Raw moments are accumulated so a
// body's vertices are visited once; the residual comes from the largest
// eigenvalue (sum w|x'|^2 + sum w|p'|^2 - 2 lambda over the centred points).
struct FRagdollRigidFit
{
	INT    N;
	FLOAT  W;
	b3Vec3 SP, SX;
	FLOAT  SPP, SXX;
	FLOAT  M[3][3];   // sum w * p_i * x_j

	FRagdollRigidFit() : N( 0 ), W( 0.0f ), SP( b3Vec3_zero ), SX( b3Vec3_zero ), SPP( 0.0f ), SXX( 0.0f )
	{
		for( INT i=0; i<3; i++ )
			for( INT j=0; j<3; j++ )
				M[i][j] = 0.0f;
	}
	void Add( const b3Vec3& P, const b3Vec3& X, FLOAT w )
	{
		const FLOAT p[3] = { P.x, P.y, P.z }, x[3] = { X.x, X.y, X.z };
		N++;
		W += w;
		SP = b3Add( SP, b3MulSV( w, P ) );
		SX = b3Add( SX, b3MulSV( w, X ) );
		SPP += w * b3Dot( P, P );
		SXX += w * b3Dot( X, X );
		for( INT i=0; i<3; i++ )
			for( INT j=0; j<3; j++ )
				M[i][j] += w * p[i] * x[j];
	}
	// Returns the weighted RMS residual, or -1 when under-determined.
	FLOAT Solve( b3Quat& Q, b3Vec3& T ) const
	{
		Q = b3Quat_identity;
		T = b3Vec3_zero;
		if( N < 3 || W <= 1e-6f )
			return -1.0f;
		const b3Vec3 CP = b3MulSV( 1.0f / W, SP );
		const b3Vec3 CX = b3MulSV( 1.0f / W, SX );
		const FLOAT cp[3] = { CP.x, CP.y, CP.z }, cx[3] = { CX.x, CX.y, CX.z };
		FLOAT S[3][3];
		for( INT i=0; i<3; i++ )
			for( INT j=0; j<3; j++ )
				S[i][j] = M[i][j] - W * cp[i] * cx[j];
		const FLOAT Sxx = S[0][0], Sxy = S[0][1], Sxz = S[0][2];
		const FLOAT Syx = S[1][0], Syy = S[1][1], Syz = S[1][2];
		const FLOAT Szx = S[2][0], Szy = S[2][1], Szz = S[2][2];
		FLOAT Nm[4][4] =
		{
			{ Sxx + Syy + Szz, Syz - Szy,        Szx - Sxz,        Sxy - Syx        },
			{ Syz - Szy,       Sxx - Syy - Szz,  Sxy + Syx,        Szx + Sxz        },
			{ Szx - Sxz,       Sxy + Syx,       -Sxx + Syy - Szz,  Syz + Szy        },
			{ Sxy - Syx,       Szx + Sxz,        Syz + Szy,       -Sxx - Syy + Szz  },
		};
		FLOAT E[4];
		const FLOAT Lambda = RagdollJacobi4Max( Nm, E );
		const FLOAT Len = appSqrt( E[0]*E[0] + E[1]*E[1] + E[2]*E[2] + E[3]*E[3] );
		if( Len < 1e-6f )
			return -1.0f;
		Q.v.x = E[1] / Len; Q.v.y = E[2] / Len; Q.v.z = E[3] / Len; Q.s = E[0] / Len;
		T = b3Sub( CX, b3RotateVector( Q, CP ) );
		const FLOAT Pc  = SPP - W * b3Dot( CP, CP );
		const FLOAT Xc  = SXX - W * b3Dot( CX, CX );
		const FLOAT Err = Max( Pc + Xc - 2.0f * Lambda, 0.0f );
		return appSqrt( Err / W );
	}
};

// Spawn pose from the dying pawn's animation frame (2026-10-11, user: "the
// pawn has a frozen pose, then the ragdoll spawns in a different position").
// The pawn is ~0.2 s into its Dead* animation when the carcass is made
// (Botpack Dying: Sleep(0.2); SpawnCarcass()), still mostly upright, and the
// corpse should continue from exactly that frame instead of the random pose.
// The pawn's ULodMesh is a vertex animation (one position per vertex per
// frame) of the SAME model the dataset was derived from (the Selection viewer
// mesh, re-exported with another vertex numbering: RagdollLiveMaps.h holds
// the permutation), so every live vertex has a dataset bone bind. Per body: a
// weighted rigid fit (Horn) of the bind positions of its vertices onto their
// world positions in the pawn's current frame (blended between the two
// keyframes like the renderer), then forward kinematics from the fitted
// pelvis with the fitted RELATIVE rotations so the joint frames coincide
// exactly. Hinges keep only their axis component, clamped to the limits; a
// body with too few vertices, a poor fit (the mesh squashes mid-fall) or an
// impossible relative rotation follows its parent. Returns false (random
// pose fallback) without a dataset / map / sequence or when the pelvis fails.
static UBOOL ApplyAnimationPose( FRagdollState& S, ARagdollCarcass* C, const FRagdollDatasetEntry* DS, ULodMesh* LiveMesh, const FVector* Anch, const FRagdollJointSpec* Spec )
{
	guard(ApplyAnimationPose);
	if( !DS || !LiveMesh || !RagdollBoolProp( C, TEXT("bAnimPose"), 1 ) )
		return 0;
	const FRagdollLiveMap* LM = FindRagdollLiveMap( LiveMesh->GetName() );
	const INT ModelVerts = LiveMesh->FrameVerts - LiveMesh->SpecialVerts;
	if( !LM || LM->NumVerts != ModelVerts || LM->NumVerts > DS->NumVerts )
	{
		debugf( TEXT("RagdollPkg: animpose %s skipped: map=%s mesh=%s verts=%i/%i"), C->GetName(), LM ? TEXT("ok") : TEXT("none"), LiveMesh->GetName(), ModelVerts, LM ? LM->NumVerts : 0 );
		return 0;
	}
	const FMeshAnimSeq* Seq = LiveMesh->GetAnimSeq( C->AnimSequence );
	if( !Seq || Seq->NumFrames <= 0 )
	{
		debugf( TEXT("RagdollPkg: animpose %s skipped: no sequence '%s' on %s"), C->GetName(), *C->AnimSequence, LiveMesh->GetName() );
		return 0;
	}
	// Keyframes: the renderer shows frame StartFrame + AnimFrame*NumFrames
	// blended with the next one of the sequence; a negative AnimFrame is a
	// tween INTO the first frame (the previous pose is not available here).
	FLOAT F = C->AnimFrame;
	if( F < 0.0f )
		F = 0.0f;
	if( F > 0.9999f )
		F = 0.9999f;
	const FLOAT Pos   = F * Seq->NumFrames;
	const INT   Rel0  = appFloor( Pos );
	const INT   Rel1  = Min( Rel0 + 1, Seq->NumFrames - 1 );
	const FLOAT Alpha = Pos - Rel0;
	const INT   F0 = Seq->StartFrame + Rel0;
	const INT   F1 = Seq->StartFrame + Rel1;
	LiveMesh->Verts.Load();
	const INT FV = LiveMesh->FrameVerts;
	if( F0 < 0 || ( F1 + 1 ) * FV > LiveMesh->Verts.Num() )
	{
		debugf( TEXT("RagdollPkg: animpose %s skipped: frame %i past %i verts"), C->GetName(), F1, LiveMesh->Verts.Num() );
		return 0;
	}
	// Model -> world, the chain UMesh::GetFrame builds for the renderer with
	// an identity camera: (V - Origin) through Location/Rotation/RotOrigin/Scale.
	const FLOAT DrawScale = C->DrawScale > 0.0f ? C->DrawScale : 1.0f;
	const FCoords ToWorld = GMath.UnitCoords * ( C->Location + C->PrePivot ) * C->Rotation * LiveMesh->RotOrigin * FScale( LiveMesh->Scale * DrawScale, 0.0f, SHEER_None );

	FRagdollRigidFit Fit[RAGDOLL_BONE_COUNT];
	for( INT k=0; k<ModelVerts; k++ )
	{
		const INT d = LM->Map[k];
		if( d < 0 || d >= DS->NumVerts )
			continue;
		const FVector V0 = LiveMesh->Verts( F0 * FV + LiveMesh->SpecialVerts + k ).Vector();
		const FVector V1 = LiveMesh->Verts( F1 * FV + LiveMesh->SpecialVerts + k ).Vector();
		const FVector Wd = ( V0 + ( V1 - V0 ) * Alpha - LiveMesh->Origin ).TransformPointBy( ToWorld );
		const b3Vec3 X = { Wd.X, Wd.Y, Wd.Z };
		for( INT slot=0; slot<3; slot++ )
		{
			const INT   b = DS->BindBones[d*3 + slot];
			const FLOAT w = DS->BindWeights[d*3 + slot];
			if( w < 0.25f || b < 0 || b >= S.NumBodies )
				continue;
			const FRagdollBoneDef& D = DS->Bones[b];
			const FVector& O = DS->BindOffsets[d*3 + slot];
			const b3Vec3 P = { DrawScale * ( D.CX + O.X ), DrawScale * ( D.CY + O.Y ), DrawScale * ( D.CZ + O.Z ) };
			Fit[b].Add( P, X, w );
		}
	}

	// Per-body world transforms. The pelvis must fit; a body with fewer than 4
	// vertices or an RMS above 8 UU (x DrawScale) follows its parent instead.
	const FLOAT MaxRms = 8.0f * DrawScale;
	b3Quat WQ[RAGDOLL_BONE_COUNT];
	b3Vec3 WP[RAGDOLL_BONE_COUNT];
	UBOOL  Ok[RAGDOLL_BONE_COUNT];
	FLOAT  Rms[RAGDOLL_BONE_COUNT];
	FLOAT  RmsSum = 0.0f;
	INT    RmsN = 0;
	for( INT i=0; i<S.NumBodies; i++ )
	{
		b3Vec3 T;
		Rms[i] = Fit[i].Solve( WQ[i], T );
		Ok[i]  = ( Fit[i].N >= 4 && Rms[i] >= 0.0f && Rms[i] <= MaxRms );
		const FRagdollBoneDef& D = DS->Bones[i];
		const b3Vec3 Ci = { DrawScale * D.CX, DrawScale * D.CY, DrawScale * D.CZ };
		WP[i] = b3Add( b3RotateVector( WQ[i], Ci ), T );
		if( Ok[i] )
		{
			RmsSum += Rms[i];
			RmsN++;
		}
	}
	if( !Ok[BONE_Pelvis] )
	{
		debugf( TEXT("RagdollPkg: animpose %s skipped: pelvis fit n=%i rms=%.1f"), C->GetName(), Fit[BONE_Pelvis].N, Rms[BONE_Pelvis] );
		return 0;
	}

	// Relative rotations down the chain against each parent's EFFECTIVE
	// rotation (a failed body inherits its parent's). Hinges (elbows, knees)
	// keep the component about their bind-space axis (+X), clamped to the
	// joint range; a ball/pivot joint more than 150 deg from the bind pose is
	// a mis-fit and follows its parent too.
	b3Quat Local[RAGDOLL_BONE_COUNT];
	b3Quat Eff[RAGDOLL_BONE_COUNT];
	FString Fails;
	for( INT i=0; i<S.NumBodies; i++ )
	{
		Local[i] = b3Quat_identity;
		const INT P = GPoseChain[i].Parent;
		if( P < 0 )
		{
			Eff[i] = WQ[i];
			continue;
		}
		Eff[i] = Eff[P];
		if( !Ok[i] )
		{
			Fails += FString::Printf( TEXT(" %i(n%i,%.0f)"), i, Fit[i].N, Rms[i] );
			continue;
		}
		b3Quat Rel = b3NormalizeQuat( b3InvMulQuat( Eff[P], WQ[i] ) );
		if( Rel.s < 0.0f )
			Rel = b3NegateQuat( Rel );
		const INT Sp = i - 1;   // joint spec of body i (AddJoint order, bodies 1..11)
		if( Sp >= 0 && Sp < RAGDOLL_TUNED_JOINTS && Spec[Sp].Kind == JK_Hinge )
		{
			FLOAT Ang = 2.0f * appAtan2( Rel.v.x, Rel.s );
			if( Ang > PI )
				Ang -= 2.0f * PI;
			if( Ang < -PI )
				Ang += 2.0f * PI;
			const FLOAT Lo = ClampAngle( Spec[Sp].L0 * GJointLimitScale );
			const FLOAT Hi = ClampAngle( Spec[Sp].L1 * GJointLimitScale );
			Ang = ClampF( Ang, Lo, Hi );
			Rel = b3MakeQuatFromAxisAngle( { 1.0f, 0.0f, 0.0f }, Ang );
		}
		else if( Rel.s < 0.2588f )   // cos(150/2 deg)
		{
			Fails += FString::Printf( TEXT(" %i(rot)"), i );
			continue;
		}
		Local[i] = Rel;
		Eff[i]   = b3MulQuat( Eff[P], Rel );
	}

	RagdollPoseFK( S, DS->Bones, Anch, DrawScale, Local, WQ[BONE_Pelvis], WP[BONE_Pelvis] );

	static INT GAnimDbg = 0;
	if( GAnimDbg < 12 )
	{
		GAnimDbg++;
		const b3Vec3 Fw = b3RotateVector( WQ[BONE_Pelvis], { 0.0f, 1.0f, 0.0f } );
		const FLOAT  Yaw = appAtan2( Fw.y, Fw.x ) * 180.0f / PI;
		debugf( TEXT("RagdollPkg: animpose %s seq=%s frame=%.2f (%i+%.2f) rms=%.1f UU bodies=%i/%i pelvis=(%.0f,%.0f,%.0f) loc=(%.0f,%.0f,%.0f) facing=%.0f pawnyaw=%.0f fails=%s"),
			C->GetName(), *C->AnimSequence, C->AnimFrame, F0, Alpha, RmsN ? RmsSum / RmsN : -1.0f, RmsN, S.NumBodies,
			WP[BONE_Pelvis].x, WP[BONE_Pelvis].y, WP[BONE_Pelvis].z, C->Location.X, C->Location.Y, C->Location.Z,
			Yaw, ( C->Rotation.Yaw & 65535 ) * 360.0f / 65536.0f, Fails.Len() ? *Fails : TEXT(" none") );
	}
	return 1;
	unguard;
}

static INT NearestBodyImpl( const FRagdollState* S, const FVector& Loc );

// The body under a shot. The engine reports hits on the corpse's collision
// CYLINDER (grown to the sprawl, so the point can sit 30+ UU off any limb)
// and NearestBody-by-centre then picked whatever limb lay closest to the
// cylinder wall, not what the shot went through. This casts the shot ray
// (HitLocation along the momentum direction, started well before the
// point) against every live skeleton body's hull and returns the first one
// hit, with the hit point (metres) in *PointM. -1 when the ray clears every
// box (a graze on the cylinder); *GrazeBody then holds the body whose
// centre passes nearest the ray and *GrazeDistM that distance.
static INT RagdollRayHitBody( const FRagdollState& S, const FVector& OriginUU, const FVector& DirN, FLOAT LenUU, b3Vec3* PointM, INT* GrazeBody, FLOAT* GrazeDistM )
{
	const b3Vec3 O = { OriginUU.X * RAGDOLL_U2M, OriginUU.Y * RAGDOLL_U2M, OriginUU.Z * RAGDOLL_U2M };
	const b3Vec3 T = { DirN.X * LenUU * RAGDOLL_U2M, DirN.Y * LenUU * RAGDOLL_U2M, DirN.Z * LenUU * RAGDOLL_U2M };
	const b3Vec3 D = { DirN.X, DirN.Y, DirN.Z };
	b3QueryFilter F = b3DefaultQueryFilter();
	INT   Best = -1, Near = -1;
	FLOAT BestF = 2.0f, NearD = 1e9f;
	for( INT i=0; i<S.NumBodies; i++ )
	{
		if( !S.Alive[i] || !b3Body_IsValid( S.Bodies[i] ) )
			continue;
		const b3WorldTransform BT = b3Body_GetTransform( S.Bodies[i] );
		b3BodyCastResult R = b3Body_CastRay( S.Bodies[i], O, T, F, 1.0f, BT );
		if( R.hit && R.fraction < BestF )
		{
			BestF = R.fraction;
			Best  = i;
			if( PointM )
				*PointM = R.point;
		}
		// Distance from the body centre to the ray line (for grazes).
		const b3Vec3 Rel = b3Sub( BT.p, O );
		const FLOAT  Along = b3Dot( Rel, D );
		const b3Vec3 Perp = b3Sub( Rel, b3MulSV( Along, D ) );
		const FLOAT  Dist = b3Length( Perp );
		if( Along > -0.2f && Dist < NearD )
		{
			NearD = Dist;
			Near  = i;
		}
	}
	if( GrazeBody )
		*GrazeBody = Near;
	if( GrazeDistM )
		*GrazeDistM = NearD;
	return Best;
}

// Death velocities, applied once the bodies sit in their posed spawn
// configuration. The corpse starts as ONE moving body: the pawn's velocity
// plus a random tumble about the pelvis, every body's linear velocity being
// V + w x r so the joints have nothing to fight on the first step, with only
// a small per-body jitter on top. Up to 2026-10-11 every body drew its own
// random spin of up to DeathSpin rad/s, which flung the limbs to their
// limits within the first half second ("starfish" corpses, legs in the
// splits). The killing blow then kicks the body it hit, off-centre, so a
// headshot snaps the head back and a chest rocket folds the torso; the
// engine already gave the whole pawn Momentum/Mass (inherited through
// C->Velocity), this is the LOCAL share on top. DeathSpin and
// DeathHitScale are the console knobs.
static void ApplyDeathVelocities( FRagdollState& S, ARagdollCarcass* C, const FVector& DeathVel, FLOAT DeathSpin )
{
	const b3Vec3 P0 = b3Body_GetPosition( S.Bodies[BONE_Pelvis] );

	// Killing blow (RagdollCarcass.RecordKillingBlow fills LastHitLocation /
	// LastHitMomentum / LastHitDamage from the mutator's hit record before
	// BuildRagdoll; LastHitDamage includes the overkill).
	const FVector Mom    = RagdollVectorProp( C, TEXT("LastHitMomentum"), FVector( 0, 0, 0 ) );
	const FVector HitLoc = RagdollVectorProp( C, TEXT("LastHitLocation"), FVector( 0, 0, 0 ) );
	const FLOAT   MomLen = Mom.Size();
	const FLOAT   HitDmg = RagdollFloatProp( C, TEXT("LastHitDamage"), 0.0f );

	// Whole-body tumble (2026-10-11, user: "a little randomness at init,
	// sometimes none, in ~20% of cases a full spin when the damage is way
	// too much"). Three outcomes per corpse:
	//  - none (most corpses: they just drop with the pawn's velocity),
	//  - a mild tumble (DeathSpin x 0.25..0.75 rad/s),
	//  - a full spin: DeathSpin x 1.5..3 rad/s, odds DeathSpinChance (0.2)
	//    scaled by the damage (killing blow + overkill, / 80: an enforcer
	//    kill almost never, a rocket overkill usually), about a random axis
	//    with all three components, biased perpendicular to the shot so the
	//    body goes end over end rather than spinning on the bullet's axis.
	const FLOAT K        = ClampF( HitDmg / 80.0f, 0.0f, 2.5f );
	const FLOAT BigOdds  = ClampF( RagdollFloatProp( C, TEXT("DeathSpinChance"), 0.2f ), 0.0f, 1.0f ) * K;
	const FLOAT MildOdds = 0.30f;
	const FLOAT Roll     = appFrand();
	FLOAT Mag = 0.0f;
	INT   Kind = 0;
	if( Roll < BigOdds )
	{
		Mag  = DeathSpin * ( 1.5f + 1.5f * appFrand() ) * ClampF( K, 0.6f, 1.5f );
		Kind = 2;
	}
	else if( Roll < BigOdds + MildOdds )
	{
		Mag  = DeathSpin * ( 0.25f + 0.5f * appFrand() );
		Kind = 1;
	}
	// Upright spawn (2026-10-11 evening, user: "they only bend a knee and
	// stay upright"): the animation pose is taken 0.2 s into the Dead*
	// sequence, where the pawn is still standing, and a ragdoll standing on
	// the flat ends of its shin boxes with the joint springs pulling toward
	// the bind pose balances there when nothing pushes it. A corpse whose
	// pelvis is more than 0.5 m above its lowest body (standing ~0.9 m,
	// lying < 0.3 m) always gets at least a mild tumble, about a HORIZONTAL
	// axis so it falls over - away from the shooter when there was a shot.
	FLOAT LowZ = P0.z;
	for( INT i=0; i<S.NumBodies; i++ )
		LowZ = Min( LowZ, b3Body_GetPosition( S.Bodies[i] ).z );
	const UBOOL bUpright = ( P0.z - LowZ ) > 0.5f;
	if( bUpright && Kind == 0 )
	{
		Mag  = DeathSpin * ( 0.4f + 0.4f * appFrand() );
		Kind = 1;
	}
	b3Vec3 Axis = { appFrand() * 2.0f - 1.0f, appFrand() * 2.0f - 1.0f, ( appFrand() * 2.0f - 1.0f ) * 0.6f };
	if( MomLen > 1.0f )
	{
		// Mix in a direction perpendicular to the shot: torque = r x J.
		const b3Vec3 D    = { Mom.X / MomLen, Mom.Y / MomLen, Mom.Z / MomLen };
		const b3Vec3 Perp = b3Cross( D, Axis );
		Axis = b3Add( b3MulSV( 0.4f, Axis ), b3MulSV( 0.6f, Perp ) );
	}
	if( bUpright && Kind == 1 )
	{
		// Topple: w = up x D makes the head move along the shot direction.
		b3Vec3 Dh = { 0.0f, 0.0f, 0.0f };
		if( MomLen > 1.0f )
			Dh = { Mom.X / MomLen, Mom.Y / MomLen, 0.0f };
		if( b3Length( Dh ) < 0.05f )
		{
			const FLOAT A = appFrand() * 2.0f * PI;
			Dh = { (FLOAT)appCos( A ), (FLOAT)appSin( A ), 0.0f };
		}
		Dh = b3Normalize( Dh );
		const b3Vec3 Up = { 0.0f, 0.0f, 1.0f };
		Axis = b3Add( b3Cross( Up, Dh ), b3MulSV( 0.2f, Axis ) );
		Axis.z *= 0.2f;
	}
	if( b3Length( Axis ) < 0.01f )
		Axis = { 0.0f, 1.0f, 0.0f };
	const b3Vec3 W = b3MulSV( Mag, b3Normalize( Axis ) );
	const FLOAT Jitter = ( Kind > 0 ? 0.35f : 0.12f ) * DeathSpin;
	static INT GSpinDbg = 0;
	if( GSpinDbg < 12 )
	{
		GSpinDbg++;
		debugf( TEXT("RagdollPkg: deathspin %s kind=%i mag=%.2f rad/s dmg=%.0f odds=%.2f upright=%i rise=%.2f axis=(%.2f,%.2f,%.2f)"), C->GetName(), Kind, Mag, HitDmg, BigOdds, bUpright, P0.z - LowZ, W.x, W.y, W.z );
	}
	for( INT i=0; i<S.NumBodies; i++ )
	{
		const b3Vec3 R = b3Sub( b3Body_GetPosition( S.Bodies[i] ), P0 );
		b3Vec3 V = b3Add( b3Vec3{ DeathVel.X, DeathVel.Y, DeathVel.Z }, b3Cross( W, R ) );
		V.x += ( appFrand() * 2.0f - 1.0f ) * 0.25f;
		V.y += ( appFrand() * 2.0f - 1.0f ) * 0.25f;
		V.z += ( appFrand() * 2.0f - 1.0f ) * 0.15f;
		b3Body_SetLinearVelocity( S.Bodies[i], V );
		b3Vec3 Wi = W;
		Wi.x += ( appFrand() * 2.0f - 1.0f ) * Jitter;
		Wi.y += ( appFrand() * 2.0f - 1.0f ) * Jitter;
		Wi.z += ( appFrand() * 2.0f - 1.0f ) * Jitter;
		b3Body_SetAngularVelocity( S.Bodies[i], Wi );
	}

	// Killing blow: the local kick on the body the shot went through.
	const FLOAT   HitScale = RagdollFloatProp( C, TEXT("DeathHitScale"), 0.8f );
	if( MomLen < 1.0f || HitScale <= 0.0f )
		return;
	// The body the shot went through: cast the shot ray through the posed
	// skeleton (the engine's hit point is on the pawn cylinder); fall back
	// to the nearest body centre when the ray clears every box.
	const FVector DirUU( Mom.X / MomLen, Mom.Y / MomLen, Mom.Z / MomLen );
	b3Vec3 RayPt = b3Vec3_zero;
	INT Graze = -1;
	INT Body = RagdollRayHitBody( S, HitLoc - DirUU * 80.0f, DirUU, 220.0f, &RayPt, &Graze, NULL );
	const UBOOL bRay = ( Body >= 0 );
	if( Body < 0 )
		Body = ( Graze >= 0 ) ? Graze : NearestBodyImpl( &S, HitLoc );
	if( Body < 0 )
		return;
	if( bRay )
		RagdollRecordHit( S, Body, RayPt );   // the killing blow places the cut too
	// Delta-v the pawn took from the shot (UT: Velocity += Momentum / Mass),
	// scaled and capped; the hit body gets it on top of the whole-body push.
	const FLOAT PawnMass = C->Mass > 1.0f ? C->Mass : 100.0f;
	const FLOAT DeltaV   = Min( ( MomLen / PawnMass ) * RAGDOLL_U2M * HitScale, 5.0f );
	const b3Vec3 Dir = { Mom.X / MomLen, Mom.Y / MomLen, Mom.Z / MomLen };
	const b3Vec3 Pb  = b3Body_GetPosition( S.Bodies[Body] );
	// Lever arm: the ray's entry point on the hull when it hit one, else the
	// engine's cylinder point capped at 0.25 m to keep the torque sane.
	b3Vec3 Off = b3Sub( bRay ? RayPt : b3Vec3{ HitLoc.X * RAGDOLL_U2M, HitLoc.Y * RAGDOLL_U2M, HitLoc.Z * RAGDOLL_U2M }, Pb );
	const FLOAT OffLen = b3Length( Off );
	if( OffLen > 0.25f )
		Off = b3MulSV( 0.25f / OffLen, Off );
	const FLOAT BodyMass = b3Body_GetMass( S.Bodies[Body] );
	b3Body_ApplyLinearImpulse( S.Bodies[Body], b3MulSV( DeltaV * BodyMass, Dir ), b3Add( Pb, Off ), true );
	// The neighbours in the chain take a third at their centres so the kick
	// reads as a wave through the body rather than one part snapping away.
	for( INT j=0; j<S.NumBodies; j++ )
	{
		if( j == Body || !( GRagdollBoneParent[j] == Body || GRagdollBoneParent[Body] == j ) )
			continue;
		b3Body_ApplyLinearImpulseToCenter( S.Bodies[j], b3MulSV( DeltaV * 0.33f * b3Body_GetMass( S.Bodies[j] ), Dir ), true );
	}
	static INT GHitDbg = 0;
	if( GHitDbg < 8 )
	{
		GHitDbg++;
		debugf( TEXT("RagdollPkg: deathhit %s body=%i ray=%i dv=%.2f m/s mom=%.0f lever=%.2f"), C->GetName(), Body, bRay, DeltaV, MomLen, Min( OffLen, 0.25f ) );
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
	GRagdollWantMovers = RagdollBoolProp( C, TEXT("bMoverCollision"), 1 );   // read if the world gets built now
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
	// Mass: the segment table (GRagdollBodyMassKg) x BodyMassScale.
	const FLOAT MassScale = ClampF( RagdollFloatProp( C, TEXT("BodyMassScale"), 1.0f ), 0.1f, 10.0f );
	// Gravity: the world runs at UT's default 950 UU/s^2 and the GravityScale
	// knob scales it; the ZONE's gravity (low-grav zones) is applied live per
	// body by RagdollApplyZones every step (since 2026-10-11 evening; before
	// that the spawn zone's gravity was baked in once, via ZoneGravityUU).
	FLOAT GravScale = ClampF( RagdollFloatProp( C, TEXT("GravityScale"), 1.0f ), 0.0f, 5.0f );
	S.GravityScale  = GravScale;
	S.AngDamp       = AngDamp;
	S.WaterBuoyancy = ClampF( RagdollFloatProp( C, TEXT("WaterBuoyancy"), 1.08f ), 0.0f, 3.0f );
	S.WaterDrag     = ClampF( RagdollFloatProp( C, TEXT("WaterDrag"), 2.0f ), 0.0f, 10.0f );
	S.AirDrag       = ClampF( RagdollFloatProp( C, TEXT("AirDrag"), 0.5f ), 0.0f, 10.0f );
	S.ZoneWindScale = ClampF( RagdollFloatProp( C, TEXT("ZoneWindScale"), 1.0f ), 0.0f, 5.0f );
	static INT GTrimDbg = 0;
	const UBOOL bTrimLog = ( GTrimDbg++ < 2 );
	FLOAT TotalMass = 0.0f;
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
		Bd.gravityScale   = GravScale;
		Bd.userData       = C;
		S.Bodies[i] = b3CreateBody( S.World, &Bd );
		// Velocities are assigned by ApplyDeathVelocities once the bodies sit
		// in the posed spawn configuration (the tumble term depends on it).

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
		Sd.density = GRagdollBodyMassKg[i] * MassScale / ( 8.0f * HX * HY * HZ );
		TotalMass += GRagdollBodyMassKg[i] * MassScale;
		Sd.baseMaterial.friction     = 0.6f;
		Sd.baseMaterial.restitution  = 0.05f;
		// b3DefaultShapeDef sets filter.categoryBits = UINT64_MAX, which
		// overlaps EVERY bit including the geometry category. The clearance ray
		// masks on the geometry category, so without an explicit body category
		// it would hit the corpse's own bodies and false-lift every spawn.
		Sd.filter.categoryBits = RAGDOLL_BODY_CATEGORY;
		b3BoxHull Hull = b3MakeOffsetBoxHull( HX, HY, HZ, Offset );
		b3ShapeId MainShape = b3CreateHullShape( S.Bodies[i], &Sd, &Hull.base );
		RagdollRecordHull( S, i, MainShape, HullOff, Half, Sd.density );
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

	S.bPlanarCuts       = RagdollBoolProp( C, TEXT("bPlanarCuts"), 1 );
	S.bFragmentBodies   = RagdollBoolProp( C, TEXT("bFragmentBodies"), 0 );
	// Cut variation / caps (docs/plans/gore-variation.md).
	S.bModelledCaps     = RagdollBoolProp( C, TEXT("bModelledCaps"), 1 );
	S.CutVariation      = ClampF( RagdollFloatProp( C, TEXT("CutVariation"), 1.0f ), 0.0f, 1.0f );
	S.bBoneStubs        = RagdollBoolProp( C, TEXT("bBoneStubs"), 0 );
	S.BoneStubChance    = ClampF( RagdollFloatProp( C, TEXT("BoneStubChance"), 0.7f ), 0.0f, 1.0f );
	S.BoneStubLength    = ClampF( RagdollFloatProp( C, TEXT("BoneStubLength"), 1.0f ), 0.0f, 3.0f );
	S.DrawScale         = DrawScale;
	GJointFrictionScale = RagdollFloatProp( C, TEXT("JointFrictionScale"), 1.0f );
	GJointLimitScale    = RagdollFloatProp( C, TEXT("JointLimitScale"), 1.0f );
	GJointSpringHertz   = ClampF( RagdollFloatProp( C, TEXT("JointSpringHertz"), 0.8f ), 0.0f, 5.0f );
	GJointSpringDamping = ClampF( RagdollFloatProp( C, TEXT("JointSpringDamping"), 0.8f ), 0.0f, 5.0f );
	GJointConstraintHertz   = ClampF( RagdollFloatProp( C, TEXT("ConstraintHertz"), 120.0f ), 10.0f, 400.0f );
	GJointConstraintDamping = ClampF( RagdollFloatProp( C, TEXT("ConstraintDamping"), 5.0f ), 0.1f, 20.0f );
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
	// Spawn pose: the dying pawn's current animation frame when the mesh can
	// be read through the dataset (bAnimPose), else the random pose.
	if( !ApplyAnimationPose( S, C, DS, LiveMesh, Anch, Spec ) )
		ApplyDeathPose( S, C, Anch );
	ClearCorpseSpawn( S, LW );
	ApplyDeathVelocities( S, C, DeathVel, DeathSpin );

	// Player mesh (preferred): capture the dead pawn's ULodMesh and attach it to
	// the bones. Falls back to the procedural box mannequin when the source mesh
	// isn't a usable vertex-mesh (e.g. skeletal/animated-only content).
	S.Mesh = CreatePlayerRagdollMesh( C, S );
	if( !S.Mesh )
		S.Mesh = CreateRagdollMesh( C );
	if( S.Mesh )
		C->Mesh = S.Mesh;

	S.bBuilt = true;
	debugf( TEXT("RagdollPkg: BuildRagdoll actor=%s loc=(%.0f,%.0f,%.0f) scale=%.2f mass=%.1fkg gravity=%.2fx"), C->GetName(), C->Location.X, C->Location.Y, C->Location.Z, DrawScale, TotalMass, GravScale );
	unguard;
}

/*-----------------------------------------------------------------------------
	Environment: zones and movers (docs/plans/environment.md).
-----------------------------------------------------------------------------*/

// Half extent (UU) of a body for the water sampling: the mean box half extent
// of a skeleton body, the bounding radius of a fragment.
static FLOAT RagdollBodyHalfExtentUU( const FRagdollState& S, INT Bone, FLOAT DrawScale )
{
	if( Bone >= RAGDOLL_BONE_COUNT )
	{
		const INT e = Bone - RAGDOLL_BONE_COUNT;
		return ( e >= 0 && e < S.NumExtra ) ? Max( S.ExtraRadius[e] * DrawScale, 2.0f ) : 4.0f;
	}
	const FRagdollBoneDef* Defs = S.BoneDefs ? S.BoneDefs : GRagdollBoneDefs;
	const FRagdollBoneDef& D = Defs[Bone];
	return Max( ( D.HX + D.HY + D.HZ ) / 3.0f * DrawScale, 2.0f );
}

static AZoneInfo* RagdollZoneAt( ULevel* Level, ALevelInfo* LI, const FVector& P )
{
	FPointRegion R = Level->Model->PointRegion( LI, P );
	return R.Zone ? R.Zone : LI;
}

// Water sample: 1 in water, 0 in air, -1 inside solid BSP (no leaf): a
// sample below the pool floor says nothing about the medium, so the caller
// counts it like the body centre. (A corpse resting on the pool floor had
// its lower sample in the floor, read "air", and never got full buoyancy.)
static INT RagdollWaterSample( ULevel* Level, ALevelInfo* LI, const FVector& P )
{
	FPointRegion R = Level->Model->PointRegion( LI, P );
	if( R.iLeaf == INDEX_NONE )
		return -1;
	AZoneInfo* Z = R.Zone ? R.Zone : LI;
	return Z->bWaterZone ? 1 : 0;
}

static void RagdollSetBodyDamping( FRagdollState& S, b3BodyId Bid, FLOAT Wet, UBOOL bWind )
{
	b3Body_SetLinearDamping( Bid, 0.05f + S.WaterDrag * Wet + ( bWind ? S.AirDrag : 0.0f ) );
	b3Body_SetAngularDamping( Bid, S.AngDamp + S.WaterDrag * Wet );
}

// Despawn the corpse without any gore: hidden now, destroyed by the engine's
// LifeSpan expiry next tick (Destroyed -> ShutdownRagdoll).
static void RagdollDoom( FRagdollState& S, ARagdollCarcass* C, FLOAT Life, UBOOL bHide )
{
	S.bDoomed = 1;
	if( bHide )
		C->bHidden = 1;
	if( C->LifeSpan <= 0.0f || C->LifeSpan > Life )
		C->LifeSpan = Life;
}

// Per body, every step: gravity of the zone the body is in, buoyancy and
// drag in water zones, drag toward a zone's ZoneVelocity (currents, wind),
// and the destructive / kill zones. Runs for every corpse before the shared
// world step (forces accumulate until b3World_Step).
static void RagdollApplyZones( FRagdollState& S, ARagdollCarcass* C )
{
	ULevel* Level = C->GetLevel();
	ALevelInfo* LI = C->Level;
	if( !Level || !Level->Model || !LI || S.bDoomed )
		return;
	const FLOAT DrawScale = ( C->DrawScale > 0.0f ) ? C->DrawScale : 1.0f;
	const FLOAT G = RAGDOLL_UT_GRAVITY * RAGDOLL_U2M;   // m/s^2 at zone gravity 950
	const INT NumBones = RagdollNumBones( S );
	// Buoyancy diagnostic (first 40 lines, every 25th sync of a corpse with a
	// wet pelvis): total lift vs weight and the bodies that are not fully wet.
	static INT GWetDbg = 0;
	const UBOOL bWetDbg = ( GWetDbg < 40 && ( S.SyncCount % 25 ) == 0 && S.BodyWet[BONE_Pelvis] > 0.0f );
	FLOAT WetLift = 0.0f, WetWeight = 0.0f;
	FString WetDry;
	for( INT b=0; b<NumBones && b<RAGDOLL_MAX_BONES; b++ )
	{
		if( !RagdollBoneAlive( S, b ) )
			continue;
		b3BodyId Bid = RagdollBody( S, b );
		if( !b3Body_IsValid( Bid ) )
			continue;
		b3Vec3 P = b3Body_GetPosition( Bid );
		const FVector PU( P.x * RAGDOLL_M2U, P.y * RAGDOLL_M2U, P.z * RAGDOLL_M2U );
		AZoneInfo* Z = RagdollZoneAt( Level, LI, PU );
		const UBOOL bNewZone = ( Z != S.BodyZone[b] );
		const UBOOL bWind = !Z->bWaterZone && S.ZoneWindScale > 0.0f && Z->ZoneVelocity.SizeSquared() > 1.0f;

		// Destructive (lava, slime) / kill zones: the main island takes the
		// corpse with it, a detached island is removed on its own.
		if( Z->bDestructive || Z->bKillZone )
		{
			const INT Root = RagdollIslandRoot( S, b );
			debugf( TEXT("RagdollPkg: %s %s body=%i root=%i zone=%s"), Z->bDestructive ? TEXT("destructive") : TEXT("killzone"), C->GetName(), b, Root, Z->GetName() );
			if( Root == BONE_Pelvis )
			{
				RagdollDoom( S, C, Z->bDestructive ? 0.01f : 1.0f, Z->bDestructive );
				return;
			}
			if( Root < RAGDOLL_BONE_COUNT )
				RagdollRemoveIsland( S, Root );
			else if( b3Body_IsValid( Bid ) )
			{
				b3DestroyBody( Bid );
				S.ExtraBodies[Root - RAGDOLL_BONE_COUNT] = b3_nullBodyId;
			}
			return;   // the body arrays changed: finish next step
		}

		// Gravity of this zone (ZoneGravity is a vector; Z carries it, a
		// sideways component becomes a force).
		FLOAT ZoneFrac = 1.0f;
		if( Z->ZoneGravity.SizeSquared() > 1.0f )
			ZoneFrac = ClampF( -Z->ZoneGravity.Z / RAGDOLL_UT_GRAVITY, -3.0f, 3.0f );
		const FLOAT Grav = S.GravityScale * ZoneFrac;
		if( Abs( Grav - S.BodyGrav[b] ) > 0.001f )
		{
			b3Body_SetGravityScale( Bid, Grav );
			S.BodyGrav[b] = Grav;
		}
		const FLOAT Mass = b3Body_GetMass( Bid );
		b3Vec3 Force = { 0.0f, 0.0f, 0.0f };
		if( Abs( Z->ZoneGravity.X ) > 1.0f || Abs( Z->ZoneGravity.Y ) > 1.0f )
		{
			Force.x += Mass * Z->ZoneGravity.X * RAGDOLL_U2M * S.GravityScale;
			Force.y += Mass * Z->ZoneGravity.Y * RAGDOLL_U2M * S.GravityScale;
		}

		// Water: submerged fraction from the centre and +-half extent along Z.
		FLOAT Wet = 0.0f;
		FLOAT H = 0.0f;
		INT SU = 0, SD = 0;
		if( Z->bWaterZone )
		{
			H = RagdollBodyHalfExtentUU( S, b, DrawScale );
			INT In = 1;
			SU = RagdollWaterSample( Level, LI, PU + FVector( 0, 0, H ) );
			SD = RagdollWaterSample( Level, LI, PU - FVector( 0, 0, H ) );
			if( SU != 0 ) In++;   // water, or solid (counts like the centre)
			if( SD != 0 ) In++;
			Wet = In / 3.0f;
		}
		if( bNewZone )
		{
			if( Z->bWaterZone && ( !S.BodyZone[b] || !S.BodyZone[b]->bWaterZone ) )
			{
				// Splash: the water takes half the entry speed at once.
				b3Vec3 V = b3Body_GetLinearVelocity( Bid );
				b3Body_SetLinearVelocity( Bid, b3MulSV( 0.5f, V ) );
				// Diagnostic (first entry per corpse): the water column at this
				// XY: where the water starts/ends and the solid floor.
				if( S.ZoneLogCount == 0 )
				{
					FLOAT Floor = -1e9f, Surface = -1e9f, Bottom = 1e9f;
					for( FLOAT z = PU.Z - 200.0f; z <= PU.Z + 200.0f; z += 4.0f )
					{
						const INT Smp = RagdollWaterSample( Level, LI, FVector( PU.X, PU.Y, z ) );
						if( Smp == 1 ) { Surface = Max( Surface, z ); Bottom = Min( Bottom, z ); }
						if( Smp == -1 && z < PU.Z ) Floor = Max( Floor, z );
					}
					debugf( TEXT("RagdollPkg: watercolumn %s at=(%.0f,%.0f) bodyZ=%.1f water=%.0f..%.0f solidBelow=%.0f"), C->GetName(), PU.X, PU.Y, PU.Z, Bottom, Surface, Floor );
				}
			}
			if( S.ZoneLogCount < 6 && ( Z != LI || Wet > 0.0f || bWind || Abs( Grav - S.GravityScale ) > 0.001f ) )
			{
				S.ZoneLogCount++;
				debugf( TEXT("RagdollPkg: zone %s body=%i zone=%s water=%.2f grav=%.2f wind=%i vel=(%.0f,%.0f,%.0f)"), C->GetName(), b, Z->GetName(), Wet, Grav, bWind, Z->ZoneVelocity.X, Z->ZoneVelocity.Y, Z->ZoneVelocity.Z );
			}
			S.BodyZone[b] = Z;
		}
		if( bNewZone || Abs( Wet - S.BodyWet[b] ) > 0.01f )
		{
			RagdollSetBodyDamping( S, Bid, Wet, bWind );
			S.BodyWet[b] = Wet;
		}
		if( Wet > 0.0f )
		{
			// Buoyancy against the zone's own gravity, plus the current.
			Force.z += Wet * Mass * G * Max( Grav, 0.0f ) * S.WaterBuoyancy;
			Force.x += Wet * Mass * S.WaterDrag * Z->ZoneVelocity.X * RAGDOLL_U2M;
			Force.y += Wet * Mass * S.WaterDrag * Z->ZoneVelocity.Y * RAGDOLL_U2M;
			Force.z += Wet * Mass * S.WaterDrag * Z->ZoneVelocity.Z * RAGDOLL_U2M;
		}
		else if( bWind )
		{
			const FLOAT K = Mass * S.AirDrag * S.ZoneWindScale * RAGDOLL_U2M;
			Force.x += K * Z->ZoneVelocity.X;
			Force.y += K * Z->ZoneVelocity.Y;
			Force.z += K * Z->ZoneVelocity.Z;
		}
		if( bWetDbg )
		{
			WetLift   += Force.z;
			WetWeight += Mass * G * Grav;
			if( Wet < 0.99f )
				WetDry += FString::Printf( TEXT(" %i:%.2f(%s,z=%.0f,u%i,d%i)"), b, Wet, Z->GetName(), PU.Z, SU, SD );
		}
		if( Force.x != 0.0f || Force.y != 0.0f || Force.z != 0.0f )
			// A zone change wakes a sleeping body; so does net-positive buoyancy
			// (a corpse that stopped on the pool floor fell asleep and a force
			// with wake=false is ignored: it never floated up).
			b3Body_ApplyForceToCenter( Bid, Force, bNewZone || ( Wet > 0.0f && Wet * S.WaterBuoyancy > 1.0f ) );
	}
	if( bWetDbg )
	{
		GWetDbg++;
		const b3Vec3 V = b3Body_GetLinearVelocity( RagdollBody( S, BONE_Pelvis ) );
		debugf( TEXT("RagdollPkg: wet %s sync=%i lift=%.0f weight=%.0f pelvisZ=%.1f vz=%.2f awake=%i dry=%s"),
			C->GetName(), S.SyncCount, WetLift, WetWeight, b3Body_GetPosition( RagdollBody( S, BONE_Pelvis ) ).z * RAGDOLL_M2U, V.z,
			b3Body_IsAwake( RagdollBody( S, BONE_Pelvis ) ), WetDry.Len() ? *WetDry : TEXT("none") );
	}
}

// Crushed by a mover: a body centre inside a mover brush. The main island
// despawns the corpse at once (no gib, no blood: "crushed = disappears"); a
// detached island is removed alone. bRecentOnly limits the test to movers
// that moved in the last second (the periodic check); the 'Crushed' damage
// path tests every mover.
static void RagdollCrushCheck( FRagdollState& S, ARagdollCarcass* C, FRagdollLevelWorld& LW, UBOOL bRecentOnly )
{
	if( S.bDoomed || !LW.bBuilt || LW.Movers.Num() == 0 )
		return;
	const INT NumBones = RagdollNumBones( S );
	for( INT b=0; b<NumBones && b<RAGDOLL_MAX_BONES; b++ )
	{
		if( !RagdollBoneAlive( S, b ) )
			continue;
		b3BodyId Bid = RagdollBody( S, b );
		if( !b3Body_IsValid( Bid ) )
			continue;
		b3Vec3 P = b3Body_GetPosition( Bid );
		const FVector PU( P.x * RAGDOLL_M2U, P.y * RAGDOLL_M2U, P.z * RAGDOLL_M2U );
		AMover* M = RagdollPointInMover( LW, PU, bRecentOnly );
		if( !M )
		{
			S.CrushHits[b] = 0;
			continue;
		}
		// Debounce: a body can dip into a fast mover for a step or two while
		// the contacts push it out; only consecutive hits (2 from the
		// engine's per-tick 'Crushed' damage, 3 from the periodic check)
		// count as crushed.
		if( S.CrushHits[b] < 250 )
			S.CrushHits[b]++;
		if( S.CrushHits[b] < ( bRecentOnly ? 8 : 2 ) )   // periodic: 8 x 40 ms = inside for 0.3 s
			continue;
		const INT Root = RagdollIslandRoot( S, b );
		debugf( TEXT("RagdollPkg: crushed %s body=%i root=%i mover=%s at=(%.0f,%.0f,%.0f) hits=%i"), C->GetName(), b, Root, M->GetName(), PU.X, PU.Y, PU.Z, S.CrushHits[b] );
		if( Root == BONE_Pelvis )
		{
			S.bCrushed = 1;
			RagdollDoom( S, C, 0.01f, 1 );
			return;
		}
		if( Root < RAGDOLL_BONE_COUNT )
			RagdollRemoveIsland( S, Root );
		else
		{
			b3DestroyBody( Bid );
			S.ExtraBodies[Root - RAGDOLL_BONE_COUNT] = b3_nullBodyId;
		}
		return;
	}
}

// Sustained push (RagdollPush): the stored force on the hit body at its
// stored body-space point, a third per kilogram on the jointed neighbours
// at their centres, every tick until PushUntil. A body already moving at
// RAGDOLL_PUSH_MAX_V along the push gets nothing more (the beam cannot fling
// a limb faster than a direct hit would).
#define RAGDOLL_PUSH_MAX_V 6.0f
static void RagdollApplyPush( FRagdollState& S, ARagdollCarcass* C )
{
	if( S.PushBody < 0 )
		return;
	const FLOAT Now = C->Level ? C->Level->TimeSeconds : 0.0f;
	if( Now >= S.PushUntil || S.bDoomed || S.PushBody >= S.NumBodies || !S.Alive[S.PushBody] || !b3Body_IsValid( S.Bodies[S.PushBody] ) )
	{
		S.PushBody = -1;
		return;
	}
	const INT Body = S.PushBody;
	const FLOAT BodyMass = Max( b3Body_GetMass( S.Bodies[Body] ), 0.1f );
	const FLOAT Accel = S.PushForce / BodyMass;   // m/s^2 on the hit body
	const b3Vec3 V = b3Body_GetLinearVelocity( S.Bodies[Body] );
	if( b3Dot( V, S.PushDir ) < RAGDOLL_PUSH_MAX_V )
		b3Body_ApplyForce( S.Bodies[Body], b3MulSV( S.PushForce, S.PushDir ), b3Body_GetWorldPoint( S.Bodies[Body], S.PushLocal ), true );
	// Jointed neighbours a third per kg, the rest of the island a quarter:
	// unlike a shot, the beam has to overcome the floor friction of the WHOLE
	// corpse (0.6 x 78 kg x 19 m/s^2 = 890 N) or the hit body only tugs its
	// joints. Headless, 684 N on the spine alone moved it 3 UU in a second;
	// the same packet on an upper arm (263 m/s^2, so the arm hit the cap and
	// dragged the body) moved the corpse 83 UU.
	const INT Root = RagdollIslandRoot( S, Body );
	for( INT j=0; j<S.NumBodies; j++ )
	{
		if( j == Body || !S.Alive[j] || !b3Body_IsValid( S.Bodies[j] ) )
			continue;
		if( RagdollIslandRoot( S, j ) != Root )
			continue;   // a torn-off part is not jointed any more
		const UBOOL bNeighbour = ( GRagdollBoneParent[j] == Body || GRagdollBoneParent[Body] == j );
		const b3Vec3 Vj = b3Body_GetLinearVelocity( S.Bodies[j] );
		if( b3Dot( Vj, S.PushDir ) >= RAGDOLL_PUSH_MAX_V )
			continue;
		b3Body_ApplyForceToCenter( S.Bodies[j], b3MulSV( Accel * ( bNeighbour ? 0.33f : 0.25f ) * b3Body_GetMass( S.Bodies[j] ), S.PushDir ), true );
	}
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
	// Zone effects for THIS corpse's bodies (every corpse, every tick; the
	// forces wait for the next world step).
	RagdollApplyZones( *S, C );
	RagdollApplyPush( *S, C );   // sustained beam push (RagdollPush), forces wait for the step too
	// One world step per ENGINE TICK (2026-10-11 evening): appSeconds()
	// advances between two corpses' Timers inside the same tick, so every
	// corpse after the first stepped the world again by ~1 ms, and Box3D
	// clears the applied forces at the end of EVERY step: the buoyancy and
	// current forces of every corpse but the first acted for a millisecond
	// (the pool corpse sank at full gravity while its lift/weight ledger
	// read +8%), and N corpses cost N full solver passes per tick.
	if( C->Level && C->Level->TimeSeconds == LW.LastTickTime )
		return;
	FLOAT Now = appSeconds().GetFloat();
	FLOAT dt  = Now - LW.LastStepTime;
	if( dt <= 0.0f )
		return;
	LW.LastStepTime = Now;
	if( C->Level )
		LW.LastTickTime = C->Level->TimeSeconds;

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
	RagdollSyncMovers( LW, dt );   // drive the kinematic mover bodies to their engine poses
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

	// Crushed by a lift / door / train that moved in the last second: every
	// 2nd sync (docs/plans/environment.md).
	if( ( S->SyncCount & 1 ) == 0 )
		RagdollCrushCheck( *S, C, GetRagdollLevelWorld(), true );
	// Ride diagnostic: while a mover moves, every 10th sync, the pelvis
	// against the nearest moving mover (first 60 lines per level).
	{
		static INT GRideDbg = 0;
		FRagdollLevelWorld& LWd = GetRagdollLevelWorld();
		if( GRideDbg < 60 && ( S->SyncCount % 10 ) == 0 && LWd.bBuilt )
		{
			const FLOAT Now = appSeconds().GetFloat();
			for( INT m=0; m<LWd.Movers.Num(); m++ )
			{
				FRagdollMoverBody& MB = LWd.Movers(m);
				if( !MB.bEnabled || !MB.Mover || Now - MB.LastMoveTime > 0.2f )
					continue;
				b3Vec3 PP = b3Body_GetPosition( S->Bodies[BONE_Pelvis] );
				const FVector PU( PP.x * RAGDOLL_M2U, PP.y * RAGDOLL_M2U, PP.z * RAGDOLL_M2U );
				FBox EB = MB.Mover->Brush->GetCollisionBoundingBox( MB.Mover );
				if( PU.X < EB.Min.X - 100.0f || PU.X > EB.Max.X + 100.0f || PU.Y < EB.Min.Y - 100.0f || PU.Y > EB.Max.Y + 100.0f || Abs( PU.Z - MB.Mover->Location.Z ) > 400.0f )
					continue;
				b3Vec3 PV = b3Body_GetLinearVelocity( S->Bodies[BONE_Pelvis] );
				b3Vec3 KP = b3Body_GetPosition( MB.Body );
				b3Vec3 KV = b3Body_GetLinearVelocity( MB.Body );
				INT Asleep = 0, Touch = 0;
				for( INT b=0; b<S->NumBodies; b++ )
				{
					if( !S->Alive[b] || !b3Body_IsValid( S->Bodies[b] ) )
						continue;
					if( !b3Body_IsAwake( S->Bodies[b] ) )
						Asleep++;
					b3ContactData Data[32];
					const INT N = b3Body_GetContactData( S->Bodies[b], Data, 32 );
					for( INT n=0; n<N; n++ )
					{
						b3BodyId BA = b3Shape_GetBody( Data[n].shapeIdA );
						b3BodyId BB = b3Shape_GetBody( Data[n].shapeIdB );
						if( B3_ID_EQUALS( BA, MB.Body ) || B3_ID_EQUALS( BB, MB.Body ) )
						{
							for( INT k=0; k<Data[n].manifoldCount; k++ )
								if( Data[n].manifolds[k].pointCount > 0 ) { Touch++; break; }
						}
					}
				}
				GRideDbg++;
				debugf( TEXT("RagdollPkg: ride %s sync=%i mover=%s engZ=%.1f kinZ=%.1f kinVz=%.2f pelvisZ=%.1f pelvisVz=%.2f touching=%i asleep=%i/%i"),
					C->GetName(), S->SyncCount, MB.Mover->GetName(), MB.Mover->Location.Z, KP.z * RAGDOLL_M2U, KV.z, PU.Z, PV.z, Touch, Asleep, S->NumBodies );
				break;
			}
		}
	}

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

/*-----------------------------------------------------------------------------
	Damage model (2026-10-11): per-body ledger, momentum-based impulses.

	Replaces ApplyDamageImpulse, which gave EVERY body 3.5-8 m/s in the shot
	direction plus an upward pop on every hit (an enforcer round hopped the
	whole corpse) and picked the "hit" body by centre distance to a point on
	the collision cylinder. Now:
	 - RagdollHit (bullets, blades, beams): the shot ray is cast through the
	   skeleton; only the body it enters takes the impulse, at the entry
	   point, sized from the engine's momentum (UT: delta-v = Momentum /
	   pawn mass) through a small coupling mass (HitImpulseMass, ~4 kg: a
	   bullet's own momentum, not the pawn knock-back) and capped per body.
	   The body's ledger grows by Damage x CutPower.
	 - RagdollBlast (rockets, flak shells, grenades, shock combos): every body
	   in range takes the blast's falloff share, (1 - d/R)^2 by the closest
	   point of its hull, as a radial impulse at that point and as ledger
	   damage, so a rocket at the knees takes the shins, one on the chest the
	   head and forearms, and the far leg nothing.
	A body whose ledger passes GRagdollSeverThreshold x DismemberScale is
	returned in the sever mask; script tears it off (blood, sound, bleed).
-----------------------------------------------------------------------------*/

// Bodies attached to the pelvis whose ledger crossed the threshold. The
// spine's threshold is the TorsoSeverDamage knob (0 = never): the upper body
// tears off at the waist (docs/plans/gore-variation.md).
static INT RagdollSeverMask( const FRagdollState& S, FLOAT Scale, FLOAT TorsoThr )
{
	INT Mask = 0;
	for( INT b=1; b<S.NumBodies; b++ )
	{
		if( !S.Alive[b] || RagdollIslandRoot( S, b ) != BONE_Pelvis )
			continue;
		FLOAT Thr = GRagdollSeverThreshold[b];
		if( b == BONE_Spine )
			Thr = ( TorsoThr > 0.0f ) ? TorsoThr : 1e9f;
		if( S.PartDamage[b] >= Thr * Scale )
			Mask |= ( 1 << b );
	}
	return Mask;
}

static FLOAT RagdollTorsoThreshold( UObject* C )
{
	return RagdollFloatProp( C, TEXT("TorsoSeverDamage"), 130.0f );
}

// RagdollHit(HitLocation, Momentum, Damage, CutPower, ImpulseScale, out SeverMask)
// Returns the body the shot entered (-1: grazed the cylinder, nothing hit).
// Shared by RagdollHit / RagdollPush: the body a shot from HitLocation along
// Momentum enters (or grazes within ~12 UU), the hit point (m, world) and the
// unit direction. Returns -1 when the ray misses the whole skeleton.
static INT RagdollShotBody( FRagdollState& S, ARagdollCarcass* C, const FVector& HitLocation, const FVector& Momentum, FVector* OutDir, FLOAT* OutMLen, b3Vec3* OutPt, UBOOL* OutGraze )
{
	// Shot direction: the momentum; without one (touch damage) a random
	// horizontal direction through the hit point.
	FVector Dir = Momentum;
	FLOAT MLen = Dir.Size();
	if( MLen > 1.0f )
		Dir /= MLen;
	else
	{
		const FLOAT A = appFrand() * 2.0f * PI;
		Dir = FVector( appCos( A ), appSin( A ), -0.2f ).SafeNormal();
	}

	// Ray: from 80 UU before the engine's point (which is on the collision
	// cylinder, possibly inside the sprawl) to 80 UU past the far side.
	const FLOAT Reach = 80.0f + 2.0f * Max( C->CollisionRadius, C->CollisionHeight ) + 80.0f;
	b3Vec3 Pt = b3Vec3_zero;
	INT   Graze = -1;
	FLOAT GrazeD = 1e9f;
	INT   Body = RagdollRayHitBody( S, HitLocation - Dir * 80.0f, Dir, Reach, &Pt, &Graze, &GrazeD );
	UBOOL bGraze = 0;
	if( Body < 0 )
	{
		// Nothing under the ray. The engine has already decided this shot hit
		// the corpse (the chunk is gone, the trace stopped on the cylinder),
		// so the body whose centre passes nearest the ray takes it, at its
		// closest point to the ray. Until 2026-10-11 anything farther than
		// ~12 UU was dropped as a miss: the cylinder is grown to the sprawl
		// (radius ~70, height ~25 on a lying corpse), so a flak chunk
		// clipping its top arrived 15-30 UU above the limbs and six flak
		// shots did nothing. bGraze only marks the hit as indirect now
		// (half impulse, full ledger).
		if( Graze < 0 )
			return -1;
		Body   = Graze;
		bGraze = 1;
		const b3Vec3 Pb = b3Body_GetPosition( S.Bodies[Body] );
		const b3Vec3 O  = { ( HitLocation.X - Dir.X * 80.0f ) * RAGDOLL_U2M, ( HitLocation.Y - Dir.Y * 80.0f ) * RAGDOLL_U2M, ( HitLocation.Z - Dir.Z * 80.0f ) * RAGDOLL_U2M };
		const b3Vec3 D  = { Dir.X, Dir.Y, Dir.Z };
		const b3Vec3 OnRay = b3Add( O, b3MulSV( b3Dot( b3Sub( Pb, O ), D ), D ) );
		b3Body_GetClosestPoint( S.Bodies[Body], &Pt, OnRay );
	}
	*OutDir = Dir; *OutMLen = MLen; *OutPt = Pt; *OutGraze = bGraze;
	return Body;
}

// The shot's impulse on the hit body (N s): the pawn's delta-v for this
// momentum (UT: Momentum / Mass, Mass 100 for players) through the coupling
// mass. 2026-10-11 afternoon: coupling 4 kg gave a torso 0.11 m/s per
// enforcer round and a forearm 0.9 ("minigun has no effect"); 25 kg (a third
// of the corpse) moves the torso ~0.7 m/s and flicks a forearm to the cap.
static FLOAT RagdollShotImpulse( ARagdollCarcass* C, FLOAT MLen, FLOAT ImpulseScale, UBOOL bGraze )
{
	const FLOAT PawnMass = 100.0f;
	const FLOAT DvPawn   = ( MLen / PawnMass ) * RAGDOLL_U2M;   // m/s
	const FLOAT Couple   = ClampF( RagdollFloatProp( C, TEXT("HitImpulseMass"), 25.0f ), 0.0f, 100.0f );
	return DvPawn * Couple * ImpulseScale * ( bGraze ? 0.5f : 1.0f );
}

// Ledger entry for a shot (and where on the bone it landed, for the cut
// placement); returns the sever mask.
static INT RagdollShotLedger( FRagdollState& S, ARagdollCarcass* C, INT Body, const b3Vec3& Pt, INT Damage, FLOAT CutPower, UBOOL bGraze, FLOAT* OutCut, FLOAT* OutScale )
{
	const FLOAT Cut = Max( Damage, 0 ) * CutPower;   // bGraze: the engine charged the full damage, book it
	S.PartDamage[Body] += Cut;
	RagdollRecordHit( S, Body, Pt );
	const FLOAT Scale = Max( RagdollFloatProp( C, TEXT("DismemberScale"), 1.0f ), 0.05f );
	*OutCut = Cut; *OutScale = Scale;
	return RagdollSeverMask( S, Scale, RagdollTorsoThreshold( C ) );
}

void ARagdollCarcass::execRagdollHit( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execRagdollHit);
	P_GET_VECTOR( HitLocation );
	P_GET_VECTOR( Momentum );
	P_GET_INT( Damage );
	P_GET_FLOAT( CutPower );
	P_GET_FLOAT( ImpulseScale );
	P_GET_INT_REF( SeverMask );
	P_FINISH;
	*(INT*)Result = -1;
	*SeverMask = 0;

	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt )
		return;

	FVector Dir; FLOAT MLen; b3Vec3 Pt; UBOOL bGraze;
	const INT Body = RagdollShotBody( *S, this, HitLocation, Momentum, &Dir, &MLen, &Pt, &bGraze );
	if( Body < 0 )
		return;

	// Impulse on the hit body, capped per body. The jointed neighbours take a
	// third at their centres, as the killing blow does, so a torso hit rocks
	// the upper body instead of one box tugging its joints.
	const FLOAT BodyMass = Max( b3Body_GetMass( S->Bodies[Body] ), 0.1f );
	FLOAT J = RagdollShotImpulse( this, MLen, ImpulseScale, bGraze );
	const FLOAT MaxDv = 6.0f;
	if( J > MaxDv * BodyMass )
		J = MaxDv * BodyMass;
	if( J > 0.0f )
	{
		const b3Vec3 Imp = { Dir.X * J, Dir.Y * J, Dir.Z * J };
		b3Body_ApplyLinearImpulse( S->Bodies[Body], Imp, Pt, true );
		const FLOAT Dv = J / BodyMass;
		for( INT j=0; j<S->NumBodies; j++ )
		{
			if( j == Body || !S->Alive[j] || !( GRagdollBoneParent[j] == Body || GRagdollBoneParent[Body] == j ) )
				continue;
			if( RagdollIslandRoot( *S, j ) != RagdollIslandRoot( *S, Body ) )
				continue;   // a torn-off part is not jointed any more
			const FLOAT Mj = b3Body_GetMass( S->Bodies[j] );
			b3Body_ApplyLinearImpulseToCenter( S->Bodies[j], b3MulSV( Dv * 0.33f * Mj, b3Vec3{ Dir.X, Dir.Y, Dir.Z } ), true );
		}
	}

	FLOAT Cut, Scale;
	*SeverMask = RagdollShotLedger( *S, this, Body, Pt, Damage, CutPower, bGraze, &Cut, &Scale );
	*(INT*)Result = Body;

	static INT GHitDbg = 0;
	if( GHitDbg < 400 )
	{
		GHitDbg++;
		debugf( TEXT("RagdollPkg: hit %s body=%i graze=%i dmg=%i cut=%.1f ledger=%.0f/%.0f dv=%.2f m/s mom=%.0f sever=0x%x"),
			GetName(), Body, bGraze, Damage, Cut, S->PartDamage[Body], GRagdollSeverThreshold[Body] * Scale, J / BodyMass, MLen, *SeverMask );
	}
	unguard;
}

// RagdollPush(HitLocation, Momentum, Damage, CutPower, ImpulseScale, Duration, out SeverMask)
// A beam packet (pulse rifle secondary: PBolt.CheckBeam hands the corpse
// Damage x 0.22 s and MomentumTransfer x 0.22 s every 0.22 s while the beam
// rests on it). Same ray and ledger as RagdollHit, but the impulse becomes a
// FORCE of Impulse / Duration on the hit body for Duration seconds (applied
// by RagdollApplyPush before every world step, replacing any earlier push),
// so the beam shoves and drags the corpse instead of twitching it 4-5 times
// a second: an impulse of 0.2 m/s on a torso lying on the floor is gone to
// friction within one frame. Returns the body hit (-1 = missed).
void ARagdollCarcass::execRagdollPush( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execRagdollPush);
	P_GET_VECTOR( HitLocation );
	P_GET_VECTOR( Momentum );
	P_GET_INT( Damage );
	P_GET_FLOAT( CutPower );
	P_GET_FLOAT( ImpulseScale );
	P_GET_FLOAT( Duration );
	P_GET_INT_REF( SeverMask );
	P_FINISH;
	*(INT*)Result = -1;
	*SeverMask = 0;

	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt )
		return;

	FVector Dir; FLOAT MLen; b3Vec3 Pt; UBOOL bGraze;
	const INT Body = RagdollShotBody( *S, this, HitLocation, Momentum, &Dir, &MLen, &Pt, &bGraze );
	if( Body < 0 )
		return;

	Duration = ClampF( Duration, 0.02f, 2.0f );
	const FLOAT J = RagdollShotImpulse( this, MLen, ImpulseScale, bGraze );
	S->PushBody  = Body;
	S->PushDir   = b3Vec3{ Dir.X, Dir.Y, Dir.Z };
	S->PushForce = J / Duration;
	S->PushLocal = b3Body_GetLocalPoint( S->Bodies[Body], Pt );
	S->PushUntil = ( Level ? Level->TimeSeconds : 0.0f ) + Duration;
	b3Body_SetAwake( S->Bodies[Body], true );

	FLOAT Cut, Scale;
	*SeverMask = RagdollShotLedger( *S, this, Body, Pt, Damage, CutPower, bGraze, &Cut, &Scale );
	*(INT*)Result = Body;

	static INT GPushDbg = 0;
	if( GPushDbg < 400 )
	{
		GPushDbg++;
		const FLOAT BodyMass = Max( b3Body_GetMass( S->Bodies[Body] ), 0.1f );
		debugf( TEXT("RagdollPkg: push %s body=%i graze=%i dmg=%i cut=%.1f ledger=%.0f/%.0f force=%.0f N (%.1f m/s^2) for %.2f s mom=%.0f sever=0x%x"),
			GetName(), Body, bGraze, Damage, Cut, S->PartDamage[Body], GRagdollSeverThreshold[Body] * Scale, S->PushForce, S->PushForce / BodyMass, Duration, MLen, *SeverMask );
	}
	unguard;
}

// RagdollBlast(Origin, Radius, Damage, Momentum, CutPower, ImpulseScale, out SeverMask)
// Damage and Momentum are what the ENGINE handed this corpse: HurtRadius has
// already scaled the blast's at-origin values (rocket 75 / 80000 / 220 UU)
// by the explosion's distance to the corpse cylinder (full when it touches
// the cylinder, which is grown to the sprawl). They are therefore the values
// at the body NEAREST the origin, which takes them in full; every other
// body falls off with its EXTRA distance beyond the nearest one (push over
// Radius, tearing over half of it), so the far side of the corpse still
// stays whole. Until 2026-10-11 night this scaled by the distance from the
// origin a second time: a rocket exploding on the cylinder 65 UU from the
// nearest shin (engine: 55 damage) booked 8 points of cut on it ("rockets
// from area damage: no limbs flying off"). Kicks every body radially at its
// closest point and books Damage x CutPower x falloff on its ledger.
// Returns the nearest body (-1 when no body is within Radius).
void ARagdollCarcass::execRagdollBlast( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execRagdollBlast);
	P_GET_VECTOR( Origin );
	P_GET_FLOAT( Radius );
	P_GET_INT( Damage );
	P_GET_FLOAT( Momentum );
	P_GET_FLOAT( CutPower );
	P_GET_FLOAT( ImpulseScale );
	P_GET_INT_REF( SeverMask );
	P_FINISH;
	*(INT*)Result = -1;
	*SeverMask = 0;

	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt || Radius <= 1.0f )
		return;

	const b3Vec3 O = { Origin.X * RAGDOLL_U2M, Origin.Y * RAGDOLL_U2M, Origin.Z * RAGDOLL_U2M };
	const FLOAT  R = Radius * RAGDOLL_U2M;
	const FLOAT  DvPawn = ( Max( Momentum, 0.0f ) / 100.0f ) * RAGDOLL_U2M;   // m/s on the nearest body
	const FLOAT  MaxDv  = 10.0f;
	const INT    N = RagdollNumBones( *S );

	// Pass 1: closest point and distance of every live body; the nearest one.
	TArray<b3Vec3> Cp;   Cp.AddZeroed( N );
	TArray<FLOAT>  Dist; Dist.AddZeroed( N );
	INT   Nearest = -1;
	FLOAT DMin    = 1e9f;
	for( INT b=0; b<N; b++ )
	{
		Dist(b) = -1.0f;
		if( !RagdollBoneAlive( *S, b ) )
			continue;
		const b3BodyId Bid = RagdollBody( *S, b );
		if( !b3Body_IsValid( Bid ) )
			continue;
		Dist(b) = b3Body_GetClosestPoint( Bid, &Cp(b), O );
		if( Dist(b) < DMin )
		{
			DMin    = Dist(b);
			Nearest = b;
		}
	}
	if( Nearest < 0 || DMin >= R )
		return;

	// Pass 2: push and ledger by the extra distance beyond the nearest body.
	INT InRange = 0;
	for( INT b=0; b<N; b++ )
	{
		if( Dist(b) < 0.0f )
			continue;
		const FLOAT Extra = Dist(b) - DMin;
		FLOAT f = 1.0f - Extra / R;
		if( f <= 0.0f )
			continue;
		f *= f;   // quadratic falloff for the push
		FLOAT fc = 1.0f - Extra / ( 0.5f * R );
		fc = ( fc > 0.0f ) ? fc * fc : 0.0f;
		InRange++;

		// Radial impulse at the closest point, biased upward so parts arc.
		const b3BodyId Bid = RagdollBody( *S, b );
		b3Vec3 Dir = b3Sub( Cp(b), O );
		FLOAT L = b3Length( Dir );
		if( L < 0.02f )
		{
			const FLOAT A = appFrand() * 2.0f * PI;
			Dir = { (FLOAT)appCos( A ), (FLOAT)appSin( A ), 0.5f };
			L = b3Length( Dir );
		}
		Dir = b3MulSV( 1.0f / L, Dir );
		Dir.z += 0.35f;
		Dir = b3Normalize( Dir );
		const FLOAT Dv = Min( DvPawn * f * ImpulseScale, MaxDv );
		if( Dv > 0.0f )
		{
			const FLOAT M = b3Body_GetMass( Bid );
			b3Body_ApplyLinearImpulse( Bid, b3MulSV( Dv * M, Dir ), Cp(b), true );
		}
		if( b < RAGDOLL_BONE_COUNT && fc > 0.0f )
		{
			S->PartDamage[b] += Max( Damage, 0 ) * fc * CutPower;
			RagdollRecordHit( *S, b, Cp(b) );   // the blast tears at the point nearest to it
		}
	}
	const FLOAT Scale = Max( RagdollFloatProp( this, TEXT("DismemberScale"), 1.0f ), 0.05f );
	*SeverMask = RagdollSeverMask( *S, Scale, RagdollTorsoThreshold( this ) );
	*(INT*)Result = Nearest;

	static INT GBlastDbg = 0;
	if( GBlastDbg < 400 )
	{
		GBlastDbg++;
		debugf( TEXT("RagdollPkg: blast %s origin=(%.0f,%.0f,%.0f) r=%.0f dmg=%i mom=%.0f bodies=%i nearest=b%i dmin=%.0fUU ledger=%.0f sever=0x%x"),
			GetName(), Origin.X, Origin.Y, Origin.Z, Radius, Damage, Momentum, InRange, Nearest, DMin * RAGDOLL_M2U,
			( Nearest < RAGDOLL_BONE_COUNT ) ? S->PartDamage[Nearest] : 0.0f, *SeverMask );
	}
	unguard;
}

// RagdollLedger(Body): the body's ledger (diagnostics / tests).
// RagdollCrushed(): run the crush test against EVERY mover now (the engine's
// mover encroachment reported 'Crushed' damage on the corpse cylinder);
// returns 1 when the main island was inside a mover (the corpse is already
// hidden and expiring; script Destroy()s it at once).
void ARagdollCarcass::execRagdollCrushed( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execRagdollCrushed);
	P_FINISH;
	*(DWORD*)Result = 0;
	FRagdollState* S = GetRagdollStates().Find( this );
	if( !S || !S->bBuilt )
		return;
	RagdollCrushCheck( *S, this, GetRagdollLevelWorld(), false );
	*(DWORD*)Result = S->bCrushed ? 1 : 0;
	unguard;
}

void ARagdollCarcass::execRagdollLedger( FFrame& Stack, RESULT_DECL )
{
	guard(ARagdollCarcass::execRagdollLedger);
	P_GET_INT( Body );
	P_FINISH;
	FRagdollState* S = GetRagdollStates().Find( this );
	*(FLOAT*)Result = ( S && S->bBuilt && Body >= 0 && Body < RAGDOLL_BONE_COUNT ) ? S->PartDamage[Body] : 0.0f;
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
	RagdollTrimHullsForCut( *S, Body );   // mid-bone cut: the hull follows the severed part

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

// Gib(Damage, HitLocation, bTotal): blow the corpse apart. How MANY joints go scales
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
	P_GET_UBOOL( bTotalIn );
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

	// Blast size (2026-10-10 user feedback: "gibbing way too easily";
	// 2026-10-11 night: "severed limbs on a direct hit, tiny bits only after
	// ~3 rockets"):
	//  - a TOTAL gib (everything comes apart, torso halves included) when
	//    script says so (bTotal: the corpse's cumulative damage passed
	//    250+ in one hit, or the script's total gib) or at 250+ in one
	//    hit (redeemer, crushers, telefrags);
	//  - otherwise exactly ONE part: the joint nearest the hit (the engine
	//    gibbed a pawn with overkill but no blast origin reached the body);
	//    RagdollBlast's direct-hit guarantee is the usual path for rockets;
	//  - below GibWholeLimbDamage (90) only the distal parts come off
	//    (forearm/shin/head stand in for "hand/foot": the rig has no hand or
	//    foot bodies), above it a whole arm or leg may go;
	//  - the spine and neck joints never split short of a total gib.
	const UBOOL bTotal = bTotalIn || ( Damage >= 250 );

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

	INT Want = bTotal ? NumCand : 1;
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
	for( INT b=1; b<RAGDOLL_BONE_COUNT; b++ )
		if( CutNow[b] )
			RagdollTrimHullsForCut( *S, b );

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
