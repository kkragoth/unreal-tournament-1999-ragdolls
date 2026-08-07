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

// Runtime state for one ragdolling carcass. Stored in a process-global map
// keyed by the owning actor so URagdollMesh::GetFrame can reach it.
struct FRagdollState
{
	UBOOL         bBuilt;
	UBOOL         bFrameLogged;
	UBOOL         bFrameRangeLogged;
	UBOOL         bOutOfWorldLogged;
	INT           SyncCount;
	b3WorldId     World;   // shared level world (see FRagdollLevelWorld)
	URagdollMesh* Mesh;
	b3BodyId      Bodies[RAGDOLL_BONE_COUNT];
	b3Vec3        RestPositions[RAGDOLL_BONE_COUNT];
	b3Quat        RestRotations[RAGDOLL_BONE_COUNT];

	// Real-mesh mode: per-output-vertex bone assignment (one entry per mesh
	// vertex). VertOffsets holds each vertex's actor-local offset (UU, DrawScale
	// applied) from its assigned bone's rest center, so GetFrame can rigidly
	// attach the player's mesh to the Box3D bodies. Empty in box-mannequin mode.
	TArray<INT>     VertBones;
	TArray<FVector> VertOffsets;
};

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

// Real-mesh factory (UnRagdollMesh.cpp). Captures the owning carcass's player
// mesh (ULodMesh) at its current dead-pose frame and returns a URagdollMesh
// whose vertices are rigidly attached to the nearest Box3D bone (per-vertex
// assignments are stored in S.VertBones/VertOffsets). Returns NULL if the mesh
// is not a usable ULodMesh, in which case the caller keeps the box mannequin.
URagdollMesh* CreatePlayerRagdollMesh( class ARagdollCarcass* Carcass, FRagdollState& S );

#endif
