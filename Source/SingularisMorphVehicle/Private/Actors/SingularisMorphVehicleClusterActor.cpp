#include "Actors/SingularisMorphVehicleClusterActor.h"

#include "Components/SingularisMorphVehicleClusterUnionComponent.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Objects/SingularisMorphVehicleClusterUnionAdapter.h"

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

	// 4) 创建集群联合适配器并配置引用
	USingularisMorphVehicleClusterUnionAdapter* Adapter = CreateDefaultSubobject<
		USingularisMorphVehicleClusterUnionAdapter>(
		TEXT("ClusterUnionAdapter")
	);
	Adapter->ClusterUnionComponentReference.OtherActor = this;
	Adapter->ClusterUnionComponentReference.PathToComponent = VehicleClusterUnionComponent->GetName();
	Adapter->ClusterUnionComponentReference.ComponentProperty = VehicleClusterUnionComponent->GetFName();

	// 5) 替换模拟组件的默认物理适配器为集群联合适配器
	VehicleMovementComponent->PhysicsAdapter = Adapter;

	SetRemoteRoleForBackwardsCompat(ROLE_SimulatedProxy);
}

void ASingularisMorphVehicleClusterActor::BeginPlay()
{
	Super::BeginPlay();

	// 集群子件组装由集群联合组件统一完成（仅权威端生效）
	if (VehicleClusterUnionComponent)
		VehicleClusterUnionComponent->AddOwnedComponentsToCluster();
}

void ASingularisMorphVehicleClusterActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}
