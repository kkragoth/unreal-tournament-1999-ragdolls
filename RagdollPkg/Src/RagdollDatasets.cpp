/*=============================================================================
	RagdollDatasets.cpp
	Per-model precomputed ragdoll dataset lookup. The datasets themselves are
	generated offline by tools/build_rig.py (RagdollPkg\Src\RagdollDatasets.h):
	bind-space verts/faces/wedges/materials + derived bone defs + joint anchors
	+ per-vertex bone binding. A live player mesh name (e.g. "Commando") maps
	to its Selection viewer dataset (e.g. "SelectionMale1").
=============================================================================*/

#include "RagdollPkg.h"
#include "RagdollInternal.h"
#include "RagdollDatasets.h"

const FRagdollDatasetEntry* FindRagdollDataset( const TCHAR* LiveMeshName )
{
	if( !LiveMeshName || !*LiveMeshName )
		return NULL;
	for( INT i=0; i<GRagdollDatasetCount; i++ )
		if( appStrcmp( GRagdollDatasetTable[i].LiveMeshName, LiveMeshName ) == 0 )
			return &GRagdollDatasetTable[i];
	return NULL;
}

UBOOL RagdollForceBoxes( UObject* Obj )
{
	if( !Obj )
		return 0;
	const FString V = Obj->GetPropertyText( TEXT("bBoxMannequins") );
	return appStricmp( *V, TEXT("True") ) == 0 || appAtoi( *V ) != 0;
}
