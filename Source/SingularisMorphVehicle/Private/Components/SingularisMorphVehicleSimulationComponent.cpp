#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include <atomic>
#include <Components/PrimitiveComponent.h>
#include <Engine/Canvas.h>
#include <Engine/Engine.h>
#include <Engine/Font.h>
#include <Engine/World.h>
#include <GameFramework/WorldSettings.h>
#include <Physics/Experimental/PhysScene_Chaos.h>
#include <PhysicsEngine/BodyInstance.h>
#include <PhysicsEngine/PhysicsObjectExternalInterface.h>
#include <PhysicsProxy/ClusterUnionPhysicsProxy.h>
#include <PhysicsProxy/SingleParticlePhysicsProxy.h>
#include <SimModule/SimModulesInclude.h>
#include <SimModule/SimulationModuleBase.h>

#include "SingularisMorphVehicle.h"
#include "Components/SingularisAxleSUComponent.h"
#include "Components/SingularisClutchSUComponent.h"
#include "Components/SingularisEngineSUComponent.h"
#include "Components/SingularisMorphVehicleSUComponent.h"
#include "Components/SingularisMotorSUComponent.h"
#include "Components/SingularisTransmissionSUComponent.h"
#include "Components/SingularisWheelSUComponent.h"
#include "Core/SingularisMorphVehicleSimulationCU.h"
#include "Interfaces/SingularisMorphVehicleSUInterface.h"
#include "Objects/SingularisMorphVehiclePhysicsAdapter.h"
#include "Subsystems/SingularisMorphVehicleMappingSubsystem.h"
#include "Subsystems/SingularisMorphVehicleSchedulerSubsystem.h"
#include "Types/SingularisMorphVehicleInputProducer.h"

USingularisMorphVehicleSimulationComponent::USingularisMorphVehicleSimulationComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;

	SuspensionTraceCollisionResponses.SetAllChannels(ECR_Block);
	SuspensionTraceCollisionResponses.SetResponse(ECC_Vehicle, ECR_Ignore);
	SuspensionTraceCollisionResponses.SetResponse(ECC_EngineTraceChannel1, ECR_Ignore);

	bUsingNetworkPhysicsPrediction = Chaos::FPhysicsSolverBase::IsNetworkPhysicsPredictionEnabled();
	CurrentAsyncDataType = AsyncInvalid;

	// 网络物理预测开启时创建网络物理组件（数据历史与本地输入中继的载体）
	if (bUsingNetworkPhysicsPrediction)
	{
		NetworkPhysicsComponent = CreateDefaultSubobject<UNetworkPhysicsComponent>(
			TEXT("SingularisNetworkPhysicsComponent")
		);
		NetworkPhysicsComponent->SetNetAddressable();
		NetworkPhysicsComponent->SetIsReplicated(true);
	}

	InputProducerClass = USingularisMorphVehicleDefaultInputProducer::StaticClass();

	// 默认控制输入集：覆盖 ChaosVehicles 各模拟模块读取的全部控制输入名，
	// 用户可在编辑器中增删；缺失的输入名会导致对应模块读取失败。
	InputConfig =
	{
		{Chaos::ThrottleControlName, EModuleInputValueType::MAxis1D},
		{Chaos::SteeringControlName, EModuleInputValueType::MAxis1D},
		{Chaos::BrakeControlName, EModuleInputValueType::MAxis1D},
		{Chaos::HandbrakeControlName, EModuleInputValueType::MAxis1D},
		{Chaos::ClutchControlName, EModuleInputValueType::MAxis1D},
		{Chaos::BoostControlName, EModuleInputValueType::MAxis1D},
		{Chaos::PitchControlName, EModuleInputValueType::MAxis1D},
		{Chaos::RollControlName, EModuleInputValueType::MAxis1D},
		{Chaos::YawControlName, EModuleInputValueType::MAxis1D},
		{Chaos::ReverseControlName, EModuleInputValueType::MBoolean},
		{Chaos::ChangeUpControlName, EModuleInputValueType::MBoolean},
		{Chaos::ChangeDownControlName, EModuleInputValueType::MBoolean}
	};
}

bool USingularisMorphVehicleSimulationComponent::ShouldCreatePhysicsState() const
{
	return true;
}

void USingularisMorphVehicleSimulationComponent::OnCreatePhysicsState()
{
	Super::OnCreatePhysicsState();

	// 1) 构造上下文并初始化适配器（若用户在编辑器中配置了适配器实例）
	if (PhysicsAdapter)
	{
		const FSingularisMorphVehiclePhysicsAdapterContext Context{this};
		PhysicsAdapter->Initialize(Context);
	}

	// 2) 创建物理线程端模拟
	CreateVehicleSimulation();

	// 3) 聚合 SU 输入配置并实例化输入生产者
	SetupInputConfiguration();

	// 4) 网络物理预测：注册输入/状态数据历史
	if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent)
	{
		FScopedModuleInputInitializer SetSetup(CombinedInputConfiguration);
		NetworkPhysicsComponent->CreateDataHistory<FPhysicsSingularisMorphVehicleTraits>(this);

		if (IsLocallyControlled())
			NetworkPhysicsComponent->SetIsRelayingLocalInputs(true);
	}

	// 5) 模块生命周期事件自订阅（将事件分发为 SU 组件的 OnAdded/OnRemoved）
	OnSimulationModuleAddedNativeEvent.RemoveAll(this);
	OnSimulationModuleAddedNativeEvent.AddUObject(
		this,
		&USingularisMorphVehicleSimulationComponent::OnSimulationModuleInitialized
	);
	OnSimulationModuleRemovedNativeEvent.RemoveAll(this);
	OnSimulationModuleRemovedNativeEvent.AddUObject(
		this,
		&USingularisMorphVehicleSimulationComponent::OnSimulationModuleRemovedCallback
	);

	// 6) 若需禁止休眠则设置集群粒子为永不睡眠
	IPhysicsProxyBase* Proxy = GetPhysicsProxy();
	if (bKeepVehicleAwake && Proxy && Proxy->GetType() == EPhysicsProxyType::ClusterUnionProxy)
	{
		if (auto* CUProxy = static_cast<Chaos::FClusterUnionPhysicsProxy*>(Proxy))
		{
			if (auto* Particle = CUProxy->GetParticle_External())
				Particle->SetSleepType(Chaos::ESleepType::NeverSleep);
		}
	}

	// 7) 自忽略（悬挂射线不命中自身）
	if (AActor* Owner = GetOwner())
		ActorsToIgnore.AddUnique(Owner);
}

void USingularisMorphVehicleSimulationComponent::OnDestroyPhysicsState()
{
	// 1) 解绑模块生命周期事件
	OnSimulationModuleAddedNativeEvent.RemoveAll(this);
	OnSimulationModuleRemovedNativeEvent.RemoveAll(this);

	// 2) 网络物理预测：注销数据历史
	if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent)
		NetworkPhysicsComponent->RemoveDataHistory();

	// 3) 销毁物理线程端模拟
	DestroyVehicleSimulation();

	// 4) 终止适配器（解绑事件）
	if (PhysicsAdapter) PhysicsAdapter->Terminate();

	CachedPhysicsProxy = nullptr;

	Super::OnDestroyPhysicsState();
}

int32 USingularisMorphVehicleSimulationComponent::AddSimulationModule(
	Chaos::ISimulationModuleBase* CoreModule,
	const FTransform& ComponentTransform,
	const int32 ParentIndex,
	const int32 TransformIndex,
	const Chaos::FUniqueIdx ParticleIndex,
	const FTransform& PhysicalTransform
)
{
	// 手动模式 API：转发到 Internal API
	return AddModuleToTree(
		CoreModule,
		ComponentTransform,
		ParentIndex,
		TransformIndex,
		ParticleIndex,
		PhysicalTransform
	);
}

