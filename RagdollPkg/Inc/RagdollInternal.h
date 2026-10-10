/*=============================================================================
	RagdollInternal.h
	Package-internal state shared between the native carcass (RagdollCarcass.cpp)
	and the procedural mesh (UnRagdollMesh.cpp). Not part of the public API.
=============================================================================*/

#ifndef __RAGDOLLINTERNAL_H__
#define __RAGDOLLINTERNAL_H__

#include "RagdollPkg.h"
#include "box3d/types.h"
#include "box3d/math_functions.h"
#include "box3d/constants.h"

class URagdollMesh;

// Unit conversion. Box3D works in meters; UT works in "Unreal units" (UU).
#define RAGDOLL_U2M 0.02f
#define RAGDOLL_M2U 50.0f

// Shape filter category for the level-geometry mesh body. Ragdoll bodies keep
// the default category (1); geometry gets its own bit so the spawn-clearance
// ray can query "level geometry only" without hitting corpses.
#define RAGDOLL_GEOM_CATEGORY 0x00000002ULL
#define RAGDOLL_BODY_CATEGORY 0x00000001ULL

// Canonical bone layout (Unreal units, Z up). Half extents and center offsets
// are relative to the pelvis origin and are scaled by the actor DrawScale.
struct FRagdollBoneDef
{
	float HX, HY, HZ;    // half extents
	float CX, CY, CZ;    // center offset from pelvis
	float MassKg;        // approximate mass in kilograms
};

#define RAGDOLL_BONE_COUNT 14

// Extra "fragment" bodies created when a gib splits a body's skin into
// several chunks (RagdollFragmentBody). Vertex bind bone indices >=
// RAGDOLL_BONE_COUNT address them; every helper that maps a bone index to a
// Box3D body / bind centre goes through RagdollBody / RagdollBoneCenter.
#define RAGDOLL_MAX_EXTRA 24
#define RAGDOLL_MAX_BONES ( RAGDOLL_BONE_COUNT + RAGDOLL_MAX_EXTRA )

enum ERagdollBone
{
	BONE_Pelvis     = 0,
	BONE_Spine      = 1,
	BONE_Neck       = 2,
	BONE_Head       = 3,
	BONE_UpperArmL  = 4,
	BONE_UpperArmR  = 5,
	BONE_ForearmL   = 6,
	BONE_ForearmR   = 7,
	BONE_ThighL     = 8,
	BONE_ThighR     = 9,
	BONE_ShinL      = 10,
	BONE_ShinR      = 11,
	BONE_FootL      = 12,
	BONE_FootR      = 13
};

// Real-mesh mode per-vertex smooth skin data (one entry per mesh vertex). Each
// entry holds up to 3 bones with normalized weights summing to 1 (weight[0]
// dominant, lower weights may be 0); Off is the bind-space offset (UU,
// DrawScale=1) from the bone center, scaled by DrawScale in GetFrame.
struct FRagdollVertexBind
{
	INT    Bone[3];
	FVector Off[3];
	FLOAT  Weight[3];
};

// Runtime state for one ragdolling carcass. Stored in a process-global map
// keyed by the owning actor so URagdollMesh::GetFrame can reach it.
struct FRagdollState
{
	UBOOL         bBuilt;
	UBOOL         bFrameLogged;
	UBOOL         bFrameRangeLogged;
	UBOOL         bOutOfWorldLogged;
	INT   SyncCount;
	INT   NumBodies;  // bodies actually created (12 real-mesh, 14 box)
	b3WorldId     World;   // shared level world (see FRagdollLevelWorld)
	URagdollMesh* Mesh;
	b3BodyId      Bodies[RAGDOLL_BONE_COUNT];
	b3Vec3        RestPositions[RAGDOLL_BONE_COUNT];
	b3Quat        RestRotations[RAGDOLL_BONE_COUNT];
	const FRagdollBoneDef* BoneDefs;  // active skeleton (dataset or canonical)

	// Death pose captured from the dying pawn (set by SetDeathPose before
	// BuildRagdoll): the corpse spawns facing the pawn's yaw with its arms
	// raised toward the aim pitch instead of frozen in the arms-down bind pose.
	INT   SpawnYaw;      // dead pawn facing (Unreal units)
	INT   SpawnPitch;    // dead pawn aim pitch (Unreal units)
	UBOOL bHasDeathPose;

	// Per-vertex smooth skin data (indexed by mesh vertex); empty in box mode.
	TArray<FRagdollVertexBind> VertBinds;

	// Gore state. Joints[child] is the Box3D joint attaching body 'child' to
	// its parent (GRagdollBoneParent); Cut[child] once that joint is destroyed
	// (dismemberment/gib), Alive[body] false once the body itself is removed
	// (decapitation hands the head to the stock UTHeads chunk). BindPos holds
	// the bind-space position (UU, DrawScale=1) of every mesh vertex, grown as
	// seam vertices are duplicated; CapMaterial indexes the dark wound material
	// used by the stump caps (-1 = none, caps use material 0).
	b3JointId Joints[RAGDOLL_BONE_COUNT];
	UBOOL     Alive[RAGDOLL_BONE_COUNT];
	UBOOL     Cut[RAGDOLL_BONE_COUNT];
	const struct FRagdollDatasetEntry* Dataset;
	TArray<FVector> BindPos;
	INT       CapMaterial;
	INT       NumSplits;      // how many times the mesh has been re-split (diagnostics)

