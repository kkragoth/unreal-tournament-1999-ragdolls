/*=============================================================================
	UnRagdollGeometry.cpp
	Bakes the level BSP into a static Box3D triangle mesh so ragdoll corpses
	collide with the real level (floors, walls, ramps, stairs).

	One Box3D world is shared by every corpse in a level: it holds a single
	static body (the whole-level mesh) plus all ragdoll bodies/joints. The world
	origin coincides with the level origin (UU -> meters via RAGDOLL_U2M), so
	BSP vertices map straight into Box3D coordinates.
=============================================================================*/

#include "RagdollPkg.h"
#include "RagdollInternal.h"
#include "UnRagdollMesh.h"
#include "box3d/box3d.h"

/*-----------------------------------------------------------------------------
	Process-global level world.
-----------------------------------------------------------------------------*/

static FRagdollLevelWorld GRagdollLevel;
UBOOL GRagdollWantMovers = 1;

FRagdollLevelWorld& GetRagdollLevelWorld()
{
	return GRagdollLevel;
}

void TeardownLevelWorld()
{
	FRagdollLevelWorld& LW = GetRagdollLevelWorld();
	if( !LW.bBuilt )
		return;

	if( LW.Mesh )
		b3DestroyMesh( LW.Mesh );
	LW.Mesh = NULL;

	if( b3World_IsValid( LW.World ) )
		b3DestroyWorld( LW.World );   // takes the mover bodies with it
	LW.Movers.Empty();
	LW.World    = b3_nullWorldId;
	LW.Geometry = b3_nullBodyId;
	LW.Level    = NULL;
	LW.MinZ     = 0.0f;
	LW.bBuilt   = false;
	debugf( TEXT("RagdollPkg: TeardownLevelWorld") );
}

/*-----------------------------------------------------------------------------
	BSP -> triangle mesh.
-----------------------------------------------------------------------------*/

// Collector for the solid BSP polygons of one level.
struct FRagdollGeomCollector
{
	TArray<b3Vec3> Vertices;
	TArray<INT>    Indices;
	FLOAT          MinZ;

	FRagdollGeomCollector()
	: MinZ( 1.0e18f )
	{}

	void AddPolygon( const FVector* Poly, INT NumV, const FPlane& Plane )
	{
		// Plane normal points into the room (front face). Use it to wind the
		// fan triangles so the mesh's faces face the room.
		FVector Normal( Plane.X, Plane.Y, Plane.Z );

		const INT Base = Vertices.Num();
		for( INT j=0; j<NumV; j++ )
		{
			FVector P = Poly[j];
			Vertices.AddItem( b3Vec3{ P.X * RAGDOLL_U2M, P.Y * RAGDOLL_U2M, P.Z * RAGDOLL_U2M } );
			MinZ = Min( MinZ, P.Z );
		}

		// Winding: (0,1,2) must face the plane normal; flip the fan if not.
		const b3Vec3& A = Vertices(Base);
		const b3Vec3& B = Vertices(Base + 1);
		const b3Vec3& D = Vertices(Base + 2);
		b3Vec3 Cross;
		Cross.x = ( B.y - A.y ) * ( D.z - A.z ) - ( B.z - A.z ) * ( D.y - A.y );
		Cross.y = ( B.z - A.z ) * ( D.x - A.x ) - ( B.x - A.x ) * ( D.z - A.z );
		Cross.z = ( B.x - A.x ) * ( D.y - A.y ) - ( B.y - A.y ) * ( D.x - A.x );
		FLOAT Dot = Cross.x * Normal.X + Cross.y * Normal.Y + Cross.z * Normal.Z;
		const UBOOL Flip = Dot < 0.0f;

		// Front faces (toward the room). Also emit each polygon BACKWARDS so the
		// level mesh collides from both sides: a corpse shoved below a thin floor
		// (or spawning half-embedded) otherwise finds no surface beneath it and
		// falls through the ground forever.
		for( INT j=1; j<NumV-1; j++ )
		{
			if( Flip )
			{
				Indices.AddItem( Base );
				Indices.AddItem( Base + j + 1 );
				Indices.AddItem( Base + j );
			}
			else
			{
				Indices.AddItem( Base );
				Indices.AddItem( Base + j );
				Indices.AddItem( Base + j + 1 );
			}
		}
		for( INT j=1; j<NumV-1; j++ )
		{
			if( Flip )
			{
				Indices.AddItem( Base );
				Indices.AddItem( Base + j );
				Indices.AddItem( Base + j + 1 );
			}
			else
			{
				Indices.AddItem( Base );
				Indices.AddItem( Base + j + 1 );
				Indices.AddItem( Base + j );
			}
		}
	}
};

