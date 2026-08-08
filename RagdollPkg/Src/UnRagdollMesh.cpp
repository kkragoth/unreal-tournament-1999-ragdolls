/*=============================================================================
	UnRagdollMesh.cpp
	Ragdoll mesh: either the procedural box mannequin (fallback) or the dead
	player's real mesh built from a precomputed per-model dataset (Selection
	viewer mesh, bind-space geometry + per-vertex bone assignment) and rigidly
	attached to the Box3D bones via GetFrame's boneWorld * bindOffset emission.
=============================================================================*/

#include "UnRagdollMesh.h"
#include "RagdollInternal.h"
#include "box3d/box3d.h"

// TEMP TEST: render the real mesh as a static captured pose (bones at rest)
// instead of live Box3D bone transforms. Set to 1 to isolate whether the
// renderer chokes on the mesh data vs the bone-deformation emission.
#define RAGDOLL_TEST_STATIC 0

/*-----------------------------------------------------------------------------
	Box corner helper (box-mannequin mode).
-----------------------------------------------------------------------------*/

static const INT VertsPerBox = 8;

// Build the vertex->triangle adjacency (Connects/VertLinks) from the mesh's own
// Faces/Wedges. URender::DrawLodMesh's per-vertex lighting-normal pass reads
// Connects(iVertex)/VertLinks(...) directly for every drawn vertex; empty
// arrays are out-of-bounds reads that crash the renderer.
static void BuildMeshAdjacency( URagdollMesh* Mesh )
{
	Mesh->Connects.Empty();
	Mesh->VertLinks.Empty();
	Mesh->Connects.Add( Mesh->FrameVerts );
	for( INT v=0; v<Mesh->FrameVerts; v++ )
	{
		FMeshVertConnect& C = Mesh->Connects( v );
		C.NumVertTriangles   = 0;
		C.TriangleListOffset = Mesh->VertLinks.Num();
		for( INT f=0; f<Mesh->Faces.Num(); f++ )
		{
			const FMeshFace& Face = Mesh->Faces( f );
			for( INT j=0; j<3; j++ )
				if( Mesh->Wedges( Face.iWedge[j] ).iVertex == v )
				{
					Mesh->VertLinks.Add( f );
					C.NumVertTriangles++;
					break;
				}
		}
	}
}

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
	Mesh creation (box-mannequin fallback).
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
	// LOD scaffolding. URender::DrawLodMesh reads FaceLevel/CollapseWedgeThus/
	// CollapsePointThus for every face; leaving them empty is an out-of-bounds
	// read (garbage triangles + access violation). Pin full detail so no LOD
	// collapse happens and wedge iVertex indexes our dense FrameVerts emission.
	Mesh->LODMinVerts   = Mesh->ModelVerts;
	Mesh->LODStrength   = 0.0f;
	Mesh->LODMorph      = 0.0f;
	Mesh->LODZDisplace  = 0.0f;
	Mesh->LODHysteresis = 0.0f;
	Mesh->MeshScaleMax  = 1.0f;
	Mesh->FaceLevel.Add( Mesh->Faces.Num() );
	for( INT f=0; f<Mesh->Faces.Num(); f++ )
		Mesh->FaceLevel( f ) = 0;
	Mesh->CollapseWedgeThus.Add( Mesh->Wedges.Num() );
	for( INT w=0; w<Mesh->Wedges.Num(); w++ )
		Mesh->CollapseWedgeThus( w ) = (_WORD)w;
	Mesh->CollapsePointThus.Add( Mesh->FrameVerts );
	for( INT v=0; v<Mesh->FrameVerts; v++ )
		Mesh->CollapsePointThus( v ) = (_WORD)v;
	// Per-vertex lighting adjacency; the renderer reads Connects/VertLinks for
	// every drawn vertex (see comment above BuildMeshAdjacency).
	BuildMeshAdjacency( Mesh );
	debugf( TEXT("RagdollMesh: CreateRagdollMesh(numBoxVerts=%i)"), Mesh->FrameVerts );
	return Mesh;
	unguard;
}

