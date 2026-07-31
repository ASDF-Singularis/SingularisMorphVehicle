#include "Actors/SingularisMorphVehiclePawn.h"

#include <Components/StaticMeshComponent.h>

#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehiclePawn)

ASingularisMorphVehiclePawn::ASingularisMorphVehiclePawn()
{
	// 1) 启用网络复制与移动复制
	bReplicates = true;
	SetReplicatingMovement(true);

	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bCanEverTick = true;

	// 2) 创建静态网格体组件作为根组件
	StaticMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StaticMeshComponent"));
	SetRootComponent(StaticMeshComponent);

	// 3) 创建载具模拟组件（不使用集群物理）
	VehicleMovementComponent = CreateDefaultSubobject<USingularisMorphVehicleSimulationComponent>(
		TEXT("VehicleMovementComponent")
	);
	VehicleMovementComponent->SetUpdatedComponent();

	SetRemoteRoleForBackwardsCompat(ROLE_SimulatedProxy);
}

void ASingularisMorphVehiclePawn::BeginPlay()
{
	Super::BeginPlay();
}

void ASingularisMorphVehiclePawn::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);
}
