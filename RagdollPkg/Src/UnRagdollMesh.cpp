/*=============================================================================
	UnRagdollMesh.cpp
	Ragdoll mesh: either the procedural box mannequin (fallback) or the dead
	player's real mesh captured at death and rigidly attached to the Box3D
	bones (per-vertex nearest-bone assignment).
=============================================================================*/

#include "UnRagdollMesh.h"
#include "RagdollInternal.h"
#include "box3d/box3d.h"

// TEMP TEST: render the real mesh as a static captured pose (bones at rest)
// instead of live Box3D bone transforms. Set to 1 to isolate whether the
// renderer chokes on the mesh data vs the bone-deformation emission.
#define RAGDOLL_TEST_STATIC 0

// Real-mesh skinning is DISABLED: the dead player's standing pose (arms at
// sides) can't be attached to the spread T-pose physics skeleton without the
// nearest-bone assignment gluing limbs to the torso and tearing when the bodies
// swing. Set to 1 to re-enable the capture path (createPlayerRagdollMesh below);
// 0 makes every corpse fall back to the procedural box mannequin.
#define RAGDOLL_USE_REAL_MESH 0

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
	Real-mesh capture: attach the dead player's ULodMesh to the bones.
-----------------------------------------------------------------------------*/

URagdollMesh* CreatePlayerRagdollMesh( ARagdollCarcass* Carcass, FRagdollState& S )
{
	guard(CreatePlayerRagdollMesh);
#if RAGDOLL_USE_REAL_MESH
	ULodMesh* Src = Cast<ULodMesh>( Carcass ? Carcass->Mesh : NULL );
	if( !Src || !Src->Verts.Num() || Src->FrameVerts <= 0 || Src->Faces.Num() == 0 )
		return NULL;

	FLOAT DrawScale = ( Carcass->DrawScale > 0.0f ) ? Carcass->DrawScale : 1.0f;

	// Mesh import transform: (vert - Origin) rotated by RotOrigin, then scaled
	// by the mesh Scale and the actor DrawScale -> actor-local space, matching
	// the bone-definition frame (pelvis at origin, Z up, arms along +-X).
	FCoords RotCoords = GMath.UnitCoords * Src->RotOrigin;
	const FVector MOrigin = Src->Origin;
	const FLOAT MX = Src->Scale.X, MY = Src->Scale.Y, MZ = Src->Scale.Z;

	const INT NumVerts = Src->FrameVerts;
	INT MaxWedgeVert = 0;
	for( INT f=0; f<Src->Wedges.Num(); f++ )
		MaxWedgeVert = Max( MaxWedgeVert, (INT)Src->Wedges(f).iVertex );
	debugf( TEXT("RagdollMesh: probe mesh=%s FrameVerts=%i ModelVerts=%i SpecialVerts=%i OldFrameVerts=%i Remap=%i WedgeMax=%i AnimFrames=%i"),
		Src->GetName(), Src->FrameVerts, Src->ModelVerts, Src->SpecialVerts, Src->OldFrameVerts, Src->RemapAnimVerts.Num(), MaxWedgeVert, Src->AnimFrames );

	// Reference frame + skeleton fit.
	//
	// The corpse's own anim name ('Dead') is NOT a real sequence in the mesh
	// (Commando has Dead1..Dead9B + DeathEnd instead), so GetAnimSeq returns
	// NULL and the old code fell back to iFrame=0, frame 0 of 'All' - a STANDING
	// pose. Standing never matches the T-pose skeleton: the capture came out a
	// thin vertical stick (Z -86..84) and nearest-center assignment glued the
	// whole torso onto the Head bone (143 verts). Instead we scan EVERY frame of
	// EVERY sequence (dead anims are contorted and never spread; Taunt/Wave/Flip
	// frames hold arms out like the T-pose), fit each to the skeleton's body
	// length, run the nearest-bone assignment, and pick the frame whose verts
	// distribute most evenly across all 14 bones (smallest max-min range). A
	// spread frame wins because its limbs sit where the T-pose bones expect them.
	//
	// FitScale maps the captured pose (mesh units, up to ~2x the skeleton) onto
	// the skeleton scale so bone centers and vertex offsets agree.
	FLOAT SkeletonBodyLen = ( ( GRagdollBoneDefs[BONE_Head].CZ + GRagdollBoneDefs[BONE_Head].HZ )
		- ( GRagdollBoneDefs[BONE_FootL].CZ - GRagdollBoneDefs[BONE_FootL].HZ ) ) * DrawScale;
	FLOAT FitScale = 1.0f;
	INT iFrame = 0;
	{
		INT   BestRange = 0x7fffffff;
		FLOAT BestMean  = 1e30f;
		// Top-6 candidates by (min bone count desc, range asc) so the sprawl
		// landscape is visible in the log.
		INT    TopMin[6];  FLOAT TopRange[6];  FLOAT TopMean[6];  INT TopFrame[6];
		FString TopSeq[6];
		appMemset( TopMin, -1, sizeof(TopMin) );
		for( INT s=0; s<Src->AnimSeqs.Num(); s++ )
		{
			const FMeshAnimSeq& AS = Src->AnimSeqs(s);
			if( AS.NumFrames <= 0 )
				continue;
			for( INT fr=0; fr<AS.NumFrames; fr++ )
			{
				INT F = AS.StartFrame + fr;
				if( F < 0 || F >= Src->AnimFrames )
					continue;

				// Transformed bbox of this frame in actor-local space.
				FVector FMin(1e30f,1e30f,1e30f), FMax(-1e30f,-1e30f,-1e30f);
				for( INT k=0; k<NumVerts; k++ )
				{
					FVector P( Src->Verts( F*NumVerts + k ).Vector() );
					P = ( P - MOrigin ).TransformPointBy( RotCoords );
					P = FVector( P.X*MX, P.Y*MY, P.Z*MZ ) * DrawScale;
					FMin.X=Min(FMin.X,P.X); FMin.Y=Min(FMin.Y,P.Y); FMin.Z=Min(FMin.Z,P.Z);
					FMax.X=Max(FMax.X,P.X); FMax.Y=Max(FMax.Y,P.Y); FMax.Z=Max(FMax.Z,P.Z);
				}
				FVector FD = FMax - FMin;
				FLOAT LongAxis = Max( FD.X, Max( FD.Y, FD.Z ) );
				FLOAT Fit = ( LongAxis > 0.0f ) ? SkeletonBodyLen / LongAxis : 1.0f;

				// Score the nearest-bone assignment for this (fitted) frame.
				INT PerBone[ RAGDOLL_BONE_COUNT ];
				appMemset( PerBone, 0, sizeof(PerBone) );
				FLOAT DistSum = 0.0f;
				for( INT k=0; k<NumVerts; k++ )
				{
					FVector P( Src->Verts( F*NumVerts + k ).Vector() );
					P = ( P - MOrigin ).TransformPointBy( RotCoords );
					P = FVector( P.X*MX, P.Y*MY, P.Z*MZ ) * DrawScale * Fit;
					INT BestBone = 0;
					FLOAT BestD = 1e30f;
					for( INT b=0; b<RAGDOLL_BONE_COUNT; b++ )
					{
						const FRagdollBoneDef& D = GRagdollBoneDefs[b];
						FLOAT dx = P.X - D.CX * DrawScale;
						FLOAT dy = P.Y - D.CY * DrawScale;
						FLOAT dz = P.Z - D.CZ * DrawScale;
						FLOAT d = dx*dx + dy*dy + dz*dz;
						if( d < BestD )
						{
							BestD = d;
							BestBone = b;
						}
					}
					PerBone[ BestBone ]++;
					DistSum += appSqrt( BestD );
				}
				INT MaxCount = PerBone[0], MinCount = PerBone[0];
				for( INT b=1; b<RAGDOLL_BONE_COUNT; b++ )
				{
					MaxCount = Max( MaxCount, PerBone[b] );
					MinCount = Min( MinCount, PerBone[b] );
				}
				INT Range = MaxCount - MinCount;
				FLOAT Mean = DistSum / NumVerts;
				// Track the 6 frames with the highest MinCount (bones not
				// starved), tie-broken by smallest range then smallest mean.
				for( INT t=0; t<6; t++ )
				{
					if( TopMin[t] < 0 || MinCount > TopMin[t]
						|| ( MinCount == TopMin[t] && ( Range < TopRange[t] || ( Range == TopRange[t] && Mean < TopMean[t] ) ) ) )
					{
						for( INT u=5; u>t; u-- )
						{
						TopMin[u]=TopMin[u-1]; TopRange[u]=TopRange[u-1]; TopMean[u]=TopMean[u-1];
						TopFrame[u]=TopFrame[u-1]; TopSeq[u]=TopSeq[u-1];
					}
					TopMin[t]=MinCount; TopRange[t]=(FLOAT)Range; TopMean[t]=Mean;
					TopFrame[t]=F; TopSeq[t]=*AS.Name;
						break;
					}
				}
				// Even distribution = every body part owns its verts; among
				// ties the one whose verts sit closest to their bones.
				if( Range < BestRange || ( Range == BestRange && Mean < BestMean ) )
				{
					BestRange = Range;
					BestMean  = Mean;
					FitScale  = Fit;
					iFrame    = F;
				}
			}
		}
		for( INT t=0; t<6 && TopMin[t] >= 0; t++ )
			debugf( TEXT("RagdollMesh:   cand%i seq=%s f=%i min=%i range=%.0f mean=%.1f"),
				t, *TopSeq[t], TopFrame[t], TopMin[t], TopRange[t], TopMean[t] );
		debugf( TEXT("RagdollMesh: pick bestframe=%i fit=%.3f skelLen=%.1f range=%i meanDist=%.1f"),
			iFrame, FitScale, SkeletonBodyLen, BestRange, BestMean );
	}
	if( iFrame < 0 )
		iFrame = 0;
	if( iFrame >= Src->AnimFrames )
		iFrame = Src->AnimFrames - 1;

	S.VertBones.Empty( NumVerts );
	S.VertOffsets.Empty( NumVerts );
	S.VertBones.Add( NumVerts );
	S.VertOffsets.Add( NumVerts );

	for( INT k=0; k<NumVerts; k++ )
	{
		FMeshVert MV = Src->Verts( iFrame * NumVerts + k );
		FVector P( MV.X, MV.Y, MV.Z );
		P = ( P - MOrigin ).TransformPointBy( RotCoords );
		P = FVector( P.X * MX, P.Y * MY, P.Z * MZ ) * DrawScale * FitScale;

		// Attach to the nearest bone center (actor-local, DrawScale applied).
		INT BestBone = 0;
		FLOAT BestD = 1e30f;
		for( INT b=0; b<RAGDOLL_BONE_COUNT; b++ )
		{
			const FRagdollBoneDef& D = GRagdollBoneDefs[b];
			FLOAT dx = P.X - D.CX * DrawScale;
			FLOAT dy = P.Y - D.CY * DrawScale;
			FLOAT dz = P.Z - D.CZ * DrawScale;
			FLOAT d = dx*dx + dy*dy + dz*dz;
			if( d < BestD )
			{
				BestD = d;
				BestBone = b;
			}
		}
		S.VertBones(k)   = BestBone;
		S.VertOffsets(k) = P - FVector(
			GRagdollBoneDefs[BestBone].CX * DrawScale,
			GRagdollBoneDefs[BestBone].CY * DrawScale,
			GRagdollBoneDefs[BestBone].CZ * DrawScale );
	}

	{
		FVector PMin( 1e30f, 1e30f, 1e30f ), PMax( -1e30f, -1e30f, -1e30f );
		FVector RMin( 1e30f, 1e30f, 1e30f ), RMax( -1e30f, -1e30f, -1e30f );
		INT PerBone[ RAGDOLL_BONE_COUNT ];
		appMemset( PerBone, 0, sizeof(PerBone) );
		for( INT k=0; k<NumVerts; k++ )
		{
			FVector P( Src->Verts( iFrame * NumVerts + k ).Vector() );
			RMin.X = Min( RMin.X, P.X ); RMin.Y = Min( RMin.Y, P.Y ); RMin.Z = Min( RMin.Z, P.Z );
			RMax.X = Max( RMax.X, P.X ); RMax.Y = Max( RMax.Y, P.Y ); RMax.Z = Max( RMax.Z, P.Z );
			P = ( P - MOrigin ).TransformPointBy( RotCoords );
			P = FVector( P.X * MX, P.Y * MY, P.Z * MZ ) * DrawScale * FitScale;
			PMin.X = Min( PMin.X, P.X ); PMin.Y = Min( PMin.Y, P.Y ); PMin.Z = Min( PMin.Z, P.Z );
			PMax.X = Max( PMax.X, P.X ); PMax.Y = Max( PMax.Y, P.Y ); PMax.Z = Max( PMax.Z, P.Z );
			PerBone[ S.VertBones(k) ]++;
		}
		debugf( TEXT("RagdollMesh: capture rawmesh=%s rmin=(%.1f,%.1f,%.1f) rmax=(%.1f,%.1f,%.1f)"), Src->GetName(), RMin.X, RMin.Y, RMin.Z, RMax.X, RMax.Y, RMax.Z );

		// Source-mesh topology probe: count the SOURCE's own long-edge triangles
		// using its raw per-frame verts (the exact indices the engine reads when
		// it renders this mesh normally). If the source itself has ~0 spikes, our
		// emitted buffer order must differ from the source's render indexing.
		{
			FVector SMin( 1e30f, 1e30f, 1e30f ), SMax( -1e30f, -1e30f, -1e30f );
			for( INT k=0; k<NumVerts; k++ )
			{
				FVector P = Src->Verts( iFrame * NumVerts + k ).Vector();
				SMin.X = Min( SMin.X, P.X ); SMin.Y = Min( SMin.Y, P.Y ); SMin.Z = Min( SMin.Z, P.Z );
				SMax.X = Max( SMax.X, P.X ); SMax.Y = Max( SMax.Y, P.Y ); SMax.Z = Max( SMax.Z, P.Z );
			}
			FVector SD = SMax - SMin;
			FLOAT SDiag = appSqrt( SD.X*SD.X + SD.Y*SD.Y + SD.Z*SD.Z );
			FLOAT SThresh = SDiag * 0.6f;
			INT nSpike = 0;
			for( INT f=0; f<Src->Faces.Num(); f++ )
			{
				FVector T[3];
				INT nValid = 0;
				for( INT j=0; j<3; j++ )
				{
					INT w = Src->Faces( f ).iWedge[j];
					if( w < 0 || w >= Src->Wedges.Num() ) continue;
					INT v = Src->Wedges( w ).iVertex;
					if( v < 0 || v >= NumVerts ) continue;
					T[nValid++] = Src->Verts( iFrame * NumVerts + v ).Vector();
				}
				if( nValid == 3 )
				{
					FLOAT MaxEdge = 0.0f;
					for( INT a=0; a<3; a++ )
						for( INT b=a+1; b<3; b++ )
							MaxEdge = Max( MaxEdge, FDist( T[a], T[b] ) );
					if( MaxEdge > SThresh )
						nSpike++;
				}
			}
			debugf( TEXT("RagdollMesh: SrcTris srcmesh=%s faces=%i wedgeMax=%i diag=%.0f thresh=%.0f spike=%i"),
				Src->GetName(), Src->Faces.Num(), Src->Wedges.Num(), SDiag, SThresh, nSpike );
		}
		debugf( TEXT("RagdollMesh: capture mesh=%s Pmin=(%.1f,%.1f,%.1f) Pmax=(%.1f,%.1f,%.1f) bones={%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i}"),
			Src->GetName(), PMin.X, PMin.Y, PMin.Z, PMax.X, PMax.Y, PMax.Z,
			PerBone[0],PerBone[1],PerBone[2],PerBone[3],PerBone[4],PerBone[5],PerBone[6],PerBone[7],
			PerBone[8],PerBone[9],PerBone[10],PerBone[11],PerBone[12],PerBone[13] );
		debugf( TEXT("RagdollMesh: capture srcmesh=%s origin=(%.1f,%.1f,%.1f) scale=(%.3f,%.3f,%.3f) rot=(%i,%i,%i) animseq=%s animframe=%.2f"),
			Src->GetName(), MOrigin.X, MOrigin.Y, MOrigin.Z, Src->Scale.X, Src->Scale.Y, Src->Scale.Z,
			Src->RotOrigin.Pitch, Src->RotOrigin.Yaw, Src->RotOrigin.Roll,
			Carcass ? *Carcass->AnimSequence : TEXT("none"), Carcass ? Carcass->AnimFrame : 0.0f );

		// Frame enumeration probe: dump every anim sequence, then the transformed
		// bbox of each frame in the corpse's current sequence, so a sprawled
		// (arms-out, legs-straight) dead frame can be pinned for the capture
		// instead of relying on the actor's AnimFrame. A standing stick (small X,
		// large Z) never matches the T-pose skeleton; a flat sprawl does.
		{
			// NOTE: keep every line under ~1000 chars. GLog formats into a fixed
			// stack buffer (FOutputDeviceFile::Serialize uses Ch[1024]); longer
			// lines overflow it and crash with a stack-buffer-overrun fail-fast.
			const FMeshAnimSeq* Seq2 = Src->GetAnimSeq( Carcass ? Carcass->AnimSequence : NAME_None );
			INT F0 = Seq2 ? Seq2->StartFrame : 0;
			INT FN = Seq2 ? Seq2->NumFrames : 0;
			debugf( TEXT("RagdollMesh: SeqFrames seq=%s iframe=%i start=%i num=%i all=%i"),
				Carcass ? *Carcass->AnimSequence : TEXT("none"), iFrame, F0, FN, Src->AnimFrames );
			for( INT s=0; s<Src->AnimSeqs.Num(); s++ )
			{
				const FMeshAnimSeq& AS = Src->AnimSeqs(s);
				debugf( TEXT("RagdollMesh:   seq %s start=%i num=%i"), *AS.Name, AS.StartFrame, AS.NumFrames );
			}
			// Dump the chosen frame's transformed bbox so the sprawl can be
			// visually verified (flat/small Z, wide X/Y = lying corpse).
			{
				FVector FMin(1e30f,1e30f,1e30f), FMax(-1e30f,-1e30f,-1e30f);
				for( INT k=0; k<NumVerts; k++ )
				{
					FVector P = Src->Verts( iFrame*NumVerts + k ).Vector();
					P = ( P - MOrigin ).TransformPointBy( RotCoords );
					P = FVector( P.X*MX, P.Y*MY, P.Z*MZ ) * DrawScale * FitScale;
					FMin.X=Min(FMin.X,P.X); FMin.Y=Min(FMin.Y,P.Y); FMin.Z=Min(FMin.Z,P.Z);
					FMax.X=Max(FMax.X,P.X); FMax.Y=Max(FMax.Y,P.Y); FMax.Z=Max(FMax.Z,P.Z);
				}
				FVector D = FMax-FMin;
				debugf( TEXT("RagdollMesh:   chosen frame=%i bbox(X%.0f,Y%.0f,Z%.0f)"), iFrame, D.X, D.Y, D.Z );
			}
		}
	}

	URagdollMesh* Mesh = ConstructObject<URagdollMesh>( URagdollMesh::StaticClass(), UObject::GetTransientPackage(), NAME_None, RF_Public );
	if( !Mesh )
		return NULL;

	// Our GetFrame emits world-space verts from the Box3D bodies, so the
	// engine's model->view chain must not re-apply the source mesh import
	// transform (Origin/Scale/RotOrigin stay neutral).
	Mesh->Scale           = FVector( 1, 1, 1 );
	Mesh->Origin          = FVector( 0, 0, 0 );
	Mesh->RotOrigin       = FRotator( 0, 0, 0 );
	Mesh->FrameVerts      = NumVerts;
	Mesh->ModelVerts      = NumVerts;
	Mesh->SpecialVerts    = 0;
	Mesh->AnimFrames      = 1;
	Mesh->WeaponBoneIndex = -1;

	// Fallback frame data = captured reference pose (fitted actor-local verts,
	// matching the skinned emission so the no-state GetFrame path shows the
	// same corpse pose).
	for( INT k=0; k<NumVerts; k++ )
	{
		FMeshVert MV = Src->Verts( iFrame * NumVerts + k );
		FVector P( MV.X, MV.Y, MV.Z );
		P = ( P - MOrigin ).TransformPointBy( RotCoords );
		P = FVector( P.X * MX, P.Y * MY, P.Z * MZ ) * DrawScale * FitScale;
		new( Mesh->Verts ) FMeshVert( P );
	}

	// Geometry: copy the source mesh's faces, wedges, materials, textures.
	// Wedge iVertex indexes directly into the per-frame vert array (UT99 LOD
	// meshes bake the SpecialVerts offset and RemapAnimVerts at load time).
	Mesh->Faces     = Src->Faces;
	Mesh->Wedges    = Src->Wedges;
	Mesh->Materials = Src->Materials;
	Mesh->Textures  = Src->Textures;

	// LOD scaffolding. URender::DrawLodMesh reads FaceLevel/CollapseWedgeThus/
	// CollapsePointThus for every face; leaving them empty is an out-of-bounds
	// read (garbage triangles + access violation). Pin full detail so no LOD
	// collapse happens and the source wedge iVertex values (baked at load to
	// index the dense per-frame vert array) match our FrameVerts emission.
	Mesh->LODMinVerts   = Mesh->ModelVerts;
	Mesh->LODStrength   = 0.0f;
	Mesh->LODMorph      = 0.0f;
	Mesh->LODZDisplace  = 0.0f;
	Mesh->LODHysteresis = 0.0f;
	Mesh->MeshScaleMax  = Max( Src->MeshScaleMax, 1.0f );
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

	debugf( TEXT("RagdollMesh: CreatePlayerRagdollMesh mesh=%s verts=%i faces=%i frame=%i bones=%i"),
		Src->GetName(), NumVerts, Mesh->Faces.Num(), iFrame, RAGDOLL_BONE_COUNT );
	return Mesh;
	unguard;
#else
	// Real-mesh skinning disabled — all corpses render as the box mannequin.
	// The standing corpse pose cannot attach to the spread T-pose physics
	// skeleton (see docs-skinning-findings.md).
	unguard;
	return NULL;
#endif
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
	// Real-mesh mode: per-vertex bone data captured at build time (indexes the
	// Box3D bodies directly). Box-mannequin mode only when the vert count is
	// exactly the 14-bone box layout. Anything else uses the captured fallback
	// pose so the mesh never indexes Bodies[] out of bounds.
	UBOOL bRealMesh = S && S->bBuilt && S->VertBones.Num() == FrameVerts && FrameVerts != RAGDOLL_BONE_COUNT * VertsPerBox;
	UBOOL bBoxMesh  = S && S->bBuilt && FrameVerts == RAGDOLL_BONE_COUNT * VertsPerBox;
	if( !bRealMesh && !bBoxMesh && LODRequest > 0 && LODRequest < Num )
		Num = LODRequest;

	if( S && S->bBuilt && !S->bFrameLogged )
	{
		S->bFrameLogged = true;
		debugf( TEXT("RagdollMesh: GetFrame first owner=%s FrameVerts=%i VertBones=%i real=%i box=%i LOD=%i"),
			Owner ? Owner->GetName() : TEXT("none"), FrameVerts, S->VertBones.Num(), bRealMesh, bBoxMesh, LODRequest );
		if( bRealMesh )
		{
			for( INT d=0; d<8 && d<Num; d++ )
			{
				INT Bone = S->VertBones(d);
				FVector Off = S->VertOffsets(d);
				b3WorldTransform T = b3Body_GetTransform( S->Bodies[Bone] );
				FVector Center( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
				b3Vec3 Rot = b3RotateVector( T.q, { Off.X, Off.Y, Off.Z } );
				FVector Pos = Center + FVector( Rot.x, Rot.y, Rot.z );
				debugf( TEXT("RagdollMesh: realvert[%i] bone=%i off=(%.1f,%.1f,%.1f) world=(%.1f,%.1f,%.1f) ownerloc=(%.1f,%.1f,%.1f)"),
					d, Bone, Off.X, Off.Y, Off.Z, Pos.X, Pos.Y, Pos.Z,
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
			// Real-mesh mode: each vertex is rigidly attached to its assigned
			// bone. The bone's current world transform places it; the stored
			// actor-local offset (DrawScale applied, from the bone rest center)
			// is rotated by the bone's current world rotation.
			INT Bone = S->VertBones(i);
			FVector Off = S->VertOffsets(i);
			b3WorldTransform T = b3Body_GetTransform( S->Bodies[Bone] );
			FVector Center( T.p.x * RAGDOLL_M2U, T.p.y * RAGDOLL_M2U, T.p.z * RAGDOLL_M2U );
			b3Vec3 Rot = b3RotateVector( T.q, { Off.X, Off.Y, Off.Z } );
			Pos = Center + FVector( Rot.x, Rot.y, Rot.z );
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
				INT Bone = S->VertBones(i);
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
