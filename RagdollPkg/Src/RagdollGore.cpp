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
	     The dominant-bone partition is refined by a CUT PLANE per cut joint
	     (fitted to the heat-weight seam ring, see the Island split section)
	     so the boundary between the two islands is planar wherever the mesh
	     allows it.
	  2. A face whose vertices fall in two islands is a SEAM face. It is
	     clipped against the plane separating the islands: new vertices on the
	     plane, each side keeps its 1-2 triangles. Where no plane separates
	     the face (fused regions such as the armpit) it is replaced by one
	     copy per island with the foreign vertices duplicated and bound 100%
	     to a bone of that island, as a closed-ish skirt.
	  3. The on-plane edges of each island form a planar rim that is
	     fan-capped from its centroid with the dark wound material (exact
	     flat cross-section). Fallback skirt rims are fan-capped only close to
	     their centroid. Because every material is PF_TwoSided the cap winding
	     does not matter.

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
		// Dark: a cut should read as a shadowed cross-section, not a flag.
		const INT  Tone  = i & 15;
		const BYTE R     = (BYTE)( 30 + Tone * 4 );         // 30..90
		const BYTE G     = (BYTE)( 2 + ( Tone >> 2 ) * 2 ); // 2..8
		const BYTE B     = (BYTE)( 2 + ( Tone >> 3 ) * 2 ); // 2..4
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
	Tex->MipZero   = FColor( 50, 4, 3, 255 );
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
	Island split (planar cuts).

	Prototype + measurements: tools/plane_cut.py, docs/gore-planar-cut-findings.md.

	1. Islands: RagdollIslandRoot per body. A vertex starts in the island of its
	   dominant bind bone and is then refined by every cut joint's plane:
	   child-dominated vertices on the parent side of the plane move to the
	   parent island (the butt stays with the pelvis when a thigh goes), and
	   parent-dominated vertices on the child side move to the child island
	   when they are within the seam ring's wander (W) and radius (R) - so a
	   shoulder plane never claims the chest it also passes through.
	2. A face spanning two islands is clipped against the plane separating
	   them (the joint plane of either island, else a fragment plane): the
	   crossing edges get new vertices on the plane (bind position / UV
	   interpolated, skin weights of the inside endpoint), each side keeps its
	   1 or 2 triangles. Faces no plane separates (fused regions: the armpit,
	   the collar on some models) fall back to one copy per island with the
	   foreign vertices duplicated and bound rigidly to the near side.
	3. The on-plane edges of each island form a planar rim, fan-capped from
	   its centroid with the wound material. Fallback rims are fan-capped only
	   for edges close to their centroid so the fan cannot reach through skin.
-----------------------------------------------------------------------------*/

#define RAGDOLL_CUT_EPS          0.02f   // UU: |d| below this is "on the plane"
#define RAGDOLL_CUT_BAND_MARGIN  2.0f    // parent verts this far beyond the ring's wander still move
#define RAGDOLL_CUT_RADIUS_SCALE 1.35f   // ... when within this multiple of the ring radius ...
#define RAGDOLL_CUT_RADIUS_MARGIN 1.0f
#define RAGDOLL_CUT_BOX_SCALE    1.3f    // ... and within this multiple of the child's box radius
#define RAGDOLL_CUT_BOX_MARGIN   1.5f
#define RAGDOLL_FALLBACK_CAP_SCALE  0.8f // non-planar rims: fan only edges this close to the centroid
#define RAGDOLL_FALLBACK_CAP_MARGIN 1.5f

static FVector BoneCenter( const FRagdollState& S, INT Bone )
{
	return RagdollBoneCenter( S, Bone );
}

static FLOAT Dot3( const FVector& A, const FVector& B )
{
	return A.X * B.X + A.Y * B.Y + A.Z * B.Z;
}

static FVector SafeNormal( const FVector& V )
{
	const FLOAT L = appSqrt( Dot3( V, V ) );
	return L > 1e-6f ? V / L : FVector( 0, 0, 1 );
}

