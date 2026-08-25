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

UBOOL RagdollForceBoxes()
{
	guard(RagdollForceBoxes);
	// bBoxMannequins is a globalconfig var on ARagdollCarcass, persisted under
	// [RagdollPkg.RagdollCarcass] in the game ini. The console "set" command
	// saves it immediately, so per-spawn reads are always current.
	FString Ini = FString( appBaseDir() ) + TEXT("UnrealTournament.ini");
	TCHAR Buf[32] = TEXT("");
	GConfig->GetString( TEXT("RagdollPkg.RagdollCarcass"), TEXT("bBoxMannequins"), Buf, ARRAY_COUNT(Buf), *Ini );
	return appStricmp( Buf, TEXT("True") ) == 0 || appAtoi( Buf ) != 0;
	unguard;
}