static void CollectBspGeometry( ULevel* Level, FRagdollGeomCollector& C )
{
	UModel* Model = Level->Model;
	if( !Model )
		return;

	// Mover/water/low-gravity handling: the main level BSP only contains static
	// CSG geometry. Mover brushes (doors/elevators) keep their own dynamic
	// Models and never appear here, and transparent polys (water, glass) are
	// flagged NF_NotCsg, so both are excluded automatically.
	const INT NumNodes = Model->Nodes.Num();
	for( INT i=0; i<NumNodes; i++ )
	{
		const FBspNode& Node = Model->Nodes(i);
		if( Node.NumVertices < 3 || !Node.IsCsg() )
			continue;
		const FBspSurf& Surf = Model->Surfs(Node.iSurf);
		if( Surf.PolyFlags & PF_NotSolid )
			continue;

		FVector Poly[24];
		const INT NumV = Node.NumVertices;
		for( INT j=0; j<NumV; j++ )
			Poly[j] = Model->Points( Model->Verts(Node.iVertPool + j).pVertex );

		C.AddPolygon( Poly, NumV, Node.Plane );
	}
}

static void BuildLevelWorld( FRagdollLevelWorld& LW, ULevel* Level )
{
	FRagdollGeomCollector Collect;
	CollectBspGeometry( Level, Collect );
	if( Collect.Indices.Num() < 3 )
	{
		debugf( TEXT("RagdollPkg: BuildLevelWorld FAILED no solid BSP geometry level=%s"), Level->GetName() );
		return;
	}

	b3MeshDef Md = {};
	Md.vertices         = (b3Vec3*)Collect.Vertices.GetData();
	Md.indices          = (int32_t*)Collect.Indices.GetData();
	Md.materialIndices  = NULL;
	Md.weldTolerance    = 0.002f;   // weld 2mm-apart BSP verts
	Md.vertexCount      = Collect.Vertices.Num();
	Md.triangleCount    = Collect.Indices.Num() / 3;
	Md.weldVertices     = true;
	Md.useMedianSplit   = true;     // BSP geometry is grid-like; median split is fast
	Md.identifyEdges    = false;
	b3MeshData* Mesh = b3CreateMesh( &Md, NULL, 0 );
	if( !Mesh )
	{
		debugf( TEXT("RagdollPkg: BuildLevelWorld FAILED b3CreateMesh level=%s"), Level->GetName() );
		return;
	}

	b3WorldDef Wd = b3DefaultWorldDef();
	// UT's default zone gravity (950 UU/s^2 = 19 m/s^2 at 50 UU/m), not
	// Earth's: until 2026-10-11 the world ran at 9.81, so every corpse fell at
	// half the rate of the player's own jumps and read as floaty. Per-zone
	// gravity (low-grav zones) and the GravityScale knob are applied per body
	// through gravityScale in BuildRagdollImpl.
	Wd.gravity     = { 0.0f, 0.0f, -RAGDOLL_UT_GRAVITY * RAGDOLL_U2M };
	Wd.enableSleep = true;
	Wd.workerCount = 0;
	// Overlap recovery speed cap (default 3 m/s). A lift rises at 300+ UU/s
	// (6+ m/s): with the default cap a corpse lying on it sank INTO the
	// moving brush faster than the contacts could push it out and was
	// reported crushed. 12 m/s covers every retail mover.
	Wd.contactSpeed = 12.0f;
	// Stiffer contacts (default 30 Hz / 10): a corpse dropping 100 UU onto a
	// lift that outran it (6.3 m/s down, the corpse starts from rest) lands
	// at ~14 m/s and sank a shin 5-6 UU into the platform; 60 Hz halves the
	// impact penetration (Box3D clamps it to a quarter of the substep rate).
	Wd.contactHertz = 60.0f;

	b3BodyDef Bd = b3DefaultBodyDef();
	Bd.type     = b3_staticBody;

	b3ShapeDef Sd = b3DefaultShapeDef();
	Sd.baseMaterial.friction    = 0.5f;
	Sd.baseMaterial.restitution = 0.0f;
	Sd.filter.categoryBits      = RAGDOLL_GEOM_CATEGORY;

	LW.Level    = Level;
	LW.World    = b3CreateWorld( &Wd );
	LW.Geometry = b3CreateBody( LW.World, &Bd );
	LW.Mesh     = Mesh;
	LW.MinZ     = Collect.MinZ;
	LW.LastStepTime = appSeconds().GetFloat();
	LW.LastTickTime = -1.0f;
	LW.bBuilt   = true;

	b3CreateMeshShape( LW.Geometry, &Sd, Mesh, b3Vec3_one );

	// Movers (doors / lifts / trains) as kinematic bodies, unless the first
	// corpse's bMoverCollision config flag turned them off.
	if( GRagdollWantMovers )
		RagdollBuildMovers( LW, Level );
	else
		debugf( TEXT("RagdollPkg: movers level=%s skipped (bMoverCollision off)"), Level->GetName() );

	// Mesh bounds in Unreal units (bounds*RAGDOLL_M2U), verifying the baked BSP
	// spans the whole level rather than a bogus sliver.
	FLOAT MnX=1e9f, MnY=1e9f, MnZ=1e9f, MxX=-1e9f, MxY=-1e9f, MxZ=-1e9f;
	for( INT i=0; i<Collect.Vertices.Num(); i++ )
	{
		const b3Vec3& V = Collect.Vertices(i);
		MnX = Min( MnX, V.x ); MnY = Min( MnY, V.y ); MnZ = Min( MnZ, V.z );
		MxX = Max( MxX, V.x ); MxY = Max( MxY, V.y ); MxZ = Max( MxZ, V.z );
	}
	debugf( TEXT("RagdollPkg: BuildLevelWorld level=%s nodes=%i tris=%i minZ=%.0f bounds=(%.0f..%.0f, %.0f..%.0f, %.0f..%.0f)"),
		Level->GetName(), Collect.Vertices.Num(), Collect.Indices.Num() / 3, Collect.MinZ,
		MnX*RAGDOLL_M2U, MxX*RAGDOLL_M2U, MnY*RAGDOLL_M2U, MxY*RAGDOLL_M2U, MnZ*RAGDOLL_M2U, MxZ*RAGDOLL_M2U );
}