// Radius of a bone's box perpendicular to its (vertical, bind-space) axis.
static FLOAT BoneRadius( const FRagdollState& S, INT Bone )
{
	if( Bone >= RAGDOLL_BONE_COUNT )
	{
		const INT e = Bone - RAGDOLL_BONE_COUNT;
		return ( e < S.NumExtra ) ? S.ExtraRadius[e] : 2.0f;
	}
	const FRagdollBoneDef* Defs = S.BoneDefs ? S.BoneDefs : GRagdollBoneDefs;
	return Max( Defs[Bone].HX, Defs[Bone].HY );
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

// Append a vertex at bind position P skinned like 'Like' (same bones and
// weights, offsets recomputed for P): linear-blend skinning of a new point.
static INT AddBlendVertex( FRagdollState& S, URagdollMesh* Mesh, const FVector& P, const FRagdollVertexBind& Like )
{
	INT nv = S.BindPos.Num();
	S.BindPos.AddItem( P );
	INT bi = S.VertBinds.Add();
	FRagdollVertexBind& B = S.VertBinds( bi );
	for( INT k=0; k<3; k++ )
	{
		B.Bone[k]   = Like.Bone[k];
		B.Weight[k] = Like.Weight[k];
		B.Off[k]    = ( Like.Weight[k] > 0.0f ) ? P - BoneCenter( S, Like.Bone[k] ) : FVector( 0, 0, 0 );
	}
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

// Fit the cut plane of joint 'c' (child body) to the current heat-weight seam
// ring between c's island and its parent's island. See the findings doc: the
// skeleton anchors are the wrong place (the spine-neck anchor sits at the back
// of the neck BELOW the shoulder tops on every model), the mesh seam is right.
static FRagdollCutPlane FitJointPlane( const FRagdollState& S, INT c, const INT* Island )
{
	guard(FitJointPlane);
	FRagdollCutPlane Pl;
	const INT p  = GRagdollBoneParent[c];
	const INT Ip = Island[p];
	const URagdollMesh* Mesh = S.Mesh;
	const INT NumV = S.VertBinds.Num();

	TArray<BYTE> Ring;
	Ring.AddZeroed( NumV );
	INT NumRing = 0;
	for( INT f=0; f<Mesh->Faces.Num(); f++ )
	{
		const FMeshFace& F = Mesh->Faces( f );
		INT v[3], isl[3];
		UBOOL HasC = 0, HasP = 0;
		for( INT k=0; k<3; k++ )
		{
			v[k]   = Mesh->Wedges( F.iWedge[k] ).iVertex;
			isl[k] = ( v[k] >= 0 && v[k] < NumV ) ? Island[ S.VertBinds( v[k] ).Bone[0] ] : -1;
			if( isl[k] == c )  HasC = 1;
			if( isl[k] == Ip ) HasP = 1;
		}
		if( !HasC || !HasP )
			continue;
		for( INT k=0; k<3; k++ )
			if( ( isl[k] == c || isl[k] == Ip ) && !Ring( v[k] ) )
			{
				Ring( v[k] ) = 1;
				NumRing++;
			}
	}
	FVector C( 0, 0, 0 ), L( 0, 0, 0 );
	INT NumL = 0;
	for( INT v=0; v<NumV; v++ )
	{
		if( Ring( v ) )
			C += S.BindPos( v );
		// Whole limb below the joint by the BONE tree (not the island), so the
		// normal is the same whether or not joints further down are cut too.
		if( RagdollInSubtree( S.VertBinds( v ).Bone[0], c ) )
		{
			L += S.BindPos( v );
			NumL++;
		}
	}
	if( NumRing < 3 || NumL == 0 )
	{
		// No seam ring (should not happen for a live joint): anchor + axis.
		Pl.A = S.Dataset ? S.Dataset->Anchors[c - 1] : BoneCenter( S, c );
		Pl.N = SafeNormal( BoneCenter( S, c ) - Pl.A );
		Pl.W = 2.0f;
		Pl.R = 1.6f * BoneRadius( S, c ) + 1.5f;
		Pl.bValid = 1;
		return Pl;
	}
	C /= (FLOAT)NumRing;
	L /= (FLOAT)NumL;
	Pl.A = C;
	Pl.N = SafeNormal( L - C );
	Pl.W = 0.0f;
	Pl.R = 0.0f;
	for( INT v=0; v<NumV; v++ )
	{
		if( !Ring( v ) )
			continue;
		const FVector Rel = S.BindPos( v ) - C;
		const FLOAT d = Dot3( Rel, Pl.N );
		if( Abs( d ) > Pl.W ) Pl.W = Abs( d );
		{ const FLOAT Perp = (FLOAT)appSqrt( Max( Dot3( Rel, Rel ) - d * d, 0.0f ) ); if( Perp > Pl.R ) Pl.R = Perp; }
	}
	Pl.bValid = 1;
	debugf( TEXT("RagdollPkg: cut plane joint=%i ring=%i A=(%.1f %.1f %.1f) N=(%.2f %.2f %.2f) W=%.1f R=%.1f"),
		c, NumRing, Pl.A.X, Pl.A.Y, Pl.A.Z, Pl.N.X, Pl.N.Y, Pl.N.Z, Pl.W, Pl.R );
	return Pl;
	unguard;
}

// Rim/cap group ids: island * 256 + sub, sub = joint child (1..13),
// 32 + fragment plane index, or 64 + source island for fallback rims.
#define CUT_GROUP_JOINT( I, c )  ( (I) * 256 + (c) )
#define CUT_GROUP_FRAG( I, k )   ( (I) * 256 + 32 + (k) )
#define CUT_GROUP_DUP( I, J )    ( (I) * 256 + 64 + (J) )
#define CUT_EDGE_KEY( G, a, b )  ( ( (QWORD)(G) << 32 ) | ( (QWORD)(a) << 16 ) | (QWORD)(b) )

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

	// Joint planes of the cut joints, fitted once when the cut first appears.
	for( INT c=1; c<S.NumBodies && c<RAGDOLL_BONE_COUNT; c++ )
		if( S.Alive[c] && S.Cut[c] && !S.JointPlanes[c].bValid )
			S.JointPlanes[c] = FitJointPlane( S, c, Island );

	// 1. Vertex islands: dominant bone, refined by the joint planes, then
	//    single-island rebinding.
	TArray<INT> VIsl;
	TArray<INT> Pref;
	VIsl.Add( NumV0 );
	Pref.Add( NumV0 );
	TArray<FRagdollVertexBind> Orig = S.VertBinds;
	INT NumMoved = 0;
	for( INT v=0; v<NumV0; v++ )
	{
		FRagdollVertexBind& B = S.VertBinds( v );
		const INT b = B.Bone[0];
		INT I = ( b >= 0 && b < RAGDOLL_MAX_BONES ) ? Island[b] : -1;
		INT Pf = -1;
		const FVector& P = S.BindPos( v );
		for( INT c=1; c<S.NumBodies && c<RAGDOLL_BONE_COUNT; c++ )
		{
			if( !S.Alive[c] || !S.Cut[c] || !S.JointPlanes[c].bValid )
				continue;
			const FRagdollCutPlane& Pl = S.JointPlanes[c];
			const INT par = GRagdollBoneParent[c];
			const FVector Rel = P - Pl.A;
			const FLOAT d = Dot3( Rel, Pl.N );
			if( b == c && d < -RAGDOLL_CUT_EPS )
			{
				I = Island[par]; Pf = par; NumMoved++;
			}
			else if( b == par && d > RAGDOLL_CUT_EPS && d < Pl.W + RAGDOLL_CUT_BAND_MARGIN )
			{
				const FLOAT Perp = appSqrt( Max( Dot3( Rel, Rel ) - d * d, 0.0f ) );
				const FLOAT Lim  = Min( Pl.R * RAGDOLL_CUT_RADIUS_SCALE + RAGDOLL_CUT_RADIUS_MARGIN,
				                        BoneRadius( S, c ) * RAGDOLL_CUT_BOX_SCALE + RAGDOLL_CUT_BOX_MARGIN );
				if( Perp < Lim )
				{
					I = c; Pf = c; NumMoved++;
				}
			}
		}
		VIsl( v ) = I;
		Pref( v ) = Pf;

		// Keep only the weights on bones of island I, renormalised; rigid on
		// the preferred / nearest island bone when none remain.
		FLOAT Sum = 0.0f;
		INT n = 0;
		FRagdollVertexBind NB = B;
		for( INT k=0; k<3; k++ )
		{
			if( B.Weight[k] <= 0.0f )
				continue;
			const INT bk = B.Bone[k];
			if( bk < 0 || bk >= RAGDOLL_MAX_BONES || !RagdollBoneAlive( S, bk ) || Island[bk] != I )
				continue;
			NB.Bone[n]   = bk;
			NB.Off[n]    = B.Off[k];
			NB.Weight[n] = B.Weight[k];
			Sum += B.Weight[k];
			n++;
		}
		if( n == 0 && I >= 0 )
		{
			INT Bone = ( Pf >= 0 && Island[Pf] == I ) ? Pf : NearestBoneInIsland( S, Island, I, P );
			if( Bone < 0 )
				Bone = BONE_Pelvis;
			NB.Bone[0]   = Bone;
			NB.Off[0]    = P - BoneCenter( S, Bone );
			NB.Weight[0] = 1.0f;
			Sum = 1.0f;
			n = 1;
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

	// 2. Seam faces.
	TMap<INT,INT>     DupMap;     // (v * 64 + island) -> duplicate of v in that island
	TMap<QWORD,INT>   Isect;      // (island, a, b) -> vertex on the plane between a and b
	TMap<QWORD,INT>   EdgeCount;  // (group, a, b) -> count; rim edges have count 1
	TMap<QWORD,INT>   SeenPt;     // (group, v) -> 1, unique rim points per planar group
	TMap<INT,FVector> ApexSum;    // group -> sum of rim points (planar) / duplicated positions (fallback)
	TMap<INT,INT>     ApexN;
	TArray<FMeshFace> NewFaces;
	INT NumSeam = 0, NumPlanar = 0, NumFallback = 0;

	struct FHelp
	{
		static INT Dup( FRagdollState& S, URagdollMesh* Mesh, const INT* Island, TArray<FRagdollVertexBind>& Orig,
			TMap<INT,INT>& DupMap, TMap<INT,FVector>& ApexSum, TMap<INT,INT>& ApexN, INT v, INT J, INT I )
		{
			const INT Key = v * 64 + I;
			INT* Found = DupMap.Find( Key );
			if( Found )
				return *Found;
			INT Bone = -1;
			const FRagdollVertexBind& OB = Orig( v );
			for( INT k=0; k<3; k++ )
				if( OB.Weight[k] > 0.0f && OB.Bone[k] >= 0 && OB.Bone[k] < RAGDOLL_MAX_BONES && Island[OB.Bone[k]] == I )
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
			Accum( ApexSum, ApexN, CUT_GROUP_DUP( I, J ), P );
			return nv;
		}
		static void Accum( TMap<INT,FVector>& ApexSum, TMap<INT,INT>& ApexN, INT G, const FVector& P )
		{
			FVector* Sum = ApexSum.Find( G );
			if( Sum )
			{
				*Sum += P;
				( *ApexN.Find( G ) )++;
			}
			else
			{
				ApexSum.Set( G, P );
				ApexN.Set( G, 1 );
			}
		}
		static void CountEdge( TMap<QWORD,INT>& EdgeCount, INT G, INT a, INT b )
		{
			const QWORD Key = CUT_EDGE_KEY( G, Min( a, b ), Max( a, b ) );
			INT* Cnt = EdgeCount.Find( Key );
			if( Cnt )
				( *Cnt )++;
			else
				EdgeCount.Set( Key, 1 );
		}
		// Plane 'Pl' separates the face when every I vertex is at s*d <= eps and
		// every J vertex at s*d >= -eps for s = +1 or -1 (and not all on it).
		static UBOOL Separates( const FRagdollCutPlane& Pl, const FVector* P, const INT* isl, INT I, FLOAT* d, FLOAT& s )
		{
			UBOOL Any = 0;
			for( INT k=0; k<3; k++ )
			{
				d[k] = Dot3( P[k] - Pl.A, Pl.N );
				if( Abs( d[k] ) > RAGDOLL_CUT_EPS )
					Any = 1;
			}
			if( !Any )
				return 0;
			for( INT si=0; si<2; si++ )
			{
				s = si ? -1.0f : 1.0f;
				UBOOL Ok = 1;
				for( INT k=0; k<3 && Ok; k++ )
					Ok = ( isl[k] == I ) ? ( s * d[k] <= RAGDOLL_CUT_EPS ) : ( s * d[k] >= -RAGDOLL_CUT_EPS );
				if( Ok )
					return 1;
			}
			return 0;
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
		{
			NewFaces.AddItem( F );
			continue;
		}
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

		// Find a plane separating the two islands: joint plane of either (if
		// it is a cut child), then the fragment planes in creation order.
		const FRagdollCutPlane* Plane = NULL;
		INT   PlaneSub = -1;     // group sub-id of the plane
		FLOAT d[3], s = 1.0f;
		if( NumPresent == 2 )
		{
			const INT I = Present[0], J = Present[1];
			FVector P[3];
			for( INT k=0; k<3; k++ )
				P[k] = S.BindPos( v[k] );
			for( INT side=0; side<2 && !Plane; side++ )
			{
				const INT X = side ? J : I;
				if( X > 0 && X < RAGDOLL_BONE_COUNT && S.Cut[X] && S.JointPlanes[X].bValid
					&& FHelp::Separates( S.JointPlanes[X], P, isl, I, d, s ) )
				{
					Plane = &S.JointPlanes[X];
					PlaneSub = X;
				}
			}
			for( INT k=0; k<S.NumFragPlanes && !Plane; k++ )
				if( S.FragPlanes[k].bValid && FHelp::Separates( S.FragPlanes[k], P, isl, I, d, s ) )
				{
					Plane = &S.FragPlanes[k];
					PlaneSub = 32 + k;
				}
		}

		if( !Plane )
		{
			// Fallback: one copy per island present, foreign vertices duplicated.
			NumFallback++;
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
					INT nv = FHelp::Dup( S, Mesh, Island, Orig, DupMap, ApexSum, ApexN, v[k], isl[k], I );
					NF.iWedge[k] = (_WORD)AddWedge( Mesh, nv, U, V );
				}
				NewFaces.AddItem( NF );
				for( INT e=0; e<3; e++ )
				{
					const INT e2 = ( e + 1 ) % 3;
					if( isl[e] == I || isl[e2] == I )
						continue;
					FHelp::CountEdge( EdgeCount, CUT_GROUP_DUP( I, isl[e] ), v[e], v[e2] );
				}
			}
			continue;
		}

		// Planar clip: each side keeps the part of the triangle in its half-space.
		NumPlanar++;
		for( INT p=0; p<2; p++ )
		{
			const INT   I    = Present[p];
			const FLOAT Side = p ? -s : s;      // I's half-space is Side*d <= 0
			INT   PolyW[4];
			UBOOL PolyOn[4];
			INT   NumPoly = 0;
			for( INT k=0; k<3; k++ )
			{
				const INT k2 = ( k + 1 ) % 3;
				const FLOAT dk = Side * d[k], dk2 = Side * d[k2];
				if( dk <= RAGDOLL_CUT_EPS && NumPoly < 4 )
				{
					const FMeshWedge& W = Mesh->Wedges( F.iWedge[k] );
					INT wk;
					if( isl[k] == I )
						wk = F.iWedge[k];
					else
						wk = AddWedge( Mesh, FHelp::Dup( S, Mesh, Island, Orig, DupMap, ApexSum, ApexN, v[k], isl[k], I ), W.TexUV.U, W.TexUV.V );
					PolyW[NumPoly]  = wk;
					PolyOn[NumPoly] = ( Abs( d[k] ) <= RAGDOLL_CUT_EPS );
					NumPoly++;
				}
				if( ( ( dk < -RAGDOLL_CUT_EPS && dk2 > RAGDOLL_CUT_EPS ) || ( dk > RAGDOLL_CUT_EPS && dk2 < -RAGDOLL_CUT_EPS ) ) && NumPoly < 4 )
				{
					const INT a = Min( v[k], v[k2] ), b = Max( v[k], v[k2] );
					const QWORD Key = CUT_EDGE_KEY( I, a, b );
					INT* Found = Isect.Find( Key );
					INT xv;
					const FLOAT t = d[k] / ( d[k] - d[k2] );
					if( Found )
						xv = *Found;
					else
					{
						const FVector X = S.BindPos( v[k] ) + ( S.BindPos( v[k2] ) - S.BindPos( v[k] ) ) * t;
						const INT Inside = ( dk < 0.0f ) ? k : k2;   // endpoint on I's side
						xv = AddBlendVertex( S, Mesh, X, S.VertBinds( v[Inside] ) );
						Isect.Set( Key, xv );
					}
					const FMeshWedge& W1 = Mesh->Wedges( F.iWedge[k] );
					const FMeshWedge& W2 = Mesh->Wedges( F.iWedge[k2] );
					const BYTE U = (BYTE)Clamp( appRound( W1.TexUV.U + ( (FLOAT)W2.TexUV.U - W1.TexUV.U ) * t ), 0, 255 );
					const BYTE V = (BYTE)Clamp( appRound( W1.TexUV.V + ( (FLOAT)W2.TexUV.V - W1.TexUV.V ) * t ), 0, 255 );
					PolyW[NumPoly]  = AddWedge( Mesh, xv, U, V );
					PolyOn[NumPoly] = 1;
					NumPoly++;
				}
			}
			if( NumPoly < 3 )
				continue;
			for( INT i=1; i<NumPoly-1; i++ )
			{
				FMeshFace NF;
				NF.MaterialIndex = F.MaterialIndex;
				NF.iWedge[0] = (_WORD)PolyW[0];
				NF.iWedge[1] = (_WORD)PolyW[i];
				NF.iWedge[2] = (_WORD)PolyW[i+1];
				NewFaces.AddItem( NF );
			}
			const INT G = ( PlaneSub < 32 ) ? CUT_GROUP_JOINT( I, PlaneSub ) : CUT_GROUP_FRAG( I, PlaneSub - 32 );
			for( INT i=0; i<NumPoly; i++ )
			{
				const INT i2 = ( i + 1 ) % NumPoly;
				if( !PolyOn[i] || !PolyOn[i2] )
					continue;
				const INT a = Mesh->Wedges( PolyW[i] ).iVertex, b = Mesh->Wedges( PolyW[i2] ).iVertex;
				FHelp::CountEdge( EdgeCount, G, a, b );
				for( INT q=0; q<2; q++ )
				{
					const INT pv = q ? b : a;
					const QWORD PKey = CUT_EDGE_KEY( G, pv, 0 );
					if( !SeenPt.Find( PKey ) )
					{
						SeenPt.Set( PKey, 1 );
						FHelp::Accum( ApexSum, ApexN, G, S.BindPos( pv ) );
					}
				}
			}
		}
	}

	// Replace the face list (TArray<FMeshFace> has the full interface).
	Mesh->Faces.Empty( NewFaces.Num() );
	for( INT i=0; i<NewFaces.Num(); i++ )
		Mesh->Faces.AddItem( NewFaces( i ) );

	// 3. Caps: fan every rim edge from its group's centroid.
	TMap<INT,INT> ApexVert;   // group -> apex vertex
	INT NumCaps = 0;
	const INT CapMat = S.CapMaterial >= 0 ? S.CapMaterial : 0;
	for( TMap<QWORD,INT>::TIterator It( EdgeCount ); It; ++It )
	{
		if( It.Value() != 1 )
			continue;
		const QWORD Key = It.Key();
		const INT G   = (INT)( Key >> 32 );
		INT a = (INT)( ( Key >> 16 ) & 0xFFFF );
		INT b = (INT)( Key & 0xFFFF );
		const INT I   = G / 256;
		const INT Sub = G % 256;
		const UBOOL bFallback = ( Sub >= 64 );

		FVector* Sum = ApexSum.Find( G );
		INT*     N   = ApexN.Find( G );
		if( !Sum || !N || *N <= 0 )
			continue;
		const FVector Apex = *Sum / (FLOAT)*N;

		if( bFallback )
		{
			// Rim keyed by the ORIGINAL foreign vertices: fan their copies in I,
			// and only near the centroid so the fan stays inside the fused zone.
			INT* Da = DupMap.Find( a * 64 + I );
			INT* Db = DupMap.Find( b * 64 + I );
			if( !Da || !Db )
				continue;
			const FLOAT Lim = RAGDOLL_FALLBACK_CAP_SCALE * BoneRadius( S, I ) + RAGDOLL_FALLBACK_CAP_MARGIN;
			if( FDistSquared( S.BindPos( *Da ), Apex ) > Lim * Lim || FDistSquared( S.BindPos( *Db ), Apex ) > Lim * Lim )
				continue;
			a = *Da;
			b = *Db;
		}

		INT* Found = ApexVert.Find( G );
		INT ApexV;
		if( Found )
			ApexV = *Found;
		else
		{
			INT Bone = -1;
			if( Sub > 0 && Sub < RAGDOLL_BONE_COUNT )
			{
				// Joint cut: the cap rides the bone adjacent to the cut.
				if( I == Sub )
					Bone = Sub;
				else if( Island[ GRagdollBoneParent[Sub] ] == I )
					Bone = GRagdollBoneParent[Sub];
			}
			if( Bone < 0 )
				Bone = NearestBoneInIsland( S, Island, I, Apex );
			if( Bone < 0 )
				Bone = BONE_Pelvis;
			ApexV = AddBoundVertex( S, Mesh, Apex, Bone );
			ApexVert.Set( G, ApexV );
		}
		INT fi = Mesh->Faces.Add();
		FMeshFace& CF = Mesh->Faces( fi );
		CF.MaterialIndex = (_WORD)CapMat;
		CF.iWedge[0] = (_WORD)AddWedge( Mesh, a,     8, 248 );
		CF.iWedge[1] = (_WORD)AddWedge( Mesh, b,   248, 248 );
		CF.iWedge[2] = (_WORD)AddWedge( Mesh, ApexV, 128,  8 );
		NumCaps++;
	}

	RefreshMeshTables( Mesh, S.BindPos.Num() );
	S.NumSplits++;
	debugf( TEXT("RagdollPkg: SplitIslands #%i moved=%i seam=%i planar=%i fallback=%i caps=%i verts %i->%i faces %i->%i wedges %i->%i"),
		S.NumSplits, NumMoved, NumSeam, NumPlanar, NumFallback, NumCaps, NumV0, Mesh->FrameVerts, NumF0, Mesh->Faces.Num(), NumW0, Mesh->Wedges.Num() );
	unguard;
}

/*-----------------------------------------------------------------------------
	Fragmentation (torso / head into several chunks) by random planes.
-----------------------------------------------------------------------------*/

INT RagdollFragmentBody( FRagdollState& S, INT Body, INT K, FLOAT DrawScale )
{
	guard(RagdollFragmentBody);
	if( !S.bBuilt || !MeshIsSkinned( S ) || Body < 0 || Body >= S.NumBodies || !S.Alive[Body] || K < 2 )
		return 0;
	if( S.NumExtra + K - 1 > RAGDOLL_MAX_EXTRA )
		K = RAGDOLL_MAX_EXTRA - S.NumExtra + 1;
	if( S.NumFragPlanes + K - 1 > RAGDOLL_MAX_EXTRA )
		K = RAGDOLL_MAX_EXTRA - S.NumFragPlanes + 1;
	if( K < 2 )
		return 0;

	// Vertices dominated by this body.
	TArray<INT> V;
	for( INT v=0; v<S.VertBinds.Num(); v++ )
		if( S.VertBinds( v ).Bone[0] == Body && S.VertBinds( v ).Weight[0] > 0.0f )
			V.AddItem( v );
	if( V.Num() < K * 6 )
		return 0;

	// K-1 planes, each halving the largest chunk so far: random normal through
	// the chunk's centroid (jittered along the normal), accepted when both
	// sides keep at least 6 vertices. Every plane is recorded so the seam
	// clipper in RagdollSplitIslands can find it.
	TArray<INT> Label;
	Label.AddZeroed( V.Num() );
	INT Count[RAGDOLL_MAX_EXTRA + 1];
	for( INT k=0; k<=RAGDOLL_MAX_EXTRA; k++ )
		Count[k] = 0;
	Count[0] = V.Num();
	INT NumChunks = 1;
	for( INT k=1; k<K; k++ )
	{
		INT Big = 0;
		for( INT c=1; c<NumChunks; c++ )
			if( Count[c] > Count[Big] )
				Big = c;
		if( Count[Big] < 12 )
			break;
		FVector Ctr( 0, 0, 0 );
		for( INT i=0; i<V.Num(); i++ )
			if( Label( i ) == Big )
				Ctr += S.BindPos( V( i ) );
		Ctr /= (FLOAT)Count[Big];
		UBOOL Ok = 0;
		FRagdollCutPlane Pl;
		for( INT Try=0; Try<12 && !Ok; Try++ )
		{
			Pl.N = SafeNormal( FVector( appFrand() * 2.0f - 1.0f, appFrand() * 2.0f - 1.0f, appFrand() * 2.0f - 1.0f ) );
			FLOAT Ext = 0.0f;
			for( INT i=0; i<V.Num(); i++ )
				if( Label( i ) == Big )
					{ const FLOAT Ad = Abs( Dot3( S.BindPos( V( i ) ) - Ctr, Pl.N ) ); if( Ad > Ext ) Ext = Ad; }
			Pl.A = Ctr + Pl.N * ( ( appFrand() * 0.6f - 0.3f ) * Ext );
			INT Pos = 0;
			for( INT i=0; i<V.Num(); i++ )
				if( Label( i ) == Big && Dot3( S.BindPos( V( i ) ) - Pl.A, Pl.N ) > 0.0f )
					Pos++;
			Ok = ( Pos >= 6 && Pos <= Count[Big] - 6 );
		}
		if( !Ok )
			break;
		for( INT i=0; i<V.Num(); i++ )
			if( Label( i ) == Big && Dot3( S.BindPos( V( i ) ) - Pl.A, Pl.N ) > 0.0f )
			{
				Label( i ) = k;
				Count[Big]--;
				Count[k]++;
			}
		Pl.W = 0.0f;
		Pl.R = 0.0f;
		Pl.bValid = 1;
		S.FragPlanes[S.NumFragPlanes++] = Pl;
		NumChunks = k + 1;
	}
	if( NumChunks < 2 )
		return 0;

	// Chunk 0 keeps the existing body. Every other chunk gets a box body sized
	// to its bind-space bounds, placed by the parent's current transform.
	const b3WorldTransform PT = b3Body_GetTransform( S.Bodies[Body] );
	const b3Vec3 PV = b3Body_GetLinearVelocity( S.Bodies[Body] );
	const b3Vec3 PW = b3Body_GetAngularVelocity( S.Bodies[Body] );
	const FLOAT  ParentMass = b3Body_GetMass( S.Bodies[Body] );
	const FVector ParentC = RagdollBoneCenter( S, Body );
	INT Made = 0;
	for( INT k=1; k<NumChunks; k++ )
	{
		if( Count[k] <= 0 )
			continue;
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
		Sd.density = ( ParentMass / NumChunks ) / ( 8.0f * HX * HY * HZ );
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
	debugf( TEXT("RagdollPkg: fragment body=%i verts=%i K=%i chunks=%i made=%i extras=%i planes=%i"), Body, V.Num(), K, NumChunks, Made, S.NumExtra, S.NumFragPlanes );
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

void RagdollPaintWound( FRagdollState& S, ARagdollCarcass* C, const FVector& Hit, const FVector& Dir, INT Damage, const TCHAR* DamageType )
{
	guard(RagdollPaintWound);
	if( !S.bBuilt || !MeshIsSkinned( S ) || !C )
		return;
	URagdollMesh* M = S.Mesh;
	const FLOAT DS = C->DrawScale > 0.0f ? C->DrawScale : 1.0f;

	// Find the skin face the shot marks. Hits on a corpse land on its
	// collision cylinder, which can be far from the skin of a corpse lying
	// flat, so when the shot direction is known the face is picked ALONG the
	// ray: nearest to the ray, preferring the entry side (small t). Without a
	// direction (and as a fallback) use the face nearest the hit point.
	TArray<FVector> W;
	W.Add( M->FrameVerts );
	for( INT v=0; v<M->FrameVerts; v++ )
		W( v ) = VertWorld( S, v, DS );
	INT BestF = -1, BestW = -1;
	FLOAT BestD = 1e30f;      // nearest-point distance^2 (fallback)
	INT   RayF  = -1;
	FLOAT RayScore = 1e30f, RayPerp = 0.0f, RayT = 0.0f;
	FVector D = Dir;
	const FLOAT DL = (FLOAT)appSqrt( D.X*D.X + D.Y*D.Y + D.Z*D.Z );
	const UBOOL bRay = DL > 0.001f;
	if( bRay )
		D /= DL;
	for( INT f=0; f<M->Faces.Num(); f++ )
	{
		const FMeshFace& F = M->Faces( f );
		if( S.CapMaterial >= 0 && F.MaterialIndex == S.CapMaterial )
			continue;
		FVector Cen( 0, 0, 0 );
		for( INT k=0; k<3; k++ )
			Cen += W( M->Wedges( F.iWedge[k] ).iVertex );
		Cen /= 3.0f;
		const FVector Rel = Cen - Hit;
		const FLOAT Dist2 = Rel.X*Rel.X + Rel.Y*Rel.Y + Rel.Z*Rel.Z;
		if( Dist2 < BestD ) { BestD = Dist2; BestF = f; }
		if( bRay )
		{
			const FLOAT t = Rel.X*D.X + Rel.Y*D.Y + Rel.Z*D.Z;
			if( t < -25.0f || t > 200.0f )
				continue;
			const FLOAT Perp2 = Max( Dist2 - t * t, 0.0f );
			if( Perp2 > 12.0f * 12.0f )
				continue;
			const FLOAT Ahead = Max( t, 0.0f );
			const FLOAT Score = Perp2 + Ahead * Ahead * 0.04f;   // 20 UU along the ray ~ 4 UU off it
			if( Score < RayScore ) { RayScore = Score; RayF = f; RayPerp = (FLOAT)appSqrt( Perp2 ); RayT = t; }
		}
	}
	UBOOL bByRay = 0;
	if( RayF >= 0 )
	{
		BestF  = RayF;
		bByRay = 1;
	}
	else if( BestF < 0 || BestD > 45.0f * 45.0f )
	{
		debugf( TEXT("RagdollPkg: wound %s miss: nearest face %i at %.0f UU (ray=%i)"), C->GetName(), BestF, BestF >= 0 ? appSqrt( BestD ) : -1.0f, bRay );
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
	if( GWoundDbg < 60 )
	{
		GWoundDbg++;
		debugf( TEXT("RagdollPkg: wound %s type=%s dmg=%i face=%i ray=%i perp=%.1f t=%.0f d=%.0f slot=%i uv=(%.0f,%.0f) r=%.1f blobs=%i"),
			C->GetName(), DamageType ? DamageType : TEXT(""), Damage, BestF, bByRay, RayPerp, RayT, appSqrt( BestD ), t, U, V, Style.Radius, Style.Blobs );
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
