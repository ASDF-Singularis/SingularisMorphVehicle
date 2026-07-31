#include "Actors/SingularisMorphVehicleSkeletalActor.h"

#include <Components/SkeletalMeshComponent.h>

#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleSkeletalActor)

ASingularisMorphVehicleSkeletalActor::ASingularisMorphVehicleSkeletalActor()
{
	// 1) 启用网络复制与移动复制
	bReplicates = true;
	SetReplicatingMovement(true);

	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bCanEverTick = true;

	// 2) 创建骨骼网格体组件作为根组件
	SkeletalMeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("SkeletalMeshComponent"));
	SetRootComponent(SkeletalMeshComponent);

	// 3) 创建载具模拟组件（骨骼Actor不使用集群物理）
	VehicleMovementComponent = CreateDefaultSubobject<USingularisMorphVehicleSimulationComponent>(
		TEXT("VehicleMovementComponent")
	);

	SetRemoteRoleForBackwardsCompat(ROLE_SimulatedProxy);
}

void ASingularisMorphVehicleSkeletalActor::BeginPlay()
{
	Super::BeginPlay();
}

void ASingularisMorphVehicleSkeletalActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}