FRagdollLevelWorld& GetRagdollLevelWorldFor( ULevel* Level )
{
	FRagdollLevelWorld& LW = GetRagdollLevelWorld();
	if( !LW.bBuilt || LW.Level != Level )
	{
		if( LW.bBuilt )
			TeardownLevelWorld();
		BuildLevelWorld( LW, Level );
	}
	return LW;
}

/*-----------------------------------------------------------------------------
	Spawn clearance.
-----------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------------
	Movers as kinematic bodies (docs/plans/environment.md).

	The static mesh only holds the level's CSG; a Mover (door, lift, train)
	keeps its own brush model and moves by changing Location/Rotation. Each
	one becomes a kinematic Box3D body carrying one thin convex slab per brush
	face (the face polygon plus a copy pushed RAGDOLL_MOVER_SLAB UU into the
	brush), driven to the mover's pose before every world step. Slabs rather
	than a single hull keep non-convex movers (train cars) hollow; Box3D mesh
	shapes are out because they only collide on static bodies.

	Coordinates: brush vertices -> world through the engine's own
	ABrush::ToWorld (PrePivot, MainScale, Rotation, PostScale, Location) ->
	the mover's rigid frame (Location, Rotation) so the body transform is
	exactly (Location, quat(Rotation)) at every pose. Non-identity PostScale
	would not survive a rotating mover (it is applied after the rotation);
	movers never use it in retail maps, and the build logs a WARN if one does.
-----------------------------------------------------------------------------*/

#define RAGDOLL_MOVER_SLAB 16.0f      // slab thickness into the brush (UU), clamped to the brush depth per face
#define RAGDOLL_CRUSH_DEPTH 6.0f      // a body centre must be this far inside a mover on every axis to count as crushed
#define RAGDOLL_MOVER_CATCHUP 0.06f   // seconds over which a kinematic mover closes on the engine pose

// Rotation -> world axes of the local basis (the UnrealScript GetAxes idiom:
// GMath.UnitCoords / Rotation; its XAxis is the rotated forward vector). The
// matching Box3D quaternion has those axes as the columns of its matrix, the
// inverse of what SyncRagdollImpl does with OrthoRotation.
static b3Quat RagdollQuatFromRotator( const FRotator& Rot, FCoords* OutAxes )
{
	FCoords Rc = GMath.UnitCoords / Rot;
	if( OutAxes )
		*OutAxes = Rc;
	b3Matrix3 M;
	M.cx = { Rc.XAxis.X, Rc.XAxis.Y, Rc.XAxis.Z };
	M.cy = { Rc.YAxis.X, Rc.YAxis.Y, Rc.YAxis.Z };
	M.cz = { Rc.ZAxis.X, Rc.ZAxis.Y, Rc.ZAxis.Z };
	return b3MakeQuatFromMatrix( &M );
}

// World point -> the mover's rigid frame (UU in, UU out).
static FVector RagdollMoverLocal( const FVector& W, const FVector& Loc, const FCoords& Axes )
{
	const FVector D = W - Loc;
	return FVector( D | Axes.XAxis, D | Axes.YAxis, D | Axes.ZAxis );
}

// Collect the brush's face polygons in WORLD space (UU), each with the
// engine's plane normal (orientation fixed up against the brush centroid
// below). Prefers the editor polys (clean convex faces, outward normals);
// falls back to the brush BSP nodes.
struct FRagdollMoverFace
{
	FVector V[FBspNode::MAX_FINAL_VERTICES];
	INT     Num;
	FVector N;
};

static void RagdollCollectMoverFaces( AMover* M, TArray<FRagdollMoverFace>& Faces )
{
	UModel* B = M->Brush;
	if( !B )
		return;
	const FCoords ToWorld = M->ToWorld();
	if( B->Polys && B->Polys->Element.Num() > 0 )
	{
		for( INT i=0; i<B->Polys->Element.Num(); i++ )
		{
			const FPoly& P = B->Polys->Element(i);
			if( P.NumVertices < 3 || ( P.PolyFlags & PF_NotSolid ) )
				continue;
			FRagdollMoverFace F;
			F.Num = Min( (INT)P.NumVertices, (INT)FBspNode::MAX_FINAL_VERTICES );
			for( INT j=0; j<F.Num; j++ )
				F.V[j] = P.Vertex[j].TransformPointBy( ToWorld );
			F.N = P.Normal.TransformVectorBy( ToWorld ).SafeNormal();
			Faces.AddItem( F );
		}
		return;
	}
	for( INT i=0; i<B->Nodes.Num(); i++ )
	{
		const FBspNode& Node = B->Nodes(i);
		if( Node.NumVertices < 3 )
			continue;
		if( Node.iSurf >= 0 && Node.iSurf < B->Surfs.Num() && ( B->Surfs(Node.iSurf).PolyFlags & PF_NotSolid ) )
			continue;
		FRagdollMoverFace F;
		F.Num = Min( (INT)Node.NumVertices, (INT)FBspNode::MAX_FINAL_VERTICES );
		for( INT j=0; j<F.Num; j++ )
			F.V[j] = B->Points( B->Verts( Node.iVertPool + j ).pVertex ).TransformPointBy( ToWorld );
		F.N = FVector( Node.Plane.X, Node.Plane.Y, Node.Plane.Z ).TransformVectorBy( ToWorld ).SafeNormal();
		Faces.AddItem( F );
	}
}