void USingularisMorphVehicleSimulationComponent::RemoveSimulationModule(const int32 ModuleGuid)
{
	if (ModuleGuid == INDEX_NONE) return;

	// 1) 通知旧模块终止：模块对象由游戏线程创建，终止回调必须在游戏线程执行，
	//    用于释放悬挂约束等外部资源。FSimModuleTree::DeleteNode 只 delete 模块对象，
	//    不会调用 OnTermination_External，若不在此释放，每次重建都会泄漏物理悬挂约束，
	//    旧约束会持续对集群根粒子施加力，导致抖动与车轮飞散。
	//    模块指针取自游戏线程映射表，不读取物理线程拥有的模拟树。
	if (Chaos::ISimulationModuleBase** FoundModule = GuidToCoreModule.Find(ModuleGuid))
	{
		if (Chaos::ISimulationModuleBase* Mod = *FoundModule)
		{
			Mod->SetAnimationEnabled(false);
			Mod->SetStateFlags(Chaos::eSimModuleState::Disabled);
			Mod->OnTermination_External();

			BroadcastModuleRemovedEvent(Mod->GetSimType(), Mod->GetGuid(), Mod->GetTreeIndex());
		}
		GuidToCoreModule.Remove(ModuleGuid);
	}
	else if (StoredTreeUpdates.GetNewModules().ContainsByPredicate(
		[ModuleGuid](const Chaos::FPendingModuleAdds& Pending)
		{
			return Pending.NewSimModule && Pending.NewSimModule->GetGuid() == ModuleGuid;
		}
	))
	{
		// 模块尚未提交给物理线程却不在映射中：删除仍会交给树执行，但终止回调无处可发，
		// 悬挂约束等外部资源会泄漏。模块入树必须经 AddModuleToTree 登记
		ensureMsgf(false, TEXT("[RemoveSimulationModule] Module GUID %d is pending but untracked"), ModuleGuid);
	}

	// 2) 清理映射缓存，防止悬空引用随增删累积
	if (const TWeakObjectPtr<UActorComponent>* Component = PhysicsGuidToComponent.Find(ModuleGuid))
	{
		if (auto* SUComp = Cast<USingularisMorphVehicleSUComponent>(Component->Get()))
		{
			SUComp->SetModuleGuid(INDEX_NONE);
			SUComp->SetTreeIndex(INDEX_NONE);
		}

		ComponentToPhysicsObjects.Remove(Component->Get());
		PhysicsGuidToComponent.Remove(ModuleGuid);
	}

	// 3) 维护动画槽位：删除指向被移除模块的槽位，被多个模块共享的槽位由仍存活的模块接管，
	//    其余槽位按删除情况整体前移，保持槽位下标与数组下标一致
	const int32 NumSetups = ModuleAnimationSetups.Num();
	TArray<int32> SlotRemap;
	SlotRemap.Init(INDEX_NONE, NumSetups);

	auto NumRemainingSetups = 0;
	for (auto I = 0; I < NumSetups; ++I)
	{
		if (ModuleAnimationSetups[I].ModuleGUID == ModuleGuid)
		{
			// 共享同一骨骼的模块（如车轮与悬挂）接管槽位，无接管者时该槽位失效
			Chaos::ISimulationModuleBase* Successor = nullptr;
			for (const auto& Pair : GuidToCoreModule)
			{
				if (Pair.Value && Pair.Value->GetAnimationSetupIndex() == I &&
					Pair.Value->GetBoneName() == ModuleAnimationSetups[I].BoneName)
				{
					Successor = Pair.Value;
					break;
				}
			}

			if (!Successor) continue;

			// 接管槽位时同步接管者的初始姿态：非骨骼动画以初始姿态为增量基准
			// （模块的 InitialParticleTransform 即注册槽位时使用的初始变换）
			ModuleAnimationSetups[I].ModuleGUID = Successor->GetGuid();
			ModuleAnimationSetups[I].InitialRotOffset = Successor->GetInitialParticleTransform().GetRotation();
			ModuleAnimationSetups[I].InitialLocOffset = Successor->GetInitialParticleTransform().GetTranslation();
		}

		SlotRemap[I] = NumRemainingSetups++;
	}

	if (NumRemainingSetups != NumSetups)
	{
		auto Write = 0;
		for (auto I = 0; I < NumSetups; ++I)
		{
			if (SlotRemap[I] != INDEX_NONE)
				ModuleAnimationSetups[Write++] = ModuleAnimationSetups[I];
		}
		ModuleAnimationSetups.SetNum(NumRemainingSetups);
	}

	for (const auto& Pair : GuidToCoreModule)
	{
		Chaos::ISimulationModuleBase* Module = Pair.Value;
		if (!Module) continue;

		const int32 Slot = Module->GetAnimationSetupIndex();
		if (!SlotRemap.IsValidIndex(Slot) || SlotRemap[Slot] == INDEX_NONE) continue;

		Module->SetAnimationData(Module->GetBoneName(), Module->GetAnimationOffset(), SlotRemap[Slot]);
	}

	// 4) 排队删除树节点
	StoredTreeUpdates.RemoveNode(ModuleGuid);

	// 5) 拓扑已变更：标记待重建网络复制结构
	bPendingReplicationStructureRebuild = true;
}

void USingularisMorphVehicleSimulationComponent::FinalizeModuleUpdates()
{
	UE_LOG(
		LogSingularisMorphVehicle,
		Verbose,
		TEXT("[FinalizeModuleUpdates] VehicleSimulationPT=%s, StoredTreeUpdates pending adds=%d"),
		VehicleSimulationPT.IsValid() ? TEXT("valid") : TEXT("null"),
		StoredTreeUpdates.GetNewModules().Num()
	);

	if (VehicleSimulationPT)
		VehicleSimulationPT->AppendTreeUpdates(&StoredTreeUpdates);

	// 新增模块会使网络状态历史的模块布局失效，标记待重建
	if (StoredTreeUpdates.GetNewModules().Num() > 0)
		bPendingReplicationStructureRebuild = true;

	StoredTreeUpdates = Chaos::FSimTreeUpdates();

	// 手动注册路径没有重建流程，根物理对象可能尚未绑定，
	// 缺绑将使阻尼写入与约束创建整体失效
	if (RootPhysicsObject == nullptr)
		CacheRootPhysicsObject(GetPhysicsProxy());

	// 设置物理阻尼（抑制悬挂振荡）
	UpdatePhysicalProperties();
}

