#include "Objects/SingularisMorphVehicleClusterUnionAdapter.h"

#include <Engine/World.h>
#include <PhysicsEngine/ClusterUnionComponent.h>

#include "Components/SingularisMorphVehicleClusterUnionComponent.h"
#include "Components/SingularisMorphVehicleSUComponent.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Subsystems/SingularisMorphVehicleMappingSubsystem.h"

void USingularisMorphVehicleClusterUnionAdapter::Initialize(const FSingularisMorphVehiclePhysicsAdapterContext& Context)
{
	// 1) 通过 Context 获取 Owner Actor 并解析集群联合引用
	if (AActor* Owner = Context.SimulationComponent ? Context.SimulationComponent->GetOwner() : nullptr)
		ResolveClusterUnionComponent(Owner);

	if (!ClusterUnionComponent.IsValid()) return;

	// 2) 防止重复绑定（Initialize 幂等）
	if (bEventsBound) return;

	ClusterUnionComponent->OnComponentAddedNativeEvent.AddUObject(
		this,
		&USingularisMorphVehicleClusterUnionAdapter::OnClusterComponentAdded
	);
	ClusterUnionComponent->OnComponentRemovedNativeEvent.AddUObject(
		this,
		&USingularisMorphVehicleClusterUnionAdapter::OnClusterComponentRemoved
	);

	bEventsBound = true;
}

void USingularisMorphVehicleClusterUnionAdapter::Terminate()
{
	if (ClusterUnionComponent.IsValid())
	{
		ClusterUnionComponent->OnComponentAddedNativeEvent.RemoveAll(this);
		ClusterUnionComponent->OnComponentRemovedNativeEvent.RemoveAll(this);
	}
	ClusterUnionComponent.Reset();
	bDirty = false;
	bEventsBound = false;
}

bool USingularisMorphVehicleClusterUnionAdapter::IsReady() const
{
	if (!ClusterUnionComponent.IsValid()) return false;
	const Chaos::FClusterUnionPhysicsProxy* Proxy = ClusterUnionComponent->GetPhysicsProxyPublic();
	return Proxy && !Proxy->GetSyncedData_External().ChildParticles.IsEmpty();
}

bool USingularisMorphVehicleClusterUnionAdapter::IsDirty() const
{
	return bDirty;
}

FString USingularisMorphVehicleClusterUnionAdapter::GetAdapterName() const
{
	return TEXT("ClusterUnionAdapter");
}

IPhysicsProxyBase* USingularisMorphVehicleClusterUnionAdapter::GetPhysicsProxy() const
{
	return ClusterUnionComponent.IsValid()
		       ? ClusterUnionComponent->GetPhysicsProxyPublic()
		       : nullptr;
}

FTransform USingularisMorphVehicleClusterUnionAdapter::GetReferenceTransform() const
{
	return ClusterUnionComponent.IsValid()
		       ? ClusterUnionComponent->GetComponentTransform()
		       : FTransform::Identity;
}

FSingularisMorphVehiclePhysicsAdapterSnapshot USingularisMorphVehicleClusterUnionAdapter::ConsumeSnapshot()
{
	// 1) 集群联合组件有效性检查
	if (!ClusterUnionComponent.IsValid()) return {};

	const Chaos::FClusterUnionPhysicsProxy* Proxy = ClusterUnionComponent->GetPhysicsProxyPublic();
	if (!Proxy) return {};

	const auto& ChildParticles = Proxy->GetSyncedData_External().ChildParticles;
	if (ChildParticles.IsEmpty()) return {};

	// 2) 获取集群子组件列表
	TArray<USceneComponent*> ClusterChildren;
	ClusterUnionComponent->GetChildrenComponents(true, ClusterChildren);

	// 3) 获取 Subsystem 用于物理组件到 SU 组件的映射
	const UWorld* World = GetWorld();
	if (!World) return {};
	const USingularisMorphVehicleMappingSubsystem* Subsystem =
		World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>();
	if (!Subsystem) return {};

	// 4) 遍历集群子组件，构建完整快照
	FSingularisMorphVehiclePhysicsAdapterSnapshot Snapshot;
	const int32 Count = FMath::Min(ChildParticles.Num(), ClusterChildren.Num());
	Snapshot.Entities.Reserve(Count);

	for (auto I = 0; I < Count; I++)
	{
		UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(ClusterChildren[I]);
		if (!PrimComp) continue;

		USingularisMorphVehicleSUComponent* SUComp = Subsystem->FindSUComponent(PrimComp);
		if (!SUComp) continue;

		FSingularisMorphVehiclePhysicsAdapterSnapshotEntity Entity;
		Entity.SUComponent = SUComp;
		// 写入粒子的真实唯一索引（而非数组下标）：
		// 模块通过 ParticleIdx 在集群中查找自身粒子（GetClusterParticle），
		// 若使用数组下标会匹配不到粒子，甚至错配到其它子粒子，导致车轮/悬挂动画与受力失效。
		Entity.ParticleIndex = ChildParticles[I].ParticleIdx.Idx;
		Entity.ChildToParent = ChildParticles[I].ChildToParent;

		Snapshot.Entities.Emplace(MoveTemp(Entity));
	}

	// 5) 清除脏标记
	bDirty = false;

	return Snapshot;
}

void USingularisMorphVehicleClusterUnionAdapter::ResolveClusterUnionComponent(AActor* Owner)
{
	if (!IsValid(Owner)) return;
	if (UActorComponent* ResolvedComp = ClusterUnionComponentReference.GetComponent(Owner))
		ClusterUnionComponent = Cast<USingularisMorphVehicleClusterUnionComponent>(ResolvedComp);
}

void USingularisMorphVehicleClusterUnionAdapter::OnClusterComponentAdded(
	UPrimitiveComponent* Component,
	const TArray<FClusterUnionBoneData>& BonesData,
	const TArray<FClusterUnionBoneData>& RemovedBoneIDs,
	bool bIsNew
)
{
	if (!bIsNew || !IsValid(Component)) return;

	bDirty = true;

	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[ClusterUnionAdapter] OnClusterComponentAdded: Component=%s"),
		*GetNameSafe(Component)
	);
}

void USingularisMorphVehicleClusterUnionAdapter::OnClusterComponentRemoved(
	UPrimitiveComponent* Component,
	const TArray<FClusterUnionBoneData>& RemovedBonesData
)
{
	if (!IsValid(Component)) return;

	bDirty = true;

	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[ClusterUnionAdapter] OnClusterComponentRemoved: Component=%s"),
		*GetNameSafe(Component)
	);
}