void RagdollBuildMovers( FRagdollLevelWorld& LW, ULevel* Level )
{
	guard(RagdollBuildMovers);
	INT TotalSlabs = 0, Logged = 0;
	for( INT a=0; a<Level->Actors.Num(); a++ )
	{
		AMover* M = Cast<AMover>( Level->Actors(a) );
		if( !M || M->bDeleteMe || !M->Brush )
			continue;

		TArray<FRagdollMoverFace> Faces;
		RagdollCollectMoverFaces( M, Faces );
		if( Faces.Num() < 1 )
		{
			debugf( TEXT("RagdollPkg: mover %s has no solid faces, skipped"), M->GetName() );
			continue;
		}

		// Brush centroid (world) to orient every face normal OUT of the brush.
		FVector Centroid( 0, 0, 0 );
		INT     NumV = 0;
		FVector Mn( 1e9f, 1e9f, 1e9f ), Mx( -1e9f, -1e9f, -1e9f );
		for( INT f=0; f<Faces.Num(); f++ )
			for( INT j=0; j<Faces(f).Num; j++ )
			{
				const FVector& V = Faces(f).V[j];
				Centroid += V; NumV++;
				Mn.X = Min( Mn.X, V.X ); Mn.Y = Min( Mn.Y, V.Y ); Mn.Z = Min( Mn.Z, V.Z );
				Mx.X = Max( Mx.X, V.X ); Mx.Y = Max( Mx.Y, V.Y ); Mx.Z = Max( Mx.Z, V.Z );
			}
		Centroid /= Max( NumV, 1 );

		FCoords Axes;
		const b3Quat Q = RagdollQuatFromRotator( M->Rotation, &Axes );

		b3BodyDef Bd = b3DefaultBodyDef();
		Bd.type     = b3_kinematicBody;
		Bd.position = { M->Location.X * RAGDOLL_U2M, M->Location.Y * RAGDOLL_U2M, M->Location.Z * RAGDOLL_U2M };
		Bd.rotation = Q;
		Bd.userData = NULL;   // "level geometry" to the contact census; b3Body_GetType tells movers apart
		b3BodyId Body = b3CreateBody( LW.World, &Bd );

		b3ShapeDef Sd = b3DefaultShapeDef();
		Sd.baseMaterial.friction    = 0.6f;
		Sd.baseMaterial.restitution = 0.0f;
		Sd.filter.categoryBits      = RAGDOLL_GEOM_CATEGORY;
		Sd.density                  = 0.0f;   // kinematic: mass is irrelevant

		INT Slabs = 0;
		for( INT f=0; f<Faces.Num(); f++ )
		{
			FRagdollMoverFace& F = Faces(f);
			FVector N = F.N;
			if( N.SizeSquared() < 0.5f )
			{
				N = ( ( F.V[1] - F.V[0] ) ^ ( F.V[2] - F.V[0] ) ).SafeNormal();
				if( N.SizeSquared() < 0.5f )
					continue;
			}
			if( ( ( Centroid - F.V[0] ) | N ) > 0.0f )
				N = -N;   // pointed into the brush: flip outward
			// Slab depth: 16 UU (a 12 UU slab's mid-plane sat 6 UU in, and a
			// shin driven 5-6 UU into a lift by a hard landing flipped the
			// contact normal and was pushed THROUGH), but never deeper than
			// the brush itself along this face's normal, so a thin door or
			// lift gets no phantom thickness on its far side.
			FLOAT Depth = 0.0f;
			for( INT g=0; g<Faces.Num(); g++ )
				for( INT k=0; k<Faces(g).Num; k++ )
					Depth = Max( Depth, ( ( F.V[0] - Faces(g).V[k] ) | N ) );
			Depth = Clamp( Depth, 4.0f, RAGDOLL_MOVER_SLAB );
			b3Vec3 Pts[2 * FBspNode::MAX_FINAL_VERTICES];
			for( INT j=0; j<F.Num; j++ )
			{
				const FVector A = RagdollMoverLocal( F.V[j], M->Location, Axes ) * RAGDOLL_U2M;
				const FVector B = RagdollMoverLocal( F.V[j] - N * Depth, M->Location, Axes ) * RAGDOLL_U2M;
				Pts[j]         = { A.X, A.Y, A.Z };
				Pts[F.Num + j] = { B.X, B.Y, B.Z };
			}
			b3HullData* H = b3CreateHull( Pts, 2 * F.Num, 2 * F.Num );
			if( !H )
				continue;   // degenerate (sliver) face
			b3CreateHullShape( Body, &Sd, H );   // the world keeps its own copy
			b3DestroyHull( H );
			Slabs++;
		}

		FRagdollMoverBody MB;
		MB.Mover        = M;
		MB.Body         = Body;
		MB.LastLoc      = M->Location;
		MB.LastRot      = M->Rotation;
		MB.LastMoveTime = -1000.0f;
		MB.Vel          = FVector( 0, 0, 0 );
		MB.NumSlabs     = Slabs;
		MB.bEnabled     = 1;
		// Face planes in the rigid frame + convexity (exact inside test).
		MB.bConvex   = 1;
		MB.NumPlanes = 0;
		for( INT f=0; f<Faces.Num() && MB.NumPlanes < 64; f++ )
		{
			FRagdollMoverFace& F = Faces(f);
			FVector N = F.N;
			if( N.SizeSquared() < 0.5f )
				N = ( ( F.V[1] - F.V[0] ) ^ ( F.V[2] - F.V[0] ) ).SafeNormal();
			if( N.SizeSquared() < 0.5f )
				continue;
			if( ( ( Centroid - F.V[0] ) | N ) > 0.0f )
				N = -N;
			const FVector LN = FVector( N | Axes.XAxis, N | Axes.YAxis, N | Axes.ZAxis );
			const FVector LP = RagdollMoverLocal( F.V[0], M->Location, Axes );
			MB.LocalPlanes[MB.NumPlanes++] = FPlane( LP, LN );
		}
		if( Faces.Num() > 64 )
			MB.bConvex = 0;   // planes truncated: no exact test
		for( INT f=0; f<Faces.Num() && MB.bConvex; f++ )
			for( INT j=0; j<Faces(f).Num && MB.bConvex; j++ )
			{
				const FVector LV = RagdollMoverLocal( Faces(f).V[j], M->Location, Axes );
				for( INT k=0; k<MB.NumPlanes; k++ )
					if( MB.LocalPlanes[k].PlaneDot( LV ) > 1.0f )
					{
						MB.bConvex = 0;
						break;
					}
			}
		LW.Movers.AddItem( MB );
		TotalSlabs += Slabs;

		// Validation: our transformed vertex bounds against the engine's own
		// world bounds for the brush, and the rotator round trip through the
		// axes we hand Box3D. A mismatch means the kinematic body is not
		// where the engine's mover is.
		if( Logged < 6 )
		{
			Logged++;
			FBox EB = M->Brush->GetCollisionBoundingBox( M );
			const FLOAT Err = Max( Max( Abs( EB.Min.X - Mn.X ), Abs( EB.Min.Y - Mn.Y ) ), Max( Max( Abs( EB.Min.Z - Mn.Z ), Abs( EB.Max.X - Mx.X ) ), Max( Abs( EB.Max.Y - Mx.Y ), Abs( EB.Max.Z - Mx.Z ) ) ) );
			const FRotator Back = Axes.OrthoRotation();
			const INT RotErr = Max( Max( Abs( (INT)(SWORD)( Back.Yaw - M->Rotation.Yaw ) ), Abs( (INT)(SWORD)( Back.Pitch - M->Rotation.Pitch ) ) ), Abs( (INT)(SWORD)( Back.Roll - M->Rotation.Roll ) ) );
			const UBOOL bPostScale = ( Abs( M->PostScale.Scale.X - 1.0f ) > 0.001f || Abs( M->PostScale.Scale.Y - 1.0f ) > 0.001f || Abs( M->PostScale.Scale.Z - 1.0f ) > 0.001f );
			debugf( TEXT("RagdollPkg: mover %s faces=%i slabs=%i convex=%i loc=(%.0f,%.0f,%.0f) rot=(%i,%i,%i) bounds=(%.0f..%.0f, %.0f..%.0f, %.0f..%.0f) engineBounds=(%.0f..%.0f, %.0f..%.0f, %.0f..%.0f) boundsErr=%.1f rotErr=%i%s%s"),
				M->GetName(), Faces.Num(), Slabs, MB.bConvex, M->Location.X, M->Location.Y, M->Location.Z, M->Rotation.Pitch, M->Rotation.Yaw, M->Rotation.Roll,
				Mn.X, Mx.X, Mn.Y, Mx.Y, Mn.Z, Mx.Z, EB.Min.X, EB.Max.X, EB.Min.Y, EB.Max.Y, EB.Min.Z, EB.Max.Z, Err, RotErr,
				( Err > 2.0f || RotErr > 2 ) ? TEXT(" WARN mismatch") : TEXT(""), bPostScale ? TEXT(" WARN PostScale") : TEXT("") );
		}
	}
	debugf( TEXT("RagdollPkg: movers level=%s count=%i slabs=%i"), Level->GetName(), LW.Movers.Num(), TotalSlabs );
	unguard;
}

