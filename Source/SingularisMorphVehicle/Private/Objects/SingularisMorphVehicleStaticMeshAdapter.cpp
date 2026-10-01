#include "Objects/SingularisMorphVehicleStaticMeshAdapter.h"

#include <Components/StaticMeshComponent.h>
#include <GameFramework/Actor.h>
#include <PhysicsEngine/BodyInstance.h>
#include <PhysicsProxy/SingleParticlePhysicsProxy.h>

#include "SingularisMorphVehicle.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"

void USingularisMorphVehicleStaticMeshAdapter::Initialize(
	const FSingularisMorphVehiclePhysicsAdapterContext& Context
)
{
	// 1) 通过 Context 获取 Owner Actor 并解析本体组件
	AActor* Owner = Context.SimulationComponent ? Context.SimulationComponent->GetOwner() : nullptr;
	if (!IsValid(Owner))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Error,
			TEXT("[StaticMeshAdapter] Initialize called without a valid owner - the adapter stays idle")
		);
		return;
	}

	ResolveBodyComponent(Owner);
	if (!BodyComponent.IsValid())
	{
		// 解析失败会使载具永不模拟（IsReady 恒 false，每帧静默早退），
		// 属于最难定位的一类故障，必须显式告警
		UE_LOG(
			LogSingularisMorphVehicle,
			Error,
			TEXT(
				"[StaticMeshAdapter] Failed to resolve a body primitive on %s from reference '%s' - check BodyComponentReference"
			),
			*GetNameSafe(Owner),
			*BodyComponentReference.PathToComponent
		);
		return;
	}

	// 2) 置初始脏标记，保证首帧必然拉取一次快照完成首次装配。
	//    物理状态重建会经由 SimulationComponent 的 OnCreatePhysicsState 重新调用 Initialize，
	//    故此处无需处理该情形
	bDirty = true;
	CachedAssembly.Reset();
}

void USingularisMorphVehicleStaticMeshAdapter::Terminate()
{
	BodyComponent.Reset();
	CachedAssembly.Reset();
	bDirty = false;
}

bool USingularisMorphVehicleStaticMeshAdapter::IsReady() const
{
	// 就绪意味着物理后端可用：本体组件有效且其物理体已建立
	return BodyComponent.IsValid()
		&& BodyComponent->HasValidPhysicsState()
		&& GetPhysicsProxy() != nullptr;
}

bool USingularisMorphVehicleStaticMeshAdapter::IsDirty() const
{
	if (bDirty) return true;

	// 场景组件的挂载/卸载没有可订阅的引擎事件，故在此按帧比对装配签名：
	// 模块网格体增删（运行时变型）改变签名即触发一次全量重建。
	// 签名仅含组件集合，不含变换——变换每帧由模拟/动画改写，纳入会退化为逐帧重建。
	TArray<TWeakObjectPtr<UPrimitiveComponent>> CurrentAssembly;
	GatherAssembly(CurrentAssembly);

	if (CurrentAssembly.Num() != CachedAssembly.Num())
	{
		bDirty = true;
		return true;
	}

	for (int32 Index = 0; Index < CurrentAssembly.Num(); ++Index)
	{
		if (CurrentAssembly[Index] != CachedAssembly[Index])
		{
			bDirty = true;
			return true;
		}
	}

	return false;
}

FString USingularisMorphVehicleStaticMeshAdapter::GetAdapterName() const
{
	return TEXT("StaticMeshAdapter");
}

IPhysicsProxyBase* USingularisMorphVehicleStaticMeshAdapter::GetPhysicsProxy() const
{
	if (!BodyComponent.IsValid()) return nullptr;

	const FBodyInstance* BodyInstance = BodyComponent->GetBodyInstance();
	return BodyInstance ? BodyInstance->GetPhysicsActor() : nullptr;
}

FTransform USingularisMorphVehicleStaticMeshAdapter::GetReferenceTransform() const
{
	return BodyComponent.IsValid()
		       ? BodyComponent->GetComponentTransform()
		       : FTransform::Identity;
}

FSingularisMorphVehiclePhysicsAdapterSnapshot USingularisMorphVehicleStaticMeshAdapter::ConsumeSnapshot()
{
	// 1) 前置条件未满足时不消费脏标记：物理体尚未建立时清除标记会让首次装配永久丢失，
	//    保留标记由下一个物理帧重试
	if (!BodyComponent.IsValid()) return {};
	if (GetPhysicsProxy() == nullptr) return {};

	// 2) 前置条件齐备：消费脏标记，按当前装配签名全量产出实体
	const FTransform ReferenceTransform = BodyComponent->GetComponentTransform();
	bDirty = false;

	TArray<TWeakObjectPtr<UPrimitiveComponent>> CurrentAssembly;
	GatherAssembly(CurrentAssembly);
	CachedAssembly = CurrentAssembly;

	// 3) 单刚体后端没有可区分的子粒子，模块的粒子索引仅为标识与静止基准键，
	//    取无效值——单粒子施加路径不读取该索引，与参考实现的非集群路径一致。
	//    模块网格体携带相对本体的位姿，作为引擎写回动画位移后仍可复用的静止基准。
	FSingularisMorphVehiclePhysicsAdapterSnapshot Snapshot;
	Snapshot.Entities.Reserve(CurrentAssembly.Num());

	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakComponent : CurrentAssembly)
	{
		UPrimitiveComponent* Component = WeakComponent.Get();
		if (!Component) continue;

		FSingularisMorphVehiclePhysicsAdapterSnapshotEntity& Entity =
			Snapshot.Entities.AddDefaulted_GetRef();
		Entity.PrimitiveComponent = Component;
		Entity.ParticleIndex = INDEX_NONE;
		Entity.ChildToParent = Component->GetComponentTransform().GetRelativeTransform(ReferenceTransform);
	}

	return Snapshot;
}

void USingularisMorphVehicleStaticMeshAdapter::ResolveBodyComponent(AActor* Owner)
{
	if (!IsValid(Owner)) return;

	UActorComponent* ResolvedComp = BodyComponentReference.GetComponent(Owner);
	if (!ResolvedComp)
	{
		// 显式引用填错时不静默回退：回退到根组件会掩盖配置错误，
		// 使载具以错误的物理体运行。仅当引用完全留空时才回退
		if (!BodyComponentReference.PathToComponent.IsEmpty()) return;

		ResolvedComp = Owner->GetRootComponent();
	}

	BodyComponent = Cast<UPrimitiveComponent>(ResolvedComp);
	if (!BodyComponent.IsValid() && ResolvedComp)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Error,
			TEXT("[StaticMeshAdapter] Reference on %s resolved to %s which is not a primitive component"),
			*GetNameSafe(Owner),
			*GetNameSafe(ResolvedComp)
		);
	}
}

void USingularisMorphVehicleStaticMeshAdapter::GatherAssembly(
	TArray<TWeakObjectPtr<UPrimitiveComponent>>& OutAssembly
) const
{
	OutAssembly.Reset();

	UPrimitiveComponent* Body = BodyComponent.Get();
	if (!Body) return;

	// 本体在首位：作为树根模块的驱动组件与物理粒子归属
	OutAssembly.Add(Body);

	// 后代静态网格体即模块网格体（轮胎、悬挂等视觉部件）。
	// 遍历全部层级而非仅直接子件，允许模块网格体挂在中间空节点之下
	TArray<USceneComponent*> Children;
	Body->GetChildrenComponents(true, Children);

	OutAssembly.Reserve(OutAssembly.Num() + Children.Num());
	for (USceneComponent* Child : Children)
	{
		if (UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Child))
			OutAssembly.Add(StaticMesh);
	}
}
