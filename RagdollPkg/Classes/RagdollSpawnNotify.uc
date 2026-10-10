// RagdollSpawnNotify: intercepts the spawn of a player corpse (the stock
// UTHumanCarcass subclasses chosen by a Pawn's CarcassType: TMale1Carcass,
// TFemale1Carcass, TBossCarcass, ...) and replaces it with a RagdollCarcass.
// The spawning Pawn then calls Initfor() on the returned actor, so skin/mesh
// data are copied as usual.
//
// Only UTHumanCarcass is intercepted. Every other Carcass subclass keeps its
// stock behaviour: the gib chunks (UT_MaleTorso, UT_MaleArm, UTLiver, ...) and
// above all the UTHeads chunk that PlayDecap spawns BEFORE the corpse exists
// (intercepting it produced a second full ragdoll after every headshot, and
// the stock head carries the dead player's camera retarget which we keep).
//
// Carcasses owned by a RagdollCarcass are also passed through: the ragdoll
// spawns the stock carcass class itself to drive the stock gib effects.
class RagdollSpawnNotify extends SpawnNotify;

function PostBeginPlay()
{
	Super.PostBeginPlay();
	Log("RagdollSpawnNotify: registered, ActorClass=" $ ActorClass, 'RagdollTest');
}

function Actor SpawnNotification(Actor A)
{
	local RagdollCarcass R;

	if ( A.IsA('RagdollCarcass') )
		return A;
	if ( !A.IsA('UTHumanCarcass') )
		return A;
	if ( A.Owner != None && A.Owner.IsA('RagdollCarcass') )
		return A;

	Log("RagdollSpawnNotify: intercepting " $ A $ " at " $ A.Location, 'RagdollTest');
	A.Destroy();
	R = Spawn(class'RagdollCarcass',,, A.Location, A.Rotation);
	Log("RagdollSpawnNotify: spawned " $ R, 'RagdollTest');
	return R;
}