void USingularisMorphVehicleSimulationComponent::RebuildFromSnapshot(
	const FSingularisMorphVehiclePhysicsAdapterSnapshot& Snapshot
)
{
	if (!VehicleSimulationPT) return;

	// 1) 解析映射子系统：SU 组件的查询统一由映射完成
	const UWorld* World = GetWorld();
	const USingularisMorphVehicleMappingSubsystem* Subsystem =
		World ? World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>() : nullptr;
	if (!Subsystem) return;

	AActor* Owner = GetOwner();
	if (!IsValid(Owner)) return;

	// 2) 空快照：无集群实体可建。此前已构建过模块（载具完全解体）时清除全部模拟模块，
	//    否则无操作，保证幂等（集群尚未组装完成时反复调用不产生副作用）。
	if (Snapshot.Entities.IsEmpty())
	{
		if (GuidToCoreModule.IsEmpty()) return;

		ClearAllSimulationModules();
		return;
	}

	// 3) 构建 物理组件 → 实体 查找表（快照仅描述物理侧信息）。
	//    同一物理组件贡献多个粒子时后写入的实体覆盖先前的（模块按组件映射，无法区分多粒子）；
	//    必须用 FindOrAdd 赋值而非 Add：TMap::Add 允许重复键，会让查找结果不确定
	TMap<TObjectPtr<UPrimitiveComponent>, const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity*>
		EntityMap;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;

		// 集群联合适配器以物理组件为装配单位，一个组件贡献多个子粒子时无法把它们分别绑定到模块：
		// 骨骼网格体载具需要专用的骨骼网格体适配器（骨骼↔模块的配置由适配器自身携带），
		// 本路径不支持该用法，命中即告警
		if (EntityMap.Contains(Entity.PrimitiveComponent))
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT(
					"[RebuildFromSnapshot] Component %s contributes multiple cluster particles - the cluster union adapter cannot bind them to module components separately; skeletal mesh vehicles require a dedicated skeletal mesh adapter"
				),
				*GetNameSafe(Entity.PrimitiveComponent)
			);
		}

		EntityMap.FindOrAdd(Entity.PrimitiveComponent) = &Entity;
	}

	// 4) 统一收集本次参与重建的 SU 组件：
	//    a) 快照实体上通过映射子系统注册的 SU（集群子件）；
	//    b) Owner 上未出现在快照中的 SU（纯仿真模块，如未入簇的引擎/变速箱）。
	//    单一来源保证各 Pass 的遍历与类型分派一致，避免遗漏动力链模块。
	TArray<USingularisMorphVehicleSUComponent*> AllSUComponents;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;
		for (USingularisMorphVehicleSUComponent* SUComp :
		     Subsystem->FindSUComponents(Entity.PrimitiveComponent))
		{
			if (SUComp)
				AllSUComponents.AddUnique(SUComp);
		}
	}
	{
		TArray<UActorComponent*> ActorComponents;
		Owner->GetComponents<UActorComponent>(ActorComponents);
		for (UActorComponent* ActorComponent : ActorComponents)
		{
			if (auto* SUComp = Cast<USingularisMorphVehicleSUComponent>(ActorComponent))
				AllSUComponents.AddUnique(SUComp);
		}
	}

	// 5) 底盘守卫：不存在 Chassis 类型 SU 时（车身件脱离集群 = 载具解体），清除全部模拟模块
	auto bHasChassis = false;
	for (const USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (SUComp->GetModuleType() == ESingularisMorphVehicleModuleType::Chassis)
		{
			bHasChassis = true;
			break;
		}
	}
	if (!bHasChassis)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[RebuildFromSnapshot] Chassis not found in snapshot (%d entities) - clearing all simulation modules"),
			Snapshot.Entities.Num()
		);

		ClearAllSimulationModules();
		return;
	}

	// 5a) 幂等守卫：模块布局与新快照一致时跳过重建。
	//     适配器可能重复上报同一批子件（例如引擎在物理重同步时重发集群事件），
	//     冗余的变更信号若每次都触发全量重建，会逐帧销毁并重建悬挂约束、反复重基准
	//     静止位姿，表现为载具抖动与持续漂移。
	//     判据：SU 数量、每个 SU 的驱动组件、其物理粒子索引、以及模块存在性均一致。
	//     模块类型与父子关系由 SU 集合与引用确定性推导，上述二者一致即整棵树一致。
	bool bTopologyUnchanged = ComponentToPhysicsObjects.Num() == AllSUComponents.Num();
	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (!bTopologyUnchanged) break;

		const FSingularisMorphVehicleComponentData* ExistingModule = ComponentToPhysicsObjects.Find(SUComp);
		if (!ExistingModule)
		{
			bTopologyUnchanged = false;
			break;
		}

		const Chaos::ISimulationModuleBase* const* FoundModule = GuidToCoreModule.Find(ExistingModule->Guid);
		if (!FoundModule || !*FoundModule)
		{
			bTopologyUnchanged = false;
			break;
		}

		UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(SUComp->DrivenComponent.GetComponent(Owner));
		if (ExistingModule->ProxyComponentToAnimate.Get() != static_cast<USceneComponent*>(ProxyComp))
		{
			bTopologyUnchanged = false;
			break;
		}

		const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity* Entity = ProxyComp
			                                                                    ? EntityMap.FindRef(ProxyComp)
			                                                                    : nullptr;
		const int32 ExpectedParticleIdx = Entity ? Entity->ParticleIndex : Chaos::FUniqueIdx().Idx;

		bTopologyUnchanged = (*FoundModule)->GetParticleIndex().Idx == ExpectedParticleIdx;
	}

	if (bTopologyUnchanged) return;

	// 6) 模块集合可能已变化（变形增删部件），刷新输入配置。
	//     配置未变更时该调用为空操作，不会清空输入容器。
	SetupInputConfiguration();

	const FTransform ReferenceTransform = PhysicsAdapter
		                                      ? PhysicsAdapter->GetReferenceTransform()
		                                      : FTransform::Identity;

	// 7) 移除旧模块：收集已有 GUID 并全部标记删除
	TArray<int32> ExistingGuids;
	ExistingGuids.Reserve(GuidToCoreModule.Num());
	for (const auto& Pair : GuidToCoreModule)
		ExistingGuids.Add(Pair.Key);

	UE_LOG(
		LogSingularisMorphVehicle,
		Verbose,
		TEXT("[RebuildFromSnapshot] Removing %d old modules, adding %d SU components from snapshot"),
		ExistingGuids.Num(),
		AllSUComponents.Num()
	);

	for (int32 Guid : ExistingGuids)
		RemoveSimulationModule(Guid);

	// 8) 清空所有缓存，保证幂等
	ComponentToPhysicsObjects.Empty();
	PhysicsGuidToComponent.Empty();
	ModuleAnimationSetups.Empty();
	NextConstructionIndex = 0;

	// 8a) 缓存根物理对象（首次或重建时刷新）
	//     模块构造期需要 RootPhysicsObject 进行 OnConstruction_External 初始化
	if (RootPhysicsObject == nullptr)
		CacheRootPhysicsObject(GetPhysicsProxy());

	// 9) 核心 Lambda：将单个 SU 注册到模拟树。
	//    通过 SU 的 DrivenComponent 解析物理组件：
	//    - 物理组件在快照实体中（集群子粒子）→ 复用其粒子索引与集群变换；
	//    - 驱动组件缺失或不在本次快照中（纯仿真模块）→ 粒子无效，变换取组件/根相对变换。
	TSet<USingularisMorphVehicleSUComponent*> ProcessedComponents;

	auto AddEntity = [&](USingularisMorphVehicleSUComponent* SUComp, const int32 ParentIndex) -> int32
	{
		if (!SUComp || ProcessedComponents.Contains(SUComp)) return INDEX_NONE;

		Chaos::ISimulationModuleBase* CoreModule = SUComp->CreateNewCoreModule();
		if (!CoreModule) return INDEX_NONE;

		// SU 的动画开关是模块动画的总开关：部分模块类型（引擎、变速箱等）
		// 不自带动画而不会在 CreateNewCoreModule 中同步该标志，此处统一同步
		CoreModule->SetAnimationEnabled(SUComp->GetAnimationEnabled());

		UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
			SUComp->DrivenComponent.GetComponent(Owner)
		);
		const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity* Entity = ProxyComp
			                                                                    ? EntityMap.FindRef(ProxyComp)
			                                                                    : nullptr;

		FTransform CompTransform = FTransform::Identity;
		if (ProxyComp)
			CompTransform = ProxyComp->GetComponentTransform().GetRelativeTransform(ReferenceTransform);
		else if (const USceneComponent* RootComp = Owner->GetRootComponent())
			CompTransform = RootComp->GetComponentTransform().GetRelativeTransform(ReferenceTransform);
		CompTransform = SUComp->TransformOffset * CompTransform;

		const int32 TransformIndex = NextConstructionIndex++;

		const int32 ModuleIdx = AddModuleToTree(
			CoreModule,
			CompTransform,
			ParentIndex,
			TransformIndex,
			Entity ? Chaos::FUniqueIdx(Entity->ParticleIndex) : Chaos::FUniqueIdx(),
			Entity ? Entity->ChildToParent : FTransform::Identity,
			SUComp->GetBoneName(),
			SUComp->GetAnimationOffset()
		);

		if (ModuleIdx == INDEX_NONE) return INDEX_NONE;

		SUComp->SetModuleGuid(CoreModule->GetGuid());

		FSingularisMorphVehicleComponentData CompData;
		CompData.Guid = CoreModule->GetGuid();
		CompData.ProxyComponentToAnimate = ProxyComp;
		ComponentToPhysicsObjects.Add(SUComp, CompData);
		PhysicsGuidToComponent.Add(CoreModule->GetGuid(), SUComp);
		ProcessedComponents.Add(SUComp);

		return ModuleIdx;
	};

	// 8) 按确定性顺序重建物理模拟树。
	//    拓扑约定：Wheel 挂扭矩源（轮轴/底盘），Suspension 挂其配对车轮之下。
	//    悬挂作为车轮的子节点使叶先序执行时悬挂先于车轮求解，
	//    车轮同一物理步内即可消费到最新的悬挂法向力。

	// 10a) Pass 1: Chassis → root
	int32 ChassisIndex = INDEX_NONE;
	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (SUComp->GetModuleType() != ESingularisMorphVehicleModuleType::Chassis) continue;

		ChassisIndex = AddEntity(SUComp, INDEX_NONE);
		if (ChassisIndex != INDEX_NONE) break;
	}

	// 底盘模块创建失败时，后续节点会以无效父索引入树，树退化为多根并打乱叶先序执行序，
	// 此时与载具解体同样处理
	if (ChassisIndex == INDEX_NONE)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Error,
			TEXT("[RebuildFromSnapshot] Chassis module creation failed - clearing all simulation modules")
		);

		ClearAllSimulationModules();
		return;
	}

	// 10b) Pass 2: 原动机（引擎/电机）→ 离合器 → 变速箱 动力链（通过 Linked 引用串联）
	//     记录 SU 组件 → 变速箱树索引，供轮轴与车轮解析扭矩父节点
	TMap<TObjectPtr<UActorComponent>, int32> TransmissionIndexByComponent;
	int32 FirstPrimeMoverIndex = INDEX_NONE;
	int32 FirstClutchIndex = INDEX_NONE;

	// 引擎与电机同为原动机，共用同一接线规则，可共存构成混动
	auto ResolveLinkedClutch = [&](
		const USingularisMorphVehicleSUComponent* PrimeMover
	)
		-> USingularisClutchSUComponent*
	{
		if (const auto* EngineSU = Cast<USingularisEngineSUComponent>(PrimeMover))
			return Cast<USingularisClutchSUComponent>(EngineSU->LinkedClutch.GetComponent(Owner));

		if (const auto* MotorSU = Cast<USingularisMotorSUComponent>(PrimeMover))
			return Cast<USingularisClutchSUComponent>(MotorSU->LinkedClutch.GetComponent(Owner));

		return nullptr;
	};

	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		const ESingularisMorphVehicleModuleType Type = SUComp->GetModuleType();
		if (Type != ESingularisMorphVehicleModuleType::Engine &&
			Type != ESingularisMorphVehicleModuleType::Motor)
			continue;

		const int32 PrimeMoverIndex = AddEntity(SUComp, ChassisIndex);
		if (PrimeMoverIndex == INDEX_NONE) continue;

		if (FirstPrimeMoverIndex == INDEX_NONE) FirstPrimeMoverIndex = PrimeMoverIndex;

		USingularisClutchSUComponent* ClutchSU = ResolveLinkedClutch(SUComp);
		if (!ClutchSU) continue;

		const int32 ClutchIndex = AddEntity(ClutchSU, PrimeMoverIndex);
		if (FirstClutchIndex == INDEX_NONE) FirstClutchIndex = ClutchIndex;

		if (auto* TransSU = Cast<USingularisTransmissionSUComponent>(
			ClutchSU->LinkedTransmission.GetComponent(Owner)
		))
		{
			if (const int32 TransmissionIndex = AddEntity(TransSU, ClutchIndex);
				TransmissionIndex != INDEX_NONE)
				TransmissionIndexByComponent.FindOrAdd(TransSU) = TransmissionIndex;
		}
	}

	// 10b-2) Pass 2b: 未被动力链引用的离合器/变速箱兜底接入动力链，并登记变速箱树索引。
	//         动力链拓扑由 LinkedClutch/LinkedTransmission 显式引用决定，未配置引用时静默丢弃
	//         会让用户在编辑器中配置的模块凭空消失；此处按“首个上游扭矩源”接管并告警，
	//         使缺失引用的车辆仍能输出扭矩。离合器先于变速箱接入，保证变速箱能挂到离合器之下。
	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (SUComp->GetModuleType() != ESingularisMorphVehicleModuleType::Clutch) continue;
		if (ProcessedComponents.Contains(SUComp)) continue;

		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT(
				"[RebuildFromSnapshot] Clutch %s is not referenced by any prime mover - attach it under the first prime mover and set LinkedClutch on that prime mover to declare the powertrain chain"
			),
			*SUComp->GetName()
		);

		const int32 ClutchIndex = AddEntity(
			SUComp,
			FirstPrimeMoverIndex != INDEX_NONE ? FirstPrimeMoverIndex : ChassisIndex
		);
		if (FirstClutchIndex == INDEX_NONE) FirstClutchIndex = ClutchIndex;
	}

	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (SUComp->GetModuleType() != ESingularisMorphVehicleModuleType::Transmission) continue;
		if (ProcessedComponents.Contains(SUComp)) continue;

		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT(
				"[RebuildFromSnapshot] Transmission %s is not referenced by any clutch - attach it under the first clutch and set LinkedTransmission on the clutch to declare the powertrain chain"
			),
			*SUComp->GetName()
		);

		auto ParentIdx = ChassisIndex;
		if (FirstClutchIndex != INDEX_NONE)
			ParentIdx = FirstClutchIndex;
		else if (FirstPrimeMoverIndex != INDEX_NONE)
			ParentIdx = FirstPrimeMoverIndex;

		if (const int32 TransmissionIndex = AddEntity(SUComp, ParentIdx); TransmissionIndex != INDEX_NONE)
			TransmissionIndexByComponent.FindOrAdd(SUComp) = TransmissionIndex;
	}

	// 10c) Pass 3: Axle → 链接的 Transmission 下（无则 Chassis 下）
	TMap<TObjectPtr<UActorComponent>, int32> AxleIndexByComponent;
	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (SUComp->GetModuleType() != ESingularisMorphVehicleModuleType::Axle) continue;

		int32 ParentIdx = ChassisIndex;
		if (const auto* AxleSU = Cast<USingularisAxleSUComponent>(SUComp))
		{
			if (const int32* FoundIdx = TransmissionIndexByComponent.Find(
				AxleSU->LinkedTransmission.GetComponent(Owner)
			))
				ParentIdx = *FoundIdx;
		}

		if (const int32 AxleIndex = AddEntity(SUComp, ParentIdx); AxleIndex != INDEX_NONE)
			AxleIndexByComponent.FindOrAdd(SUComp) = AxleIndex;
	}

	// 10d) Pass 4: Wheel → 链接轮轴 / Chassis；记录 物理组件 → 车轮索引供悬挂配对
	TMap<TObjectPtr<UPrimitiveComponent>, int32> WheelIndexByComponent;
	TMap<TObjectPtr<UActorComponent>, int32> WheelIndexByComponentKey;
	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (SUComp->GetModuleType() != ESingularisMorphVehicleModuleType::Wheel) continue;

		int32 ParentIdx = ChassisIndex;
		if (const auto* WheelSU = Cast<USingularisWheelSUComponent>(SUComp))
		{
			if (const int32* FoundIdx = AxleIndexByComponent.Find(
				WheelSU->LinkedAxle.GetComponent(Owner)
			))
				ParentIdx = *FoundIdx;
		}

		const int32 WheelIndex = AddEntity(SUComp, ParentIdx);
		if (WheelIndex == INDEX_NONE) continue;

		WheelIndexByComponentKey.FindOrAdd(SUComp) = WheelIndex;
		if (UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
			SUComp->DrivenComponent.GetComponent(Owner)
		))
			WheelIndexByComponent.FindOrAdd(ProxyComp) = WheelIndex;
	}

	// 10e) Pass 5: Suspension → 配对车轮之下（无则 Chassis 下）。
	//     配对优先级：同一物理组件上的车轮 → 车轮 LinkedSuspension 显式引用 → Chassis。
	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (SUComp->GetModuleType() != ESingularisMorphVehicleModuleType::Suspension) continue;

		int32 ParentIdx = ChassisIndex;

		if (UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
			SUComp->DrivenComponent.GetComponent(Owner)
		))
		{
			if (const int32* FoundIdx = WheelIndexByComponent.Find(ProxyComp))
				ParentIdx = *FoundIdx;
		}

		if (ParentIdx == ChassisIndex)
		{
			// 反向解析显式引用：查找 LinkedSuspension 指向本悬挂的车轮
			for (const auto& Pair : WheelIndexByComponentKey)
			{
				const auto* WheelSU = Cast<USingularisWheelSUComponent>(Pair.Key.Get());
				if (WheelSU && WheelSU->LinkedSuspension.GetComponent(Owner) == SUComp)
				{
					ParentIdx = Pair.Value;
					break;
				}
			}
		}

		// 配对失败：悬挂挂到底盘下将无法与车轮建立交叉链接（车轮失去悬挂力），须告警
		if (ParentIdx == ChassisIndex)
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT(
					"[RebuildFromSnapshot] Suspension %s has no paired wheel - attach it to a wheel via the same physics component or the wheel LinkedSuspension reference"
				),
				*SUComp->GetName()
			);
		}

		AddEntity(SUComp, ParentIdx);
	}

	// 10f) Pass 6: 其余模块（Aerofoil、Thruster 等）→ Chassis
	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		const ESingularisMorphVehicleModuleType Type = SUComp->GetModuleType();
		if (Type == ESingularisMorphVehicleModuleType::Chassis ||
			Type == ESingularisMorphVehicleModuleType::Engine ||
			Type == ESingularisMorphVehicleModuleType::Motor ||
			Type == ESingularisMorphVehicleModuleType::Clutch ||
			Type == ESingularisMorphVehicleModuleType::Transmission ||
			Type == ESingularisMorphVehicleModuleType::Suspension ||
			Type == ESingularisMorphVehicleModuleType::Wheel ||
			Type == ESingularisMorphVehicleModuleType::Axle)
			continue;

		AddEntity(SUComp, ChassisIndex);
	}

	// 11) 批量提交到物理线程
	FinalizeModuleUpdates();
}

