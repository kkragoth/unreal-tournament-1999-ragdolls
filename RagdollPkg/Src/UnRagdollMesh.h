/*=============================================================================
	UnRagdollMesh.h
	Procedurally generated box-mannequin mesh. Subclasses USkeletalMesh so the
	retail renderers use the 5-argument GetFrame, which we override to emit
	view-space vertices computed from the Box3D bone transforms of the owning
	ragdoll carcass (absolute world bone positions transformed by the world->view
	Coords the renderer passes in). This class is C++-only (no UnrealScript
	counterpart).
=============================================================================*/

#ifndef __UNRAGDOLLMESH_H__
#define __UNRAGDOLLMESH_H__

#include "RagdollPkg.h"

class RAGDOLLPKG_API URagdollMesh : public USkeletalMesh
{
	DECLARE_CLASS(URagdollMesh,USkeletalMesh,0,RagdollPkg)

	// UObject interface.
	URagdollMesh();

	// UMesh interface.
	virtual void GetFrame( FVector* Verts, INT Size, FCoords Coords, AActor* Owner, INT& LODRequest );
	virtual FBox GetRenderBoundingBox( const AActor* Owner, UBOOL Exact );
	virtual FSphere GetRenderBoundingSphere( const AActor* Owner, UBOOL Exact );
};

#endif
