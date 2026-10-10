/*=============================================================================
	RagdollGore.cpp
	Dismemberment mesh surgery for the real-mesh ragdoll.

	The physics side of a cut is just "destroy the joint" (RagdollCarcass.cpp):
	the detached bodies stay in the same Box3D world and the same FRagdollState,
	so GetFrame keeps skinning them and the part visibly flies off for free.
	What this file does is keep the SKIN from rubber-banding across the cut:

	  1. Every body belongs to a rigid island (RagdollIslandRoot: walk up the
	     bone tree until a cut joint). A vertex's island is that of its
	     dominant bind bone. Its weights on bones of OTHER islands are dropped
	     and the rest renormalised, so a stump vertex no longer follows the
	     limb and vice versa.
	  2. A face whose vertices fall in two (or three) islands is a SEAM face.
	     It is replaced by one copy per island present: the foreign vertices of
	     each copy are duplicated and bound 100% to a bone of that island (one
	     the vertex already had weight on, else the nearest), so each island
	     keeps a closed-ish skirt of skin at the cut instead of a triangle
	     stretched between the two parts.
	  3. The open rim of each skirt is the set of edges whose two endpoints
	     were both foreign and which appear in exactly one seam face (the face
	     across that edge lies entirely in the other island, so it was not
	     copied). Every such edge is fan-capped from the centroid of the
	     foreign vertices duplicated into the island, bound to the island's
	     nearest bone, with the dark wound material. Because every material is
	     PF_TwoSided the cap winding does not matter.

	The algorithm is generic in the island partition, so a single routine
	serves single-limb dismemberment, repeated cuts, and a full gib (every body
	its own island). Decapitation on top removes the head island's faces and
	bodies entirely (the stock UTHeads chunk is the visible head).
=============================================================================*/

#include "RagdollPkg.h"
#include "RagdollInternal.h"
#include "UnRagdollMesh.h"
#include "box3d/box3d.h"

/*-----------------------------------------------------------------------------
	Bone tree helpers.
-----------------------------------------------------------------------------*/

const INT GRagdollBoneParent[RAGDOLL_BONE_COUNT] =
{
	-1,             // Pelvis
	BONE_Pelvis,    // Spine
	BONE_Spine,     // Neck
	BONE_Neck,      // Head
	BONE_Spine,     // UpperArmL
	BONE_Spine,     // UpperArmR
	BONE_UpperArmL, // ForearmL
	BONE_UpperArmR, // ForearmR
	BONE_Pelvis,    // ThighL
	BONE_Pelvis,    // ThighR
	BONE_ThighL,    // ShinL
	BONE_ThighR,    // ShinR
	BONE_ShinL,     // FootL
	BONE_ShinR,     // FootR
};

INT RagdollIslandRoot( const FRagdollState& S, INT Body )
{
	if( Body >= RAGDOLL_BONE_COUNT )
		return Body;   // fragments are always their own island
	INT b = Body;
	while( b >= 0 && b < RAGDOLL_BONE_COUNT && !S.Cut[b] && GRagdollBoneParent[b] >= 0 )
		b = GRagdollBoneParent[b];
	return b;
}

UBOOL RagdollInSubtree( INT Body, INT Ancestor )
{
	if( Body >= RAGDOLL_BONE_COUNT )
		return 0;
	INT b = Body;
	while( b >= 0 )
	{
		if( b == Ancestor )
			return 1;
		b = GRagdollBoneParent[b];
	}
	return 0;
}

INT RagdollNumBones( const FRagdollState& S )
{
	return RAGDOLL_BONE_COUNT + S.NumExtra;
}

b3BodyId RagdollBody( const FRagdollState& S, INT Bone )
{
	if( Bone >= RAGDOLL_BONE_COUNT )
	{
		const INT e = Bone - RAGDOLL_BONE_COUNT;
		return ( e < S.NumExtra ) ? S.ExtraBodies[e] : b3_nullBodyId;
	}
	return ( Bone >= 0 && Bone < RAGDOLL_BONE_COUNT ) ? S.Bodies[Bone] : b3_nullBodyId;
}

UBOOL RagdollBoneAlive( const FRagdollState& S, INT Bone )
{
	if( Bone >= RAGDOLL_BONE_COUNT )
		return ( Bone - RAGDOLL_BONE_COUNT ) < S.NumExtra;
	return Bone >= 0 && Bone < S.NumBodies && S.Alive[Bone];
}

FVector RagdollBoneCenter( const FRagdollState& S, INT Bone )
{
	if( Bone >= RAGDOLL_BONE_COUNT )
	{
		const INT e = Bone - RAGDOLL_BONE_COUNT;
		return ( e < S.NumExtra ) ? S.ExtraCenter[e] : FVector( 0, 0, 0 );
	}
	const FRagdollBoneDef* Defs = S.BoneDefs ? S.BoneDefs : GRagdollBoneDefs;
	return FVector( Defs[Bone].CX, Defs[Bone].CY, Defs[Bone].CZ );
}

/*-----------------------------------------------------------------------------
	Wound texture.
-----------------------------------------------------------------------------*/