// Wake every corpse near a mover that is moving: Box3D does not wake a
// SLEEPING body that a kinematic body it already touches starts to carry
// (the resting contact never "begins" again), so a corpse asleep on a lift
// was left hanging in the air and then run through by the brush. Proximity
// (engine brush box + 200 UU) rather than contact lists, which do not list
// a sleeping island's contacts reliably. Returns the number of corpses woken.
static INT RagdollWakeNearMover( const FRagdollMoverBody& MB )
{
	AMover* M = MB.Mover;
	if( !M || !M->Brush )
		return 0;
	FBox EB = M->Brush->GetCollisionBoundingBox( M );
	const FLOAT Pad = 200.0f;
	INT Woken = 0;
	TMap<AActor*,FRagdollState>& Map = GetRagdollStates();
	for( TMap<AActor*,FRagdollState>::TIterator It( Map ); It; ++It )
	{
		FRagdollState& S = It.Value();
		if( !S.bBuilt )
			continue;
		UBOOL bNear = 0, bAsleep = 0;
		const INT NumBones = RagdollNumBones( S );
		for( INT b=0; b<NumBones && !bNear; b++ )
		{
			if( !RagdollBoneAlive( S, b ) )
				continue;
			b3BodyId Bid = RagdollBody( S, b );
			if( !b3Body_IsValid( Bid ) )
				continue;
			b3Vec3 P = b3Body_GetPosition( Bid );
			const FVector PU( P.x * RAGDOLL_M2U, P.y * RAGDOLL_M2U, P.z * RAGDOLL_M2U );
			if( PU.X > EB.Min.X - Pad && PU.X < EB.Max.X + Pad && PU.Y > EB.Min.Y - Pad && PU.Y < EB.Max.Y + Pad && PU.Z > EB.Min.Z - Pad && PU.Z < EB.Max.Z + Pad )
				bNear = 1;
		}
		if( !bNear )
			continue;
		for( INT b=0; b<NumBones; b++ )
		{
			if( !RagdollBoneAlive( S, b ) )
				continue;
			b3BodyId Bid = RagdollBody( S, b );
			if( b3Body_IsValid( Bid ) && !b3Body_IsAwake( Bid ) )
			{
				b3Body_SetAwake( Bid, true );
				bAsleep = 1;
			}
		}
		if( bAsleep )
			Woken++;
	}
	return Woken;
}

