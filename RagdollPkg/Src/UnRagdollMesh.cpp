/*=============================================================================
	UnRagdollMesh.cpp
	Procedural box-mannequin mesh for ragdolls.
=============================================================================*/

#include "UnRagdollMesh.h"
#include "RagdollInternal.h"
#include "box3d/box3d.h"

/*-----------------------------------------------------------------------------
	Box corner helper.
-----------------------------------------------------------------------------*/

static const INT VertsPerBox = 8;

// Corner offsets of a box centered at the origin, half extents scaled by DrawScale.
static FVector BoxCorner( const FRagdollBoneDef& D, INT Corner, FLOAT DrawScale )
{
	FLOAT HX = D.HX * DrawScale;
	FLOAT HY = D.HY * DrawScale;
	FLOAT HZ = D.HZ * DrawScale;
	switch( Corner )
	{
		case 0: return FVector( -HX, -HY, -HZ );
		case 1: return FVector(  HX, -HY, -HZ );
		case 2: return FVector(  HX,  HY, -HZ );
		case 3: return FVector( -HX,  HY, -HZ );
		case 4: return FVector( -HX, -HY,  HZ );
		case 5: return FVector(  HX, -HY,  HZ );
		case 6: return FVector(  HX,  HY,  HZ );
		default:return FVector( -HX,  HY,  HZ );
	}
}

/*-----------------------------------------------------------------------------
	Mesh creation.
-----------------------------------------------------------------------------*/

URagdollMesh* CreateRagdollMesh( ARagdollCarcass* Carcass )
{
	guard(CreateRagdollMesh);
	URagdollMesh* Mesh = ConstructObject<URagdollMesh>( URagdollMesh::StaticClass(), UObject::GetTransientPackage(), NAME_None, RF_Public );
	if( !Mesh )
		return NULL;

	FLOAT DrawScale = Carcass ? Carcass->DrawScale : 1.0f;
	if( DrawScale <= 0.0f )
		DrawScale = 1.0f;

	const INT NumVerts = RAGDOLL_BONE_COUNT * VertsPerBox;

	Mesh->Scale         = FVector( 1, 1, 1 );
	Mesh->FrameVerts    = NumVerts;
	Mesh->ModelVerts    = NumVerts;
	Mesh->SpecialVerts  = 0;
	Mesh->AnimFrames    = 1;
	Mesh->WeaponBoneIndex = -1;

	// Rest-pose packed verts (fallback data for non-GetFrame paths).
	for( INT b=0; b<RAGDOLL_BONE_COUNT; b++ )
		for( INT c=0; c<VertsPerBox; c++ )
			new( Mesh->Verts ) FMeshVert( BoxCorner( GRagdollBoneDefs[b], c, 1.0f ) );

	// Single material; the actor's MultiSkins[0] (the dead player's skin) is
	// resolved by the renderer through UMesh::GetTexture. Also add it to the
	// Textures array so the renderer's per-texture lock loop has an entry.
	INT MatIndex = Mesh->Materials.Add();
	Mesh->Materials( MatIndex ).PolyFlags    = PF_TwoSided;
	Mesh->Materials( MatIndex ).TextureIndex = 0;
	if( Carcass )
	{
		UTexture* Skin = Carcass->MultiSkins[0] ? Carcass->MultiSkins[0] : Carcass->Skin;
		if( Skin )
		{
			INT TexIndex = Mesh->Textures.Add();
			Mesh->Textures( TexIndex ) = Skin;
		}
	}

	// Box faces: 6 quads per bone, each split into 2 triangles.
	static const INT FaceVerts[6][4] =
	{
		{ 0, 3, 2, 1 }, // -Z
		{ 4, 5, 6, 7 }, // +Z
		{ 1, 5, 6, 2 }, // +X
		{ 0, 4, 7, 3 }, // -X
		{ 3, 7, 6, 2 }, // +Y
		{ 0, 1, 5, 4 }, // -Y
	};

	for( INT b=0; b<RAGDOLL_BONE_COUNT; b++ )
	{
		INT Base = b * VertsPerBox;
		for( INT f=0; f<6; f++ )
		{
			INT C[4] = { Base+FaceVerts[f][0], Base+FaceVerts[f][1], Base+FaceVerts[f][2], Base+FaceVerts[f][3] };
			for( INT t=0; t<2; t++ )
			{
				INT FaceIdx = Mesh->Faces.Add();
				FMeshFace& Face = Mesh->Faces( FaceIdx );
				Face.MaterialIndex = 0;
				for( INT k=0; k<3; k++ )
				{
					INT Corner = ( t==0 )
						? ( k==2 ? C[2] : ( k==1 ? C[1] : C[0] ) )
						: ( k==2 ? C[3] : ( k==1 ? C[2] : C[0] ) );
					INT WedgeIdx = Mesh->Wedges.Add();
					FMeshWedge& W = Mesh->Wedges( WedgeIdx );
					W.iVertex = Corner;
					W.TexUV.U = ( Corner==C[0] || Corner==C[3] ) ? 0 : 255;
					W.TexUV.V = ( Corner==C[0] || Corner==C[1] ) ? 0 : 255;
					Face.iWedge[k] = WedgeIdx;
				}
			}
		}
	}
	debugf( TEXT("RagdollMesh: CreateRagdollMesh numVerts=%i faces=%i wedges=%i materials=%i animFrames=%i drawScale=%.2f"),
		Mesh->Verts.Num(), Mesh->Faces.Num(), Mesh->Wedges.Num(), Mesh->Materials.Num(), Mesh->AnimFrames, DrawScale );
	return Mesh;
	unguard;
}