// A 16x16 palettised texture of dark-red noise. Built once in the transient
// package and rooted so garbage collection never frees it; every corpse's cap
// material references the same object.
UTexture* RagdollCapTexture()
{
	guard(RagdollCapTexture);
	static UTexture* GCapTexture = NULL;
	if( GCapTexture )
		return GCapTexture;

	UPalette* Pal = ConstructObject<UPalette>( UPalette::StaticClass(), UObject::GetTransientPackage(), NAME_None, RF_Public );
	UTexture* Tex = ConstructObject<UTexture>( UTexture::StaticClass(), UObject::GetTransientPackage(), NAME_None, RF_Public );
	if( !Pal || !Tex )
		return NULL;

	Pal->Colors.Empty();
	Pal->Colors.Add( 256 );
	for( INT i=0; i<256; i++ )
	{
		// Index -> shade: mostly deep red, a few darker clots and brighter
		// flecks so the cap reads as a wound instead of a flat disc.
		const INT  Tone  = i & 15;
		const BYTE R     = (BYTE)( 70 + Tone * 7 );         // 70..175
		const BYTE G     = (BYTE)( 4 + ( Tone >> 2 ) * 3 ); // 4..13
		const BYTE B     = (BYTE)( 4 + ( Tone >> 3 ) * 4 ); // 4..8
		Pal->Colors( i ) = FColor( R, G, B, 255 );
	}

	Tex->Format    = TEXF_P8;
	Tex->Palette   = Pal;
	Tex->UBits     = 4;
	Tex->VBits     = 4;
	Tex->USize     = 16;
	Tex->VSize     = 16;
	Tex->UClamp    = 16;
	Tex->VClamp    = 16;
	Tex->MipZero   = FColor( 100, 8, 6, 255 );
	Tex->MaxColor  = FColor( 255, 255, 255, 255 );
	Tex->PolyFlags = 0;
	Tex->Mips.Empty();
	new( Tex->Mips ) FMipmap( 4, 4 );
	FMipmap& Mip = Tex->Mips( 0 );
	for( INT i=0; i<Mip.DataArray.Num(); i++ )
	{
		// Blotchy noise: coarse 2x2 cells with a jittered tone so the texture
		// is not pure per-texel static at close range.
		const INT x = i & 15, y = i >> 4;
		const INT Cell = ( ( x >> 1 ) * 7 + ( y >> 1 ) * 13 ) & 15;
		const INT Jit  = appRand() & 3;
		Mip.DataArray( i ) = (BYTE)( ( Cell + Jit ) & 15 );
	}

	Pal->AddToRoot();
	Tex->AddToRoot();
	GCapTexture = Tex;
	debugf( TEXT("RagdollPkg: created cap texture %s"), Tex->GetFullName() );
	return Tex;
	unguard;
}

/*-----------------------------------------------------------------------------
	Mesh adjacency.
-----------------------------------------------------------------------------*/

void RagdollBuildMeshAdjacency( URagdollMesh* Mesh )
{
	guard(RagdollBuildMeshAdjacency);
	Mesh->Connects.Empty();
	Mesh->VertLinks.Empty();
	Mesh->Connects.Add( Mesh->FrameVerts );
	// Bucket faces by vertex first (O(F)), then emit per vertex: the old
	// per-vertex face scan was O(V*F), too slow to re-run after every cut.
	TArray<INT> Count;
	Count.AddZeroed( Mesh->FrameVerts );
	for( INT f=0; f<Mesh->Faces.Num(); f++ )
	{
		const FMeshFace& Face = Mesh->Faces( f );
		INT Seen[3] = { -1, -1, -1 };
		for( INT j=0; j<3; j++ )
		{
			INT v = Mesh->Wedges( Face.iWedge[j] ).iVertex;
			if( v < 0 || v >= Mesh->FrameVerts || v == Seen[0] || v == Seen[1] )
				continue;
			Seen[j] = v;
			Count( v )++;
		}
	}
	INT Total = 0;
	for( INT v=0; v<Mesh->FrameVerts; v++ )
	{
		FMeshVertConnect& C = Mesh->Connects( v );
		C.NumVertTriangles   = 0;
		C.TriangleListOffset = Total;
		Total += Count( v );
	}
	Mesh->VertLinks.AddZeroed( Total );
	for( INT f=0; f<Mesh->Faces.Num(); f++ )
	{
		const FMeshFace& Face = Mesh->Faces( f );
		INT Seen[3] = { -1, -1, -1 };
		for( INT j=0; j<3; j++ )
		{
			INT v = Mesh->Wedges( Face.iWedge[j] ).iVertex;
			if( v < 0 || v >= Mesh->FrameVerts || v == Seen[0] || v == Seen[1] )
				continue;
			Seen[j] = v;
			FMeshVertConnect& C = Mesh->Connects( v );
			Mesh->VertLinks( C.TriangleListOffset + C.NumVertTriangles ) = f;
			C.NumVertTriangles++;
		}
	}
	unguard;
}

// Resize the LOD scaffolding to the current face/wedge/vertex counts (full
// detail, identity collapse lists) and rebuild the adjacency.
static void RefreshMeshTables( URagdollMesh* Mesh, INT NumVerts )
{
	// TArray<_WORD> is a dllimport instantiation from Core that only exports
	// Add/Num/operator(), so these tables are grown in place and never
	// emptied: entries past the live count are simply unused.
	Mesh->FrameVerts  = NumVerts;
	Mesh->ModelVerts  = NumVerts;
	Mesh->LODMinVerts = NumVerts;
	if( Mesh->FaceLevel.Num() < Mesh->Faces.Num() )
		Mesh->FaceLevel.Add( Mesh->Faces.Num() - Mesh->FaceLevel.Num() );
	for( INT f=0; f<Mesh->FaceLevel.Num(); f++ )
		Mesh->FaceLevel( f ) = 0;
	if( Mesh->CollapseWedgeThus.Num() < Mesh->Wedges.Num() )
		Mesh->CollapseWedgeThus.Add( Mesh->Wedges.Num() - Mesh->CollapseWedgeThus.Num() );
	for( INT w=0; w<Mesh->CollapseWedgeThus.Num(); w++ )
		Mesh->CollapseWedgeThus( w ) = (_WORD)w;
	if( Mesh->CollapsePointThus.Num() < NumVerts )
		Mesh->CollapsePointThus.Add( NumVerts - Mesh->CollapsePointThus.Num() );
	for( INT v=0; v<Mesh->CollapsePointThus.Num(); v++ )
		Mesh->CollapsePointThus( v ) = (_WORD)v;
	RagdollBuildMeshAdjacency( Mesh );
}

/*-----------------------------------------------------------------------------
	Island split.
-----------------------------------------------------------------------------*/

static FVector BoneCenter( const FRagdollState& S, INT Bone )
{
	return RagdollBoneCenter( S, Bone );
}