void RagdollSyncMovers( FRagdollLevelWorld& LW, FLOAT dt )
{
	guard(RagdollSyncMovers);
	if( !LW.bBuilt || LW.Movers.Num() == 0 )
		return;
	const FLOAT Now = appSeconds().GetFloat();
	static INT GSyncDbg = 0;
	for( INT i=0; i<LW.Movers.Num(); i++ )
	{
		FRagdollMoverBody& MB = LW.Movers(i);
		if( !MB.bEnabled || !b3Body_IsValid( MB.Body ) )
			continue;
		AMover* M = MB.Mover;
		if( !M || M->bDeleteMe )
		{
			b3Body_Disable( MB.Body );
			MB.bEnabled = 0;
			MB.Mover    = NULL;
			continue;
		}
		const UBOOL bMoved = ( M->Location != MB.LastLoc ) || ( M->Rotation != MB.LastRot );
		const UBOOL bWasMoving = ( Now - MB.LastMoveTime ) < 0.5f;
		if( !bMoved && !bWasMoving )
			continue;   // parked: nothing to do (velocity was zeroed when it stopped)

		// Engine velocity of the mover from its last two poses. The kinematic
		// body is driven toward the target over T = max(dt, CATCHUP) seconds
		// but each step only lasts dt, so right after a step it still trails
		// the target by Vel x (T - dt); aiming the target that far AHEAD of
		// the engine pose puts the body ON the engine brush after every step
		// instead of 20-30 UU behind it (the Deck16 lift at 315 UU/s carried
		// the corpse 28 UU inside its platform, 2026-10-11). A lead of the
		// full CATCHUP put it 16-22 UU ahead on a 20 Hz server (dt 0.05).
		if( bMoved )
		{
			const FLOAT Interval = Now - MB.LastMoveTime;
			if( Interval > 0.0005f && Interval < 0.5f )
				MB.Vel = ( M->Location - MB.LastLoc ) / Interval;
			else
				MB.Vel = FVector( 0, 0, 0 );
		}
		else if( ( Now - MB.LastMoveTime ) > 0.2f )
			MB.Vel = FVector( 0, 0, 0 );   // no engine update for 0.2 s: it stopped, park on the pose
		const FVector Lead = M->Location + MB.Vel * Max( RAGDOLL_MOVER_CATCHUP - dt, 0.0f );
		b3WorldTransform Target;
		Target.p = { Lead.X * RAGDOLL_U2M, Lead.Y * RAGDOLL_U2M, Lead.Z * RAGDOLL_U2M };
		Target.q = RagdollQuatFromRotator( M->Rotation, NULL );
		if( bMoved || bWasMoving )
		{
			// Velocity toward the engine pose, spread over RAGDOLL_MOVER_CATCHUP
			// seconds rather than this one step: the engine updates the mover
			// at ITS tick rate (a 20 Hz dedicated server moves a Deck16 lift
			// 16 UU per update), and driving the whole jump in one 20 ms step
			// made the kinematic body lurch at 2-3x the lift's speed and
			// tunnel through the corpse lying on it (slab 12 UU). Spread out,
			// the body moves at the lift's real speed and trails the engine
			// brush by less than one engine tick; contacts carry the corpse.
			b3Body_SetTargetTransform( MB.Body, Target, Max( dt, RAGDOLL_MOVER_CATCHUP ), true );
		}
		if( bMoved )
		{
			const UBOOL bStarted = !bWasMoving;
			MB.LastLoc      = M->Location;
			MB.LastRot      = M->Rotation;
			MB.LastMoveTime = Now;
			// Wake the corpses around a mover that starts moving (and again
			// every ~0.3 s while it moves: a corpse may fall asleep on it).
			static FLOAT GLastWake = -1000.0f;
			if( bStarted || Now - GLastWake > 0.3f )
			{
				GLastWake = Now;
				const INT Woken = RagdollWakeNearMover( MB );
				if( ( bStarted || Woken ) && GSyncDbg < 40 )
				{
					GSyncDbg++;
					b3Vec3 KP = b3Body_GetPosition( MB.Body );
					debugf( TEXT("RagdollPkg: moversync %s %s engZ=%.1f kinZ=%.1f woke=%i"), M->GetName(), bStarted ? TEXT("START") : TEXT("moving"), M->Location.Z, KP.z * RAGDOLL_M2U, Woken );
				}
			}
		}
		else if( ( Now - MB.LastMoveTime ) >= 0.5f - 0.5f * dt )
		{
			// Stopped half a second ago: kill the kinematic velocity and
			// snap the last bit of drift onto the engine pose.
			b3Body_SetLinearVelocity( MB.Body, b3Vec3_zero );
			b3Body_SetAngularVelocity( MB.Body, b3Vec3_zero );
			MB.Vel = FVector( 0, 0, 0 );
			const b3Vec3 Park = { M->Location.X * RAGDOLL_U2M, M->Location.Y * RAGDOLL_U2M, M->Location.Z * RAGDOLL_U2M };
			b3Body_SetTransform( MB.Body, Park, Target.q );
		}
	}
	unguard;
}