	// Fragment bodies (see RAGDOLL_MAX_EXTRA). ExtraCenter is the fragment's
	// bind-space centre (UU, DrawScale=1), ExtraRadius its bounding radius.
	// Per-corpse copies of the skin textures that wounds are painted into
	// (one per MultiSkins slot, made lazily on the first wound in that slot).
	UTexture* WoundSkins[8];

	INT       NumExtra;
	b3BodyId  ExtraBodies[RAGDOLL_MAX_EXTRA];
	FVector   ExtraCenter[RAGDOLL_MAX_EXTRA];
	FLOAT     ExtraRadius[RAGDOLL_MAX_EXTRA];

	FRagdollState()
	: bBuilt( 0 ), bFrameLogged( 0 ), bFrameRangeLogged( 0 ), bOutOfWorldLogged( 0 )
	, SyncCount( 0 ), NumBodies( 0 ), World( b3_nullWorldId ), Mesh( NULL ), BoneDefs( NULL )
	, SpawnYaw( 0 ), SpawnPitch( 0 ), bHasDeathPose( 0 )
	, Dataset( NULL ), CapMaterial( -1 ), NumSplits( 0 ), NumExtra( 0 )
	{
		for( INT t=0; t<8; t++ )
			WoundSkins[t] = NULL;
		for( INT e=0; e<RAGDOLL_MAX_EXTRA; e++ )
		{
			ExtraBodies[e] = b3_nullBodyId;
			ExtraCenter[e] = FVector( 0, 0, 0 );
			ExtraRadius[e] = 0.0f;
		}
		for( INT i=0; i<RAGDOLL_BONE_COUNT; i++ )
		{
			Bodies[i]        = b3_nullBodyId;
			RestPositions[i] = b3Vec3_zero;
			RestRotations[i] = b3Quat_identity;
			Joints[i]        = b3_nullJointId;
			Alive[i]         = 0;
			Cut[i]           = 0;
		}
	}
};

// Parent body of each bone (-1 for the pelvis root); the joint of body i
// attaches i to GRagdollBoneParent[i].
extern const INT GRagdollBoneParent[RAGDOLL_BONE_COUNT];

// Root body of the rigid island 'Body' belongs to: walk up the parent chain
// until a cut joint (or the pelvis). Bodies in the same island move together.
INT RagdollIslandRoot( const FRagdollState& S, INT Body );

// True when 'Body' is 'Ancestor' or hangs below it in the bone tree.
UBOOL RagdollInSubtree( INT Body, INT Ancestor );

// Bone index -> Box3D body / liveness / bind-space centre, covering both the
// skeleton bodies (< RAGDOLL_BONE_COUNT) and the fragment bodies.
INT      RagdollNumBones( const FRagdollState& S );
b3BodyId RagdollBody( const FRagdollState& S, INT Bone );
UBOOL    RagdollBoneAlive( const FRagdollState& S, INT Bone );
FVector  RagdollBoneCenter( const FRagdollState& S, INT Bone );

// Split the skin bound to 'Body' into K chunks (k-means on bind positions):
// chunk 0 keeps the existing body, every other chunk gets a new box body at
// its place in the world with the parent's velocity. Call after
// RagdollSplitIslands (so the body's vertices are single-bone) and run
// RagdollSplitIslands again afterwards to close the new seams. Returns the
// number of fragment bodies created.
INT RagdollFragmentBody( FRagdollState& S, INT Body, INT K, FLOAT DrawScale );

// Shared level geometry world: ONE Box3D world per level that holds the level
// BSP baked into a static triangle mesh plus every ragdoll's bodies/joints.
// Built lazily on the first ragdoll spawn and torn down when the last corpse
// disappears. Because the world origin coincides with the level origin (UU
// mapped by RAGDOLL_U2M), BSP vertices drop straight into Box3D coordinates.
struct FRagdollLevelWorld
{
	ULevel*     Level;          // ULevel whose BSP this world was built from
	b3WorldId   World;          // Box3D world handle (shared by all corpses)
	b3BodyId    Geometry;       // static body holding the level triangle mesh
	b3MeshData* Mesh;           // owned mesh data; must outlive the world
	FLOAT       MinZ;           // lowest solid BSP vertex Z (UU), for despawn
	FLOAT       LastStepTime;   // last appSeconds() stamp we stepped (dedupe)
	UBOOL       bBuilt;
};

// The process-global level world (one instance; only one level runs at a time).
FRagdollLevelWorld& GetRagdollLevelWorld();

// Get the level world for the given level, (re)building it if the level changed.
FRagdollLevelWorld& GetRagdollLevelWorldFor( ULevel* Level );