static INT NearestBoneInIsland( const FRagdollState& S, const INT* Island, INT I, const FVector& P )
{
	INT Best = -1;
	FLOAT BestD = 1e30f;
	for( INT b=0; b<RagdollNumBones( S ); b++ )
	{
		if( !RagdollBoneAlive( S, b ) || Island[b] != I )
			continue;
		FLOAT D = FDistSquared( P, BoneCenter( S, b ) );
		if( D < BestD ) { BestD = D; Best = b; }
	}
	return Best;
}

static UBOOL MeshIsSkinned( const FRagdollState& S )
{
	return S.Mesh && S.VertBinds.Num() == S.Mesh->FrameVerts && S.BindPos.Num() == S.Mesh->FrameVerts
		&& S.Mesh->FrameVerts != RAGDOLL_BONE_COUNT * 8;
}

// Append a vertex bound rigidly to 'Bone' at bind position P.
static INT AddBoundVertex( FRagdollState& S, URagdollMesh* Mesh, const FVector& P, INT Bone )
{
	INT nv = S.BindPos.Num();
	S.BindPos.AddItem( P );
	INT bi = S.VertBinds.Add();
	FRagdollVertexBind& B = S.VertBinds( bi );
	for( INT k=0; k<3; k++ )
	{
		B.Bone[k]   = Bone;
		B.Off[k]    = FVector( 0, 0, 0 );
		B.Weight[k] = 0.0f;
	}
	B.Off[0]    = P - BoneCenter( S, Bone );
	B.Weight[0] = 1.0f;
	new( Mesh->Verts ) FMeshVert( P );
	return nv;
}

static INT AddWedge( URagdollMesh* Mesh, INT iVertex, BYTE U, BYTE V )
{
	INT w = Mesh->Wedges.Add();
	FMeshWedge& W = Mesh->Wedges( w );
	W.iVertex = (_WORD)iVertex;
	W.TexUV.U = U;
	W.TexUV.V = V;
	return w;
}

