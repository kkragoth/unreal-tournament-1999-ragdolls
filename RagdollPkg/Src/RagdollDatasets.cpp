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

FLOAT RagdollFloatProp( UObject* Obj, const TCHAR* Name, FLOAT Default )
{
	guard(RagdollFloatProp);
	if( !Obj )
		return Default;
	for( TFieldIterator<UFloatProperty> It( Obj->GetClass() ); It; ++It )
	{
		UFloatProperty* P = *It;
		if( appStricmp( P->GetName(), Name ) == 0 )
		{
			FLOAT V = *(FLOAT*)( (BYTE*)Obj + P->Offset );
			return V < 0.0f ? Default : V;
		}
	}
	return Default;
	unguard;
}

UBOOL RagdollBoolProp( UObject* Obj, const TCHAR* Name, UBOOL Default )
{
	guard(RagdollBoolProp);
	// Read a live bool value through the script property list. The console
	// "set" command updates the class default immediately and new carcasses
	// inherit it at spawn, so this is always current - without depending on
	// which ini file/path GConfig has cached.
	if( !Obj )
		return Default;
	for( TFieldIterator<UBoolProperty> It( Obj->GetClass() ); It; ++It )
	{
		UBoolProperty* P = *It;
		if( appStricmp( P->GetName(), Name ) == 0 )
			return ( *(BITFIELD*)( (BYTE*)Obj + P->Offset ) & P->BitMask ) ? 1 : 0;
	}
	return Default;
	unguard;
}

UBOOL RagdollForceBoxes( UObject* Obj )
{
	guard(RagdollForceBoxes);
	return RagdollBoolProp( Obj, TEXT("bBoxMannequins"), 0 );
	unguard;
}