// Destroy the level world (mesh + world). Safe to call when not built.
void TeardownLevelWorld();

// Lift any corpse bodies that spawn embedded in the level mesh (ramp/stairs
// deaths) so the first Box3D step does not explode them. Call after the bodies
// and joints are built, before the first StepRagdoll.
void ClearCorpseSpawn( FRagdollState& S, FRagdollLevelWorld& LW );

// Canonical bone definitions (shared with the mesh builder).
extern const FRagdollBoneDef GRagdollBoneDefs[RAGDOLL_BONE_COUNT];

// Process-global actor -> state map (lazily initialized on first use).
TMap<AActor*, FRagdollState>& GetRagdollStates();

// Mesh factory (UnRagdollMesh.cpp). Returns a new URagdollMesh built from the
// canonical bone definitions for the given carcass.
URagdollMesh* CreateRagdollMesh( class ARagdollCarcass* Carcass );

// Precomputed per-model ragdoll dataset (bind space, Unreal units, DrawScale=1,
// pelvis at origin). Generated by tools/build_rig.py into RagdollDatasets.h.
struct FRagdollDatasetEntry
{
	const TCHAR*     LiveMeshName;   // pawn mesh name, e.g. TEXT("Commando")
	const TCHAR*     DatasetName;    // source selection mesh, for logging
	INT              NumVerts;       // dense wedge-referenced verts
	INT              NumFaces;
	INT              NumWedges;
	INT              NumMats;
	const FVector*       Verts;      // bind-space positions
	const FMeshFace*     Faces;
	const FMeshWedge*    Wedges;
	const FMeshMaterial* Materials;
	const FRagdollBoneDef* Bones;    // 14 derived defs
	const FVector*       Anchors;    // 13 joint anchors (BuildRagdollImpl order)
	const INT*           BindBones;  // NumVerts*3, index 0 is dominant
	const FVector*       BindOffsets;// NumVerts*3 (UU, DrawScale=1)
	const FLOAT*         BindWeights;// NumVerts*3, sum to 1 per vertex
};

// Look up the dataset for a live player mesh name; NULL when none (the caller
// falls back to the procedural box mannequin).
const FRagdollDatasetEntry* FindRagdollDataset( const TCHAR* LiveMeshName );

// Read a FLOAT script property by name through the class property list (same
// mechanism as RagdollForceBoxes, so console "set" tuning is picked up by the
// next corpse). Returns Default when the property is missing or negative.
FLOAT RagdollFloatProp( UObject* Obj, const TCHAR* Name, FLOAT Default );

// True when the class config flag bBoxMannequins forces box-mannequin corpses,
// overriding any available real-mesh dataset. Toggled at runtime with
// "set RagdollPkg.RagdollCarcass bBoxMannequins 1" or via UnrealTournament.ini
// ([RagdollPkg.RagdollCarcass]); affects corpses spawned afterwards.
UBOOL RagdollForceBoxes( UObject* Obj );

// Rebuild the vertex->triangle adjacency (Connects/VertLinks) from the mesh's
// Faces/Wedges. Must be re-run after any face/vertex rewrite.
void RagdollBuildMeshAdjacency( URagdollMesh* Mesh );

// Gore mesh surgery (RagdollGore.cpp). RagdollSplitIslands re-partitions the
// skinned mesh along the current Cut[] joints: every vertex is re-bound to its
// own island only, faces straddling two islands are duplicated per side with
// the foreign vertices copied and bound to the near side, and each resulting
// rim is fan-capped from the cut centroid with the wound material. Safe to
// call repeatedly (later cuts only touch faces straddling the new cut).
// RagdollRemoveIsland deletes every face of the island rooted at 'Root' and
// destroys its bodies (used for decapitation: the stock head chunk is the
// visible head). Both are no-ops for box-mannequin corpses.
void RagdollSplitIslands( FRagdollState& S );
void RagdollRemoveIsland( FRagdollState& S, INT Root );

// The dark-red wound texture used for stump caps (created once, rooted).
UTexture* RagdollCapTexture();

// Paint a wound into the corpse's skin at the mesh face nearest HitLocation.
// The skin slot's texture is copied per corpse on first use (P8 only) and
// the blotch is blended into every mip; style (size, colour, speckle,
// scatter) follows the damage type name and the damage amount.
void RagdollPaintWound( FRagdollState& S, class ARagdollCarcass* C, const FVector& HitLocation, INT Damage, const TCHAR* DamageType );

// Real-mesh factory (UnRagdollMesh.cpp). Builds a URagdollMesh from the
// precomputed dataset for the owning carcass's player mesh (ULodMesh), filling
// S.VertBones/S.VertOffsets so GetFrame can rigidly attach the Selection mesh
// to the Box3D bodies. Returns NULL when the mesh has no dataset, in which
// case the caller keeps the box mannequin.
URagdollMesh* CreatePlayerRagdollMesh( class ARagdollCarcass* Carcass, FRagdollState& S );

#endif