void USingularisMorphVehicleSimulationComponent::UpdatePhysicalProperties()
{
	IPhysicsProxyBase* Proxy = GetPhysicsProxy();

	// 1) 集群联合代理：命令队列写入粒子阻尼
	if (Proxy && Proxy->GetType() == EPhysicsProxyType::ClusterUnionProxy)
	{
		auto* CUProxy = static_cast<Chaos::FClusterUnionPhysicsProxy*>(Proxy);
		Chaos::FPBDRigidsSolver* Solver = Proxy->GetSolver<Chaos::FPBDRigidsSolver>();
		if (!Solver) return;

		Solver->EnqueueCommandImmediate(
			[CUProxy, this]()
			{
				if (auto* Particle = CUProxy->GetParticle_Internal())
				{
					Particle->SetLinearEtherDrag(LinearDamping);
					Particle->SetAngularEtherDrag(AngularDamping);
				}
			}
		);
		return;
	}

	// 2) 单粒子路径：通过根物理对象加锁写入
	if (RootPhysicsObject)
	{
		TArray<Chaos::FPhysicsObjectHandle> PhysicsObjects{RootPhysicsObject};
		FLockedWritePhysicsObjectExternalInterface Interface = FPhysicsObjectExternalInterface::LockWrite(
			PhysicsObjects
		);
		Interface->SetLinearEtherDrag(PhysicsObjects, LinearDamping);
		Interface->SetAngularEtherDrag(PhysicsObjects, AngularDamping);
	}
}

void USingularisMorphVehicleSimulationComponent::CacheRootPhysicsObject(
	IPhysicsProxyBase* Proxy
)
{
	Chaos::EnsureIsInGameThreadContext();
	using namespace Chaos;
	RootPhysicsObject = nullptr;

	if (!Proxy) return;

	switch (Proxy->GetType())
	{
	case EPhysicsProxyType::ClusterUnionProxy:
		if (auto* CUProxy = static_cast<FClusterUnionPhysicsProxy*>(Proxy))
			RootPhysicsObject = CUProxy->GetPhysicsObjectHandle();
		break;

	case EPhysicsProxyType::SingleParticleProxy:
		if (auto* ParticleProxy = static_cast<FSingleParticlePhysicsProxy*>(Proxy))
			RootPhysicsObject = ParticleProxy->GetPhysicsObject();
		break;

	default: break;
	}

	CachedPhysicsProxy = Proxy;
}

void USingularisMorphVehicleSimulationComponent::SetInputProducerClass(
	TSubclassOf<UVehicleInputProducerBase> InInputProducerClass,
	const bool bForceNewInstance
)
{
	InputProducerClass = MoveTemp(InInputProducerClass);

	if (bForceNewInstance)
		InputProducer = nullptr;

	SetupInputConfiguration(bForceNewInstance);
}

void USingularisMorphVehicleSimulationComponent::AddInput(const FModuleInputSetup& InputSetup)
{
	InputConfig.AddUnique(InputSetup);
	SetupInputConfiguration(true);
}

void USingularisMorphVehicleSimulationComponent::AddActorsToIgnore(TArray<AActor*>& ActorsIn)
{
	for (AActor* Actor : ActorsIn)
	{
		if (IsValid(Actor))
			ActorsToIgnore.AddUnique(Actor);
	}
}

void USingularisMorphVehicleSimulationComponent::RemoveActorsToIgnore(TArray<AActor*>& ActorsIn)
{
	for (AActor* Actor : ActorsIn)
		ActorsToIgnore.Remove(Actor);
}

void USingularisMorphVehicleSimulationComponent::Update(const float DeltaTime) {}

