// RagdollMutator: entry point for the RagdollPkg. Registers the spawn notify
// that replaces every player corpse (UTHumanCarcass) with a RagdollCarcass,
// so every death produces a Box3D-driven ragdoll.
//
// It also registers as a damage mutator and remembers the last hit taken by
// every pawn (location, momentum, damage type, amount). A carcass has no hit
// information of its own when Initfor runs, so RagdollCarcass looks the
// killing blow up here to tear off the body part nearest the shot.
//
// Usage: ut -mutator=RagdollPkg.RagdollMutator
class RagdollMutator extends Mutator;

struct HitRecord
{
	var Pawn   Victim;
	var vector HitLocation;
	var vector Momentum;
	var name   DamageType;
	var int    Damage;
	var float  Time;
};

var RagdollSpawnNotify SpawnNotify;
var HitRecord Hits[16];
var int       NextHit;

function PostBeginPlay()
{
	Super.PostBeginPlay();
	if ( SpawnNotify == None )
		SpawnNotify = Spawn(class'RagdollSpawnNotify');
	Level.Game.RegisterDamageMutator(self);
}

function MutatorTakeDamage(out int ActualDamage, Pawn Victim, Pawn InstigatedBy, out Vector HitLocation, out Vector Momentum, name DamageType)
{
	if ( Victim != None )
	{
		Hits[NextHit].Victim      = Victim;
		Hits[NextHit].HitLocation = HitLocation;
		Hits[NextHit].Momentum    = Momentum;
		Hits[NextHit].DamageType  = DamageType;
		Hits[NextHit].Damage      = ActualDamage;
		Hits[NextHit].Time        = Level.TimeSeconds;
		NextHit = (NextHit + 1) % ArrayCount(Hits);
	}
	Super.MutatorTakeDamage(ActualDamage, Victim, InstigatedBy, HitLocation, Momentum, DamageType);
}

// Most recent hit on Victim within MaxAge seconds; false when none.
function bool GetLastHit(Pawn Victim, float MaxAge, out vector HitLocation, out vector Momentum, out name DamageType, out int Damage)
{
	local int i, Best;
	local float BestTime;

	Best = -1;
	for ( i=0; i<ArrayCount(Hits); i++ )
	{
		if ( Hits[i].Victim != Victim || Hits[i].Victim == None )
			continue;
		if ( Level.TimeSeconds - Hits[i].Time > MaxAge )
			continue;
		if ( Best < 0 || Hits[i].Time > BestTime )
		{
			Best = i;
			BestTime = Hits[i].Time;
		}
	}
	if ( Best < 0 )
		return false;
	HitLocation = Hits[Best].HitLocation;
	Momentum    = Hits[Best].Momentum;
	DamageType  = Hits[Best].DamageType;
	Damage      = Hits[Best].Damage;
	return true;
}
