// RagdollMutator: entry point for the RagdollPkg. Registers the spawn notify
// that replaces every spawned Carcass with a RagdollCarcass, so every death
// produces a Box3D-driven ragdoll.
//
// Usage: ut -mutator=RagdollPkg.RagdollMutator
class RagdollMutator extends Mutator;

var RagdollSpawnNotify SpawnNotify;

function PostBeginPlay()
{
	Super.PostBeginPlay();
	if ( SpawnNotify == None )
		SpawnNotify = Spawn(class'RagdollSpawnNotify');
}