void USingularisMorphVehicleSimulationComponent::PreTickGT(const float DeltaTime)
{
	if (!PhysicsAdapter || !PhysicsAdapter->IsReady() || !PhysicsAdapter->IsDirty()) return;

	// 前置守卫：缺少映射子系统或 Owner 时重建必然失败。
	// 此时不消费脏标记，留待下一帧重试，避免拓扑变更被静默丢弃。
	if (!GetOwner()) return;
	const UWorld* World = GetWorld();
	if (!World || !World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>()) return;

	// 从适配器拉取完整快照并全量重建物理模拟树（Pull Model）
	const FSingularisMorphVehiclePhysicsAdapterSnapshot Snapshot = PhysicsAdapter->ConsumeSnapshot();
	RebuildFromSnapshot(Snapshot);
}

void USingularisMorphVehicleSimulationComponent::SetCurrentAsyncData(
	FSingularisMorphChaosSimModuleManagerAsyncOutput* CurOutput,
	FSingularisMorphChaosSimModuleManagerAsyncOutput* NextOutput,
	const float Alpha,
	const int32 Timestamp
)
{
	CurrentAsyncOutput = nullptr;
	NextAsyncOutput = nullptr;
	OutputInterpolationAlpha = 0.0f;

	// 1) 在管理器级别输出列表中查找本载具的输出
	if (CurOutput)
	{
		for (const auto& Output : CurOutput->VehicleOutputs)
		{
			if (Output && Output->bValid && Output->Type == CurrentAsyncDataType
				&& Output->Vehicle == this)
			{
				CurrentAsyncOutput = Output.Get();
				break;
			}
		}
	}

	// 2) 查找下一帧输出用于插值
	if (NextOutput)
	{
		for (const auto& Output : NextOutput->VehicleOutputs)
		{
			if (Output && Output->bValid && Output->Type == CurrentAsyncDataType
				&& Output->Vehicle == this)
			{
				NextAsyncOutput = Output.Get();
				OutputInterpolationAlpha = Alpha;
				break;
			}
		}
	}
}

void USingularisMorphVehicleSimulationComponent::ParallelUpdate(
	const Chaos::FCreatedModules& ModuleEvents
)
{
	if (!CurrentAsyncOutput || !CurrentAsyncOutput->bValid || !VehiclePhysicsOutput) return;

	// 1) 清理上一帧输出并按当前输出数量分配槽位
	VehiclePhysicsOutput->Clean();
	VehiclePhysicsOutput->NewlyCreatedModuleGuids = ModuleEvents.ModuleEvents;

	const int32 NumItems = CurrentAsyncOutput->VehicleSimOutput.SimTreeOutputData.Num();
	VehiclePhysicsOutput->SimTreeOutputData.SetNum(NumItems);

	// 2) 对每个模拟输出数据执行插值
	for (auto I = 0; I < NumItems; ++I)
	{
		Chaos::FSimOutputData* CurrentSimData = CurrentAsyncOutput->VehicleSimOutput.SimTreeOutputData[I];
		if (!CurrentSimData) continue;

		Chaos::FSimOutputData* NewSimData = CurrentSimData->MakeNewData();
		VehiclePhysicsOutput->SimTreeOutputData[I] = NewSimData;

		// 下一帧存在同 GUID 输出时按插值系数混合，否则保持当前帧数据
		const Chaos::FSimOutputData* NextSimData = NextAsyncOutput
			                                           ? FindModuleOutputFromGuid(
				                                           NextAsyncOutput->VehicleSimOutput,
				                                           CurrentSimData->ModuleGuid
			                                           )
			                                           : nullptr;
		NewSimData->Lerp(
			*CurrentSimData,
			NextSimData ? *NextSimData : *CurrentSimData,
			NextSimData ? OutputInterpolationAlpha : 0.0f
		);

#if UE_BUILD_DEBUG
		// 模块调试名仅在物理线程侧生成，须随拷贝一并带回游戏线程（ToString 依赖它）
		NewSimData->DebugString = CurrentSimData->DebugString;
#endif
	}

	// 3) 分发输出数据到各 SU Component，同时缓存通用状态
	for (auto I = 0; I < NumItems; ++I)
	{
		Chaos::FSimOutputData* ModuleOutput = VehiclePhysicsOutput->SimTreeOutputData[I];
		if (!ModuleOutput) continue;

		const int32 Guid = ModuleOutput->ModuleGuid;

		if (const TWeakObjectPtr<UActorComponent>* Component = PhysicsGuidToComponent.Find(Guid))
		{
			if (auto* BaseSU = Cast<ISingularisMorphVehicleSUInterface>(
				Component->Get()
			))
				BaseSU->OnOutputReady(ModuleOutput);
		}

		// 3a) 缓存常用状态（多变速箱/多引擎时取最后一个）
		if (ModuleOutput->IsSimType<Chaos::FTransmissionSimModule>())
			CurrentGear = static_cast<Chaos::FTransmissionOutputData*>(ModuleOutput)->CurrentGear;
		else if (ModuleOutput->IsSimType<Chaos::FEngineSimModule>())
		{
			const auto* EngineOutput = static_cast<const Chaos::FEngineOutputData*>(ModuleOutput);
			EngineRPM = EngineOutput->RPM;
			EngineTorque = EngineOutput->Torque;
		}

		// 3b) 将仿真动画数据从 FSimOutputData 拷贝到 ModuleAnimationSetups
		FTransform WorldTransform = FTransform::Identity;
		if (const AActor* Owner = GetOwner())
		{
			if (const USceneComponent* RootComp = Owner->GetRootComponent())
				WorldTransform = RootComp->GetComponentToWorld();
		}

		Chaos::FSimModuleAnimationData AnimData;
		ModuleOutput->GetFinalAnimDataGameThread(WorldTransform, AnimData);

		const int32 AnimIndex = AnimData.AnimationSetupIndex;
		if (AnimIndex >= 0 && AnimIndex < ModuleAnimationSetups.Num())
		{
			ModuleAnimationSetups[AnimIndex].AnimFlags |= AnimData.AnimFlags;
			ModuleAnimationSetups[AnimIndex].CombinedRotation = AnimData.CombinedRotation;

			if (AnimData.AnimFlags & Chaos::EAnimationFlags::AnimateRotation)
				ModuleAnimationSetups[AnimIndex].RotOffset = AnimData.AnimationRotOffset;

			if (AnimData.AnimFlags & Chaos::EAnimationFlags::AnimatePosition)
				ModuleAnimationSetups[AnimIndex].LocOffset = AnimData.AnimationLocOffset;
		}
	}

	// 4) 将动画数据应用到非骨骼可视化组件（悬挂压缩、车轮滚动等）
	UpdateNonSkeletalAnimations();

	UE_LOG(
		LogSingularisMorphVehicle,
		Verbose,
		TEXT("[ParallelUpdate] NumItems=%d, PhysicsGuidToComponent size=%d"),
		NumItems,
		PhysicsGuidToComponent.Num()
	);
}

void USingularisMorphVehicleSimulationComponent::ProduceInput(
	const int32 PhysicsStep,
	const int32 NumSteps,
	FSingularisMorphVehicleAsyncInput* AsyncInput
)
{
	if (!AsyncInput) return;

	IPhysicsProxyBase* Proxy = GetPhysicsProxy();
	if (!Proxy) return;

	// 1) 输入生产者产出本物理步控制输入（GT 频率缓冲合并 → PT 频率拷出并复位）
	if (InputProducer)
		InputProducer->ProduceInput(PhysicsStep, NumSteps, InputNameMap, InputsContainer);

	AsyncInput->SetVehicle(this);
	AsyncInput->Proxy = Proxy;
	AsyncInput->bIsLocallyControlled = IsLocallyControlled();
	CurrentAsyncInput = AsyncInput;

	// 2) 禁止休眠 + 控制输入容器
	AsyncInput->PhysicsInputs.NetworkInputs.VehicleInputs.KeepAwake = bKeepVehicleAwake;
	AsyncInput->PhysicsInputs.NetworkInputs.VehicleInputs.Container = InputsContainer;

	// 3) 状态输入容器
	AsyncInput->PhysicsInputs.StateInputs.StateInputContainer = StateInputContainer;

	// 4) 时间膨胀
	AsyncInput->PhysicsInputs.CurrentTimeDilation = 1.0f;
	if (const UWorld* World = GetWorld())
	{
		if (const AWorldSettings* WorldSettings = World->GetWorldSettings())
		{
			AsyncInput->PhysicsInputs.CurrentTimeDilation = FMath::Max(
				WorldSettings->GetEffectiveTimeDilation(),
				SMALL_NUMBER
			);
		}
	}

	// 5) 悬挂射线参数（地面检测必需）
	FCollisionQueryParams TraceParams(
		NAME_None,
		FCollisionQueryParams::GetUnknownStatId(),
		false,
		nullptr
	);
	TraceParams.bReturnPhysicalMaterial = true;
	TraceParams.AddIgnoredActors(ActorsToIgnore);
	TraceParams.bTraceComplex = bSuspensionTraceComplex;
	AsyncInput->PhysicsInputs.CollisionChannel = SuspensionCollisionChannel;
	AsyncInput->PhysicsInputs.TraceParams = TraceParams;
	AsyncInput->PhysicsInputs.TraceCollisionResponse = SuspensionTraceCollisionResponses;
	AsyncInput->PhysicsInputs.TraceType = TraceType;
}