// Inside test: the convex-planes test for convex brushes (exact), else the
// slab test (Box3D overlap of a 1 cm sphere with this mover's slabs, i.e.
// within RAGDOLL_MOVER_SLAB of a face on the inside).
struct FRagdollSlabProbe
{
	b3BodyId Body;
	UBOOL    bHit;
};

static bool RagdollSlabProbeFcn( b3ShapeId ShapeId, void* Context )
{
	FRagdollSlabProbe* P = (FRagdollSlabProbe*)Context;
	if( b3Shape_IsValid( ShapeId ) && B3_ID_EQUALS( b3Shape_GetBody( ShapeId ), P->Body ) )
	{
		P->bHit = 1;
		return false;
	}
	return true;
}

static UBOOL RagdollPointNearSlabs( FRagdollLevelWorld& LW, const FRagdollMoverBody& MB, const FVector& PointUU )
{
	b3Vec3 Pt = { 0.0f, 0.0f, 0.0f };
	b3ShapeProxy Proxy;
	Proxy.points = &Pt;
	Proxy.count  = 1;
	Proxy.radius = 0.001f;
	b3QueryFilter Filter = b3DefaultQueryFilter();
	Filter.maskBits = RAGDOLL_GEOM_CATEGORY;
	FRagdollSlabProbe P;
	P.Body = MB.Body;
	P.bHit = 0;
	b3Pos Origin = { PointUU.X * RAGDOLL_U2M, PointUU.Y * RAGDOLL_U2M, PointUU.Z * RAGDOLL_U2M };
	b3World_OverlapShape( LW.World, Origin, &Proxy, Filter, RagdollSlabProbeFcn, &P );
	return P.bHit;
}