/*-----------------------------------------------------------------------------
	URagdollMesh implementation.
-----------------------------------------------------------------------------*/

URagdollMesh::URagdollMesh()
{
}

void URagdollMesh::GetFrame( FVector* Verts, INT Size, FCoords Coords, AActor* Owner, INT& LODRequest )
{
	guard(URagdollMesh::GetFrame);
	FRagdollState* S = NULL;
	if( Owner )
		S = GetRagdollStates().Find( Owner );

	FLOAT DrawScale = ( Owner && Owner->DrawScale > 0.0f ) ? Owner->DrawScale : 1.0f;
	FVector FallbackOrigin = Owner ? Owner->Location : FVector( 0, 0, 0 );

	INT Num = FrameVerts;
	if( LODRequest > 0 && LODRequest < Num )
		Num = LODRequest;

	// GetFrame must emit VIEW-space verts. The engine's reference UMesh::GetFrame
	// (UnMesh.cpp) maps a model-space vertex to view space as
	//   (Verts[i] - Origin).TransformPointBy( Coords * (Owner->Location+PrePivot)
	//     * Owner->Rotation * RotOrigin * FScale(Scale*DrawScale) )
	// and the renderer projects that result directly (no extra actor transform).
	// We reproduce it: convert each Box3D world vertex to actor-local space
	// (ToWorld is orthonormal, so its Transpose() is the world->local inverse),
	// then apply the engine's model->view coords. Our local verts already include
	// DrawScale, so no FScale term is applied.
	FCoords ActorToLocal = Owner ? Owner->ToWorld().Transpose() : GMath.UnitCoords;
	FCoords ModelCoords  = Coords
		* ( Owner ? Owner->Location + Owner->PrePivot : FVector( 0, 0, 0 ) )
		* ( Owner ? Owner->Rotation : FRotator( 0, 0, 0 ) )
		* RotOrigin;

	for( INT i=0; i<Num; i++ )
	{
		INT Bone   = i / VertsPerBox;
		INT Corner = i % VertsPerBox;
		FVector Pos;
		if( S && S->bBuilt )
		{
			b3WorldTransform T = b3Body_GetTransform( S->Bodies[Bone] );
			FVector Center( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
			FVector Off = BoxCorner( GRagdollBoneDefs[Bone], Corner, DrawScale );
			b3Vec3 Rot  = b3RotateVector( T.q, { Off.X, Off.Y, Off.Z } );
			Pos = Center + FVector( Rot.x, Rot.y, Rot.z );
		}
		else
		{
			Pos = FallbackOrigin + BoxCorner( GRagdollBoneDefs[Bone], Corner, DrawScale );
		}
		// Vertices are emitted into a strided buffer: the renderer passes
		// Size = sizeof(buffer element) (e.g. sizeof(FTransTexture) in the
		// software/D3D path, sizeof(FVector) in UTGLR) and reads vertex i at
		// byte offset i*Size. The reference UMesh::GetFrame advances its output
		// pointer by Size bytes per vertex; we must too, or the renderer reads
		// uninitialized memory past our packed FVectors and draws garbage fans.
		FVector View = Pos.TransformPointBy( ActorToLocal ).TransformPointBy( ModelCoords );
		*(FVector*)( (BYTE*)Verts + (ptrdiff_t)i * Size ) = View;
	}
	unguard;
}

FBox URagdollMesh::GetRenderBoundingBox( const AActor* Owner, UBOOL Exact )
{
	guard(URagdollMesh::GetRenderBoundingBox);
	FBox Box( 0 );
	FRagdollState* S = Owner ? GetRagdollStates().Find( (AActor*)Owner ) : NULL;
	FLOAT DrawScale = ( Owner && Owner->DrawScale > 0.0f ) ? Owner->DrawScale : 1.0f;
	if( S && S->bBuilt )
	{
		for( INT i=0; i<RAGDOLL_BONE_COUNT; i++ )
		{
			b3WorldTransform T = b3Body_GetTransform( S->Bodies[i] );
			FVector C( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
			FLOAT R = 30.0f * DrawScale;
			Box += FBox( C - FVector( R, R, R ), C + FVector( R, R, R ) );
		}
		return Box;
	}
	FVector C = Owner ? Owner->Location : FVector( 0, 0, 0 );
	return FBox( C - FVector( 40, 40, 40 ), C + FVector( 40, 40, 40 ) );
	unguard;
}

FSphere URagdollMesh::GetRenderBoundingSphere( const AActor* Owner, UBOOL Exact )
{
	guard(URagdollMesh::GetRenderBoundingSphere);
	FRagdollState* S = Owner ? GetRagdollStates().Find( (AActor*)Owner ) : NULL;
	if( S && S->bBuilt )
	{
		b3WorldTransform T = b3Body_GetTransform( S->Bodies[BONE_Pelvis] );
		FVector C( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
		return FSphere( C, 40.0f * ( Owner ? Owner->DrawScale : 1.0f ) );
	}
	FVector C = Owner ? Owner->Location : FVector( 0, 0, 0 );
	return FSphere( C, 40.0f );
	unguard;
}

/*-----------------------------------------------------------------------------
	Registration.
-----------------------------------------------------------------------------*/

IMPLEMENT_CLASS( URagdollMesh );

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