void USingularisMorphVehicleSimulationComponent::PostUpdate()
{
	if (!VehiclePhysicsOutput) return;

	// 1) 广播本帧新建模块事件（物理线程 ActionTreeUpdates 收集的 NewlyCreatedModuleGuids）
	for (const Chaos::FCreatedModule& AddedModule : VehiclePhysicsOutput->NewlyCreatedModuleGuids)
		BroadcastModuleAddedEvent(AddedModule.SimType, AddedModule.Guid, AddedModule.TreeIndex);

	// 2) 模块布局发生增删后，为网络状态历史的全部帧重建复制结构，
	//    否则历史帧缺失/多出模块数据会导致 rewind 重模拟崩溃
	if (bPendingReplicationStructureRebuild)
	{
		if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent && VehicleSimulationPT)
		{
			TSharedPtr<Chaos::FBaseRewindHistory>& History = NetworkPhysicsComponent->GetStateHistory_Internal();
			if (auto* StateHistory = static_cast<Chaos::TDataRewindHistory<FNetworkSingularisMorphVehicleStates>*>(
				History.Get()
			))
			{
				for (auto I = 0; I < StateHistory->GetDataHistory().Num(); I++)
					VehicleSimulationPT->GenerateReplicationStructure(StateHistory->GetDataHistory()[I]);
			}
		}

		bPendingReplicationStructureRebuild = false;
	}
}

void USingularisMorphVehicleSimulationComponent::FinalizeSimCallbackData(
	FSingularisMorphChaosSimModuleManagerAsyncInput& Input
)
{
	// 本帧异步数据已全部消费（PostUpdate 之后、下一帧物理步进之前），复位引用
	CurrentAsyncInput = nullptr;
	CurrentAsyncOutput = nullptr;
	NextAsyncOutput = nullptr;
}

void USingularisMorphVehicleSimulationComponent::ShowDebugInfo(
	AHUD* HUD,
	UCanvas* Canvas,
	const FDebugDisplayInfo& DisplayInfo,
	float& YL,
	float& YPos
)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	UFont* RenderFont = GEngine->GetMediumFont();
	if (!RenderFont || !Canvas) return;

	Canvas->SetDrawColor(FColor::White);

	// 1) 控制输入实时值
	for (auto I = 0; I < InputsContainer.GetNumInputs(); I++)
	{
		const float Magnitude = InputsContainer.GetValueAtIndex(I).GetMagnitude();
		const FName InputName = CombinedInputConfiguration.IsValidIndex(I)
			                        ? CombinedInputConfiguration[I].Name
			                        : NAME_None;
		YPos += Canvas->DrawText(
			RenderFont,
			FString::Printf(TEXT("%s %3.2f"), *InputName.ToString(), Magnitude),
			4,
			YPos
		);
	}

	// 2) 通用状态摘要
	YPos += Canvas->DrawText(
		RenderFont,
		FString::Printf(
			TEXT("Gear %d  RPM %3.0f  Torque %3.0f  Speed(km/h) %3.1f"),
			CurrentGear,
			EngineRPM,
			EngineTorque,
			GetVehicleSpeed()
		),
		4,
		YPos
	);

	YPos += 10;

	// 3) 逐模块输出
	if (VehiclePhysicsOutput)
	{
		for (Chaos::FSimOutputData* Data : VehiclePhysicsOutput->SimTreeOutputData)
		{
			if (Data)
				YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("%s"), *Data->ToString()), 4, YPos);
		}
	}
#endif
}

const FTransform& USingularisMorphVehicleSimulationComponent::GetComponentTransform() const
{
	if (const AActor* Owner = GetOwner())
	{
		if (const USceneComponent* RootComp = Owner->GetRootComponent())
			return RootComp->GetComponentTransform();
	}

	return FTransform::Identity;
}

const Chaos::FSimOutputData* USingularisMorphVehicleSimulationComponent::GetOutputData(const int32 ModuleGuid)
{
	return VehiclePhysicsOutput
		       ? VehiclePhysicsOutput->GetOutputData(ModuleGuid)
		       : nullptr;
}

void USingularisMorphVehicleSimulationComponent::SetInputBool(
	const FName Name,
	const bool Value,
	const EModuleInputBufferActionType BufferAction
)
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInputInteger(
	const FName Name,
	const int32 Value,
	const EModuleInputBufferActionType BufferAction
)
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInputAxis1D(
	const FName Name,
	const double Value,
	const EModuleInputBufferActionType BufferAction
)
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInputAxis2D(
	const FName Name,
	const FVector2D Value,
	const EModuleInputBufferActionType BufferAction
)
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInputAxis3D(
	const FName Name,
	const FVector Value,
	const EModuleInputBufferActionType BufferAction
)
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetGearInput(const int32 Gear)
{
	// 换挡通过 ChangeUp/ChangeDown 脉冲驱动 Chaos 变速箱，
	// 目标挡位与缓存挡位比较得出升/降挡方向（变速箱自身维护换挡时序）
	if (Gear > CurrentGear)
		SetInputBool(Chaos::ChangeUpControlName, true);
	else if (Gear < CurrentGear)
		SetInputBool(Chaos::ChangeDownControlName, true);
}

void USingularisMorphVehicleSimulationComponent::SetInput(
	const FName& Name,
	const bool Value,
	const EModuleInputBufferActionType BufferAction
)
{
	SetInputBool(Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInput(
	const FName& Name,
	const int32 Value,
	const EModuleInputBufferActionType BufferAction
)
{
	SetInputInteger(Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInput(
	const FName& Name,
	const double Value,
	const EModuleInputBufferActionType BufferAction
)
{
	SetInputAxis1D(Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInput(
	const FName& Name,
	const FVector2D& Value,
	const EModuleInputBufferActionType BufferAction
)
{
	SetInputAxis2D(Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInput(
	const FName& Name,
	const FVector& Value,
	const EModuleInputBufferActionType BufferAction
)
{
	SetInputAxis3D(Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetState(const FName& Name, const bool Value)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer, InputQuantizationType);
	Inputs.SetBool(Name, Value);
}

void USingularisMorphVehicleSimulationComponent::SetState(const FName& Name, const int32 Value)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer, InputQuantizationType);
	Inputs.SetInteger(Name, Value);
}

void USingularisMorphVehicleSimulationComponent::SetState(const FName& Name, const double Value)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer, InputQuantizationType);
	Inputs.SetFloat(Name, Value);
}

void USingularisMorphVehicleSimulationComponent::SetState(const FName& Name, const FVector2D& Value)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer, InputQuantizationType);
	Inputs.SetVector2D(Name, Value);
}

void USingularisMorphVehicleSimulationComponent::SetState(const FName& Name, const FVector& Value)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer, InputQuantizationType);
	Inputs.SetVector(Name, Value);
}

void USingularisMorphVehicleSimulationComponent::SetLocallyControlled(const bool bInLocallyControlled)
{
	if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent)
	{
		// NetworkPhysicsComponent 是唯一权威来源，防止 SimulatedProxy 误标本地
		if (GetOwner() && GetOwner()->GetLocalRole() != ROLE_SimulatedProxy)
			NetworkPhysicsComponent->SetIsRelayingLocalInputs(bInLocallyControlled);
		else
			NetworkPhysicsComponent->SetIsRelayingLocalInputs(false);
		return;
	}

	// 无 NetworkPhysicsComponent 时的回退路径
	bIsLocallyControlled = false;
	if (GetOwner() && GetOwner()->GetLocalRole() != ROLE_SimulatedProxy)
		bIsLocallyControlled = bInLocallyControlled;
}

bool USingularisMorphVehicleSimulationComponent::IsLocallyControlled() const
{
	// 网络物理预测开启时，NetworkPhysicsComponent 是唯一权威来源
	if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent)
		return NetworkPhysicsComponent->IsLocallyControlled();

	if (bIsLocallyControlled)
		return true;

	// 回退：通过 Owner Pawn 的控制器判断
	if (const AActor* Owner = GetOwner())
	{
		if (const APawn* Pawn = Cast<APawn>(Owner))
		{
			if (const APlayerController* PlayerController = Cast<APlayerController>(Pawn->GetController()))
				return PlayerController->IsLocalController();
		}
	}
	return false;
}

float USingularisMorphVehicleSimulationComponent::GetVehicleSpeed() const
{
	const AActor* Owner = GetOwner();
	if (!Owner) return 0.0f;

	// cm/s → km/h
	return FMath::Abs(Owner->GetVelocity().Size() * 0.036f);
}

void USingularisMorphVehicleSimulationComponent::CreateVehicleSimulation()
{
	// 1) 前置守卫
	const UWorld* World = GetWorld();
	if (!IsValid(World) || !World->IsGameWorld()) return;

	// 2) 创建物理线程端载具模拟
	VehicleSimulationPT = MakeUnique<FSingularisMorphVehicleSimulation>(
		bUsingNetworkPhysicsPrediction,
		static_cast<int8>(World->GetNetMode())
	);

	// 3) 创建物理输出容器
	VehiclePhysicsOutput = MakeUnique<FSingularisMorphVehiclePhysicsOutput>();

	// 4) 创建模拟模块树（专用服务器不产生动画数据）
	SimulationModuleTree = MakeUnique<Chaos::FSimModuleTree>();
	SimulationModuleTree->SetAnimationEnabled(World->GetNetMode() != NM_DedicatedServer);
	SimulationModuleTree->SetSimTreeProcessingOrder(SimulationTreeProcessingOrder);

	// 5) 物理线程接管模块树所有权
	VehicleSimulationPT->Initialize(SimulationModuleTree);

	// 6) 注册到仿真管理器
	USingularisMorphVehicleSchedulerSubsystem* SchedulerSubsystem =
		World->GetSubsystem<USingularisMorphVehicleSchedulerSubsystem>();
	if (!IsValid(SchedulerSubsystem)) return;
	SchedulerSubsystem->RegisterVehicleComponent(this);
}