static UBOOL RagdollMoverContains( FRagdollLevelWorld& LW, FRagdollMoverBody& MB, const FVector& PointUU )
{
	AMover* M = MB.Mover;
	// Everything below is against the KINEMATIC body's pose, which trails
	// the engine brush by up to one engine tick while the mover moves (see
	// RagdollSyncMovers): the corpse rests on the kinematic body, so a test
	// against the engine pose would see the bodies on a rising lift as
	// "inside" the brush. The engine's UModel::PointCheck is not used: on
	// the Deck16 lifts it reported the brush's own centre as clear.
	const b3WorldTransform T = b3Body_GetTransform( MB.Body );
	const b3Vec3 P = { PointUU.X * RAGDOLL_U2M, PointUU.Y * RAGDOLL_U2M, PointUU.Z * RAGDOLL_U2M };
	FBox EB = M->Brush->GetCollisionBoundingBox( M );
	const FLOAT Slack = 40.0f;   // engine box vs kinematic pose: one engine tick of travel
	if( PointUU.X < EB.Min.X - Slack || PointUU.X > EB.Max.X + Slack
	 || PointUU.Y < EB.Min.Y - Slack || PointUU.Y > EB.Max.Y + Slack
	 || PointUU.Z < EB.Min.Z - Slack || PointUU.Z > EB.Max.Z + Slack )
		return 0;
	// Exact test for a convex brush: behind every face plane (rigid frame
	// of the kinematic pose) by more than half a UU.
	if( MB.bConvex && MB.NumPlanes >= 4 )
	{
		b3Transform Tl;
		Tl.p = { T.p.x, T.p.y, T.p.z };
		Tl.q = T.q;
		const b3Vec3 Lm = b3InvTransformPoint( Tl, P );
		const FVector L( Lm.x * RAGDOLL_M2U, Lm.y * RAGDOLL_M2U, Lm.z * RAGDOLL_M2U );
		UBOOL bInside = 1;
		for( INT k=0; k<MB.NumPlanes; k++ )
			if( MB.LocalPlanes[k].PlaneDot( L ) > -RAGDOLL_CRUSH_DEPTH )   // a carried limb dips a few UU into the top face
			{
				bInside = 0;
				break;
			}
		if( bInside )
			return 1;
	}
	// Non-convex brush: the slab probe says "inside the shell", which a limb
	// resting 1 UU into the top face of a lift also satisfies (the Deck16
	// lift is 26-faced and fails the convexity test, and a corpse that
	// landed hard on it at the bottom was deleted as crushed, 2026-10-11).
	// Crushed = the centre AND the six points RAGDOLL_CRUSH_DEPTH away along
	// the world axes are all inside: the body is engulfed, not dipped.
	if( !RagdollPointNearSlabs( LW, MB, PointUU ) )
		return 0;
	static const FVector Offs[6] = { FVector( 1, 0, 0 ), FVector( -1, 0, 0 ), FVector( 0, 1, 0 ), FVector( 0, -1, 0 ), FVector( 0, 0, 1 ), FVector( 0, 0, -1 ) };
	for( INT k=0; k<6; k++ )
		if( !RagdollPointNearSlabs( LW, MB, PointUU + Offs[k] * RAGDOLL_CRUSH_DEPTH ) )
			return 0;
	return 1;
}

AMover* RagdollPointInMover( FRagdollLevelWorld& LW, const FVector& PointUU, UBOOL bRecentOnly )
{
	guard(RagdollPointInMover);
	if( !LW.bBuilt )
		return NULL;
	const FLOAT Now = appSeconds().GetFloat();
	for( INT i=0; i<LW.Movers.Num(); i++ )
	{
		FRagdollMoverBody& MB = LW.Movers(i);
		if( !MB.bEnabled || !MB.Mover || MB.Mover->bDeleteMe || !MB.Mover->Brush )
			continue;
		if( bRecentOnly && ( Now - MB.LastMoveTime ) > 1.0f )
			continue;
		if( RagdollMoverContains( LW, MB, PointUU ) )
			return MB.Mover;
	}
	return NULL;
	unguard;
}

// The T-pose corpse is placed exactly at the death location. If the pawn died
// on a ramp, stairs, or right against a step, some bodies can spawn embedded
// in the level mesh, and Box3D explodes interpenetrating bodies on the first
// step. Before the first step, cast a short ray down at each body and lift it
// above the first solid surface so the corpse starts clear.
//
// The ray only scans a TIGHT window around the body center (+0.35m to -0.35m):
// a surface that close above the center means the body is embedded in
// geometry. Scanning farther up would falsely hit walkway/ceiling undersides
// meters above a standing corpse and launch it into the air at spawn.
void ClearCorpseSpawn( FRagdollState& S, FRagdollLevelWorld& LW )
{
	b3QueryFilter Filter = b3DefaultQueryFilter();
	Filter.maskBits       = RAGDOLL_GEOM_CATEGORY;   // level mesh only, no corpses

	INT Lifted = 0;
	static INT GDbgCorpses = 0;
	const UBOOL bDbg = ( GDbgCorpses < 3 );
	for( INT i=0; i<S.NumBodies; i++ )
	{
		b3Vec3 P = b3Body_GetPosition( S.Bodies[i] );
		b3Vec3 Origin      = { P.x, P.y, P.z + 0.35f };
		b3Vec3 Translation = { 0.0f, 0.0f, -0.70f };
		b3RayResult R = b3World_CastRayClosest( LW.World, Origin, Translation, Filter );
		if( !R.hit || R.point.z <= P.z + 0.05f )
			continue;

		if( bDbg )
			debugf( TEXT("RagdollPkg: clearance bone=%i Pz=%.2f hitZ=%.2f gap=%.0fUU"),
				i, P.z, R.point.z, ( R.point.z - P.z ) * RAGDOLL_M2U );

		// Embedded: a solid surface is within 0.35m above the body center.
		FLOAT NewZ = R.point.z + 0.15f;
		if( NewZ - P.z > 0.5f )
			NewZ = P.z + 0.5f;   // cap so limbs are not teleported away
		// Keep the death-pose rotation: resetting to identity used to snap a
		// lifted limb back to the bind orientation while its joint partner kept
		// the posed one, so the chain started every spawn with a limit violation.
		b3Body_SetTransform( S.Bodies[i], { P.x, P.y, NewZ }, b3Body_GetRotation( S.Bodies[i] ) );
		Lifted++;
	}
	if( Lifted )
	{
		if( bDbg )
			GDbgCorpses++;
		debugf( TEXT("RagdollPkg: spawn clearance lifted %i bones"), Lifted );
	}
}
