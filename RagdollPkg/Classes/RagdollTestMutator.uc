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
var RagdollCarcass BeamCorpse;               // pulse-beam push test (Timer step 3b)
var vector BeamStart;                        // its spine when the first packet landed
// Environment tests (docs/plans/environment.md): water buoyancy, a corpse
// riding a mover, crushing.
var RagdollCarcass WaterCorpse, LavaCorpse, RideCorpse, CrushCorpse, ControlCorpse;
var ZoneInfo  WaterZone;
var Mover     TestMover, CrushMover;
var bool      bZoneWasDestructive;
var vector    MoverTop;        // top surface of the mover at rest
var float     MoverRestZ;

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

	// 1b) Limb-spread sample: on the first sweep, up to six plain corpses on
	//     their own floor spots around the start (nothing piled on them, no
	//     gore), spawned BEFORE the gore corpse so they take the first slots
	//     of the native joint census ("jointangles ... legspread=").
	if ( Sweeps == 1 )
		SpawnSpreadSample(Loc);

	// 2) Drive the natives directly on a real-mesh (Commando) corpse: build,
	//    then exercise the gore natives on successive sweeps so the headless
	//    log shows SplitIslands / gib / removed-part lines without a renderer.
	R = Spawn(class'RagdollCarcass',,, Loc + vect(60,100,0));
	if ( R != None )
	{
		R.DrawScale = 1.0;
		R.SimFreq = 50.0;
		R.SimSubSteps = 8;
		R.Mesh = Mesh(DynamicLoadObject("Botpack.Commando", class'Mesh'));
		R.MultiSkins[0] = Texture(DynamicLoadObject("CommandoSkins.cmdo1", class'Texture'));
		R.MultiSkins[1] = Texture(DynamicLoadObject("CommandoSkins.cmdo2", class'Texture'));
		R.MultiSkins[2] = Texture(DynamicLoadObject("CommandoSkins.cmdo3", class'Texture'));
		R.MultiSkins[3] = Texture(DynamicLoadObject("CommandoSkins.cmdo4", class'Texture'));
		R.SetDeathPose(Rand(65536), 0);   // random facing + the random spawn pose, as Initfor would
		// Death-animation spawn pose (ApplyAnimationPose): pretend the corpse
		// came from a pawn ~0.2 s into a Dead* animation, as Initfor copies
		// it, so the headless log carries 'RagdollPkg: animpose' lines.
		R.AnimSequence = DeadAnimForSweep(Sweeps);
		R.AnimFrame = 0.05 + 0.1 * (Sweeps % 3);
		R.AnimRate = 0;
		R.BuildRagdoll();
		R.LoadGoreClasses();
		Log("RagdollTestMutator: direct RagdollCarcass " $ R $ " at " $ R.Location $ " mesh=" $ R.Mesh $ " anim=" $ R.AnimSequence $ "@" $ R.AnimFrame, 'RagdollTest');
		switch ( Sweeps % 4 )
		{
			case 0:
				DamageModelTest(R);
				break;
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

	// 3b) Pulse beam push (RagdollPush): a lone corpse settles for 2 s, then
	//     one beam packet (PBolt: 16 dmg / 1900 momentum per 0.22 s) lands on
	//     its spine from the -Y side on sweeps 5, 6 and 7 (a real beam sends
	//     one every 0.22 s; here the 0.25 s push has died down before the
	//     next). "beamtest moved=" is the spine / pelvis displacement since
	//     the first packet: expect tens of UU along +Y per packet (dragged
	//     along the floor), where the old impulse path gave ~0.
	if ( Sweeps == 3 )
	{
		BeamCorpse = SpawnTestCorpse(FindFloorSpot(StackSpot + vect(0,-90,0)));
		if ( BeamCorpse != None )
		{
			BeamCorpse.bDecorative = false;
			BeamCorpse.GotoState('Dead');
		}
	}
	if ( BeamCorpse != None && Sweeps >= 5 && Sweeps <= 8 )
	{
		if ( Sweeps == 5 )
			BeamStart = BeamCorpse.GetBodyLocation(1);
		else
			Log("RagdollTestMutator: beamtest moved=" $ VSize(BeamCorpse.GetBodyLocation(1) - BeamStart) $ " UU spine (" $ (BeamCorpse.GetBodyLocation(1) - BeamStart) $ ") pelvis=" $ VSize(BeamCorpse.GetBodyLocation(0) - BeamStart) $ " after " $ (Sweeps - 5) $ " packet(s) spineAttached=" $ BeamCorpse.BodyAttached(1) $ " ledger=" $ BeamCorpse.RagdollLedger(1), 'RagdollTest');
		if ( Sweeps <= 7 )
			BeamCorpse.TakeDamage(16, None, BeamCorpse.GetBodyLocation(1) - vect(0,30,0), vect(0,1,0) * 1900, 'zapped');
	}
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

	// 5) Environment: water, mover ride, crush (sweeps 6+).
	EnvironmentTest();
}

/*-----------------------------------------------------------------------------
	Environment tests. Every sweep logs "envtest ..." lines:
	 - water: a corpse dropped into the map's first water zone (Deck16: the
	   SlimeZone pool; its bDestructive is cleared on the actor so buoyancy
	   can be measured) must RISE toward the surface and settle (pelvis Z
	   climbing, then steady), not sink to the bottom; a second corpse goes
	   in with bDestructive restored and must be gone next sweep.
	 - mover: a corpse spawned on the first Mover's top surface, the mover
	   triggered; the corpse's pelvis Z must follow the mover's Z (ride).
	 - crush: a corpse spawned with its pelvis INSIDE that mover brush gets
	   TakeDamage(10000, 'Crushed') and must be destroyed; a control corpse
	   on open floor given the same damage must survive.
-----------------------------------------------------------------------------*/
function EnvironmentTest()
{
	local ZoneInfo Z;
	local Mover M;
	local vector HitLoc, HitNorm, Loc;
	local int i;

	if ( Sweeps == 6 )
	{
		foreach AllActors(class'ZoneInfo', Z)
			if ( Z.bWaterZone && !Z.IsA('LevelInfo') )
			{
				WaterZone = Z;
				break;
			}
		foreach AllActors(class'Mover', M)
		{
			if ( TestMover == None )
				TestMover = M;
			else if ( CrushMover == None )
				CrushMover = M;
		}
		if ( CrushMover == None )
			CrushMover = TestMover;
		Log("RagdollTestMutator: envtest setup water=" $ WaterZone $ " mover=" $ TestMover $ " crushMover=" $ CrushMover, 'RagdollTest');
		if ( WaterZone != None )
		{
			Log("RagdollTestMutator: envtest water zone " $ WaterZone $ " at " $ WaterZone.Location $ " destructive=" $ WaterZone.bDestructive $ " pain=" $ WaterZone.bPainZone $ " gravity=" $ WaterZone.ZoneGravity $ " vel=" $ WaterZone.ZoneVelocity, 'RagdollTest');
			// The zone stays non-destructive until sweep 13 so the buoyancy
			// can be measured (the native pass reads the flag every tick).
			bZoneWasDestructive = WaterZone.bDestructive;
			WaterZone.bDestructive = false;
			// Dropped from 40 UU above the zone actor (which sits inside the
			// pool) so the entry splash path runs too.
			WaterCorpse = SpawnTestCorpse(WaterZone.Location + vect(0,0,40));
			if ( WaterCorpse != None )
				WaterCorpse.LifeSpan = 0;
		}
		if ( TestMover != None )
		{
			MoverRestZ = TestMover.Location.Z;
			if ( Trace(HitLoc, HitNorm, TestMover.Location - vect(0,0,400), TestMover.Location + vect(0,0,400), false) == TestMover )
				MoverTop = HitLoc;
			else if ( Trace(HitLoc, HitNorm, TestMover.Location - vect(0,0,400), TestMover.Location + vect(0,0,400), true) != None )
				MoverTop = HitLoc;
			else
				MoverTop = TestMover.Location;
			Log("RagdollTestMutator: envtest mover " $ TestMover $ " loc=" $ TestMover.Location $ " top=" $ MoverTop $ " keys=" $ TestMover.NumKeys $ " keypos1=" $ TestMover.KeyPos[1] $ " movetime=" $ TestMover.MoveTime $ " encroach=" $ TestMover.MoverEncroachType, 'RagdollTest');
			RideCorpse = SpawnTestCorpse(MoverTop + vect(0,0,30));
			if ( RideCorpse != None )
				RideCorpse.LifeSpan = 0;
		}
	}
	if ( Sweeps == 13 && WaterZone != None )
	{
		// Destructive entry (the pool's real flag is back on): the floating
		// corpse AND a fresh one must be gone by the next sweep.
		WaterZone.bDestructive = true;
		LavaCorpse = SpawnTestCorpse(WaterZone.Location + vect(60,0,40));
		Log("RagdollTestMutator: envtest lava corpse " $ LavaCorpse $ " spawned into destructive zone (was destructive=" $ bZoneWasDestructive $ ")", 'RagdollTest');
	}
	if ( Sweeps == 16 && WaterZone != None )
		WaterZone.bDestructive = bZoneWasDestructive;
	if ( Sweeps == 8 && TestMover != None )
	{
		// The corpse has settled on the lift: run the lift's own Open label
		// (StandOpenTimed has no Trigger; players start it by standing on
		// it): up over MoveTime, StayOpenTime, back down.
		TestMover.GotoState('StandOpenTimed', 'Open');
		Log("RagdollTestMutator: envtest mover opened state=" $ TestMover.GetStateName() $ " stayOpen=" $ TestMover.StayOpenTime, 'RagdollTest');
	}
	if ( Sweeps == 10 && CrushMover != None )
	{
		// Crush: pelvis inside the second lift's brush (20 UU under its top
		// surface) + the engine's damage; control corpse on open floor.
		if ( Trace(HitLoc, HitNorm, CrushMover.Location - vect(0,0,400), CrushMover.Location + vect(0,0,400), true) != None )
			Loc = HitLoc;
		else
			Loc = CrushMover.Location;
		Loc.Z -= 20;
		CrushCorpse = SpawnTestCorpse(Loc);
		ControlCorpse = SpawnTestCorpse(StackSpot + vect(0,60,0));
		// The engine reports 'Crushed' every tick the brush overlaps the
		// cylinder; the native check needs two consecutive reports.
		if ( CrushCorpse != None )
		{
			CrushCorpse.bDecorative = false;
			CrushCorpse.GotoState('Dead');
			for ( i = 0; i < 3 && !CrushCorpse.bDeleteMe; i++ )
				CrushCorpse.TakeDamage(10000, None, CrushCorpse.Location, vect(0,0,0), 'Crushed');
		}
		if ( ControlCorpse != None )
		{
			ControlCorpse.bDecorative = false;
			ControlCorpse.GotoState('Dead');
			for ( i = 0; i < 3; i++ )
				ControlCorpse.TakeDamage(10000, None, ControlCorpse.Location, vect(0,0,0), 'Crushed');
		}
		Log("RagdollTestMutator: envtest crush spawned at " $ Loc $ " in " $ CrushMover $ " crush=" $ CrushCorpse $ " deleteMe=" $ (CrushCorpse == None || CrushCorpse.bDeleteMe) $ " control=" $ ControlCorpse $ " controlDeleteMe=" $ (ControlCorpse == None || ControlCorpse.bDeleteMe), 'RagdollTest');
	}
	if ( Sweeps >= 7 )
	{
		if ( WaterCorpse != None )
			Log("RagdollTestMutator: envtest water sweep=" $ Sweeps $ " pelvisZ=" $ WaterCorpse.GetBodyLocation(0).Z $ " topZ=" $ TopZ(WaterCorpse) $ " zoneZ=" $ WaterZone.Location.Z $ " zone=" $ WaterCorpse.Region.Zone, 'RagdollTest');
		else
			Log("RagdollTestMutator: envtest water sweep=" $ Sweeps $ " corpse gone", 'RagdollTest');
		if ( Sweeps >= 14 )
			Log("RagdollTestMutator: envtest lava sweep=" $ Sweeps $ " corpse=" $ LavaCorpse $ " (None = destroyed as expected)", 'RagdollTest');
		if ( TestMover != None )
		{
			if ( RideCorpse != None )
				Log("RagdollTestMutator: envtest mover sweep=" $ Sweeps $ " moverZ=" $ TestMover.Location.Z $ " restZ=" $ MoverRestZ $ " pelvisZ=" $ RideCorpse.GetBodyLocation(0).Z $ " topAtRest=" $ MoverTop.Z $ " state=" $ TestMover.GetStateName() $ " pelvisXY=" $ (RideCorpse.GetBodyLocation(0) * vect(1,1,0)) $ " topXY=" $ (MoverTop * vect(1,1,0)) $ " lowestBody=" $ LowestBody(RideCorpse) $ " lowestAboveTop=" $ (LowZ(RideCorpse) - (TestMover.Location.Z + (MoverTop.Z - MoverRestZ))), 'RagdollTest');
			else
				Log("RagdollTestMutator: envtest mover sweep=" $ Sweeps $ " ride corpse gone", 'RagdollTest');
		}
		if ( Sweeps >= 11 )
			Log("RagdollTestMutator: envtest crush sweep=" $ Sweeps $ " crushCorpse=" $ CrushCorpse $ " (None = destroyed as expected) control=" $ ControlCorpse $ " (must survive)", 'RagdollTest');
	}
}

// Damage model (2026-10-11): bullets into the SAME forearm must accumulate
// on that forearm only (ledger 8.5 per enforcer round with BulletCutPower
// 0.5, off at 40 = 5th round), the torso must take any number of rounds
// without losing anything, and a rocket at the knees must take the shins
// but not the arms. Shots are aimed from 100 UU away straight at the body
// centre (the retail hit point would be on the cylinder; the native ray
// cast is what makes the aim count). The spine shot goes first so its
// nothing-happens result is not confused by missing limbs.
function DamageModelTest(RagdollCarcass R)
{
	local vector Target, From, Dir, Axis, Front;
	local int i, Lost, Before;
	local RagdollCarcass R2;

	R.bDecorative = false;   // Initfor clears it on real kills; TakeDamage ignores decorative corpses
	R.bLogDamage = true;
	PinGoreKnobs(R);
	Log("RagdollTestMutator: dmgtest start state=" $ R.GetStateName() $ " inDead=" $ R.IsInState('Dead') $ " mass=" $ R.Mass, 'RagdollTest');
	R.GotoState('Dead');
	Log("RagdollTestMutator: dmgtest after GotoState state=" $ R.GetStateName() $ " inDead=" $ R.IsInState('Dead'), 'RagdollTest');
	// 1) Six enforcer rounds into the spine from the front: nothing comes off.
	//    Every TakeDamage here lands in the same tick, so NewShot() ends the
	//    0.15 s per-shot sever window between them as the real fire rates
	//    would (enforcer 0.5 s; six rounds in one window would read as a
	//    51-cut burst and trip the SeverDamage guarantee).
	Target = R.GetBodyLocation(1);
	Dir = vect(0,1,0);
	for ( i = 0; i < 6; i++ )
	{
		NewShot(R);
		R.TakeDamage(17, None, Target - Dir * 30, Dir * 3000, 'shot');
	}
	Log("RagdollTestMutator: dmgtest spine x6 enforcer ledger=" $ R.RagdollLedger(1) $ " armL=" $ R.BodyAttached(4) $ " armR=" $ R.BodyAttached(5) $ " forearmL=" $ R.BodyAttached(6) $ " forearmR=" $ R.BodyAttached(7), 'RagdollTest');

	// 2) Enforcer rounds into the right forearm until it comes off (expect 5).
	for ( i = 0; i < 8 && R.BodyAttached(7); i++ )
	{
		Target = R.GetBodyLocation(7);
		From = Target + vect(0,0,100);   // from above, straight down at the centre
		Dir = Normal(Target - From);
		NewShot(R);
		R.TakeDamage(17, None, Target - Dir * 30, Dir * 3000, 'shot');
	}
	Log("RagdollTestMutator: dmgtest forearmR rounds=" $ i $ " attached=" $ R.BodyAttached(7) $ " ledger=" $ R.RagdollLedger(7) $ " ledger upperarmR=" $ R.RagdollLedger(5) $ " shinR=" $ R.RagdollLedger(11), 'RagdollTest');

	// 3) Rocket at the left knee: shin L (and maybe thigh L) off, arms stay.
	Target = (R.GetBodyLocation(8) + R.GetBodyLocation(10)) * 0.5;
	Lost = 0;
	NewShot(R);
	R.TakeDamage(75, None, Target, vect(0,0,1) * 80000, 'RocketDeath');
	for ( i = 4; i < 12; i++ )
		if ( !R.BodyAttached(i) )
			Lost++;
	Log("RagdollTestMutator: dmgtest rocket@kneeL lost=" $ Lost $ " shinL=" $ R.BodyAttached(10) $ " thighL=" $ R.BodyAttached(8) $ " shinR=" $ R.BodyAttached(11) $ " armL=" $ R.BodyAttached(4) $ " armR=" $ R.BodyAttached(5) $ " head=" $ R.BodyAttached(3)
		$ " ledgers shinL=" $ R.RagdollLedger(10) $ " thighL=" $ R.RagdollLedger(8) $ " shinR=" $ R.RagdollLedger(11) $ " armL=" $ R.RagdollLedger(4) $ " head=" $ R.RagdollLedger(3), 'RagdollTest');

	// 4) Rockets on the chest (2026-10-11 night, "severed limbs on a direct
	//    hit, tiny bits only after ~3 rockets"), one Wound() model:
	//    #1: the ledger flags nothing (spine 67 < 85, arms ~56 < 60) and the
	//        SeverDamage guarantee takes the nearest limb's forearm (75 < 90:
	//        distal part) - expect limbsLost=1;
	//    #2: the spine ledger passes TorsoSeverDamage (2 x 75 x 0.9 = 135 >
	//        85; the nearest body takes the engine damage in full) and the
	//        upper body tears off at the waist when the spine is the body
	//        nearest the origin (look for "cut site joint=1" / "hull trim
	//        body=1"); a flagged upper arm nearer the origin goes first;
	//    #3: no total gib (only 250+ in one hit scatters), at most 2 more
	//        parts, the corpse is NOT gibbed and still has bodies attached.
	//    On a FRESH corpse (the ledgers of steps 1-3 would pollute it; the
	//    first run had the "forearm" shots land in the spine) with the knobs
	//    pinned on the instance (the D:\old ini carried TorsoSeverDamage=50
	//    from a console experiment, which tore the waist at ledger 51).
	R2 = SpawnTestCorpse(FindFloorSpot(R.Location));
	if ( R2 == None )
		return;
	R2.bDecorative = false;
	R2.bLogDamage = true;
	R2.GotoState('Dead');
	PinGoreKnobs(R2);
	Before = 0;
	for ( i = 4; i < 12; i++ )
		if ( !R2.BodyAttached(i) )
			Before++;
	// A real rocket explodes on the collision cylinder, ~20 UU outside the
	// chest skin, not at the spine centre (an origin INSIDE the torso put
	// both forearms and the head over their ledgers at once). 28 UU off the
	// spine centre along the chest normal (perpendicular to the pelvis-neck
	// axis and to its horizontal side vector), whichever way the corpse lies.
	Axis = Normal(R2.GetBodyLocation(2) - R2.GetBodyLocation(0));
	Front = Normal(Axis cross vect(0,0,1));
	if ( VSize(Front) < 0.5 )
		Front = vect(1,0,0);
	Front = Normal(Front cross Axis);
	Target = R2.GetBodyLocation(1) + Front * 28;
	NewShot(R2);
	R2.TakeDamage(75, None, Target, -Front * 80000, 'RocketDeath');
	Lost = 0;
	for ( i = 4; i < 12; i++ )
		if ( !R2.BodyAttached(i) )
			Lost++;
	Log("RagdollTestMutator: dmgtest chest rocket #1 " $ R2 $ " spine attached=" $ R2.BodyAttached(1) $ " ledger=" $ R2.RagdollLedger(1) $ " limbsLost=" $ (Lost - Before) $ " (expect 1: SeverDamage takes the nearest forearm) head=" $ R2.BodyAttached(3), 'RagdollTest');
	NewShot(R2);
	R2.TakeDamage(75, None, Target, -Front * 80000, 'RocketDeath');
	Log("RagdollTestMutator: dmgtest chest rocket #2 spine attached=" $ R2.BodyAttached(1) $ " ledger=" $ R2.RagdollLedger(1) $ " (expect detached: 2 x 67 > 85 when the spine is nearest, else a flagged arm went first) gibbed=" $ R2.bGibbed, 'RagdollTest');
	NewShot(R2);
	R2.TakeDamage(75, None, Target, -Front * 80000, 'RocketDeath');
	Lost = 0;
	for ( i = 1; i < 12; i++ )
		if ( R2.BodyAttached(i) )
			Lost++;
	Log("RagdollTestMutator: dmgtest chest rocket #3 gibbed=" $ R2.bGibbed $ " attached=" $ Lost $ " (expect gibbed=False, attached > 0)", 'RagdollTest');
}

// End the corpse's per-shot sever window: the next TakeDamage counts as a
// new shot even though it lands in the same tick as the last one.
function NewShot(RagdollCarcass R)
{
	R.SeverWindowTime = 0;
	R.SeverWindowDamage = 0;
	R.SeverWindowParts = 0;
}

// The gore knobs are globalconfig and the natives read them off the
// instance, so a value persisted in the install's ini (console "set") would
// change what the test measures. Pin the documented defaults.
function PinGoreKnobs(RagdollCarcass R)
{
	R.GoreLevel = 2;
	R.DismemberScale = 1.0;
	R.BulletCutPower = 0.5;
	R.BlastCutPower = 0.9;
	R.TorsoSeverDamage = 85;
	R.SeverDamage = 50;
	R.MaxSeverPerHit = 2;
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

// Lowest body centre (a shin box centre sits ~5 UU above the surface it
// rests on; a negative lowestAboveTop in the mover line = clipping into it).
function float LowZ(RagdollCarcass R)
{
	local int i;
	local float Z;

	Z = 100000;
	for ( i = 0; i < 12; i++ )
		Z = FMin(Z, R.GetBodyLocation(i).Z);
	return Z;
}

function int LowestBody(RagdollCarcass R)
{
	local int i, Best;
	local float Z;

	Z = 100000;
	Best = -1;
	for ( i = 0; i < 12; i++ )
		if ( R.GetBodyLocation(i).Z < Z )
		{
			Z = R.GetBodyLocation(i).Z;
			Best = i;
		}
	return Best;
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

// Six candidate spots on a ring around the start (radius ~150 UU, away from
// the gore pile at +60,+100 and the stack spot), floor-checked like
// FindFloorSpot; one plain corpse on each that passes.
function SpawnSpreadSample(vector Loc)
{
	local vector Off, HitLoc, HitNorm;
	local int i, Num;
	local RagdollCarcass R;

	for ( i = 0; i < 8 && Num < 6; i++ )
	{
		Off = vect(150,0,0) * cos(i * 0.7854) + vect(0,150,0) * sin(i * 0.7854);
		if ( Off.X > 20 && Off.Y > 50 )
			continue;   // the gore pile corner
		if ( Trace(HitLoc, HitNorm, Loc + Off - vect(0,0,100), Loc + Off, false) == None || HitNorm.Z < 0.7 )
			continue;
		R = SpawnTestCorpse(Loc + Off);
		if ( R != None )
		{
			Num++;
			Log("RagdollTestMutator: spread corpse " $ R $ " at " $ (Loc + Off), 'RagdollTest');
		}
	}
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
	R.SimSubSteps = 8;
	R.LifeSpan = 60;
	R.Mesh = Mesh(DynamicLoadObject("Botpack.Commando", class'Mesh'));
	R.MultiSkins[0] = Texture(DynamicLoadObject("CommandoSkins.cmdo1", class'Texture'));
	R.MultiSkins[1] = Texture(DynamicLoadObject("CommandoSkins.cmdo2", class'Texture'));
	R.MultiSkins[2] = Texture(DynamicLoadObject("CommandoSkins.cmdo3", class'Texture'));
	R.MultiSkins[3] = Texture(DynamicLoadObject("CommandoSkins.cmdo4", class'Texture'));
	R.SetDeathPose(Rand(65536), 0);
	R.BuildRagdoll();
	Log("RagdollTestMutator: stack corpse " $ R $ " at " $ Loc, 'RagdollTest');
	return R;
}

// Retail Commando death sequences, cycled per sweep (Dead4/Dead6 do not exist
// on the male mesh; 'Dead' alone is the Engine Carcass default and no sequence).
function name DeadAnimForSweep(int N)
{
	switch ( N % 5 )
	{
		case 0: return 'Dead1';
		case 1: return 'Dead2';
		case 2: return 'Dead3';
		case 3: return 'Dead7';
		default: return 'Dead8';
	}
	return 'Dead1';
}
