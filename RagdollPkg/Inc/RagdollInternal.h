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
	b3WorldId     World;
	b3BodyId      Ground;
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
