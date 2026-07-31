#include "Actors/SingularisMorphVehicleClusterActor.h"

#include "Components/SingularisMorphVehicleClusterUnionComponent.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleClusterActor)

ASingularisMorphVehicleClusterActor::ASingularisMorphVehicleClusterActor()
{
	// 1) 启用网络复制与移动复制
	bReplicates = true;
	SetReplicatingMovement(true);

	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bCanEverTick = true;

	// 2) 创建集群联合组件作为根组件
	VehicleClusterUnionComponent = CreateDefaultSubobject<USingularisMorphVehicleClusterUnionComponent>(
		TEXT("VehicleClusterUnionComponent")
	);
	SetRootComponent(VehicleClusterUnionComponent);

	// 3) 创建载具模拟组件
	VehicleMovementComponent = CreateDefaultSubobject<USingularisMorphVehicleSimulationComponent>(
		TEXT("VehicleMovementComponent")
	);

	SetRemoteRoleForBackwardsCompat(ROLE_SimulatedProxy);
}

void ASingularisMorphVehicleClusterActor::BeginPlay()
{
	Super::BeginPlay();
}

void ASingularisMorphVehicleClusterActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}