void RagdollSplitIslands( FRagdollState& S )
{
	guard(RagdollSplitIslands);
	if( !S.bBuilt || !MeshIsSkinned( S ) )
		return;
	URagdollMesh* Mesh = S.Mesh;

	INT Island[RAGDOLL_MAX_BONES];
	for( INT b=0; b<RAGDOLL_MAX_BONES; b++ )
		Island[b] = RagdollBoneAlive( S, b ) ? RagdollIslandRoot( S, b ) : -1;

	const INT NumV0 = Mesh->FrameVerts;
	const INT NumF0 = Mesh->Faces.Num();
	const INT NumW0 = Mesh->Wedges.Num();

	// 1. Vertex islands (dominant bone) and single-island rebinding.
	TArray<INT> VIsl;
	VIsl.Add( NumV0 );
	TArray<FRagdollVertexBind> Orig = S.VertBinds;
	for( INT v=0; v<NumV0; v++ )
	{
		FRagdollVertexBind& B = S.VertBinds( v );
		const INT I = Island[B.Bone[0]];
		VIsl( v ) = I;
		FLOAT Sum = 0.0f;
		INT n = 0;
		FRagdollVertexBind NB = B;
		for( INT k=0; k<3; k++ )
		{
			if( B.Weight[k] <= 0.0f )
				continue;
			if( Island[B.Bone[k]] != I )
				continue;
			NB.Bone[n]   = B.Bone[k];
			NB.Off[n]    = B.Off[k];
			NB.Weight[n] = B.Weight[k];
			Sum += B.Weight[k];
			n++;
		}
		for( INT k=n; k<3; k++ )
		{
			NB.Bone[k]   = NB.Bone[0];
			NB.Off[k]    = FVector( 0, 0, 0 );
			NB.Weight[k] = 0.0f;
		}
		if( Sum > 0.0f )
			for( INT k=0; k<n; k++ )
				NB.Weight[k] /= Sum;
		B = NB;
	}

	// 2. Seam faces: one copy per island present, foreign vertices duplicated.
	TMap<INT,INT>   DupMap;      // (v * 32 + island) -> duplicated vertex
	TMap<INT,INT>   EdgeCount;   // (island << 26 | a << 13 | b) -> count, a<b, a/b original verts
	TMap<INT,FVector> ApexSum;   // (island * 32 + srcIsland) -> sum of duplicated positions
	TMap<INT,INT>   ApexN;
	INT NumSeam = 0;

	// Duplicate vertex v (island J) into island I.
	struct FDup
	{
		static INT Get( FRagdollState& S, URagdollMesh* Mesh, const INT* Island, TArray<FRagdollVertexBind>& Orig,
			TMap<INT,INT>& DupMap, TMap<INT,FVector>& ApexSum, TMap<INT,INT>& ApexN, INT v, INT J, INT I )
		{
			const INT Key = v * 32 + I;
			INT* Found = DupMap.Find( Key );
			if( Found )
				return *Found;
			INT Bone = -1;
			const FRagdollVertexBind& OB = Orig( v );
			for( INT k=0; k<3; k++ )
				if( OB.Weight[k] > 0.0f && Island[OB.Bone[k]] == I )
				{
					Bone = OB.Bone[k];
					break;
				}
			const FVector P = S.BindPos( v );
			if( Bone < 0 )
				Bone = NearestBoneInIsland( S, Island, I, P );
			if( Bone < 0 )
				Bone = BONE_Pelvis;
			INT nv = AddBoundVertex( S, Mesh, P, Bone );
			DupMap.Set( Key, nv );
			const INT AKey = I * 32 + J;
			FVector* Sum = ApexSum.Find( AKey );
			if( Sum )
			{
				*Sum += P;
				( *ApexN.Find( AKey ) )++;
			}
			else
			{
				ApexSum.Set( AKey, P );
				ApexN.Set( AKey, 1 );
			}
			return nv;
		}
	};

	for( INT f=0; f<NumF0; f++ )
	{
		const FMeshFace F = Mesh->Faces( f );
		INT v[3], isl[3];
		for( INT k=0; k<3; k++ )
		{
			v[k]   = Mesh->Wedges( F.iWedge[k] ).iVertex;
			isl[k] = ( v[k] >= 0 && v[k] < NumV0 ) ? VIsl( v[k] ) : -1;
		}
		if( isl[0] == isl[1] && isl[1] == isl[2] )
			continue;
		NumSeam++;

		INT Present[3], NumPresent = 0;
		for( INT k=0; k<3; k++ )
		{
			if( isl[k] < 0 )
				continue;
			UBOOL Dupe = 0;
			for( INT j=0; j<NumPresent; j++ )
				if( Present[j] == isl[k] )
					Dupe = 1;
			if( !Dupe )
				Present[NumPresent++] = isl[k];
		}

		for( INT p=0; p<NumPresent; p++ )
		{
			const INT I = Present[p];
			FMeshFace NF = F;
			for( INT k=0; k<3; k++ )
			{
				if( isl[k] == I )
					continue;
				const BYTE U = Mesh->Wedges( F.iWedge[k] ).TexUV.U;
				const BYTE V = Mesh->Wedges( F.iWedge[k] ).TexUV.V;
				INT nv = FDup::Get( S, Mesh, Island, Orig, DupMap, ApexSum, ApexN, v[k], isl[k], I );
				NF.iWedge[k] = (_WORD)AddWedge( Mesh, nv, U, V );
			}
			if( p == 0 )
				Mesh->Faces( f ) = NF;
			else
				Mesh->Faces.AddItem( NF );

			// Rim edges of this copy: both endpoints foreign to I.
			for( INT e=0; e<3; e++ )
			{
				const INT e2 = ( e + 1 ) % 3;
				if( isl[e] == I || isl[e2] == I )
					continue;
				const INT a = Min( v[e], v[e2] ), b = Max( v[e], v[e2] );
				const INT Key = ( I << 26 ) | ( a << 13 ) | b;
				INT* Cnt = EdgeCount.Find( Key );
				if( Cnt )
					( *Cnt )++;
				else
					EdgeCount.Set( Key, 1 );
			}
		}
	}

	// 3. Caps: fan every rim edge from its island's cut centroid.
	TMap<INT,INT> ApexVert;   // (island * 32 + srcIsland) -> apex vertex
	INT NumCaps = 0;
	const INT CapMat = S.CapMaterial >= 0 ? S.CapMaterial : 0;
	for( TMap<INT,INT>::TIterator It( EdgeCount ); It; ++It )
	{
		if( It.Value() != 1 )
			continue;
		const INT Key = It.Key();
		const INT I = Key >> 26;
		const INT a = ( Key >> 13 ) & 0x1FFF;
		const INT b = Key & 0x1FFF;
		const INT J = VIsl( a );
		if( J < 0 )
			continue;
		const INT AKey = I * 32 + J;
		INT* Apex = ApexVert.Find( AKey );
		INT ApexV;
		if( Apex )
			ApexV = *Apex;
		else
		{
			FVector* Sum = ApexSum.Find( AKey );
			INT*     N   = ApexN.Find( AKey );
			if( !Sum || !N || *N <= 0 )
				continue;
			const FVector P = *Sum / (FLOAT)*N;
			INT Bone = NearestBoneInIsland( S, Island, I, P );
			if( Bone < 0 )
				Bone = BONE_Pelvis;
			ApexV = AddBoundVertex( S, Mesh, P, Bone );
			ApexVert.Set( AKey, ApexV );
		}
		INT* Da = DupMap.Find( a * 32 + I );
		INT* Db = DupMap.Find( b * 32 + I );
		if( !Da || !Db )
			continue;
		INT fi = Mesh->Faces.Add();
		FMeshFace& CF = Mesh->Faces( fi );
		CF.MaterialIndex = (_WORD)CapMat;
		CF.iWedge[0] = (_WORD)AddWedge( Mesh, *Da,   8, 248 );
		CF.iWedge[1] = (_WORD)AddWedge( Mesh, *Db, 248, 248 );
		CF.iWedge[2] = (_WORD)AddWedge( Mesh, ApexV, 128,  8 );
		NumCaps++;
	}

	RefreshMeshTables( Mesh, S.BindPos.Num() );
	S.NumSplits++;
	debugf( TEXT("RagdollPkg: SplitIslands #%i seam=%i caps=%i verts %i->%i faces %i->%i wedges %i->%i"),
		S.NumSplits, NumSeam, NumCaps, NumV0, Mesh->FrameVerts, NumF0, Mesh->Faces.Num(), NumW0, Mesh->Wedges.Num() );
	unguard;
}

/*-----------------------------------------------------------------------------
	Fragmentation (torso / head into several chunks).
-----------------------------------------------------------------------------*/

