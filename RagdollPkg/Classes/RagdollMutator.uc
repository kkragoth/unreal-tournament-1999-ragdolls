// RagdollMutator: entry point for the RagdollPkg. Registers the spawn notify
// that replaces every player corpse (UTHumanCarcass) with a RagdollCarcass,
// so every death produces a Box3D-driven ragdoll.
//
// It also registers as a damage mutator and remembers the last hit taken by
// every pawn (location, momentum, damage type, amount, and for blasts the
// explosion origin). A carcass has no hit information of its own when
// Initfor runs, so RagdollCarcass looks the killing blow up here to kick and
// tear the body parts the shot actually reached.
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
	var bool   bBlast;        // radial damage (HurtRadius): BlastOrigin is valid
	var vector BlastOrigin;   // the exploding projectile's location
	var vector VictimLocation; // where the victim stood when it was hit
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
	local vector Origin;
	local bool bBlast;
	local int i, Prev;
	local float PrevTime;

	if ( Victim != None )
	{
		// One shot, several projectiles (flak: 8-9 chunks of 16 inside a
		// tick or two): fold them into ONE record, damage and momentum
		// summed, so a flak kill reads as the 144-damage shot it was and
		// not as its last 16-damage chunk (gore and the death kick both
		// read only the latest record). The victim's newest record is
		// searched for (not just the ring's last entry: with eight bots
		// fighting, another pawn's hit lands between two chunks).
		Prev = -1;
		for ( i=0; i<ArrayCount(Hits); i++ )
			if ( Hits[i].Victim == Victim && (Prev < 0 || Hits[i].Time > PrevTime) )
			{
				Prev = i;
				PrevTime = Hits[i].Time;
			}
		if ( Prev >= 0 && Hits[Prev].DamageType == DamageType && !Hits[Prev].bBlast
			&& !class'RagdollCarcass'.static.IsBlastType(DamageType) && DamageType != 'jolted'
			&& Level.TimeSeconds - Hits[Prev].Time < 0.1 )
		{
			Hits[Prev].HitLocation = HitLocation;
			if ( Victim.Mass > 1 )
				Hits[Prev].Momentum += Momentum * Victim.Mass;
			else
				Hits[Prev].Momentum += Momentum;
			Hits[Prev].Damage += ActualDamage;   // Time stays the first chunk's: a minigun stream must not fold forever
			Hits[Prev].VictimLocation = Victim.Location;
			Super.MutatorTakeDamage(ActualDamage, Victim, InstigatedBy, HitLocation, Momentum, DamageType);
			return;
		}

		// HurtRadius hands the victim a point on its near side and the damage
		// scaled to its location; the per-body model wants the explosion
		// itself, which is the projectile still sitting at its impact point.
		// 'jolted' is a blast only when a shock ball is around (the beam has
		// no projectile).
		bBlast = class'RagdollCarcass'.static.IsBlastType(DamageType);
		if ( bBlast || DamageType == 'jolted' )
		{
			if ( class'RagdollCarcass'.static.FindBlastOrigin(self, Victim.Location,
					class'RagdollCarcass'.static.BlastRadiusFor(DamageType, ActualDamage), Origin) )
				bBlast = true;
			else if ( DamageType == 'jolted' )
				bBlast = false;
			else
				Origin = HitLocation;
		}
		Hits[NextHit].Victim      = Victim;
		Hits[NextHit].HitLocation = HitLocation;
		// Engine Pawn.TakeDamage has ALREADY divided the weapon's momentum by
		// the pawn's Mass (and raised its Z to 0.4 x size on a walking pawn)
		// when it calls the damage mutator (Engine.u text, 2026-10-11), so
		// the record holds a velocity (enforcer 30 UU/s), not the 3000 the
		// carcass natives expect. Undo the division: ApplyDeathVelocities /
		// RagdollHit divide by the pawn mass again and were giving the
		// killing blow 1/100 of its kick (deathhit dv=0.00 in every playtest).
		if ( Victim.Mass > 1 )
			Hits[NextHit].Momentum = Momentum * Victim.Mass;
		else
			Hits[NextHit].Momentum = Momentum;
		Hits[NextHit].DamageType  = DamageType;
		Hits[NextHit].Damage      = ActualDamage;
		Hits[NextHit].Time        = Level.TimeSeconds;
		Hits[NextHit].bBlast      = bBlast;
		Hits[NextHit].BlastOrigin = Origin;
		Hits[NextHit].VictimLocation = Victim.Location;
		NextHit = (NextHit + 1) % ArrayCount(Hits);
	}
	Super.MutatorTakeDamage(ActualDamage, Victim, InstigatedBy, HitLocation, Momentum, DamageType);
}

// Most recent hit on Victim within MaxAge seconds; false when none.
// HitLocation and BlastOrigin come back RELATIVE TO WHERE THE VICTIM IS NOW:
// the corpse is spawned 0.2-0.3 s after a normal death (Dying: Sleep(0.2);
// SpawnCarcass), and a rocket kill throws the pawn 800 UU/s, so it is
// 150-250 UU from the explosion by then. Unshifted, the carcass's
// RagdollBlast found every limb outside the tearing radius (110 UU) and the
// direct-hit guarantee's 50 UU, and a rocket kill tore nothing (2026-10-11
// playtest). Jumps over 600 UU (respawn, teleporter) are not shifted.
function bool GetLastHit(Pawn Victim, float MaxAge, out vector HitLocation, out vector Momentum, out name DamageType, out int Damage, out vector BlastOrigin, out int BlastKind)
{
	local int i, Best;
	local float BestTime;
	local vector Shift;

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
	Shift = Victim.Location - Hits[Best].VictimLocation;
	if ( VSize(Shift) > 600 )
		Shift = vect(0,0,0);
	HitLocation = Hits[Best].HitLocation + Shift;
	Momentum    = Hits[Best].Momentum;
	DamageType  = Hits[Best].DamageType;
	Damage      = Hits[Best].Damage;
	BlastOrigin = Hits[Best].BlastOrigin + Shift;
	BlastKind   = int(Hits[Best].bBlast);   // out params cannot be bools
	return true;
}