void USingularisMorphVehicleSimulationComponent::DestroyVehicleSimulation()
{
	// 1) 从仿真管理器注销
	if (UWorld* World = GetWorld())
	{
		if (USingularisMorphVehicleSchedulerSubsystem* SchedulerSubsystem =
			World->GetSubsystem<USingularisMorphVehicleSchedulerSubsystem>())
			SchedulerSubsystem->UnregisterVehicleComponent(this);
	}

	// 2) 未提交到物理线程的模块无其他所有者，在此终止并释放；
	//    模块对象在终止回调中释放悬挂约束等外部资源，必须先于删除调用
	for (const Chaos::FPendingModuleAdds& Pending : StoredTreeUpdates.GetNewModules())
	{
		Chaos::ISimulationModuleBase* Module = Pending.NewSimModule;
		if (!Module) continue;

		// 已被 RemoveSimulationModule 终止过的模块不重复终止：终止回调会释放悬挂约束等
		// 外部资源，二次调用会对同一句柄重复释放。判据是映射表是否仍持有该模块；
		// 它已不在模拟树中，只需在此释放对象
		if (const Chaos::ISimulationModuleBase* const* Live = GuidToCoreModule.Find(Module->GetGuid());
			!Live || *Live != Module)
		{
			delete Module;
			continue;
		}

		Module->SetStateFlags(Chaos::eSimModuleState::Disabled);
		Module->OnTermination_External();

		GuidToCoreModule.Remove(Module->GetGuid());

		delete Module;
	}

	// 3) 已提交给物理线程的模块对象归模拟树所有，此处只通知终止
	for (const auto& Pair : GuidToCoreModule)
	{
		if (Chaos::ISimulationModuleBase* Module = Pair.Value)
		{
			Module->SetStateFlags(Chaos::eSimModuleState::Disabled);
			Module->OnTermination_External();
		}
	}

	// 4) 清理游戏线程侧缓存
	GuidToCoreModule.Empty();
	ComponentToPhysicsObjects.Empty();
	PhysicsGuidToComponent.Empty();
	ModuleAnimationSetups.Empty();
	StoredTreeUpdates = Chaos::FSimTreeUpdates();

	// 5) 物理线程可能正在执行在飞帧的 Simulate，
	//    将物理线程端对象与模块树的销毁推迟到求解器命令队列执行，避免跨线程释放
	if (VehicleSimulationPT)
	{
		IPhysicsProxyBase* Proxy = GetPhysicsProxy();
		Chaos::FPBDRigidsSolver* Solver = Proxy ? Proxy->GetSolver<Chaos::FPBDRigidsSolver>() : nullptr;

		// 销毁物理状态阶段代理可能已释放，回退到物理场景的求解器：
		// 求解器不可用时会在游戏线程直接释放物理线程对象，必须尽量避免
		if (!Solver)
		{
			if (UWorld* CurrentWorld = GetWorld())
			{
				if (auto* PhysScene = CurrentWorld->GetPhysicsScene())
					Solver = PhysScene->GetSolver();
			}
		}

		if (Solver)
		{
			Solver->EnqueueCommandImmediate(
				[Pointer = MoveTemp(VehicleSimulationPT)]() mutable
				{
					Pointer.Reset();
				}
			);
		}
		else
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT("[DestroyVehicleSimulation] Solver unavailable, destroying simulation on game thread (%s)"),
				*GetNameSafe(GetOwner())
			);
		}
	}

	// 6) 清理剩余资源。根物理对象句柄指向已销毁的物理状态，必须一并清空：
	//    重建路径仅在 RootPhysicsObject 为空时重新绑定，
	//    残留句柄会使新一轮模块全部绑定到已销毁的物理对象（悬挂约束失效）
	VehicleSimulationPT.Reset();
	SimulationModuleTree.Reset();
	VehiclePhysicsOutput.Reset();
	RootPhysicsObject = nullptr;
	CachedPhysicsProxy = nullptr;
}

void USingularisMorphVehicleSimulationComponent::ClearAllSimulationModules()
{
	// 1) 逐个移除旧模块（内部会释放悬挂约束等外部资源并广播移除事件）。
	//    以 GuidToCoreModule 为唯一来源：手动注册的模块不会进入组件映射，
	//    只按组件映射清理会遗漏这些模块
	TArray<int32> ExistingGuids;
	ExistingGuids.Reserve(GuidToCoreModule.Num());
	for (const auto& Pair : GuidToCoreModule)
		ExistingGuids.Add(Pair.Key);
	for (int32 Guid : ExistingGuids)
		RemoveSimulationModule(Guid);

	// 2) 清空所有缓存，保证幂等
	ComponentToPhysicsObjects.Empty();
	PhysicsGuidToComponent.Empty();
	ModuleAnimationSetups.Empty();
	NextConstructionIndex = 0;

	// 3) 提交删除到物理线程
	FinalizeModuleUpdates();
}