INT RagdollFragmentBody( FRagdollState& S, INT Body, INT K, FLOAT DrawScale )
{
	guard(RagdollFragmentBody);
	if( !S.bBuilt || !MeshIsSkinned( S ) || Body < 0 || Body >= S.NumBodies || !S.Alive[Body] || K < 2 )
		return 0;
	if( S.NumExtra + K - 1 > RAGDOLL_MAX_EXTRA )
		K = RAGDOLL_MAX_EXTRA - S.NumExtra + 1;
	if( K < 2 )
		return 0;

	// Vertices dominated by this body.
	TArray<INT> V;
	for( INT v=0; v<S.VertBinds.Num(); v++ )
		if( S.VertBinds( v ).Bone[0] == Body && S.VertBinds( v ).Weight[0] > 0.0f )
			V.AddItem( v );
	if( V.Num() < K * 6 )
		return 0;

	// K-means on bind positions: random distinct seeds, a few Lloyd steps.
	FVector Seed[RAGDOLL_MAX_EXTRA + 1];
	INT     Count[RAGDOLL_MAX_EXTRA + 1];
	FVector Sum[RAGDOLL_MAX_EXTRA + 1];
	for( INT k=0; k<K; k++ )
	{
		INT Pick = V( appRand() % V.Num() );
		Seed[k] = S.BindPos( Pick );
		if( k > 0 )
			Seed[k] += FVector( appFrand() - 0.5f, appFrand() - 0.5f, appFrand() - 0.5f ) * 0.5f;   // never identical
	}
	TArray<INT> Label;
	Label.AddZeroed( V.Num() );
	for( INT Iter=0; Iter<6; Iter++ )
	{
		for( INT k=0; k<K; k++ ) { Count[k] = 0; Sum[k] = FVector( 0, 0, 0 ); }
		for( INT i=0; i<V.Num(); i++ )
		{
			const FVector P = S.BindPos( V( i ) );
			INT Best = 0;
			FLOAT BestD = 1e30f;
			for( INT k=0; k<K; k++ )
			{
				FLOAT D = FDistSquared( P, Seed[k] );
				if( D < BestD ) { BestD = D; Best = k; }
			}
			Label( i ) = Best;
			Count[Best]++;
			Sum[Best] += P;
		}
		for( INT k=0; k<K; k++ )
		{
			if( Count[k] > 0 )
				Seed[k] = Sum[k] / (FLOAT)Count[k];
			else
				Seed[k] = S.BindPos( V( appRand() % V.Num() ) );   // re-seed an empty cluster
		}
	}
	for( INT k=0; k<K; k++ )
		if( Count[k] == 0 )
			return 0;   // degenerate partition; leave the body whole

	// Chunk 0 keeps the existing body. Every other chunk gets a box body sized
	// to its bind-space bounds, placed by the parent's current transform.
	const b3WorldTransform PT = b3Body_GetTransform( S.Bodies[Body] );
	const b3Vec3 PV = b3Body_GetLinearVelocity( S.Bodies[Body] );
	const b3Vec3 PW = b3Body_GetAngularVelocity( S.Bodies[Body] );
	const FLOAT  ParentMass = b3Body_GetMass( S.Bodies[Body] );
	const FVector ParentC = RagdollBoneCenter( S, Body );
	INT Made = 0;
	for( INT k=1; k<K; k++ )
	{
		FVector Mn( 1e30f, 1e30f, 1e30f ), Mx( -1e30f, -1e30f, -1e30f );
		for( INT i=0; i<V.Num(); i++ )
		{
			if( Label( i ) != k )
				continue;
			const FVector& P = S.BindPos( V( i ) );
			Mn.X = Min( Mn.X, P.X ); Mn.Y = Min( Mn.Y, P.Y ); Mn.Z = Min( Mn.Z, P.Z );
			Mx.X = Max( Mx.X, P.X ); Mx.Y = Max( Mx.Y, P.Y ); Mx.Z = Max( Mx.Z, P.Z );
		}
		const FVector C = ( Mn + Mx ) * 0.5f;
		FVector H = ( Mx - Mn ) * 0.5f * 0.8f;   // skin is a shell; keep the hull a bit inside it
		H.X = Max( H.X, 1.5f ); H.Y = Max( H.Y, 1.5f ); H.Z = Max( H.Z, 1.5f );

		// World placement: parent centre + parent rotation * (bind offset).
		const FVector Rel = ( C - ParentC ) * DrawScale * RAGDOLL_U2M;
		const b3Vec3 Wp = b3Add( PT.p, b3RotateVector( PT.q, { Rel.X, Rel.Y, Rel.Z } ) );

		b3BodyDef Bd = b3DefaultBodyDef();
		Bd.type            = b3_dynamicBody;
		Bd.position        = Wp;
		Bd.rotation        = PT.q;
		Bd.linearVelocity  = PV;
		Bd.angularVelocity = PW;
		Bd.linearDamping   = 0.05f;
		Bd.angularDamping  = 0.3f;
		Bd.gravityScale    = 1.0f;
		b3BodyId Id = b3CreateBody( S.World, &Bd );
		if( B3_IS_NULL( Id ) )
			break;
		const FLOAT HX = H.X * DrawScale * RAGDOLL_U2M, HY = H.Y * DrawScale * RAGDOLL_U2M, HZ = H.Z * DrawScale * RAGDOLL_U2M;
		b3ShapeDef Sd = b3DefaultShapeDef();
		Sd.density = ( ParentMass / K ) / ( 8.0f * HX * HY * HZ );
		Sd.baseMaterial.friction    = 0.6f;
		Sd.baseMaterial.restitution = 0.05f;
		Sd.filter.categoryBits      = RAGDOLL_BODY_CATEGORY;
		b3BoxHull Hull = b3MakeBoxHull( HX, HY, HZ );
		b3CreateHullShape( Id, &Sd, &Hull.base );

		const INT e = S.NumExtra++;
		S.ExtraBodies[e] = Id;
		S.ExtraCenter[e] = C;
		S.ExtraRadius[e] = Max( Max( H.X, H.Y ), H.Z ) * 1.5f;
		const INT Bone = RAGDOLL_BONE_COUNT + e;

		// Re-bind the chunk's vertices rigidly to the new body.
		for( INT i=0; i<V.Num(); i++ )
		{
			if( Label( i ) != k )
				continue;
			FRagdollVertexBind& B = S.VertBinds( V( i ) );
			for( INT w=0; w<3; w++ )
			{
				B.Bone[w]   = Bone;
				B.Off[w]    = FVector( 0, 0, 0 );
				B.Weight[w] = 0.0f;
			}
			B.Off[0]    = S.BindPos( V( i ) ) - C;
			B.Weight[0] = 1.0f;
		}
		Made++;
	}
	// Chunk 0: drop any leftover cross weights so it is rigid on the parent.
	for( INT i=0; i<V.Num(); i++ )
	{
		if( Label( i ) != 0 )
			continue;
		FRagdollVertexBind& B = S.VertBinds( V( i ) );
		for( INT w=1; w<3; w++ )
			B.Weight[w] = 0.0f;
		B.Off[0]    = S.BindPos( V( i ) ) - ParentC;
		B.Weight[0] = 1.0f;
	}
	debugf( TEXT("RagdollPkg: fragment body=%i verts=%i K=%i made=%i extras=%i"), Body, V.Num(), K, Made, S.NumExtra );
	return Made;
	unguard;
}

/*-----------------------------------------------------------------------------
	Wound painting.
-----------------------------------------------------------------------------*/

