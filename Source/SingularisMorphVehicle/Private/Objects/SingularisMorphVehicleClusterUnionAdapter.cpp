#include "Objects/SingularisMorphVehicleClusterUnionAdapter.h"

#include <Engine/World.h>
#include <Physics/Experimental/PhysScene_Chaos.h>
#include <PhysicsEngine/ClusterUnionComponent.h>

#include "SingularisMorphVehicle.h"
#include "Components/SingularisMorphVehicleClusterUnionComponent.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"

void USingularisMorphVehicleClusterUnionAdapter::Initialize(const FSingularisMorphVehiclePhysicsAdapterContext& Context)
{
	// 1) 通过 Context 获取 Owner Actor 并解析集群联合引用
	AActor* Owner = Context.SimulationComponent ? Context.SimulationComponent->GetOwner() : nullptr;
	if (!IsValid(Owner))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Error,
			TEXT("[ClusterUnionAdapter] Initialize called without a valid owner - the adapter stays idle")
		);
		return;
	}

	ResolveClusterUnionComponent(Owner);
	if (!ClusterUnionComponent.IsValid())
	{
		// 解析失败会使载具永不模拟（IsReady 恒 false，每帧静默早退），
		// 属于最难定位的一类故障，必须显式告警
		UE_LOG(
			LogSingularisMorphVehicle,
			Error,
			TEXT(
				"[ClusterUnionAdapter] Failed to resolve a %s on %s from reference '%s' - check ClusterUnionComponentReference"
			),
			*USingularisMorphVehicleClusterUnionComponent::StaticClass()->GetName(),
			*GetNameSafe(Owner),
			*ClusterUnionComponentReference.PathToComponent
		);
		return;
	}

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

	// 事件只在集群组成变化时触发；若子件在 Initialize 之前就已加入集群，事件已经错过。
	// 此处置一次初始脏标记，保证首帧必然拉取一次快照完成首次装配
	bDirty = true;
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
	// 就绪意味着物理后端可用，而非当前存在子件：
	// 子件集合为空时仍须由 ConsumeSnapshot 产出空快照，供消费端清除模块（载具解体），
	// 否则全部子件脱离集群后旧模块会永久残留
	return ClusterUnionComponent.IsValid() && ClusterUnionComponent->GetPhysicsProxyPublic() != nullptr;
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
	// 1) 前置条件未满足时不消费脏标记：集群事件不会重发同一次变更，
	//    在此清除标记会让该次变更永久丢失；保留标记由下一个物理帧重试
	if (!ClusterUnionComponent.IsValid()) return {};

	const Chaos::FClusterUnionPhysicsProxy* Proxy = ClusterUnionComponent->GetPhysicsProxyPublic();
	if (!Proxy) return {};

	const UWorld* World = GetWorld();
	if (!World) return {};
	const FPhysScene* PhysScene = World->GetPhysicsScene();
	if (!PhysScene) return {};

	// 2) 前置条件齐备：本次变更已可处理，消费脏标记。
	//    子件集合为空时返回空快照，由消费端清除全部模块（载具解体）
	bDirty = false;

	const auto& ChildParticles = Proxy->GetSyncedData_External().ChildParticles;
	if (ChildParticles.IsEmpty()) return {};

	// 4) 遍历集群子粒子，通过粒子代理反查所属物理组件后构建完整快照。
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

	return Snapshot;
}

void USingularisMorphVehicleClusterUnionAdapter::ResolveClusterUnionComponent(AActor* Owner)
{
	if (!IsValid(Owner)) return;

	UActorComponent* ResolvedComp = ClusterUnionComponentReference.GetComponent(Owner);
	if (!ResolvedComp)
	{
		// 引用为空或路径无效（如仅填写裸组件名而非完整路径）
		return;
	}

	ClusterUnionComponent = Cast<USingularisMorphVehicleClusterUnionComponent>(ResolvedComp);
	if (!ClusterUnionComponent.IsValid())
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Error,
			TEXT("[ClusterUnionAdapter] Reference on %s resolved to %s which is not a cluster union component"),
			*GetNameSafe(Owner),
			*GetNameSafe(ResolvedComp)
		);
	}
}

void USingularisMorphVehicleClusterUnionAdapter::OnClusterComponentAdded(
	UPrimitiveComponent* Component,
	const TArray<FClusterUnionBoneData>& BonesData,
	const TArray<FClusterUnionBoneData>& RemovedBoneIDs,
	bool bIsNew
)
{
	// 仅处理新加入集群的组件：引擎在物理重同步时会重复上报同一批子件，
	// 它们并非拓扑变更，若一概置脏会在每帧触发全量重建
	if (!bIsNew || !IsValid(Component)) return;

	bDirty = true;

	UE_LOG(
		LogSingularisMorphVehicle,
		Display,
		TEXT("[ClusterUnionAdapter] OnClusterComponentAdded: Component=%s, IsNew=%d, Bones=%d, RemovedBones=%d"),
		*GetNameSafe(Component),
		bIsNew ? 1 : 0,
		BonesData.Num(),
		RemovedBoneIDs.Num()
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
		LogSingularisMorphVehicle,
		Display,
		TEXT("[ClusterUnionAdapter] OnClusterComponentRemoved: Component=%s"),
		*GetNameSafe(Component)
	);
}
