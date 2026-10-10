// RagdollTestMutator: temporary headless test driver. Drives every native
// (BuildRagdoll / StepRagdoll / SyncRagdoll / ShutdownRagdoll) on a dedicated
// server via a directly-spawned RagdollCarcass, and validates that the
// RagdollSpawnNotify swaps a bare Carcass for a RagdollCarcass. Remove before
// shipping.
class RagdollTestMutator extends Mutator;

var int Sweeps;
var NavigationPoint StartSpot;

function PostBeginPlay()
{
	Super.PostBeginPlay();
	Log("RagdollTestMutator: PostBeginPlay game=" $ Level.Game, 'RagdollTest');
	SetTimer(1.0, true);
}

function Timer()
{
	local Actor A;
	local Carcass C;
	local RagdollCarcass R;
	local vector Loc;
	local int NumR;

	Sweeps++;
	Log("RagdollTestMutator: Timer sweep=" $ Sweeps, 'RagdollTest');
	if ( Sweeps > 16 )
	{
		SetTimer(0.0, false);
		return;
	}

	if ( StartSpot == None )
		foreach AllActors(class'NavigationPoint', StartSpot)
			if ( StartSpot.IsA('PlayerStart') )
				break;
	if ( StartSpot != None )
		Loc = StartSpot.Location;
	else
		Loc = Level.Location + vect(0,0,200);

	// 1) Probe several candidate locations for a bare Carcass spawn; the
	//    RagdollSpawnNotify should swap it for a RagdollCarcass.
	Log("RagdollTestMutator: Level.SpawnNotify=" $ Level.SpawnNotify, 'RagdollTest');
	C = Spawn(class'Carcass',,, Loc);
	Log("RagdollTestMutator: Carcass@start got " $ C, 'RagdollTest');
	C = Spawn(class'Carcass',,, Loc + vect(0,0,8));
	Log("RagdollTestMutator: Carcass@start+8 got " $ C, 'RagdollTest');
	C = Spawn(class'Carcass',,, Loc + vect(0,100,64));
	Log("RagdollTestMutator: Carcass@+100,64 got " $ C, 'RagdollTest');
	C = Spawn(class'Carcass',,, Loc + vect(0,-100,64));
	Log("RagdollTestMutator: Carcass@-100,64 got " $ C, 'RagdollTest');

	// 2) Drive the natives directly on a real-mesh (Commando) corpse: build,
	//    then exercise the gore natives on successive sweeps so the headless
	//    log shows SplitIslands / gib / removed-part lines without a renderer.
	R = Spawn(class'RagdollCarcass',,, Loc + vect(60,100,0));
	if ( R != None )
	{
		R.DrawScale = 1.0;
		R.SimFreq = 50.0;
		R.SimSubSteps = 4;
		R.Mesh = Mesh(DynamicLoadObject("Botpack.Commando", class'Mesh'));
		R.MultiSkins[0] = Texture(DynamicLoadObject("CommandoSkins.cmdo1", class'Texture'));
		R.MultiSkins[1] = Texture(DynamicLoadObject("CommandoSkins.cmdo2", class'Texture'));
		R.MultiSkins[2] = Texture(DynamicLoadObject("CommandoSkins.cmdo3", class'Texture'));
		R.MultiSkins[3] = Texture(DynamicLoadObject("CommandoSkins.cmdo4", class'Texture'));
		R.BuildRagdoll();
		R.LoadGoreClasses();
		Log("RagdollTestMutator: direct RagdollCarcass " $ R $ " at " $ R.Location $ " mesh=" $ R.Mesh, 'RagdollTest');
		switch ( Sweeps % 4 )
		{
			case 1:
				R.PaintWound(R.GetBodyLocation(1) + vect(0,-8,0), 17, 'shot');
				R.PaintWound(R.GetBodyLocation(8), 30, 'shredded');
				R.PaintWound(R.GetBodyLocation(3), 70, 'exploded');
				R.PaintWound(R.GetBodyLocation(5), 40, 'jolted');
				Log("RagdollTestMutator: Dismember forearmL -> " $ R.Dismember(6, vect(0,0,300)) $ " attached=" $ R.BodyAttached(6) $ " loc=" $ R.GetBodyLocation(6), 'RagdollTest');
				Log("RagdollTestMutator: Dismember thighR -> " $ R.Dismember(9, vect(200,0,300)), 'RagdollTest');
				break;
			case 2:
				Log("RagdollTestMutator: RemovePart head -> " $ R.RemovePart(3) $ " nearest=" $ R.NearestBody(R.Location), 'RagdollTest');
				break;
			case 3:
				R.ChunkUp(40 + (Sweeps * 53) % 180);
				Log("RagdollTestMutator: ChunkUp dmg=" $ (40 + (Sweeps * 53) % 180) $ " gibbed=" $ R.bGibbed $ " lifespan=" $ R.LifeSpan, 'RagdollTest');
				break;
		}
	}

	foreach AllActors(class'RagdollCarcass', R)
		NumR++;
	Log("RagdollTestMutator: RagdollCarcass count=" $ NumR, 'RagdollTest');
}
