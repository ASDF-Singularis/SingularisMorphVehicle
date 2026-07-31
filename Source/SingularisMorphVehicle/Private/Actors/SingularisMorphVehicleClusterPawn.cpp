#include "Actors/SingularisMorphVehicleClusterPawn.h"

#include <Components/SkeletalMeshComponent.h>
#include <Components/StaticMeshComponent.h>

#include "Components/SingularisMorphVehicleClusterUnionComponent.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Objects/SingularisMorphVehicleClusterUnionAdapter.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleClusterPawn)

ASingularisMorphVehicleClusterPawn::ASingularisMorphVehicleClusterPawn()
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
	VehicleSimulationComponent = CreateDefaultSubobject<USingularisMorphVehicleSimulationComponent>(
		TEXT("VehicleSimulationComponent")
	);

	// 4) 创建集群联合适配器并配置引用
	USingularisMorphVehicleClusterUnionAdapter* Adapter = CreateDefaultSubobject<
		USingularisMorphVehicleClusterUnionAdapter>(
		TEXT("ClusterUnionAdapter")
	);
	Adapter->ClusterUnionComponentReference.OtherActor = this;
	Adapter->ClusterUnionComponentReference.PathToComponent = VehicleClusterUnionComponent->GetName();

	// 5) 替换模拟组件的默认物理适配器为集群联合适配器
	VehicleSimulationComponent->PhysicsAdapter = Adapter;

	SetRemoteRoleForBackwardsCompat(ROLE_SimulatedProxy);
}

void ASingularisMorphVehicleClusterPawn::BeginPlay()
{
	Super::BeginPlay();

	// 1) 仅服务器执行集群组件收集
	if (!HasAuthority()) return;

	// 2) 遍历 SceneComponent 层级，将几何体/静态网格/骨骼网格组件添加到集群联合
	TArray<USceneComponent*> ChildComponents;
	GetRootComponent()->GetChildrenComponents(true, ChildComponents);

	for (USceneComponent* Component : ChildComponents)
	{
		if (auto* Primitive = Cast<UPrimitiveComponent>(Component))
		{
			TArray<int32> BoneIds;

			if (const auto* SKMComp = Cast<USkeletalMeshComponent>(Primitive))
			{
				for (int32 I = 0; I < SKMComp->Bodies.Num(); I++)
					BoneIds.Add(I);
			}
			else if (Cast<UStaticMeshComponent>(Primitive) && Primitive->HasValidPhysicsState())
				BoneIds.Add(0);
			else
			{
				// 几何体集组件及其他类型：添加单个骨骼
				BoneIds.Add(0);
			}

			if (BoneIds.Num() > 0)
				VehicleClusterUnionComponent->AddComponentToCluster(Primitive, BoneIds);
		}
	}
}

void ASingularisMorphVehicleClusterPawn::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);
}
