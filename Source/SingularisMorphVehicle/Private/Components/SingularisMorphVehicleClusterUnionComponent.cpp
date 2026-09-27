#include "Components/SingularisMorphVehicleClusterUnionComponent.h"

#include <Components/SkeletalMeshComponent.h>
#include <Components/StaticMeshComponent.h>
#include <GameFramework/Actor.h>
#include <GeometryCollection/GeometryCollectionComponent.h>

#include "SingularisMorphVehicle.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleClusterUnionComponent)

USingularisMorphVehicleClusterUnionComponent::USingularisMorphVehicleClusterUnionComponent(
	const FObjectInitializer& ObjectInitializer
)
	: Super(ObjectInitializer)
{
	SetIsReplicatedByDefault(false);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

bool USingularisMorphVehicleClusterUnionComponent::HasAnySockets() const
{
	if (!Sockets.IsEmpty())
		return true;

	return Super::HasAnySockets();
}

bool USingularisMorphVehicleClusterUnionComponent::DoesSocketExist(const FName InSocketName) const
{
	for (const FSingularisMorphVehicleSocket& Socket : Sockets)
	{
		if (Socket.SocketName == InSocketName)
			return true;
	}

	return Super::DoesSocketExist(InSocketName);
}

FTransform USingularisMorphVehicleClusterUnionComponent::GetSocketTransform(
	const FName InSocketName,
	const ERelativeTransformSpace TransformSpace
) const
{
	for (const FSingularisMorphVehicleSocket& Socket : Sockets)
	{
		if (Socket.SocketName != InSocketName)
			continue;

		const FTransform SocketComponentSpaceTransform = Socket.GetLocalTransform();

		switch (TransformSpace)
		{
		case RTS_World:
			return SocketComponentSpaceTransform * GetComponentTransform();

		case RTS_Actor:
			{
				if (const AActor* Actor = GetOwner())
				{
					const FTransform SocketWorldSpaceTransform = SocketComponentSpaceTransform *
						GetComponentTransform();
					return SocketWorldSpaceTransform.GetRelativeTransform(Actor->GetTransform());
				}
				break;
			}

		case RTS_Component:
			return SocketComponentSpaceTransform;

		case RTS_ParentBoneSpace:
		default:
			check(false);
		}
	}

	return Super::GetSocketTransform(InSocketName, TransformSpace);
}

void USingularisMorphVehicleClusterUnionComponent::QuerySupportedSockets(
	TArray<FComponentSocketDescription>& OutSockets
) const
{
	for (const FSingularisMorphVehicleSocket& Socket : Sockets)
	{
		FComponentSocketDescription& Desc = OutSockets.AddZeroed_GetRef();
		Desc.Name = Socket.SocketName;
		Desc.Type = EComponentSocketType::Type::Socket;
	}
}

void USingularisMorphVehicleClusterUnionComponent::AddOwnedComponentsToCluster()
{
	// 1) 守卫：集群子件集合仅由权威端组装，其它端通过集群复制事件同步
	const AActor* Owner = GetOwner();
	if (!IsValid(Owner) || !Owner->HasAuthority()) return;

	// 2) 遍历全部后代组件（含多层嵌套），逐个按类型加入集群
	TArray<USceneComponent*> ChildComponents;
	GetChildrenComponents(true, ChildComponents);

	for (USceneComponent* Component : ChildComponents)
		AddComponentToClusterByType(Component);
}

void USingularisMorphVehicleClusterUnionComponent::AddComponentToClusterByType(USceneComponent* Component)
{
	if (!IsValid(Component)) return;

	// 本路径以「一个物理组件对应一个模拟模块」为装配单元；同一组件贡献多个物理体时，
	// 集群子件无法分别绑定到模块，骨骼网格体载具需要专用的骨骼网格体适配器
	// （骨骼与模块的对应关系由适配器自身携带）

	// 1) 静态网格体：无有效物理体时无法加入集群
	if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
	{
		if (!StaticMeshComponent->HasValidPhysicsState()) return;

		TArray<int32> BoneIds{0};
		AddComponentToCluster(StaticMeshComponent, BoneIds);
		return;
	}

	// 2) 骨骼网格体：每个物理体各占一个集群子件
	if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(Component))
	{
		TArray<int32> BoneIds;
		BoneIds.Reserve(SkeletalMeshComponent->Bodies.Num());
		for (auto I = 0; I < SkeletalMeshComponent->Bodies.Num(); ++I)
			BoneIds.Add(I);

		if (BoneIds.IsEmpty()) return;

		if (BoneIds.Num() > 1)
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT(
					"[ClusterUnionComponent] %s contributes %d cluster particles - the cluster union adapter binds one module per component and cannot map them separately; skeletal mesh vehicles require a dedicated skeletal mesh adapter"
				),
				*GetNameSafe(SkeletalMeshComponent),
				BoneIds.Num()
			);
		}

		AddComponentToCluster(SkeletalMeshComponent, BoneIds);
		return;
	}

	// 3) 几何体集：以首个体整体加入集群
	if (UGeometryCollectionComponent* GeometryCollectionComponent = Cast<UGeometryCollectionComponent>(Component))
	{
		TArray<int32> BoneIds{0};
		AddComponentToCluster(GeometryCollectionComponent, BoneIds);
	}
}