// Current world position of a skinned vertex (same blend as GetFrame).
static FVector VertWorld( const FRagdollState& S, INT v, FLOAT DrawScale )
{
	const FRagdollVertexBind& B = S.VertBinds( v );
	FVector Pos( 0, 0, 0 );
	for( INT k=0; k<3; k++ )
	{
		if( B.Weight[k] <= 0.0f )
			break;
		b3WorldTransform T = b3Body_GetTransform( RagdollBody( S, B.Bone[k] ) );
		FVector Center( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
		b3Vec3 R = b3RotateVector( T.q, { B.Off[k].X * DrawScale, B.Off[k].Y * DrawScale, B.Off[k].Z * DrawScale } );
		Pos += ( Center + FVector( R.x, R.y, R.z ) ) * B.Weight[k];
	}
	return Pos;
}

// Per-corpse writable copy of the texture in skin slot t (NULL when the
// source is not a plain palettised texture).
static UTexture* EnsureWoundSkin( FRagdollState& S, ARagdollCarcass* C, URagdollMesh* M, INT t )
{
	guard(EnsureWoundSkin);
	if( t < 0 || t >= 8 )
		return NULL;
	if( S.WoundSkins[t] )
		return S.WoundSkins[t];
	UTexture* Src = M->GetTexture( t, C );
	if( !Src || Src->Format != TEXF_P8 || !Src->Palette || Src->Mips.Num() == 0 || Src->AnimNext )
	{
		debugf( TEXT("RagdollPkg: wound skin slot=%i unusable src=%s fmt=%i pal=%i mips=%i"), t,
			Src ? Src->GetName() : TEXT("none"), Src ? (INT)Src->Format : -1, Src ? ( Src->Palette != NULL ) : 0, Src ? Src->Mips.Num() : 0 );
		return NULL;
	}
	UTexture* Copy = ConstructObject<UTexture>( UTexture::StaticClass(), UObject::GetTransientPackage(), NAME_None, RF_Public );
	if( !Copy )
		return NULL;
	Copy->Format   = Src->Format;
	Copy->Palette  = Src->Palette;
	Copy->UBits    = Src->UBits;
	Copy->VBits    = Src->VBits;
	Copy->USize    = Src->USize;
	Copy->VSize    = Src->VSize;
	Copy->UClamp   = Src->UClamp;
	Copy->VClamp   = Src->VClamp;
	Copy->MipZero  = Src->MipZero;
	Copy->MaxColor = Src->MaxColor;
	Copy->PolyFlags = Src->PolyFlags;
	Copy->Diffuse  = Src->Diffuse;
	Copy->Specular = Src->Specular;
	Copy->Alpha    = Src->Alpha;
	Copy->Scale    = Src->Scale;
	Copy->LODSet   = Src->LODSet;
	Copy->bHighColorQuality   = Src->bHighColorQuality;
	Copy->bHighTextureQuality = Src->bHighTextureQuality;
	Copy->Mips.Empty();
	for( INT i=0; i<Src->Mips.Num(); i++ )
	{
		FMipmap& SM = Src->Mips( i );
		SM.DataArray.Load();
		new( Copy->Mips ) FMipmap( SM.UBits, SM.VBits );
		FMipmap& DM = Copy->Mips( i );
		const INT N = Min( SM.DataArray.Num(), DM.DataArray.Num() );
		if( N > 0 )
			appMemcpy( &DM.DataArray( 0 ), &SM.DataArray( 0 ), N );
	}
	Copy->bRealtimeChanged = 1;
	C->MultiSkins[t] = Copy;
	if( M->Textures.IsValidIndex( t ) )
		M->Textures( t ) = Copy;
	S.WoundSkins[t] = Copy;
	debugf( TEXT("RagdollPkg: wound skin copy slot=%i src=%s %ix%i mips=%i"), t, Src->GetName(), Src->USize, Src->VSize, Src->Mips.Num() );
	return Copy;
	unguard;
}

static BYTE BestPaletteMatch( UPalette* Pal, const FColor& Want )
{
	INT Best = 1, BestD = 0x7fffffff;
	const INT N = Min( Pal->Colors.Num(), 256 );
	for( INT i=1; i<N; i++ )   // index 0 is the masked/transparent entry
	{
		const FColor& Cc = Pal->Colors( i );
		const INT dr = (INT)Cc.R - Want.R, dg = (INT)Cc.G - Want.G, db = (INT)Cc.B - Want.B;
		const INT D = dr*dr + dg*dg + db*db;
		if( D < BestD ) { BestD = D; Best = i; }
	}
	return (BYTE)Best;
}

static FLOAT ClampF( FLOAT V, FLOAT Lo, FLOAT Hi )
{
	return V < Lo ? Lo : ( V > Hi ? Hi : V );
}

struct FWoundStyle
{
	FLOAT  Radius;     // texels at a 256-wide skin
	FColor Color;      // blend target
	FLOAT  Strength;   // 0..1 blend toward Color at the centre
	FLOAT  Edge;       // 0 = hard disc, 1 = very soft/speckled rim
	INT    Blobs;      // number of blobs (shrapnel scatter)
	FLOAT  Scatter;    // texel spread of the extra blobs
};

static FWoundStyle WoundStyleFor( const TCHAR* Type, INT Damage )
{
	FWoundStyle W;
	W.Radius = 3.0f; W.Color = FColor( 70, 8, 6 ); W.Strength = 0.8f; W.Edge = 0.6f; W.Blobs = 1; W.Scatter = 0.0f;
	const FLOAT D = (FLOAT)Max( Damage, 1 );
	if( !Type || !*Type )
		return W;
	if( !appStricmp( Type, TEXT("shot") ) )
	{
		// Bullets: minigun (7-9) pinpricks, enforcer (17) small holes,
		// sniper (45) a proper entry wound.
		W.Radius   = ClampF( 1.5f + D / 12.0f, 1.5f, 5.5f );
		W.Strength = 0.9f;
		W.Edge     = 0.4f;
	}
	else if( !appStricmp( Type, TEXT("shredded") ) )
	{
		// Flak chunks / ripper blades: a cluster of small ragged cuts.
		W.Radius   = ClampF( 1.5f + D / 20.0f, 1.5f, 3.5f );
		W.Blobs    = 3 + ( Damage > 25 ? 2 : 0 );
		W.Scatter  = 7.0f;
		W.Edge     = 0.8f;
		W.Strength = 0.85f;
	}
	else if( !appStricmp( Type, TEXT("exploded") ) || !appStricmp( Type, TEXT("RedeemerDeath") ) )
	{
		// Rocket / flak shell / redeemer: broad scorched blotch.
		W.Radius   = ClampF( 5.0f + D / 12.0f, 5.0f, 13.0f );
		W.Color    = FColor( 28, 14, 10 );
		W.Strength = 0.7f;
		W.Edge     = 0.9f;
		W.Blobs    = 2;
		W.Scatter  = 5.0f;
	}
	else if( !appStricmp( Type, TEXT("jolted") ) || !appStricmp( Type, TEXT("zapped") ) )
	{
		// Shock / pulse: medium electrical burn, nearly black.
		W.Radius   = ClampF( 3.5f + D / 15.0f, 3.5f, 8.0f );
		W.Color    = FColor( 18, 12, 20 );
		W.Strength = 0.75f;
		W.Edge     = 0.7f;
	}
	else if( !appStricmp( Type, TEXT("burned") ) )
	{
		W.Radius   = ClampF( 4.0f + D / 12.0f, 4.0f, 9.0f );
		W.Color    = FColor( 20, 12, 8 );
		W.Strength = 0.8f;
		W.Edge     = 0.8f;
	}
	else if( !appStricmp( Type, TEXT("corroded") ) )
	{
		// Bio rifle: green acid splatter, speckled.
		W.Radius   = ClampF( 3.0f + D / 10.0f, 3.0f, 9.0f );
		W.Color    = FColor( 40, 110, 30 );
		W.Strength = 0.7f;
		W.Edge     = 0.9f;
		W.Blobs    = 2;
		W.Scatter  = 5.0f;
	}
	else if( !appStricmp( Type, TEXT("pounded") ) )
	{
		// Impact hammer: big bruise-like wound.
		W.Radius   = ClampF( 5.0f + D / 15.0f, 5.0f, 9.0f );
		W.Color    = FColor( 60, 6, 10 );
		W.Strength = 0.75f;
		W.Edge     = 0.5f;
	}
	else if( !appStricmp( Type, TEXT("Decapitated") ) )
	{
		W.Radius   = 6.0f;
		W.Strength = 0.9f;
		W.Edge     = 0.5f;
	}
	return W;
}

static void PaintBlob( UTexture* Tex, FLOAT U, FLOAT V, const FWoundStyle& W, FLOAT RadiusScale )
{
	UPalette* Pal = Tex->Palette;
	for( INT i=0; i<Tex->Mips.Num(); i++ )
	{
		FMipmap& Mip = Tex->Mips( i );
		if( Mip.DataArray.Num() < Mip.USize * Mip.VSize )
			continue;
		// UV bytes span the whole texture whatever its size.
		const FLOAT cx = U * Mip.USize / 256.0f;
		const FLOAT cy = V * Mip.VSize / 256.0f;
		const FLOAT r  = Max( W.Radius * RadiusScale * Mip.USize / 256.0f, 0.75f );
		const INT ir = appCeil( r );
		for( INT dy=-ir; dy<=ir; dy++ )
			for( INT dx=-ir; dx<=ir; dx++ )
			{
				const FLOAT d = appSqrt( (FLOAT)( dx*dx + dy*dy ) ) / r;
				if( d > 1.0f )
					continue;
				// Soft / speckled rim: outer texels are painted with falling
				// probability so the edge is ragged rather than a stamped disc.
				if( appFrand() < d * d * W.Edge )
					continue;
				const INT x = ( appFloor( cx ) + dx ) & ( Mip.USize - 1 );
				const INT y = ( appFloor( cy ) + dy ) & ( Mip.VSize - 1 );
				BYTE& Px = Mip.DataArray( y * Mip.USize + x );
				const FColor Old = Pal->Colors( Px );
				const FLOAT a = W.Strength * ( 1.0f - 0.45f * d );
				FColor Want(
					(BYTE)Clamp( appRound( Old.R + ( W.Color.R - Old.R ) * a ), 0, 255 ),
					(BYTE)Clamp( appRound( Old.G + ( W.Color.G - Old.G ) * a ), 0, 255 ),
					(BYTE)Clamp( appRound( Old.B + ( W.Color.B - Old.B ) * a ), 0, 255 ), 255 );
				Px = BestPaletteMatch( Pal, Want );
			}
	}
	Tex->bRealtimeChanged = 1;
}

void RagdollPaintWound( FRagdollState& S, ARagdollCarcass* C, const FVector& Hit, INT Damage, const TCHAR* DamageType )
{
	guard(RagdollPaintWound);
	if( !S.bBuilt || !MeshIsSkinned( S ) || !C )
		return;
	URagdollMesh* M = S.Mesh;
	const FLOAT DS = C->DrawScale > 0.0f ? C->DrawScale : 1.0f;

	// Nearest face to the hit (hitscan hits land on the collision cylinder,
	// so allow a generous distance but give up on far misses).
	TArray<FVector> W;
	W.Add( M->FrameVerts );
	for( INT v=0; v<M->FrameVerts; v++ )
		W( v ) = VertWorld( S, v, DS );
	INT BestF = -1, BestW = -1;
	FLOAT BestD = 1e30f;
	for( INT f=0; f<M->Faces.Num(); f++ )
	{
		const FMeshFace& F = M->Faces( f );
		if( S.CapMaterial >= 0 && F.MaterialIndex == S.CapMaterial )
			continue;
		FVector Cen( 0, 0, 0 );
		for( INT k=0; k<3; k++ )
			Cen += W( M->Wedges( F.iWedge[k] ).iVertex );
		Cen /= 3.0f;
		const FLOAT D = FDistSquared( Cen, Hit );
		if( D < BestD ) { BestD = D; BestF = f; }
	}
	if( BestF < 0 || BestD > 45.0f * 45.0f )
	{
		debugf( TEXT("RagdollPkg: wound %s miss: nearest face %i at %.0f UU"), C->GetName(), BestF, BestF >= 0 ? appSqrt( BestD ) : -1.0f );
		return;
	}
	const FMeshFace& F = M->Faces( BestF );
	FLOAT BestVD = 1e30f;
	for( INT k=0; k<3; k++ )
	{
		const FLOAT D = FDistSquared( W( M->Wedges( F.iWedge[k] ).iVertex ), Hit );
		if( D < BestVD ) { BestVD = D; BestW = F.iWedge[k]; }
	}
	if( !M->Materials.IsValidIndex( F.MaterialIndex ) )
		return;
	const INT t = M->Materials( F.MaterialIndex ).TextureIndex;
	if( t == 7 )
		return;
	UTexture* Tex = EnsureWoundSkin( S, C, M, t );
	if( !Tex )
		return;

	const FWoundStyle Style = WoundStyleFor( DamageType, Damage );
	const FMeshWedge& Wd = M->Wedges( BestW );
	// Blend the UV a little toward the face centre so the mark sits on the
	// triangle rather than exactly on a vertex / UV seam.
	FLOAT U = Wd.TexUV.U, V = Wd.TexUV.V;
	{
		FLOAT su = 0, sv = 0;
		for( INT k=0; k<3; k++ ) { su += M->Wedges( F.iWedge[k] ).TexUV.U; sv += M->Wedges( F.iWedge[k] ).TexUV.V; }
		U = U * 0.6f + ( su / 3.0f ) * 0.4f;
		V = V * 0.6f + ( sv / 3.0f ) * 0.4f;
	}
	PaintBlob( Tex, U, V, Style, 1.0f );
	for( INT b=1; b<Style.Blobs; b++ )
	{
		const FLOAT A = appFrand() * 2.0f * PI, R = Style.Scatter * ( 0.4f + appFrand() * 0.6f );
		PaintBlob( Tex, U + appCos( A ) * R, V + appSin( A ) * R, Style, 0.5f + appFrand() * 0.5f );
	}
	static INT GWoundDbg = 0;
	if( GWoundDbg < 12 )
	{
		GWoundDbg++;
		debugf( TEXT("RagdollPkg: wound %s type=%s dmg=%i face=%i d=%.0f slot=%i uv=(%.0f,%.0f) r=%.1f blobs=%i"),
			C->GetName(), DamageType ? DamageType : TEXT(""), Damage, BestF, appSqrt( BestD ), t, U, V, Style.Radius, Style.Blobs );
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Island removal (decapitation).
-----------------------------------------------------------------------------*/

void RagdollRemoveIsland( FRagdollState& S, INT Root )
{
	guard(RagdollRemoveIsland);
	if( !S.bBuilt || Root <= 0 || Root >= S.NumBodies || !S.Alive[Root] )
		return;
	// The island must be its own rigid group (joint to the parent cut) so the
	// split above has already capped the stump.
	S.Cut[Root] = 1;
	const INT Stump = GRagdollBoneParent[Root] >= 0 ? GRagdollBoneParent[Root] : BONE_Pelvis;

	if( MeshIsSkinned( S ) )
	{
		URagdollMesh* Mesh = S.Mesh;
		const INT NumV = Mesh->FrameVerts;
		TArray<UBOOL> Gone;
		Gone.AddZeroed( NumV );
		INT NumGone = 0;
		for( INT v=0; v<NumV; v++ )
		{
			FRagdollVertexBind& B = S.VertBinds( v );
			if( RagdollInSubtree( B.Bone[0], Root ) )
			{
				Gone( v ) = 1;
				NumGone++;
			}
		}
		// Drop every face touching a removed vertex (after the split no face
		// straddles islands, so these are exactly the island's faces + caps).
		INT NumF0 = Mesh->Faces.Num();
		INT Kept = 0;
		for( INT f=0; f<NumF0; f++ )
		{
			const FMeshFace& F = Mesh->Faces( f );
			UBOOL Drop = 0;
			for( INT k=0; k<3; k++ )
			{
				INT v = Mesh->Wedges( F.iWedge[k] ).iVertex;
				if( v >= 0 && v < NumV && Gone( v ) )
					Drop = 1;
			}
			if( !Drop )
				Mesh->Faces( Kept++ ) = F;
		}
		if( Kept < NumF0 )
			Mesh->Faces.Remove( Kept, NumF0 - Kept );
		// Orphaned vertices are still emitted by GetFrame: park them on the
		// stump bone so no destroyed body is ever read.
		for( INT v=0; v<NumV; v++ )
		{
			FRagdollVertexBind& B = S.VertBinds( v );
			UBOOL Fix = Gone( v );
			for( INT k=0; k<3 && !Fix; k++ )
				if( B.Weight[k] > 0.0f && RagdollInSubtree( B.Bone[k], Root ) )
					Fix = 1;
			if( !Fix )
				continue;
			for( INT k=0; k<3; k++ )
			{
				B.Bone[k]   = Stump;
				B.Off[k]    = FVector( 0, 0, 0 );
				B.Weight[k] = 0.0f;
			}
			B.Off[0]    = S.BindPos( v ) - BoneCenter( S, Stump );
			B.Weight[0] = 1.0f;
		}
		RefreshMeshTables( Mesh, NumV );
		debugf( TEXT("RagdollPkg: RemoveIsland root=%i verts=%i faces %i->%i"), Root, NumGone, NumF0, Kept );
	}

	// Destroy the island's bodies (Box3D removes their joints with them).
	for( INT b=0; b<S.NumBodies; b++ )
	{
		if( !S.Alive[b] || !RagdollInSubtree( b, Root ) )
			continue;
		if( b3Body_IsValid( S.Bodies[b] ) )
			b3DestroyBody( S.Bodies[b] );
		S.Bodies[b] = b3_nullBodyId;
		S.Joints[b] = b3_nullJointId;
		S.Alive[b]  = 0;
		S.Cut[b]    = 1;
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
