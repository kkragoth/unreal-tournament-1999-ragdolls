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

	// 2) Drive the natives directly.
	R = Spawn(class'RagdollCarcass',,, Loc + vect(60,100,0));
	if ( R != None )
	{
		R.DrawScale = 1.0;
		R.SimFreq = 50.0;
		R.SimSubSteps = 4;
		R.BuildRagdoll();
		Log("RagdollTestMutator: direct RagdollCarcass " $ R $ " at " $ R.Location, 'RagdollTest');
	}

	foreach AllActors(class'RagdollCarcass', R)
		NumR++;
	Log("RagdollTestMutator: RagdollCarcass count=" $ NumR, 'RagdollTest');
}
