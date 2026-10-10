// RagdollTestMutator: temporary headless test driver. Drives every native
// (BuildRagdoll / StepRagdoll / SyncRagdoll / ShutdownRagdoll) on a dedicated
// server via a directly-spawned RagdollCarcass, and validates that the
// RagdollSpawnNotify swaps a bare Carcass for a RagdollCarcass. Remove before
// shipping.
class RagdollTestMutator extends Mutator;

var int Sweeps;
var NavigationPoint StartSpot;
var RagdollCarcass StackLower, StackUpper;   // corpse-on-corpse stacking test
var vector StackSpot;                        // floor-checked spawn spot for it

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
	local int NumR, NumRaised;
	local float PileTop;

	Sweeps++;
	PileTop = -100000;
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
				R.PaintWound(R.GetBodyLocation(1) + vect(0,-80,0), vect(0,1,0), 17, 'shot');   // along a ray from 80 UU away
				R.PaintWound(R.GetBodyLocation(8), vect(0,0,0), 30, 'shredded');
				R.PaintWound(R.GetBodyLocation(3) + vect(0,0,60), vect(0,0,-1), 70, 'exploded');
				R.PaintWound(R.GetBodyLocation(5), vect(0,0,0), 40, 'jolted');
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

	// 3) Stacking: a corpse dropped onto a settled corpse must come to rest ON
	//    it (upper pelvis clearly above the lower one), not sink through.
	//    Lower corpse at sweep 2, upper one 70 UU above it at sweep 5 (the
	//    lower has had 3 s to settle); pelvis heights logged every sweep.
	//    The spot must be over solid floor (the first guess, 150 UU west of
	//    the start, was off a ledge and both corpses fell into the pit).
	if ( Sweeps == 2 )
	{
		StackSpot = FindFloorSpot(Loc);
		StackLower = SpawnTestCorpse(StackSpot);
	}
	if ( Sweeps == 5 )
		StackUpper = SpawnTestCorpse(StackSpot + vect(0,0,70));
	//    Reported: the highest body of each corpse (a corpse lying on the
	//    floor tops out ~15 UU above it; one resting on another should top
	//    out ~10+ UU higher than the lower one) and the pelvis-pelvis
	//    horizontal distance (small = landed on it, large = slid off).
	if ( StackLower != None && StackUpper != None )
		Log("RagdollTestMutator: stack lowerTopZ=" $ TopZ(StackLower) $ " upperTopZ=" $ TopZ(StackUpper)
			$ " lowerPelvisZ=" $ StackLower.GetBodyLocation(0).Z $ " upperPelvisZ=" $ StackUpper.GetBodyLocation(0).Z
			$ " pelvisXYdist=" $ VSize((StackUpper.GetBodyLocation(0) - StackLower.GetBodyLocation(0)) * vect(1,1,0)), 'RagdollTest');

	// 4) The gore corpses all spawn on the same spot: a pile. Its top should
	//    climb as corpses accumulate if they rest on each other.
	foreach AllActors(class'RagdollCarcass', R)
	{
		NumR++;
		if ( R != StackLower && R != StackUpper )
		{
			PileTop = FMax(PileTop, TopZ(R));
			if ( R.GetBodyLocation(0).Z > StackSpot.Z - 42 + 15 )   // pelvis more than ~15 UU above the floor
				NumRaised++;
		}
	}
	Log("RagdollTestMutator: RagdollCarcass count=" $ NumR $ " pileTopZ=" $ PileTop $ " raisedPelvises=" $ NumRaised, 'RagdollTest');
}

// Highest body centre of a corpse (bodies 0-11; detached parts included).
function float TopZ(RagdollCarcass R)
{
	local int i;
	local float Z;

	Z = -100000;
	for ( i = 0; i < 12; i++ )
		Z = FMax(Z, R.GetBodyLocation(i).Z);
	return Z;
}

// First candidate offset around the start whose floor is within 100 UU below
// the pawn-height point and whose spawn is clear of the gore pile (60,100).
function vector FindFloorSpot(vector Loc)
{
	local vector Off, HitLoc, HitNorm;
	local int i;

	for ( i = 0; i < 6; i++ )
	{
		switch ( i )
		{
			case 0: Off = vect(-120,0,0); break;
			case 1: Off = vect(120,0,0); break;
			case 2: Off = vect(0,-120,0); break;
			case 3: Off = vect(-120,-120,0); break;
			case 4: Off = vect(120,-120,0); break;
			default: Off = vect(0,0,0);
		}
		if ( Trace(HitLoc, HitNorm, Loc + Off - vect(0,0,100), Loc + Off, false) != None && HitNorm.Z > 0.7 )
		{
			Log("RagdollTestMutator: stack spot " $ (Loc + Off) $ " floorZ=" $ HitLoc.Z, 'RagdollTest');
			return Loc + Off;
		}
	}
	Log("RagdollTestMutator: stack spot fallback to start " $ Loc, 'RagdollTest');
	return Loc;
}

// A plain Commando ragdoll with no gore applied (the stacking corpses).
function RagdollCarcass SpawnTestCorpse(vector Loc)
{
	local RagdollCarcass R;

	R = Spawn(class'RagdollCarcass',,, Loc);
	if ( R == None )
		return None;
	R.DrawScale = 1.0;
	R.SimFreq = 50.0;
	R.SimSubSteps = 4;
	R.LifeSpan = 60;
	R.Mesh = Mesh(DynamicLoadObject("Botpack.Commando", class'Mesh'));
	R.MultiSkins[0] = Texture(DynamicLoadObject("CommandoSkins.cmdo1", class'Texture'));
	R.MultiSkins[1] = Texture(DynamicLoadObject("CommandoSkins.cmdo2", class'Texture'));
	R.MultiSkins[2] = Texture(DynamicLoadObject("CommandoSkins.cmdo3", class'Texture'));
	R.MultiSkins[3] = Texture(DynamicLoadObject("CommandoSkins.cmdo4", class'Texture'));
	R.BuildRagdoll();
	Log("RagdollTestMutator: stack corpse " $ R $ " at " $ Loc, 'RagdollTest');
	return R;
}
