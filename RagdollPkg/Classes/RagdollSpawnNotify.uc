// RagdollSpawnNotify: intercepts every Carcass spawn (including the default
// CarcassType chosen by a Pawn's SpawnCarcass) and replaces it with a
// RagdollCarcass. The spawning Pawn then calls Initfor() on the returned
// actor, so skin/mesh data are copied as usual.
class RagdollSpawnNotify extends SpawnNotify;

function PostBeginPlay()
{
	Super.PostBeginPlay();
	Log("RagdollSpawnNotify: registered, ActorClass=" $ ActorClass, 'RagdollTest');
}

function Actor SpawnNotification(Actor A)
{
	local RagdollCarcass R;

	Log("RagdollSpawnNotify: notification for " $ A, 'RagdollTest');
	if ( A.IsA('RagdollCarcass') )
		return A;
	if ( !A.IsA('Carcass') )
		return A;

	Log("RagdollSpawnNotify: intercepting Carcass spawn " $ A $ " at " $ A.Location, 'RagdollTest');
	A.Destroy();
	R = Spawn(class'RagdollCarcass',,, A.Location, A.Rotation);
	Log("RagdollSpawnNotify: spawned " $ R, 'RagdollTest');
	return R;
}
