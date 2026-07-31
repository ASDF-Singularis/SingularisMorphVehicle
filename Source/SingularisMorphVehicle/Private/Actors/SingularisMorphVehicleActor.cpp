#include "Actors/SingularisMorphVehicleActor.h"

#include <Components/StaticMeshComponent.h>

#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleActor)

ASingularisMorphVehicleActor::ASingularisMorphVehicleActor()
{
	bReplicates = true;
	SetReplicatingMovement(true);

	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bCanEverTick = true;

	StaticMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StaticMeshComponent"));
	SetRootComponent(StaticMeshComponent);

	VehicleMovementComponent = CreateDefaultSubobject<USingularisMorphVehicleSimulationComponent>(
		TEXT("VehicleMovementComponent")
	);

	SetRemoteRoleForBackwardsCompat(ROLE_SimulatedProxy);
}

void ASingularisMorphVehicleActor::BeginPlay()
{
	Super::BeginPlay();
}

void ASingularisMorphVehicleActor::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);
}