/*-----------------------------------------------------------------------------
	Real-mesh creation from a precomputed per-model dataset.
-----------------------------------------------------------------------------*/

URagdollMesh* CreatePlayerRagdollMesh( ARagdollCarcass* Carcass, FRagdollState& S )
{
	guard(CreatePlayerRagdollMesh);
	ULodMesh* Src = Cast<ULodMesh>( Carcass ? Carcass->Mesh : NULL );
	if( !Src )
		return NULL;
	const FRagdollDatasetEntry* DS = FindRagdollDataset( Src->GetName() );
	if( !DS )
	{
		debugf( TEXT("RagdollMesh: no dataset for mesh=%s -> box fallback"), Src->GetName() );
		return NULL;
	}

	const INT NumVerts = DS->NumVerts;
	if( NumVerts <= 0 || DS->NumFaces <= 0 || DS->NumWedges <= 0 || DS->NumMats <= 0 )
		return NULL;

	// Per-vertex smooth skinning for GetFrame (bind space, DrawScale=1; GetFrame
	// scales the offsets by the actor DrawScale when emitting). Each vertex has
	// up to 3 weighted bones (weight[0] dominant, weights sum to 1).
	S.VertBinds.Empty( NumVerts );
	S.VertBinds.Add( NumVerts );
	for( INT k=0; k<NumVerts; k++ )
	{
		FRagdollVertexBind& B = S.VertBinds( k );
		for( INT w=0; w<3; w++ )
		{
			B.Bone[w]   = DS->BindBones[ k*3 + w ];
			B.Off[w]    = DS->BindOffsets[ k*3 + w ];
			B.Weight[w] = DS->BindWeights[ k*3 + w ];
		}
	}

	URagdollMesh* Mesh = ConstructObject<URagdollMesh>( URagdollMesh::StaticClass(), UObject::GetTransientPackage(), NAME_None, RF_Public );
	if( !Mesh )
		return NULL;

	// GetFrame emits world-space verts from the Box3D bodies, so the engine's
	// model->view chain must not re-apply any import transform.
	Mesh->Scale           = FVector( 1, 1, 1 );
	Mesh->Origin          = FVector( 0, 0, 0 );
	Mesh->RotOrigin       = FRotator( 0, 0, 0 );
	Mesh->FrameVerts      = NumVerts;
	Mesh->ModelVerts      = NumVerts;
	Mesh->SpecialVerts    = 0;
	Mesh->AnimFrames      = 1;
	Mesh->WeaponBoneIndex = -1;

	// Fallback frame data = the bind pose (only read when GetFrame has no state).
	for( INT k=0; k<NumVerts; k++ )
		new( Mesh->Verts ) FMeshVert( DS->Verts[k] );

	// Geometry from the dataset. Wedge iVertex already indexes the dense array.
	Mesh->Faces.Add( DS->NumFaces );
	for( INT f=0; f<DS->NumFaces; f++ )
		Mesh->Faces( f ) = DS->Faces[f];
	Mesh->Wedges.Add( DS->NumWedges );
	for( INT w=0; w<DS->NumWedges; w++ )
		Mesh->Wedges( w ) = DS->Wedges[w];
	Mesh->Materials.Add( DS->NumMats );
	for( INT m=0; m<DS->NumMats; m++ )
		Mesh->Materials( m ) = DS->Materials[m];

	// Textures: the renderer resolves each face via UMesh::GetTexture(Count,
	// Owner) = Owner->MultiSkins[Count] (the Carcass copies the dead player's
	// MultiSkins in Initfor), so this array is just the fallback for empty slots.
	for( INT m=0; m<DS->NumMats; m++ )
	{
		UTexture* Skin = NULL;
		if( Carcass->MultiSkins[m] )
			Skin = Carcass->MultiSkins[m];
		else if( Carcass->Skin )
			Skin = Carcass->Skin;
		INT TexIndex = Mesh->Textures.Add();
		Mesh->Textures( TexIndex ) = Skin;
	}

	// LOD scaffolding (see CreateRagdollMesh for why this must exist).
	Mesh->LODMinVerts   = Mesh->ModelVerts;
	Mesh->LODStrength   = 0.0f;
	Mesh->LODMorph      = 0.0f;
	Mesh->LODZDisplace  = 0.0f;
	Mesh->LODHysteresis = 0.0f;
	Mesh->MeshScaleMax  = 1.0f;
	Mesh->FaceLevel.Add( Mesh->Faces.Num() );
	for( INT f=0; f<Mesh->Faces.Num(); f++ )
		Mesh->FaceLevel( f ) = 0;
	Mesh->CollapseWedgeThus.Add( Mesh->Wedges.Num() );
	for( INT w=0; w<Mesh->Wedges.Num(); w++ )
		Mesh->CollapseWedgeThus( w ) = (_WORD)w;
	Mesh->CollapsePointThus.Add( Mesh->FrameVerts );
	for( INT v=0; v<Mesh->FrameVerts; v++ )
		Mesh->CollapsePointThus( v ) = (_WORD)v;
	BuildMeshAdjacency( Mesh );

	debugf( TEXT("RagdollMesh: CreatePlayerRagdollMesh mesh=%s dataset=%s verts=%i faces=%i wedges=%i mats=%i"),
		Src->GetName(), DS->DatasetName, NumVerts, DS->NumFaces, DS->NumWedges, DS->NumMats );
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
	// Real-mesh mode: per-vertex smooth skin data (indexes the Box3D bodies
	// directly). Box-mannequin mode only when the vert count is exactly the
	// 14-bone box layout. Anything else uses the captured fallback pose so the
	// mesh never indexes Bodies[] out of bounds.
	UBOOL bRealMesh = S && S->bBuilt && S->VertBinds.Num() == FrameVerts && FrameVerts != RAGDOLL_BONE_COUNT * VertsPerBox;
	UBOOL bBoxMesh  = S && S->bBuilt && FrameVerts == RAGDOLL_BONE_COUNT * VertsPerBox;
	if( !bRealMesh && !bBoxMesh && LODRequest > 0 && LODRequest < Num )
		Num = LODRequest;

	if( S && S->bBuilt && !S->bFrameLogged )
	{
		S->bFrameLogged = true;
		debugf( TEXT("RagdollMesh: GetFrame first owner=%s FrameVerts=%i VertBinds=%i real=%i box=%i LOD=%i"),
			Owner ? Owner->GetName() : TEXT("none"), FrameVerts, S->VertBinds.Num(), bRealMesh, bBoxMesh, LODRequest );
		if( bRealMesh )
		{
			for( INT d=0; d<8 && d<Num; d++ )
			{
				const FRagdollVertexBind& B = S->VertBinds(d);
				FVector Pos( 0, 0, 0 );
				for( INT k=0; k<3; k++ )
				{
					if( B.Weight[k] <= 0.0f )
						break;
					b3WorldTransform T = b3Body_GetTransform( S->Bodies[B.Bone[k]] );
					FVector Center( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
					b3Vec3 Rot = b3RotateVector( T.q, { B.Off[k].X * DrawScale, B.Off[k].Y * DrawScale, B.Off[k].Z * DrawScale } );
					Pos += ( Center + FVector( Rot.x, Rot.y, Rot.z ) ) * B.Weight[k];
				}
				debugf( TEXT("RagdollMesh: realvert[%i] bone=%i w=%.2f world=(%.1f,%.1f,%.1f) ownerloc=(%.1f,%.1f,%.1f)"),
					d, B.Bone[0], B.Weight[0], Pos.X, Pos.Y, Pos.Z,
					Owner ? Owner->Location.X : 0, Owner ? Owner->Location.Y : 0, Owner ? Owner->Location.Z : 0 );
			}
		}
	}

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
		FVector Pos;
#if RAGDOLL_TEST_STATIC
		// Static test pose: every mesh renders from its captured rest data, with
		// NO live Box3D body reads. This isolates the renderer (mesh data / face
		// loop) from the physics emission so the DrawLodMesh crash can be debugged
		// without live bone transforms in the path.
		if( bRealMesh )
		{
			INT Bone = S->VertBones(i);
			FVector Off = S->VertOffsets(i);
			Pos = FallbackOrigin + Off
				+ FVector(
					GRagdollBoneDefs[Bone].CX * DrawScale,
					GRagdollBoneDefs[Bone].CY * DrawScale,
					GRagdollBoneDefs[Bone].CZ * DrawScale );
		}
		else if( bBoxMesh )
		{
			INT Bone   = i / VertsPerBox;
			INT Corner = i % VertsPerBox;
			Pos = FallbackOrigin + FVector(
				GRagdollBoneDefs[Bone].CX * DrawScale,
				GRagdollBoneDefs[Bone].CY * DrawScale,
				GRagdollBoneDefs[Bone].CZ * DrawScale )
				+ BoxCorner( GRagdollBoneDefs[Bone], Corner, DrawScale );
		}
		else
		{
			FVector Off = this->Verts.IsValidIndex( i ) ? this->Verts(i).Vector() : FVector( 0, 0, 0 );
			Pos = FallbackOrigin + Off;
		}
#else
		// Live skinned emission: every vertex is rigidly attached to its Box3D
		// bone's current world transform.
		if( bRealMesh )
		{
			// Real-mesh mode: smooth multi-bone skinning. Each vertex blends up
			// to 3 weighted bone transforms (the weights were computed offline so
			// joint-region verts transition smoothly between adjacent bones
			// instead of tearing when the limbs separate).
			const FRagdollVertexBind& B = S->VertBinds(i);
			Pos = FVector( 0, 0, 0 );
			for( INT k=0; k<3; k++ )
			{
				if( B.Weight[k] <= 0.0f )
					break;
				INT Bone = B.Bone[k];
				FVector Off = B.Off[k];
				b3WorldTransform T = b3Body_GetTransform( S->Bodies[Bone] );
				FVector Center( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
				b3Vec3 Rot = b3RotateVector( T.q, { Off.X * DrawScale, Off.Y * DrawScale, Off.Z * DrawScale } );
				Pos += ( Center + FVector( Rot.x, Rot.y, Rot.z ) ) * B.Weight[k];
			}
		}
		else if( bBoxMesh )
		{
			INT Bone   = i / VertsPerBox;
			INT Corner = i % VertsPerBox;
			b3WorldTransform T = b3Body_GetTransform( S->Bodies[Bone] );
			FVector Center( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
			FVector Off = BoxCorner( GRagdollBoneDefs[Bone], Corner, DrawScale );
			b3Vec3 Rot  = b3RotateVector( T.q, { Off.X, Off.Y, Off.Z } );
			Pos = Center + FVector( Rot.x, Rot.y, Rot.z );
		}
		else
		{
			// No state (mesh without a built ragdoll, or box fallback): emit the
			// captured reference pose relative to the actor. For the real mesh
			// Verts hold mesh-local coords and we neutralize Origin, so adding
			// the actor origin reproduces the captured pose; for the box mesh
			// Verts are bone-local box corners and the actor origin approximates
			// the pelvis, which is fine for an unattached display.
			FVector Off = this->Verts.IsValidIndex( i ) ? this->Verts(i).Vector() : FVector( 0, 0, 0 );
			Pos = FallbackOrigin + Off;
		}
#endif
		// Vertices are emitted into a strided buffer: the renderer passes
		// Size = sizeof(buffer element) (e.g. sizeof(FTransTexture) in the
		// software/D3D path, sizeof(FVector) in UTGLR) and reads vertex i at
		// byte offset i*Size. The reference UMesh::GetFrame advances its output
		// pointer by Size bytes per vertex; we must too, or the renderer reads
		// uninitialized memory past our packed FVectors and draws garbage fans.
		FVector View = Pos.TransformPointBy( ActorToLocal ).TransformPointBy( ModelCoords );
		*(FVector*)( (BYTE*)Verts + (ptrdiff_t)i * Size ) = View;
	}

	// One-shot diagnostic: flag NaN/inf output and report the emitted range. The
	// renderer's projection math faults on NaN/absurd coordinates, which shows up
	// as a crash inside URender::DrawLodMesh right after GetFrame returns.
	if( S && S->bBuilt && !S->bFrameRangeLogged )
	{
		S->bFrameRangeLogged = true;
		INT nNan = 0, nInf = 0, nOob = 0;
		FVector Mn( 1e30f, 1e30f, 1e30f ), Mx( -1e30f, -1e30f, -1e30f );
		for( INT i=0; i<Num; i++ )
		{
			FVector V = *(FVector*)( (BYTE*)Verts + (ptrdiff_t)i * Size );
			if( V.X != V.X || V.Y != V.Y || V.Z != V.Z )
				nNan++;
			if( Abs( V.X ) > 1e30f || Abs( V.Y ) > 1e30f || Abs( V.Z ) > 1e30f )
				nInf++;
			Mn.X = Min( Mn.X, V.X ); Mn.Y = Min( Mn.Y, V.Y ); Mn.Z = Min( Mn.Z, V.Z );
			Mx.X = Max( Mx.X, V.X ); Mx.Y = Max( Mx.Y, V.Y ); Mx.Z = Max( Mx.Z, V.Z );
		}
		if( bBoxMesh && !B3_IS_NULL( S->Bodies[0] ) )
			nOob = 1;
		// Camera correlation: the view transform Coords is orthonormal, so its
		// transpose is the inverse. (0,0,0).TransformPointBy(Coords.Transpose())
		// recovers the camera's world position, and Coords.XAxis is the view
		// forward axis. depth>0 means the corpse is IN FRONT of the camera, in
		// which case its emitted view-X must be positive or the emission sign
		// is wrong (renderer mirrors the verts -> garbage triangles).
		FVector CamWorld   = FVector( 0, 0, 0 ).TransformPointBy( Coords.Transpose() );
		FVector OwnerWorld = Owner ? Owner->Location : FVector( 0, 0, 0 );
		FLOAT   Depth      = ( OwnerWorld - CamWorld ) | Coords.XAxis;
		debugf( TEXT("RagdollMesh: GetFrameRange owner=%s verts=%i size=%i nan=%i inf=%i body0ok=%i min=(%.1f,%.1f,%.1f) max=(%.1f,%.1f,%.1f) cam=(%.0f,%.0f,%.0f) owner=(%.0f,%.0f,%.0f) depth=%+.0f"),
			Owner ? Owner->GetName() : TEXT("none"), Num, Size, nNan, nInf, nOob, Mn.X, Mn.Y, Mn.Z, Mx.X, Mx.Y, Mx.Z,
			CamWorld.X, CamWorld.Y, CamWorld.Z, OwnerWorld.X, OwnerWorld.Y, OwnerWorld.Z, Depth );

		// Geometry dump: the pelvis box's 8 emitted view verts and how the first
		// two faces index them (via CollapsePointThus). Verifies the triangles the
		// renderer will draw are a coherent box, not a scrambled index fan.
		if( bBoxMesh )
		{
			FString S0, S1;
			for( INT v=0; v<8; v++ )
			{
				FVector V = *(FVector*)( (BYTE*)Verts + (ptrdiff_t)v * Size );
				S0 += FString::Printf( TEXT("[%i](%.0f,%.0f,%.0f)"), v, V.X, V.Y, V.Z );
			}
			for( INT f=0; f<2; f++ )
			{
				const FMeshFace& Face = Faces( f );
				S1 += FString::Printf( TEXT(" face%i:"), f );
				for( INT k=0; k<3; k++ )
				{
					INT w = Face.iWedge[k];
					INT cw = ( _WORD )CollapseWedgeThus( w );
					INT v  = Wedges( cw ).iVertex;
					INT cv = ( _WORD )CollapsePointThus( v );
					S1 += FString::Printf( TEXT("(w%i->v%i->cv%i)"), w, v, cv );
				}
			}
			debugf( TEXT("RagdollMesh: GeomDump verts=%s%s"), *S0, *S1 );
		}
		else if( bRealMesh )
		{
			// Wedge->vertex index bounds probe: out-of-range iVertex (source LOD
			// meshes bake SpecialVerts/RemapAnimVerts into iVertex) draws garbage
			// fans / mirrored triangles.
			INT Oob = 0, OobW = 0;
			for( INT f=0; f<Faces.Num(); f++ )
			{
				const FMeshFace& Face = Faces( f );
				for( INT k=0; k<3; k++ )
				{
					INT w = Face.iWedge[k];
					if( w < 0 || w >= Wedges.Num() ) { OobW++; continue; }
					INT v = Wedges( w ).iVertex;
					if( v < 0 || v >= FrameVerts )
						Oob++;
				}
			}
			FVector V0 = *(FVector*)( (BYTE*)Verts + 0 );
			FVector V1 = *(FVector*)( (BYTE*)Verts + (ptrdiff_t)(Num-1) * Size );
			debugf( TEXT("RagdollMesh: RealProbe owner=%s faces=%i wedges=%i oobvert=%i oobwedge=%i v0=(%.0f,%.0f,%.0f) vLast=(%.0f,%.0f,%.0f)"),
				Owner ? Owner->GetName() : TEXT("none"), Faces.Num(), Wedges.Num(), Oob, OobW, V0.X, V0.Y, V0.Z, V1.X, V1.Y, V1.Z );
			// Sample emitted verts spread across the body so outliers (spiky
			// verts) are visible; a coherent corpse stays within the frame range.
			FString Sam;
			for( INT s=0; s<12; s++ )
			{
				INT i = ( s * ( Num - 1 ) ) / 11;
				FVector V = *(FVector*)( (BYTE*)Verts + (ptrdiff_t)i * Size );
				INT Bone = S->VertBinds.IsValidIndex(i) ? S->VertBinds(i).Bone[0] : -1;
				Sam += FString::Printf( TEXT("[%i@%i](%.0f,%.0f,%.0f)"), i, Bone, V.X, V.Y, V.Z );
			}
			debugf( TEXT("RagdollMesh: RealSample %s"), *Sam );
			// Triangle connectivity probe: every face must connect 3 emitted
			// verts that are spatially near each other. Triangles with a long
			// edge (spike) mean the wedge->vertex mapping is scrambled and the
			// renderer draws radiating fans. BBox diagonal = frame range.
			FVector D = Mx - Mn;
			FLOAT Diag = appSqrt( D.X*D.X + D.Y*D.Y + D.Z*D.Z );
			FLOAT SpikeThresh = Diag * 0.6f;
			INT nSpike = 0, nSkipped = 0;
			for( INT f=0; f<Faces.Num(); f++ )
			{
				const FMeshFace& Face = Faces( f );
				FVector T[3];
				INT nValid = 0;
				for( INT k=0; k<3; k++ )
				{
					INT w = Face.iWedge[k];
					if( w < 0 || w >= Wedges.Num() ) { nSkipped++; continue; }
					INT v = Wedges( w ).iVertex;
					if( v < 0 || v >= Num ) { nSkipped++; continue; }
					T[nValid++] = *(FVector*)( (BYTE*)Verts + (ptrdiff_t)v * Size );
				}
				if( nValid == 3 )
				{
					FLOAT MaxEdge = 0.0f;
					for( INT a=0; a<3; a++ )
						for( INT b=a+1; b<3; b++ )
							MaxEdge = Max( MaxEdge, FDist( T[a], T[b] ) );
					if( MaxEdge > SpikeThresh )
						nSpike++;
				}
			}
			debugf( TEXT("RagdollMesh: RealTris owner=%s faces=%i diag=%.0f thresh=%.0f spike=%i skipped=%i"),
				Owner ? Owner->GetName() : TEXT("none"), Faces.Num(), Diag, SpikeThresh, nSpike, nSkipped );
		}
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
		for( INT i=0; i<S->NumBodies; i++ )
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
