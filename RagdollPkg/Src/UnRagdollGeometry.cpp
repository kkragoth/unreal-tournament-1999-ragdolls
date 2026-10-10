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
		b3DestroyWorld( LW.World );
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
	Wd.gravity     = { 0.0f, 0.0f, -9.81f };
	Wd.enableSleep = true;
	Wd.workerCount = 0;

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
	LW.bBuilt   = true;

	b3CreateMeshShape( LW.Geometry, &Sd, Mesh, b3Vec3_one );

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