IPhysicsProxyBase* USingularisMorphVehicleSimulationComponent::GetPhysicsProxy() const
{
	// 1) 优先从适配器获取
	if (PhysicsAdapter)
	{
		IPhysicsProxyBase* Proxy = PhysicsAdapter->GetPhysicsProxy();
		if (Proxy)
		{
			CachedPhysicsProxy = Proxy;
			return Proxy;
		}
	}

	// 2) 使用缓存值
	if (CachedPhysicsProxy) return CachedPhysicsProxy;

	// 3) 回退到 Owner 根组件的物理体实例：
	//    未配置适配器（手动注册模块）或适配器尚未解析出代理时，
	//    以根组件的物理体作为载具物理来源
	if (const AActor* Owner = GetOwner())
	{
		if (const UPrimitiveComponent* RootComponent = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
		{
			if (const FBodyInstance* BodyInstance = RootComponent->GetBodyInstance())
			{
				CachedPhysicsProxy = BodyInstance->GetPhysicsActor();
				return CachedPhysicsProxy;
			}
		}
	}

	return nullptr;
}

int32 USingularisMorphVehicleSimulationComponent::GenerateNewGuid()
{
	static std::atomic<int32> Counter{0};
	return Counter.fetch_add(1);
}

void USingularisMorphVehicleSimulationComponent::AssimilateComponentInputs(
	TArray<FModuleInputSetup>& OutCombinedInputs
)
{
	// 1) 组件级控制输入配置作为基准
	OutCombinedInputs = InputConfig;

	auto AddConfigUnique = [&OutCombinedInputs](const FModuleInputSetup& Config)
	{
		if (!OutCombinedInputs.ContainsByPredicate(
			[&Config](const FModuleInputSetup& Existing)
			{
				return Existing.Name == Config.Name;
			}
		))
			OutCombinedInputs.Add(Config);
	};

	// 2) 聚合 Owner 上全部 SU 组件的输入配置，按名称去重
	//    （多个车轮可能共享同一转向输入）
	if (AActor* Owner = GetOwner())
	{
		TArray<UActorComponent*> Components;
		Owner->GetComponents<UActorComponent>(Components);

		for (UActorComponent* Component : Components)
		{
			auto* SUInterface = Cast<ISingularisMorphVehicleSUInterface>(Component);
			if (!SUInterface) continue;

			for (const FModuleInputSetup& Config : SUInterface->GetInputConfig())
				AddConfigUnique(Config);
		}
	}

	// 3) 换挡脉冲必须"消费即清零"：一次游戏帧内物理线程会执行多个子步，
	//    不清零会让变速箱在每个子步各换一挡，按一次换挡实际跳多挡
	for (FModuleInputSetup& Setup : OutCombinedInputs)
	{
		if (Setup.Name == Chaos::ChangeUpControlName || Setup.Name == Chaos::ChangeDownControlName)
			Setup.bClearAfterConsumed = true;
	}
}

void USingularisMorphVehicleSimulationComponent::SetupInputConfiguration(
	const bool bForceReinitialize
)
{
	// 1) 聚合全部 SU 组件的输入配置
	TArray<FModuleInputSetup> NewCombinedConfig;
	AssimilateComponentInputs(NewCombinedConfig);

	// 2) 配置未变更且生产者就绪时跳过重建（运行时模块增删会反复进入此函数）
	if (!bForceReinitialize && NewCombinedConfig == CombinedInputConfiguration && InputProducer)
		return;

	CombinedInputConfiguration = MoveTemp(NewCombinedConfig);

	// 3) 实例化输入生产者
	if (!InputProducer && InputProducerClass)
		InputProducer = NewObject<UVehicleInputProducerBase>(this, InputProducerClass);

	if (InputProducer)
	{
		InputProducer->InitializeContainer(CombinedInputConfiguration, InputNameMap, InputQuantizationType);

		// 测试输入缓冲（Playback/Random 生产者）直接注入物理线程
		if (InputProducer->GetTestInputBuffer() && VehicleSimulationPT)
		{
			VehicleSimulationPT->SetTestInputBuffer(
				*InputProducer->GetTestInputBuffer(),
				InputProducer->IsLoopingTestInputBuffer(),
				InputProducer->GetTestInputStartFrame()
			);
		}
	}

	// 4) 初始化控制/状态输入容器
	InputsContainer.Initialize(CombinedInputConfiguration, InputNameMap);
	StateInputContainer.Initialize(StateInputConfiguration, StateNameMap);

	// 5) 同步物理线程端输入配置
	if (VehicleSimulationPT)
	{
		VehicleSimulationPT->SetInputQuantizationType(InputQuantizationType);
		VehicleSimulationPT->SetInputMappings(InputNameMap);
		VehicleSimulationPT->SetStateMappings(StateNameMap);
	}
}

void USingularisMorphVehicleSimulationComponent::UpdateNonSkeletalAnimations()
{
	for (const FSingularisMorphModuleAnimationSetup& AnimSetup : ModuleAnimationSetups)
	{
		const int32 ModuleGuid = AnimSetup.ModuleGUID;
		if (ModuleGuid == INDEX_NONE) continue;

		// 定位模块对应的可视化组件
		USceneComponent* ComponentToAnimate = nullptr;
		for (const auto& Pair : ComponentToPhysicsObjects)
		{
			if (Pair.Value.Guid == ModuleGuid)
			{
				ComponentToAnimate = Pair.Value.ProxyComponentToAnimate;
				break;
			}
		}
		if (!ComponentToAnimate || !ComponentToAnimate->IsValidLowLevel()) continue;

		FTransform NewRelativeTransform = ComponentToAnimate->GetRelativeTransform();

		if (AnimSetup.AnimFlags & Chaos::EAnimationFlags::AnimateRotation)
		{
			NewRelativeTransform.SetRotation(
				AnimSetup.InitialRotOffset * AnimSetup.CombinedRotation
			);
		}
		if (AnimSetup.AnimFlags & Chaos::EAnimationFlags::AnimatePosition)
		{
			NewRelativeTransform.SetLocation(
				AnimSetup.InitialLocOffset + AnimSetup.LocOffset
			);
		}

		ComponentToAnimate->SetRelativeTransform(
			NewRelativeTransform,
			false,
			nullptr,
			ETeleportType::TeleportPhysics
		);
	}
}

Chaos::FSimOutputData* USingularisMorphVehicleSimulationComponent::FindModuleOutputFromGuid(
	const FSingularisMorphVehiclePhysicsOutput& OutputContainer,
	const int32 Guid
) const
{
	for (Chaos::FSimOutputData* Data : OutputContainer.SimTreeOutputData)
	{
		if (Data && Data->ModuleGuid == Guid)
			return Data;
	}

	return nullptr;
}

void USingularisMorphVehicleSimulationComponent::BroadcastModuleAddedEvent(
	const FName& ModuleType,
	const int32 Guid,
	const int32 TreeIndex
)
{
	OnSimulationModuleAddedNativeEvent.Broadcast(ModuleType, Guid, TreeIndex);
	OnSimulationModuleAddedEvent.Broadcast(ModuleType, Guid, TreeIndex);
}

void USingularisMorphVehicleSimulationComponent::BroadcastModuleRemovedEvent(
	const FName& ModuleType,
	const int32 Guid,
	const int32 TreeIndex
)
{
	OnSimulationModuleRemovedNativeEvent.Broadcast(ModuleType, Guid, TreeIndex);
	OnSimulationModuleRemovedEvent.Broadcast(ModuleType, Guid, TreeIndex);
}

int32 USingularisMorphVehicleSimulationComponent::AddModuleToTree(
	Chaos::ISimulationModuleBase* CoreModule,
	const FTransform& ComponentTransform,
	const int32 ParentIndex,
	const int32 TransformIndex,
	const Chaos::FUniqueIdx ParticleIndex,
	const FTransform& PhysicalTransform,
	const FName& BoneName,
	const FVector& AnimationOffset
)
{
	if (!CoreModule) return INDEX_NONE;

	// 1) 从 PT 端获取模拟树引用（Initialize 后树所有权已转移）
	Chaos::FSimModuleTree* SimTree = VehicleSimulationPT
		                                 ? VehicleSimulationPT->AccessSimComponentTree().Get()
		                                 : SimulationModuleTree.Get();

	if (!SimTree) return INDEX_NONE;

	// 2) 设置模块到模拟树
	CoreModule->SetSimModuleTree(SimTree);

	// 3) 在树更新中注册节点
	const int32 TreeIndex = StoredTreeUpdates.AddNodeBelow(ParentIndex, CoreModule);
	if (TreeIndex == INDEX_NONE) return INDEX_NONE;

	// 4) 配置模块属性
	CoreModule->SetGuid(GenerateNewGuid());
	CoreModule->SetTransformIndex(TransformIndex);
	CoreModule->SetParticleIndex(ParticleIndex);

	// 5) 设置物理变换（来自集群子粒子的 ChildToParent，不含编辑器偏移）
	const FTransform PhysTransform = PhysicalTransform.Equals(FTransform::Identity)
		                                 ? ComponentTransform
		                                 : PhysicalTransform;

	FTransform InitialTransform = PhysTransform;

	CoreModule->SetIntactTransform(FTransform::Identity);
	CoreModule->SetClusteredTransform(InitialTransform);
	CoreModule->SetClustered(true);
	CoreModule->SetInitialParticleTransform(InitialTransform);
	CoreModule->SetComponentTransform(ComponentTransform);

	// 覆盖 ClusteredTransform 为仅含位移的版本（与旧代码行为一致）
	FTransform ClusteredTransform(FQuat::Identity, InitialTransform.GetLocation());
	CoreModule->SetClusteredTransform(ClusteredTransform);

	// 6a) 动画绑定：注册动画槽位，供 PostUpdate 更新渲染变换
	RegisterModuleAnimationSetup(CoreModule, BoneName, AnimationOffset, InitialTransform);

	// 6b) 通知模块物理对象已就绪（悬挂在此创建约束）。
	//    手动注册路径没有重建流程，模块首次入队时根物理对象可能尚未绑定，
	//    缺绑会让 OnConstruction_External 不执行，悬挂约束不会创建
	if (RootPhysicsObject == nullptr)
	{
		if (IPhysicsProxyBase* Proxy = GetPhysicsProxy())
			CacheRootPhysicsObject(Proxy);
	}

	if (RootPhysicsObject)
		CoreModule->OnConstruction_External(RootPhysicsObject);

	// 7) 记录模块指针供游戏线程侧访问（终止回调等），避免读取物理线程拥有的模拟树
	GuidToCoreModule.Add(CoreModule->GetGuid(), CoreModule);

	return TreeIndex;
}

int32 USingularisMorphVehicleSimulationComponent::RegisterModuleAnimationSetup(
	Chaos::ISimulationModuleBase* CoreModule,
	const FName& BoneName,
	const FVector& AnimationOffset,
	const FTransform& InitialTransform
)
{
	if (!CoreModule) return INDEX_NONE;

	// 1) 未启用动画的模块不占用槽位（模拟树同样不会调用其 Animate）
	if (!CoreModule->IsAnimationEnabled()) return INDEX_NONE;

	// 2) 骨骼名非空时复用同一骨骼的既有槽位：
	//    多个模块共用一根骨骼（如车轮与悬挂）时，重复槽位会让骨骼动画对同一骨骼
	//    施加多次变换；骨骼名为空表示非骨骼动画，各模块独占槽位以对应各自的驱动组件
	int32 SetupIndex = INDEX_NONE;
	if (BoneName != NAME_None)
	{
		for (auto I = 0; I < ModuleAnimationSetups.Num(); ++I)
		{
			if (ModuleAnimationSetups[I].BoneName == BoneName)
			{
				SetupIndex = I;
				break;
			}
		}
	}

	// 3) 新建槽位记录初始姿态，供非骨骼动画按增量叠加
	if (SetupIndex == INDEX_NONE)
	{
		SetupIndex = ModuleAnimationSetups.Num();

		FSingularisMorphModuleAnimationSetup AnimSetup(
			BoneName,
			CoreModule->GetTransformIndex(),
			CoreModule->GetGuid()
		);
		AnimSetup.InitialRotOffset = InitialTransform.GetRotation();
		AnimSetup.InitialLocOffset = InitialTransform.GetTranslation();
		ModuleAnimationSetups.Add(AnimSetup);
	}

	CoreModule->SetAnimationData(BoneName, AnimationOffset, SetupIndex);

	return SetupIndex;
}

void USingularisMorphVehicleSimulationComponent::OnSimulationModuleInitialized(
	const FName& ModuleType,
	const int32 Guid,
	const int32 TreeIndex
)
{
	// 分发生命周期到 SU 组件（广播由 BroadcastModuleAddedEvent 统一执行）
	if (const TWeakObjectPtr<UActorComponent>* Component = PhysicsGuidToComponent.Find(Guid))
	{
		if (auto* SUInterface = Cast<ISingularisMorphVehicleSUInterface>(Component->Get()))
		{
			// TreeIndex 来自物理线程应用树更新后的真实索引
			SUInterface->SetTreeIndex(TreeIndex);
			SUInterface->OnAdded();
		}
	}
}

void USingularisMorphVehicleSimulationComponent::OnSimulationModuleRemovedCallback(
	const FName& ModuleType,
	const int32 Guid,
	const int32 TreeIndex
)
{
	// 分发生命周期到 SU 组件（广播由 BroadcastModuleRemovedEvent 统一执行）
	if (const TWeakObjectPtr<UActorComponent>* Component = PhysicsGuidToComponent.Find(Guid))
	{
		if (auto* SUInterface = Cast<ISingularisMorphVehicleSUInterface>(Component->Get()))
			SUInterface->OnRemoved();
	}
}
