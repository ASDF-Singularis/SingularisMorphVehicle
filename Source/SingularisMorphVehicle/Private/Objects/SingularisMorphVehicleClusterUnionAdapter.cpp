#include "Objects/SingularisMorphVehicleClusterUnionAdapter.h"

#include <Engine/World.h>
#include <Physics/Experimental/PhysScene_Chaos.h>
#include <PhysicsEngine/ClusterUnionComponent.h>

#include "Components/SingularisMorphVehicleClusterUnionComponent.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"

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

	// 2) 获取物理场景，用于粒子代理反查所属组件
	const UWorld* World = GetWorld();
	if (!World) return {};
	const FPhysScene* PhysScene = World->GetPhysicsScene();
	if (!PhysScene) return {};

	// 3) 遍历集群子粒子，通过粒子代理反查所属物理组件后构建完整快照。
	//    实体只描述物理信息（组件 + 粒子数据），SU 组件的查询由消费端
	//    （SimulationComponent）通过 MappingSubsystem 统一完成。
	//    不能按数组下标与场景子组件（GetChildrenComponents）配对：
	//    集群断裂/部件移除后，ChildParticles 的顺序与场景子组件不再一致，
	//    按下标配对会让组件拿到其它粒子的索引与变换（车轮错位、抖动、飞散）。
	FSingularisMorphVehiclePhysicsAdapterSnapshot Snapshot;
	Snapshot.Entities.Reserve(ChildParticles.Num());

	for (const auto& ChildData : ChildParticles)
	{
		UPrimitiveComponent* PrimComp = nullptr;
		if (ChildData.Proxy)
			PrimComp = PhysScene->GetOwningComponent<UPrimitiveComponent>(ChildData.Proxy);
		if (!PrimComp) continue;

		FSingularisMorphVehiclePhysicsAdapterSnapshotEntity Entity;
		Entity.PrimitiveComponent = PrimComp;
		// 写入粒子的真实唯一索引（而非数组下标）：
		// 模块通过 ParticleIdx 在集群中查找自身粒子（GetClusterParticle），
		// 若使用数组下标会匹配不到粒子，甚至错配到其它子粒子，导致车轮/悬挂动画与受力失效。
		Entity.ParticleIndex = ChildData.ParticleIdx.Idx;
		Entity.ChildToParent = ChildData.ChildToParent;

		Snapshot.Entities.Emplace(MoveTemp(Entity));
	}

	// 4) 清除脏标记
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
