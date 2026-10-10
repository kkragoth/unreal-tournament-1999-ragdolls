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
	FLOAT Friction; // joint friction torque in Nm (scaled by JointFrictionScale)
};

// Anchor offsets are canonical (relative to the pelvis) and are scaled by
// DrawScale here to match the scaled bodies. Defs selects the active skeleton:
// GRagdollBoneDefs (box fallback) or a per-model dataset's derived bones.
// Live tuning knobs (RagdollCarcass.uc globalconfig floats), filled per
// BuildRagdoll from the carcass so console "set" changes apply to new corpses.
static FLOAT GJointFrictionScale = 1.0f;
static FLOAT GJointLimitScale    = 1.0f;

static FLOAT ClampAngle( FLOAT A )
{
	const FLOAT Lim = 0.98f * PI;
	return A > Lim ? Lim : ( A < -Lim ? -Lim : A );
}

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
		J.targetAngle        = 0.0f;
		J.enableLimit        = true;
		J.lowerAngle         = ClampAngle( Spec.L0 * GJointLimitScale );
		J.upperAngle         = ClampAngle( Spec.L1 * GJointLimitScale );
		J.enableSpring       = false;
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
		b3SphericalJointDef J = b3DefaultSphericalJointDef();
		J.base.bodyIdA       = S.Bodies[BodyA];
		J.base.bodyIdB       = S.Bodies[BodyB];
		J.base.localFrameA   = FrameA;
		J.base.localFrameB   = FrameB;
		J.enableConeLimit    = true;
		J.coneAngle          = Min( Spec.L0 * GJointLimitScale, 0.98f * PI );
		J.enableTwistLimit   = true;
		J.lowerTwistAngle    = ClampAngle( Spec.L1 * GJointLimitScale );
		J.upperTwistAngle    = ClampAngle( Spec.L2 * GJointLimitScale );
		J.enableSpring       = false;
		J.enableMotor        = true;
		J.maxMotorTorque     = Spec.Friction * GJointFrictionScale;
		J.motorVelocity      = b3Vec3_zero;
		S.Joints[BodyB] = b3CreateSphericalJoint( S.World, &J );
	}
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
		KneeFlex[s] = -Var * RandRange( 0.00f, 1.10f );
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
	const FLOAT AngDamp   = RagdollFloatProp( C, TEXT("AngularDamping"), 0.15f );
	const FLOAT DeathSpin = RagdollFloatProp( C, TEXT("DeathSpin"), 2.0f );
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
	const FVector* Anch = DS ? DS->Anchors : CanonicalAnchors;

	// Ranges sized for a corpse (shoulder ~140 deg cone, elbow/knee ~130-140 deg
	// one-sided flexion, hip ~100 deg cone); friction in Nm, tiny versus the
	// limbs' gravity torque (upper arm ~4 Nm, thigh ~10 Nm) so joints flop.
	GJointFrictionScale = RagdollFloatProp( C, TEXT("JointFrictionScale"), 1.0f );
	GJointLimitScale    = RagdollFloatProp( C, TEXT("JointLimitScale"), 1.0f );
	AddJoint( S, BONE_Pelvis,    BONE_Spine,     Defs, Anch[0], DrawScale, { JK_Ball, 0.60f, -0.40f,  0.40f, 0.8f } );
	AddJoint( S, BONE_Spine,     BONE_Neck,      Defs, Anch[1], DrawScale, { JK_Ball, 0.80f, -0.50f,  0.50f, 0.5f } );
	AddJoint( S, BONE_Neck,      BONE_Head,      Defs, Anch[2], DrawScale, { JK_Ball, 0.70f, -0.50f,  0.50f, 0.3f } );
	AddJoint( S, BONE_Spine,     BONE_UpperArmL, Defs, Anch[3], DrawScale, { JK_Ball, 2.40f, -0.80f,  0.80f, 0.3f } );
	AddJoint( S, BONE_Spine,     BONE_UpperArmR, Defs, Anch[4], DrawScale, { JK_Ball, 2.40f, -0.80f,  0.80f, 0.3f } );
	AddJoint( S, BONE_UpperArmL, BONE_ForearmL,  Defs, Anch[5], DrawScale, { JK_Hinge, -0.05f, 2.40f, 0.0f, 0.15f } );
	AddJoint( S, BONE_UpperArmR, BONE_ForearmR,  Defs, Anch[6], DrawScale, { JK_Hinge, -0.05f, 2.40f, 0.0f, 0.15f } );
	AddJoint( S, BONE_Pelvis,    BONE_ThighL,    Defs, Anch[7], DrawScale, { JK_Ball, 1.20f, -0.50f,  0.50f, 0.5f } );
	AddJoint( S, BONE_Pelvis,    BONE_ThighR,    Defs, Anch[8], DrawScale, { JK_Ball, 1.20f, -0.50f,  0.50f, 0.5f } );
	AddJoint( S, BONE_ThighL,    BONE_ShinL,     Defs, Anch[9], DrawScale, { JK_Hinge, -2.00f, 0.05f, 0.0f, 0.25f } );
	AddJoint( S, BONE_ThighR,    BONE_ShinR,     Defs, Anch[10], DrawScale, { JK_Hinge, -2.00f, 0.05f, 0.0f, 0.25f } );
	// Feet are skipped from real-mesh ragdolls; only the box skeleton keeps the
	// ankle joints (bodies 12/13 only exist in the box path).
	if( !DS )
	{
		AddJoint( S, BONE_ShinL, BONE_FootL, Defs, Anch[11], DrawScale, { JK_Ball, 0.60f, -0.35f,  0.35f, 0.1f } );
		AddJoint( S, BONE_ShinR, BONE_FootR, Defs, Anch[12], DrawScale, { JK_Ball, 0.60f, -0.35f,  0.35f, 0.1f } );
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

	// How much of the corpse goes: 40 damage (the stock gib floor) tears off a
	// couple of parts, ~200 takes everything; random spread of 0.6..1.4 on top
	// and a 35% chance of a total gib above 150.
	const FLOAT Frac = ClampF( ( Damage - 30.0f ) / 170.0f, 0.12f, 1.0f );
	INT Want = appRound( Frac * NumCand * ( 0.6f + appFrand() * 0.8f ) );
	if( Damage >= 150 && appFrand() < 0.35f )
		Want = NumCand;
	Want = Clamp( Want, Min( 2, NumCand ), NumCand );

	INT NumCut = 0;
	for( INT i=0; i<Want; i++ )
		if( DetachBody( *S, Cand[i] ) )
			NumCut++;
	RagdollSplitIslands( *S );

	const FLOAT Base = 4.0f + Min( Max( Damage, 0 ), 250 ) * 0.03f;   // 4..11.5 m/s
	for( INT b=0; b<S->NumBodies; b++ )
	{
		if( !S->Alive[b] )
			continue;
		// The trunk (whatever is still joined to the pelvis) is heavy and
		// stays roughly put; detached islands fly.
		const FLOAT Weight = ( RagdollIslandRoot( *S, b ) == BONE_Pelvis ) ? 0.35f : 1.0f;
		b3WorldTransform T = b3Body_GetTransform( S->Bodies[b] );
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
		b3Vec3 V = b3Body_GetLinearVelocity( S->Bodies[b] );
		V.x += DeltaV.x; V.y += DeltaV.y; V.z += DeltaV.z;
		b3Body_SetLinearVelocity( S->Bodies[b], V );
		b3Vec3 W;
		W.x = ( appFrand() * 2.0f - 1.0f ) * 9.0f * Weight;
		W.y = ( appFrand() * 2.0f - 1.0f ) * 9.0f * Weight;
		W.z = ( appFrand() * 2.0f - 1.0f ) * 9.0f * Weight;
		b3Body_SetAngularVelocity( S->Bodies[b], W );
	}
	const INT Remaining = NumCand - NumCut;
	*(INT*)Result = Remaining;
	debugf( TEXT("RagdollPkg: gib %s dmg=%i cut=%i/%i remaining=%i base=%.1f m/s"), GetName(), Damage, NumCut, NumCand, Remaining, Base );
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

// Register ARagdollCarcass.
IMPLEMENT_CLASS( ARagdollCarcass );

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
