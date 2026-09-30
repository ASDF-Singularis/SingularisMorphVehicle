#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include <atomic>
#include <initializer_list>
#include <Components/PrimitiveComponent.h>
#include <Engine/Canvas.h>
#include <Engine/Engine.h>
#include <Engine/Font.h>
#include <Engine/World.h>
#include <GameFramework/WorldSettings.h>
#include <Misc/ScopeLock.h>
#include <Physics/Experimental/PhysScene_Chaos.h>
#include <PhysicsEngine/BodyInstance.h>
#include <PhysicsEngine/PhysicsObjectExternalInterface.h>
#include <PhysicsProxy/ClusterUnionPhysicsProxy.h>
#include <PhysicsProxy/SingleParticlePhysicsProxy.h>
#include <SimModule/SimModulesInclude.h>
#include <SimModule/SimulationModuleBase.h>

#include "SingularisMorphVehicle.h"
#include "Components/SingularisMorphVehicleSUComponent.h"
#include "Components/SingularisTransmissionSUComponent.h"
#include "Components/SingularisUprightSUComponent.h"
#include "Core/SingularisMorphVehicleSimulationCU.h"
#include "Interfaces/SingularisMorphVehicleSUInterface.h"
#include "Objects/SingularisMorphVehiclePhysicsAdapter.h"
#include "Subsystems/SingularisMorphVehicleMappingSubsystem.h"
#include "Subsystems/SingularisMorphVehicleSchedulerSubsystem.h"
#include "Types/SingularisMorphVehicleInputProducer.h"
#include "Types/SingularisMorphVehicleInputUtils.h"

namespace
{
	/**
	 * 输入配置等价判定：名称、类型与输入语义标志全部一致时才视为未变更。
	 *
	 * FModuleInputSetup 的 operator== 仅比较名称，按该判据跳过重建会让类型与
	 * 标志（衰减/消费即清零）的变更静默失效。
	 */
	bool IsInputConfigurationEqual(
		const TArray<FModuleInputSetup>& Left,
		const TArray<FModuleInputSetup>& Right
	)
	{
		if (Left.Num() != Right.Num()) return false;

		for (auto I = 0; I < Left.Num(); ++I)
		{
			const FModuleInputSetup& LeftSetup = Left[I];
			const FModuleInputSetup& RightSetup = Right[I];

			if (LeftSetup.Name != RightSetup.Name ||
				LeftSetup.Type != RightSetup.Type ||
				LeftSetup.bApplyInputDecay != RightSetup.bApplyInputDecay ||
				LeftSetup.bInverseInputDecay != RightSetup.bInverseInputDecay ||
				LeftSetup.bClearAfterConsumed != RightSetup.bClearAfterConsumed)
				return false;
		}

		return true;
	}
}

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

	// 6) 若需禁止休眠则设置装配根粒子为永不睡眠
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

	// 5) 复位跨世的缓存状态：物理状态重建后新建的变速箱从默认挡位起步，
	//    沿用上一世代的缓存会让首个 SetGearInput 产生错误方向的脉冲
	CachedPhysicsProxy = nullptr;
	CurrentGear = 0;
	bHasGearData = false;
	TargetGearInput = INDEX_NONE;
	EngineRPM = 0.0f;
	EngineTorque = 0.0f;
	bIsLocallyControlled = false;
	bPendingReplicationStructureRebuild = false;
	CurrentAsyncDataType = AsyncInvalid;
	CurrentAsyncInput = nullptr;
	CurrentAsyncOutput = nullptr;
	NextAsyncOutput = nullptr;
	OutputInterpolationAlpha = 0.0f;

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
	//    用于释放模块在物理对象上建立的外部资源。FSimModuleTree::DeleteNode 只 delete
	//    模块对象，不会调用 OnTermination_External，若不在此释放，每次重建都会泄漏一份外部资源。
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
		// 模块的外部资源会泄漏。模块入树必须经 AddModuleToTree 登记
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
	//    其余槽位按删除情况整体前移，保持槽位下标与数组下标一致。
	//    槽位表由动画线程共享，重构期间持锁
	{
		FScopeLock AnimationLock(&ModuleAnimationSetupsLock);

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

				// 清空接管前的动画残值：旧模块的标志与偏移会在接管者产出首帧数据前
				// 被当作当帧动画应用，表现为部件被钉在旧位姿
				ModuleAnimationSetups[I].AnimFlags = 0;
				ModuleAnimationSetups[I].RotOffset = FRotator::ZeroRotator;
				ModuleAnimationSetups[I].LocOffset = FVector::ZeroVector;
				ModuleAnimationSetups[I].CombinedRotation = FQuat::Identity;
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
		Display,
		TEXT("[FinalizeModuleUpdates] VehicleSimulationPT=%s, StoredTreeUpdates pending adds=%d"),
		VehicleSimulationPT.IsValid() ? TEXT("valid") : TEXT("null"),
		StoredTreeUpdates.GetNewModules().Num()
	);

	if (!StoredTreeUpdates.GetNewModules().IsEmpty() || !StoredTreeUpdates.GetDeletedModules().IsEmpty())
	{
		if (VehicleSimulationPT)
			VehicleSimulationPT->AppendTreeUpdates(&StoredTreeUpdates);

		// 新增模块会使网络状态历史的模块布局失效，标记待重建
		if (!StoredTreeUpdates.GetNewModules().IsEmpty())
			bPendingReplicationStructureRebuild = true;

		StoredTreeUpdates = Chaos::FSimTreeUpdates();
	}

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
	// 1) 前置守卫（快照已在调用方 PreTickGT 校验，此处兜底）
	if (!VehicleSimulationPT) return;

	const UWorld* World = GetWorld();
	const USingularisMorphVehicleMappingSubsystem* Subsystem =
		World ? World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>() : nullptr;
	if (!Subsystem) return;

	AActor* Owner = GetOwner();
	if (!IsValid(Owner)) return;

	// 2) 空快照：无物理实体可建。此前已构建过模块（载具完全解体）时清除全部模拟模块，
	//    否则无操作，保证幂等（物理装配尚未就绪时反复调用不产生副作用）。
	if (Snapshot.Entities.IsEmpty())
	{
		if (GuidToCoreModule.IsEmpty()) return;

		// 清除全部模拟模块（载具完全解体）
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

		// 模块按物理组件映射（一组件一模块），一个组件贡献多个粒子时无法把它们分别绑定到模块：
		// 多粒子物理来源需要专用适配器在自身侧拆分（如骨骼网格体适配器，骨骼↔模块的对应
		// 关系由适配器自身携带），本路径不支持该用法，命中即告警
		if (EntityMap.Contains(Entity.PrimitiveComponent))
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT(
					"[RebuildFromSnapshot] Component %s contributes multiple physics particles - they cannot be bound to separate module components (one module maps to one component); multi-particle sources require a dedicated adapter that splits them"
				),
				*GetNameSafe(Entity.PrimitiveComponent)
			);
		}

		EntityMap.FindOrAdd(Entity.PrimitiveComponent) = &Entity;
	}

	// 4) 统一收集本次参与重建的 SU 组件：
	//    a) 快照实体上通过映射子系统注册的 SU（物理部件）；
	//    b) Owner 上未出现在快照中的 SU（纯仿真模块，如未参与物理装配的引擎/变速箱）。
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

	// 5) 单动力链分拣：底盘/引擎/离合/变速箱/轮轴各取首个实例，多余实例不入树。
	//    有效底盘 = 无驱动组件（纯仿真模块）或驱动组件仍在本次快照中（仍在装配中）：
	//    仅以组件是否存在为判据，已离开装配的底盘件仍会被当作有效底盘，
	//    使整棵树以无效粒子索引重建（力继续施加到装配根粒子）。
	//    原动机槽由首个引擎占据，无引擎时由首个电机顶替（引擎与电机同为扭矩源）。
	USingularisMorphVehicleSUComponent* ChassisSU = nullptr;
	USingularisMorphVehicleSUComponent* PrimeMoverSU = nullptr;
	USingularisMorphVehicleSUComponent* ClutchSU = nullptr;
	USingularisMorphVehicleSUComponent* TransmissionSU = nullptr;
	USingularisMorphVehicleSUComponent* AxleSU = nullptr;

	TArray<USingularisMorphVehicleSUComponent*> Wheels;
	TArray<USingularisMorphVehicleSUComponent*> Suspensions;
	TArray<USingularisMorphVehicleSUComponent*> AuxiliaryModules;
	TArray<USingularisMorphVehicleSUComponent*> Uprights;

	auto NumChassis = 0;
	auto NumEngine = 0;
	auto NumClutch = 0;
	auto NumTransmission = 0;
	auto NumAxle = 0;

	for (USingularisMorphVehicleSUComponent* SUComp : AllSUComponents)
	{
		if (!SUComp) continue;

		switch (SUComp->GetModuleType())
		{
		case ESingularisMorphVehicleModuleType::Chassis:
			++NumChassis;
			if (!ChassisSU)
			{
				UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
					SUComp->DrivenComponent.GetComponent(SUComp->GetOwner())
				);
				if (!ProxyComp || EntityMap.Contains(ProxyComp))
					ChassisSU = SUComp;
			}
			break;

		case ESingularisMorphVehicleModuleType::Engine:
			// 引擎优先占据原动机槽，后续引擎实例不入树
			++NumEngine;
			if (!PrimeMoverSU)
				PrimeMoverSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Motor:
			// 无引擎时首个电机顶替原动机槽，其余电机作为辅助模块挂到底盘之下
			if (!PrimeMoverSU)
				PrimeMoverSU = SUComp;
			else
				AuxiliaryModules.Add(SUComp);
			break;

		case ESingularisMorphVehicleModuleType::Clutch:
			++NumClutch;
			if (!ClutchSU)
				ClutchSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Transmission:
			++NumTransmission;
			if (!TransmissionSU)
				TransmissionSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Axle:
			++NumAxle;
			if (!AxleSU)
				AxleSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Wheel:
			Wheels.Add(SUComp);
			break;

		case ESingularisMorphVehicleModuleType::Suspension:
			Suspensions.Add(SUComp);
			break;

		case ESingularisMorphVehicleModuleType::Upright:
			// 立轴为纯配置载体，不创建模块不入树，仅在槽位匹配阶段参与
			Uprights.Add(SUComp);
			break;

		default:
			AuxiliaryModules.Add(SUComp);
			break;
		}
	}

	// 6) 底盘守卫：无有效底盘（车身件脱离物理装配 = 载具解体）时清除全部模拟模块
	if (!ChassisSU)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT(
				"[RebuildFromSnapshot] No chassis present in snapshot (%d entities, %d chassis components on the actor) - clearing all simulation modules"
			),
			Snapshot.Entities.Num(),
			NumChassis
		);

		ClearAllSimulationModules();
		return;
	}

	// 7) 单槽位重复实例告警：多余实例不入树，仅提示配置冗余
	if (NumChassis > 1 || NumEngine > 1 || NumClutch > 1 || NumTransmission > 1 || NumAxle > 1)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT(
				"[RebuildFromSnapshot] Duplicate singleton module instances - only the first of each type enters the tree (chassis=%d, engine=%d, clutch=%d, transmission=%d, axle=%d)"
			),
			NumChassis,
			NumEngine,
			NumClutch,
			NumTransmission,
			NumAxle
		);
	}

	// 8) 轮位槽邻近匹配：轮胎 ↔ 立轴一对一配对。
	//    立轴的驱动组件为车架上的槽位标记场景组件，其世界位置即槽位位置；
	//    轮胎驱动组件落在某槽位匹配半径内即挂载该槽位（多候选取最近者，
	//    每个槽位至多消费一个轮胎）。匹配结果决定车轮的树内位置与槽位配置注入；
	//    未匹配的轮胎为自由轮：挂底盘之下，无驱动、无转向，仅提供滚动。
	//    相对几何随车体整体运动保持不变，匹配仅在拓扑变更时重算即可保持稳定
	TMap<USingularisMorphVehicleSUComponent*, USingularisUprightSUComponent*> WheelToUpright;
	{
		struct FUprightSlot
		{
			USingularisUprightSUComponent* UprightSU = nullptr;
			FVector SlotLocation = FVector::ZeroVector;
		};
		TArray<FUprightSlot> Slots;
		Slots.Reserve(Uprights.Num());

		for (USingularisMorphVehicleSUComponent* SUComp : Uprights)
		{
			auto* UprightSU = Cast<USingularisUprightSUComponent>(SUComp);
			const USceneComponent* SlotMarker = UprightSU
				                                    ? Cast<USceneComponent>(
					                                    UprightSU->DrivenComponent.GetComponent(UprightSU->GetOwner())
				                                    )
				                                    : nullptr;
			if (!UprightSU || !SlotMarker)
			{
				UE_LOG(
					LogSingularisMorphVehicle,
					Warning,
					TEXT(
						"[RebuildFromSnapshot] Upright %s has no slot marker (set its DrivenComponent to a scene component on the frame) - it cannot match any wheel"
					),
					*GetNameSafe(SUComp)
				);
				continue;
			}

			Slots.Add({UprightSU, SlotMarker->GetComponentTransform().GetLocation()});
		}

		TSet<USingularisMorphVehicleSUComponent*> ConsumedUprights;
		for (USingularisMorphVehicleSUComponent* SUComp : Wheels)
		{
			// 无驱动组件的纯仿真车轮无法定位，保持自由轮
			const UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
				SUComp->DrivenComponent.GetComponent(SUComp->GetOwner())
			);
			if (!ProxyComp) continue;

			const FVector WheelLocation = ProxyComp->GetComponentTransform().GetLocation();

			// 未消费槽位中距离最近且落在其匹配半径内者
			int BestIndex = INDEX_NONE;
			auto BestDistanceSq = TNumericLimits<float>::Max();
			for (auto SlotIdx = 0; SlotIdx < Slots.Num(); ++SlotIdx)
			{
				if (ConsumedUprights.Contains(Slots[SlotIdx].UprightSU)) continue;

				const float Radius = FMath::Max(Slots[SlotIdx].UprightSU->MatchRadius, 0.0f);
				const float DistanceSq = FVector::DistSquared(WheelLocation, Slots[SlotIdx].SlotLocation);
				if (DistanceSq <= Radius * Radius && DistanceSq < BestDistanceSq)
				{
					BestIndex = SlotIdx;
					BestDistanceSq = DistanceSq;
				}
			}

			if (BestIndex != INDEX_NONE)
			{
				WheelToUpright.Add(SUComp, Slots[BestIndex].UprightSU);
				ConsumedUprights.Add(Slots[BestIndex].UprightSU);
				continue;
			}

			// 无可用槽位：若存在半径内但已被消费的槽位，说明同槽位堆叠了多个轮胎
			for (const FUprightSlot& Slot : Slots)
			{
				if (!ConsumedUprights.Contains(Slot.UprightSU)) continue;

				const float Radius = FMath::Max(Slot.UprightSU->MatchRadius, 0.0f);
				if (FVector::DistSquared(WheelLocation, Slot.SlotLocation) <= Radius * Radius)
				{
					UE_LOG(
						LogSingularisMorphVehicle,
						Warning,
						TEXT(
							"[RebuildFromSnapshot] Wheel %s is within the match radius of the already occupied upright %s - only one wheel per upright, this wheel becomes a free roller"
						),
						*SUComp->GetName(),
						*GetNameSafe(Slot.UprightSU)
					);
					break;
				}
			}
		}
	}

	// 9) 装配计划：按链序展开全部计划模块及其目标父节点（父节点先于子节点）。
	//    计划是差量比对与增量构建的唯一事实来源；父节点以 SU 指针表达，
	//    具体树索引在构建期解析（现存父节点用其绝对索引，本次新建的父节点用批次局部索引）
	enum class EPlanStatus : uint8
	{
		Keep, // 保留：模块与粒子绑定均未变化，不重建
		Add, // 新建：此前无模块
		Replace, // 替换：模块存在但属性变化，先移除再新建
	};

	struct FPlanEntry
	{
		USingularisMorphVehicleSUComponent* SU = nullptr;
		USingularisMorphVehicleSUComponent* ParentSU = nullptr; // nullptr = 根（底盘）
		USingularisUprightSUComponent* MatchedUpright = nullptr; // 车轮：槽位配置注入源
		UPrimitiveComponent* ProxyComp = nullptr; // 驱动组件解析结果
		const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity* Entity = nullptr;
		EPlanStatus Status = EPlanStatus::Add;
	};

	TArray<FPlanEntry> Plan;
	TMap<USingularisMorphVehicleSUComponent*, int32> PlanIndexBySU;
	{
		auto FirstValidSU = [](
			const std::initializer_list<USingularisMorphVehicleSUComponent*> Candidates
		)
			-> USingularisMorphVehicleSUComponent*
		{
			for (USingularisMorphVehicleSUComponent* Candidate : Candidates)
				if (Candidate) return Candidate;
			return nullptr;
		};

		auto AddPlanEntry = [&](
			USingularisMorphVehicleSUComponent* SUComp,
			USingularisMorphVehicleSUComponent* ParentSU,
			USingularisUprightSUComponent* MatchedUpright
		) -> void
		{
			FPlanEntry Entry;
			Entry.SU = SUComp;
			Entry.ParentSU = ParentSU;
			Entry.MatchedUpright = MatchedUpright;
			Entry.ProxyComp = Cast<UPrimitiveComponent>(SUComp->DrivenComponent.GetComponent(SUComp->GetOwner()));
			Entry.Entity = Entry.ProxyComp ? EntityMap.FindRef(Entry.ProxyComp) : nullptr;
			PlanIndexBySU.Add(SUComp, Plan.Num());
			Plan.Add(MoveTemp(Entry));
		};

		// 链序：底盘 → 原动机 → 离合 → 变速箱 → 轮轴 → 车轮 → 悬挂 → 辅助模块
		AddPlanEntry(ChassisSU, nullptr, nullptr);
		if (PrimeMoverSU) AddPlanEntry(PrimeMoverSU, ChassisSU, nullptr);
		if (ClutchSU) AddPlanEntry(ClutchSU, PrimeMoverSU ? PrimeMoverSU : ChassisSU, nullptr);
		if (TransmissionSU)
			AddPlanEntry(TransmissionSU, FirstValidSU({ClutchSU, PrimeMoverSU, ChassisSU}), nullptr);
		if (AxleSU)
			AddPlanEntry(AxleSU, FirstValidSU({TransmissionSU, ClutchSU, PrimeMoverSU, ChassisSU}), nullptr);

		// 车轮 → 挂载槽位的挂动力链最末端，自由轮挂底盘
		USingularisMorphVehicleSUComponent* WheelParentSU = FirstValidSU(
			{AxleSU, TransmissionSU, ClutchSU, PrimeMoverSU, ChassisSU}
		);
		for (USingularisMorphVehicleSUComponent* SUComp : Wheels)
		{
			if (USingularisUprightSUComponent* const* MatchedUpright = WheelToUpright.Find(SUComp))
				AddPlanEntry(SUComp, WheelParentSU, *MatchedUpright);
			else
				AddPlanEntry(SUComp, ChassisSU, nullptr);
		}

		// 悬挂 → 同一驱动组件上的车轮之下；配对失败挂底盘并告警
		TMap<TObjectPtr<UPrimitiveComponent>, USingularisMorphVehicleSUComponent*> WheelSUByComponent;
		for (USingularisMorphVehicleSUComponent* SUComp : Wheels)
		{
			if (UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
				SUComp->DrivenComponent.GetComponent(SUComp->GetOwner())
			))
				WheelSUByComponent.FindOrAdd(ProxyComp) = SUComp;
		}

		for (USingularisMorphVehicleSUComponent* SUComp : Suspensions)
		{
			USingularisMorphVehicleSUComponent* PairedWheelSU = nullptr;
			if (UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
				SUComp->DrivenComponent.GetComponent(SUComp->GetOwner())
			))
				PairedWheelSU = WheelSUByComponent.FindRef(ProxyComp);

			// 配对失败：悬挂无法与车轮建立交叉链接（车轮失去悬挂力），须告警
			if (!PairedWheelSU)
			{
				UE_LOG(
					LogSingularisMorphVehicle,
					Warning,
					TEXT(
						"[RebuildFromSnapshot] Suspension %s has no wheel sharing its physics component - the wheel loses suspension forces; put the Wheel and Suspension units on the same physics part"
					),
					*SUComp->GetName()
				);
			}

			AddPlanEntry(SUComp, PairedWheelSU ? PairedWheelSU : ChassisSU, nullptr);
		}

		for (USingularisMorphVehicleSUComponent* SUComp : AuxiliaryModules)
			AddPlanEntry(SUComp, ChassisSU, nullptr);
	}

	// 10) 差量分派：与现存模块逐一比对。
	//     保留 = 模块存活且驱动组件、物理粒子索引、动画开关均未变化；
	//     新建 = 此前无模块；替换 = 任一属性变化（模块设置与粒子绑定在构建期固化，
	//     无法原位修改，故「修改」表现为「先移除再新建」）；
	//     计划外的现存模块、失效映射键（SU 随部件销毁）与未纳管模块一并移除
	TArray<USingularisMorphVehicleSUComponent*> RemovedSUs;
	TArray<TObjectPtr<UActorComponent>> StaleMapKeys;
	TArray<int32> UnmanagedGuids;
	{
		TSet<USingularisMorphVehicleSUComponent*> PlannedSUs;
		PlannedSUs.Reserve(Plan.Num());
		for (const FPlanEntry& Entry : Plan)
			PlannedSUs.Add(Entry.SU);

		for (const auto& Pair : ComponentToPhysicsObjects)
		{
			auto* SUComp = Cast<USingularisMorphVehicleSUComponent>(Pair.Key.Get());
			if (!IsValid(SUComp))
			{
				// 键已失效（SU 随部件 Actor 销毁）：其模块仍须移除，
				// 键无法经弱引用定位，收录键值稍后手工清除
				StaleMapKeys.Add(Pair.Key);
				continue;
			}

			if (!PlannedSUs.Contains(SUComp))
				RemovedSUs.Add(SUComp);
		}

		for (FPlanEntry& Entry : Plan)
		{
			const FSingularisMorphVehicleComponentData* Existing = ComponentToPhysicsObjects.Find(Entry.SU);
			Chaos::ISimulationModuleBase* const* ExistingModule =
				Existing ? GuidToCoreModule.Find(Existing->Guid) : nullptr;

			if (!Existing || !ExistingModule || !*ExistingModule)
				Entry.Status = EPlanStatus::Add;
			else if (Existing->ProxyComponentToAnimate.Get() != static_cast<USceneComponent*>(Entry.ProxyComp)
				|| (*ExistingModule)->GetParticleIndex().Idx !=
				(Entry.Entity ? Entry.Entity->ParticleIndex : Chaos::FUniqueIdx().Idx)
				|| (*ExistingModule)->IsAnimationEnabled() != Entry.SU->GetAnimationEnabled())
			{
				// 驱动组件/粒子绑定/动画开关变化：替换以重建模块与动画槽位
				Entry.Status = EPlanStatus::Replace;
			}
			else
				Entry.Status = EPlanStatus::Keep;
		}

		// 手动注册路径的模块（无组件映射条目）不在差量范围内；为保持重建作为唯一对账点、
		// 避免父节点更替后留在树中的悬垂挂接，随本次同步一并移除
		TSet<int32> ManagedGuids;
		ManagedGuids.Reserve(ComponentToPhysicsObjects.Num());
		for (const auto& Pair : ComponentToPhysicsObjects)
			ManagedGuids.Add(Pair.Value.Guid);

		for (const auto& Pair : GuidToCoreModule)
		{
			if (!ManagedGuids.Contains(Pair.Key))
				UnmanagedGuids.Add(Pair.Key);
		}
	}

	// 11) 父节点稳定性级联：保留模块的父模块必须同样保留且未被替换，否则级联替换。
	//     计划链序保证父节点的最终状态先于子节点确定，单遍即可完成；
	//     被移除/替换节点的子模块全部替换，不依赖树删除时的隐式重挂（避免悬垂挂接）
	for (FPlanEntry& Entry : Plan)
	{
		if (Entry.Status != EPlanStatus::Keep || !Entry.ParentSU) continue;

		const int32* ParentPlanIdx = PlanIndexBySU.Find(Entry.ParentSU);
		const FSingularisMorphVehicleComponentData* ParentData = ComponentToPhysicsObjects.Find(Entry.ParentSU);
		const FSingularisMorphVehicleComponentData* EntryData = ComponentToPhysicsObjects.Find(Entry.SU);

		const bool bParentKept = ParentPlanIdx && Plan[*ParentPlanIdx].Status == EPlanStatus::Keep;
		const bool bParentUnchanged = ParentData && EntryData && EntryData->ParentGuid == ParentData->Guid;

		if (!bParentKept || !bParentUnchanged)
			Entry.Status = EPlanStatus::Replace;
	}

	// 12) 绝对父索引可用性：新建/替换模块挂到「刚建未应用」的保留父模块时，
	//     其真实树索引尚未经物理线程回传（SU 树索引为空），无法作为批次绝对父节点；
	//     将父降级为替换即可让父子在本批次内以局部索引成链。迭代至不动点（链深有限）
	for (auto bChanged = true; bChanged;)
	{
		bChanged = false;

		for (const FPlanEntry& Entry : Plan)
		{
			if (Entry.Status != EPlanStatus::Add && Entry.Status != EPlanStatus::Replace) continue;
			if (!Entry.ParentSU) continue;

			const int32* ParentPlanIdx = PlanIndexBySU.Find(Entry.ParentSU);
			if (!ParentPlanIdx) continue;

			FPlanEntry& ParentEntry = Plan[*ParentPlanIdx];
			if (ParentEntry.Status == EPlanStatus::Keep && Entry.ParentSU->GetTreeIndex() == INDEX_NONE)
			{
				ParentEntry.Status = EPlanStatus::Replace;
				bChanged = true;
			}
		}

		if (!bChanged) break;

		// 降级为替换的父节点，其保留子节点同样级联替换
		for (FPlanEntry& Entry : Plan)
		{
			if (Entry.Status != EPlanStatus::Keep || !Entry.ParentSU) continue;

			const int32* ParentPlanIdx = PlanIndexBySU.Find(Entry.ParentSU);
			if (ParentPlanIdx && Plan[*ParentPlanIdx].Status != EPlanStatus::Keep)
				Entry.Status = EPlanStatus::Replace;
		}
	}

	// 13) 无差异：全部保留且无移除项时直接返回（冗余的变更信号不产生任何重建工作）
	auto bHasBuilds = false;
	for (const FPlanEntry& Entry : Plan)
	{
		if (Entry.Status != EPlanStatus::Keep)
		{
			bHasBuilds = true;
			break;
		}
	}

	if (!bHasBuilds && RemovedSUs.IsEmpty() && StaleMapKeys.IsEmpty() && UnmanagedGuids.IsEmpty())
		return;

	// 14) 裁剪静止基准缓存：仅保留本次快照仍存活的（驱动组件, 粒子）键。
	//     部件离开物理装配后其粒子不再存在，条目随之失效；部件重新加入装配时粒子已重建，
	//     键不匹配而触发重新捕获，不会沿用陈旧基准
	{
		TSet<FSingularisMorphModuleRestPoseKey> LiveKeys;
		LiveKeys.Reserve(Snapshot.Entities.Num());

		for (const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity& Entity : Snapshot.Entities)
		{
			if (Entity.PrimitiveComponent)
			{
				LiveKeys.Add(
					FSingularisMorphModuleRestPoseKey{Entity.PrimitiveComponent.Get(), Entity.ParticleIndex}
				);
			}
		}

		PruneModuleRestPoses(LiveKeys);
	}

	// 15) 模块集合可能已变化（变形增删部件），刷新输入配置。
	//     配置未变更时该调用为空操作，不会清空输入容器。
	SetupInputConfiguration();

	const FTransform ReferenceTransform = PhysicsAdapter
		                                      ? PhysicsAdapter->GetReferenceTransform()
		                                      : FTransform::Identity;

	// 16) 缓存根物理对象（首次或重建时刷新）
	//     模块构造期需要 RootPhysicsObject 进行 OnConstruction_External 初始化
	if (RootPhysicsObject == nullptr)
		CacheRootPhysicsObject(GetPhysicsProxy());

	// 17) 执行移除：计划外模块、替换项旧模块、失效映射键模块与未纳管模块。
	//     逐个移除（终止回调释放外部资源、动画槽位让渡、映射清理）；
	//     既有节点索引不因删除改变（槽位仅入空闲表复用），后续新增的绝对父索引仍有效
	TArray<int32> GuidsToRemove;
	GuidsToRemove.Reserve(RemovedSUs.Num() + Plan.Num() + UnmanagedGuids.Num());
	for (USingularisMorphVehicleSUComponent* SUComp : RemovedSUs)
	{
		if (const FSingularisMorphVehicleComponentData* Existing = ComponentToPhysicsObjects.Find(SUComp))
			GuidsToRemove.AddUnique(Existing->Guid);
	}
	for (const FPlanEntry& Entry : Plan)
	{
		if (Entry.Status != EPlanStatus::Replace) continue;
		if (const FSingularisMorphVehicleComponentData* Existing = ComponentToPhysicsObjects.Find(Entry.SU))
			GuidsToRemove.AddUnique(Existing->Guid);
	}
	for (const TObjectPtr<UActorComponent>& StaleKey : StaleMapKeys)
	{
		if (const FSingularisMorphVehicleComponentData* Existing = ComponentToPhysicsObjects.Find(StaleKey))
			GuidsToRemove.AddUnique(Existing->Guid);
	}
	GuidsToRemove.Append(UnmanagedGuids);

	UE_LOG(
		LogSingularisMorphVehicle,
		Display,
		TEXT("[RebuildFromSnapshot] Incremental sync: %d planned modules, %d removals"),
		Plan.Num(),
		GuidsToRemove.Num()
	);

	for (int32 Guid : GuidsToRemove)
		RemoveSimulationModule(Guid);

	// 失效映射键的条目无法经弱引用定位（模块已在上方移除），在此以键值手工清除
	for (const TObjectPtr<UActorComponent>& StaleKey : StaleMapKeys)
		ComponentToPhysicsObjects.Remove(StaleKey);

	// 18) 注册 Lambda：构建并登记单个计划条目对应的模块。
	//     驱动组件/实体/槽位配置源已在计划阶段解析；父树索引由批处理阶段解析并传入
	auto AddEntity = [&](const FPlanEntry& Entry, const int32 ParentIndex) -> int32
	{
		USingularisMorphVehicleSUComponent* SUComp = Entry.SU;
		if (!SUComp) return INDEX_NONE;

		Chaos::ISimulationModuleBase* CoreModule = SUComp->CreateNewCoreModule();
		if (!CoreModule) return INDEX_NONE;

		// SU 的动画开关是模块动画的总开关：部分模块类型（引擎、变速箱等）
		// 不自带动画而不会在 CreateNewCoreModule 中同步该标志，此处统一同步
		CoreModule->SetAnimationEnabled(SUComp->GetAnimationEnabled());

		// 挂载立轴槽位的车轮：注入槽位配置（转向/轴向/反转）。
		// 轴向与反转由核心物理路径从模块设置读取，注入必须在入树前一次性完成
		if (Entry.MatchedUpright)
			SUComp->ApplySlotConfig(CoreModule, Entry.MatchedUpright);

		FTransform ComponentTransform = FTransform::Identity;
		if (Entry.ProxyComp)
			ComponentTransform = Entry.ProxyComp->GetComponentTransform().GetRelativeTransform(ReferenceTransform);
		else if (const USceneComponent* RootComp = Owner->GetRootComponent())
			ComponentTransform = RootComp->GetComponentTransform().GetRelativeTransform(ReferenceTransform);

		// 静止基准：物理部件首次注册时从快照捕获，此后重建复用。
		// 快照的 ChildToParent 携带引擎写回的动画位移，直接重读会使基准逐次漂移
		FTransform RestTransform = FTransform::Identity;
		if (Entry.Entity)
		{
			const FSingularisMorphModuleRestPose& RestPose = AcquireModuleRestPose(
				Entry.ProxyComp,
				Entry.Entity->ParticleIndex,
				Entry.Entity->ChildToParent,
				ComponentTransform
			);

			RestTransform = RestPose.RestTransform;
			ComponentTransform = RestPose.ComponentTransform;
		}

		ComponentTransform = SUComp->TransformOffset * ComponentTransform;

		const int32 TransformIndex = NextConstructionIndex++;

		const int32 ModuleIdx = AddModuleToTree(
			CoreModule,
			ComponentTransform,
			ParentIndex,
			TransformIndex,
			Entry.Entity ? Chaos::FUniqueIdx(Entry.Entity->ParticleIndex) : Chaos::FUniqueIdx(),
			RestTransform,
			SUComp->GetBoneName(),
			SUComp->GetAnimationOffset()
		);

		if (ModuleIdx == INDEX_NONE)
		{
			// 模块已创建，入树失败必须自行释放，否则模块对象与其外部资源泄漏
			UE_LOG(
				LogSingularisMorphVehicle,
				Error,
				TEXT("[RebuildFromSnapshot] Failed to add module for %s - releasing the created module"),
				*SUComp->GetName()
			);

			CoreModule->SetStateFlags(Chaos::eSimModuleState::Disabled);
			CoreModule->OnTermination_External();
			delete CoreModule;

			return INDEX_NONE;
		}

		SUComp->SetModuleGuid(CoreModule->GetGuid());

		FSingularisMorphVehicleComponentData CompData;
		CompData.Guid = CoreModule->GetGuid();
		CompData.ProxyComponentToAnimate = Entry.ProxyComp;
		CompData.ParentGuid = Entry.ParentSU ? Entry.ParentSU->GetModuleGuid() : INDEX_NONE;
		ComponentToPhysicsObjects.Add(SUComp, CompData);
		PhysicsGuidToComponent.Add(CoreModule->GetGuid(), SUComp);

		return ModuleIdx;
	};

	// 19) 新增/替换模块分批构建。
	//     批次语义（FSimTreeUpdates）：仅每批首条可使用绝对树索引（现存父节点）或 -1（根），
	//     其余条目必须引用同批内先前条目（局部索引）。故以「父节点为现存模块」为批次根，
	//     父节点为本次新建/替换的条目并入其父所在批次，形成局部链。
	//     父先于子的计划顺序保证批次根先于其后代，链式引用始终指向同批内已建条目
	TArray<int32> BatchIdByPlanIndex;
	BatchIdByPlanIndex.Init(INDEX_NONE, Plan.Num());
	TArray<TArray<int32>> Batches;
	for (auto I = 0; I < Plan.Num(); ++I)
	{
		const FPlanEntry& Entry = Plan[I];
		if (Entry.Status == EPlanStatus::Keep) continue;

		const int32* ParentPlanIdx = Entry.ParentSU ? PlanIndexBySU.Find(Entry.ParentSU) : nullptr;
		const bool bParentInBatch = ParentPlanIdx && Plan[*ParentPlanIdx].Status != EPlanStatus::Keep;

		if (!bParentInBatch)
		{
			// 现存父节点（绝对索引）或根：本条目开启新批次
			Batches.Add({I});
			BatchIdByPlanIndex[I] = Batches.Num() - 1;
			continue;
		}

		const int32 ParentBatchId = BatchIdByPlanIndex[*ParentPlanIdx];
		ensureMsgf(
			ParentBatchId != INDEX_NONE,
			TEXT("[RebuildFromSnapshot] Parent build must precede child build in plan order")
		);

		if (ParentBatchId == INDEX_NONE)
		{
			// 计划顺序被破坏（不应发生）：跳过，避免以错误父节点入树
			UE_LOG(
				LogSingularisMorphVehicle,
				Error,
				TEXT("[RebuildFromSnapshot] Skipping module for %s - parent batch unresolved"),
				*Entry.SU->GetName()
			);
			continue;
		}

		Batches[ParentBatchId].Add(I);
		BatchIdByPlanIndex[I] = ParentBatchId;
	}

	TMap<USingularisMorphVehicleSUComponent*, int32> LocalIndexBySU;
	for (const TArray<int32>& Batch : Batches)
	{
		LocalIndexBySU.Reset();

		for (int32 PlanIdx : Batch)
		{
			const FPlanEntry& Entry = Plan[PlanIdx];

			// 解析父树索引：现存父节点用其绝对索引；本次新建/替换的父节点用本批局部索引
			auto bCanBuild = true;
			int32 ParentIndex = INDEX_NONE;
			if (Entry.ParentSU)
			{
				const int32* ParentPlanIdx = PlanIndexBySU.Find(Entry.ParentSU);
				if (ParentPlanIdx && Plan[*ParentPlanIdx].Status == EPlanStatus::Keep)
				{
					// 降级保障：保留父节点的绝对索引必定可用（INDEX_NONE 已在计划阶段降级）
					ParentIndex = Entry.ParentSU->GetTreeIndex();
					bCanBuild = ParentIndex != INDEX_NONE;
				}
				else if (const int32* LocalIdx = LocalIndexBySU.Find(Entry.ParentSU))
					ParentIndex = *LocalIdx;
				else
				{
					// 父节点未成功建入本批：子条目不可独立成根
					bCanBuild = false;
				}
			}

			if (!bCanBuild)
			{
				UE_LOG(
					LogSingularisMorphVehicle,
					Error,
					TEXT("[RebuildFromSnapshot] No valid parent for %s - module skipped"),
					*Entry.SU->GetName()
				);
				continue;
			}

			const int32 LocalIndex = AddEntity(Entry, ParentIndex);
			if (LocalIndex != INDEX_NONE)
				LocalIndexBySU.Add(Entry.SU, LocalIndex);
		}

		// 提交本批（首批同时携带本帧全部删除项：批次内先应用新增后应用删除，
		// 新增引用的现存模块在删除前仍有效）
		if (!StoredTreeUpdates.GetNewModules().IsEmpty() || !StoredTreeUpdates.GetDeletedModules().IsEmpty())
		{
			if (!StoredTreeUpdates.GetNewModules().IsEmpty())
				bPendingReplicationStructureRebuild = true;

			if (VehicleSimulationPT)
				VehicleSimulationPT->AppendTreeUpdates(&StoredTreeUpdates);
		}
		StoredTreeUpdates = Chaos::FSimTreeUpdates();
	}

	// 20) 收尾：提交残余更新（仅移除项时无批次循环），刷新物理属性与复制结构标记
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
	// Owner 在 OnCreatePhysicsState 中自忽略：移除它会使悬挂射线命中载具自身，
	// 造成悬挂长度跳变与抓地力异常，故一律保留
	const AActor* Owner = GetOwner();

	for (AActor* Actor : ActorsIn)
	{
		if (!Actor || Actor == Owner) continue;

		ActorsToIgnore.Remove(Actor);
	}
}

void USingularisMorphVehicleSimulationComponent::Update(const float /*DeltaTime*/)
{
	// 未收到变速箱输出时挡位未知：此时发脉冲会把变速箱的初始挡位误判为需升降挡
	if (TargetGearInput == INDEX_NONE || !bHasGearData || CurrentGear == TargetGearInput) return;

	// 换挡进行中：变速箱以空挡（0）过渡，此时再发脉冲会把目标挡位继续推高
	if (CurrentGear == 0) return;

	// 无变速箱模块时脉冲无处消费
	auto bHasTransmission = false;
	for (const TPair<int32, Chaos::ISimulationModuleBase*>& Pair : GuidToCoreModule)
	{
		if (Pair.Value && Pair.Value->IsSimType<Chaos::FTransmissionSimModule>())
		{
			bHasTransmission = true;
			break;
		}
	}
	if (!bHasTransmission) return;

	SetInputBool(
		CurrentGear < TargetGearInput ? Chaos::ChangeUpControlName : Chaos::ChangeDownControlName,
		true
	);
}

void USingularisMorphVehicleSimulationComponent::PreTickGT(const float DeltaTime)
{
	if (!PhysicsAdapter || !PhysicsAdapter->IsReady() || !PhysicsAdapter->IsDirty()) return;

	// 前置守卫：缺少映射子系统、Owner 或物理线程端模拟对象时重建必然失败。
	// 此时不消费脏标记，留待下一帧重试，避免拓扑变更被静默丢弃
	if (!GetOwner()) return;
	if (!VehicleSimulationPT) return;

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
	//    动画数据先累积到本地副本（槽位表由动画线程共享，避免持锁执行外部回调），
	//    并帧首清零全部槽位的标志：标志只增不减会使已停止动画的通道永久保留旧偏移，
	//    把部件钉在上一帧（或上一个模块）的位姿上
	TArray<FSingularisMorphModuleAnimationSetup> AnimSetupUpdates;
	{
		FScopeLock AnimationLock(&ModuleAnimationSetupsLock);
		AnimSetupUpdates = ModuleAnimationSetups;
	}
	for (FSingularisMorphModuleAnimationSetup& AnimSetup : AnimSetupUpdates)
	{
		AnimSetup.AnimFlags = 0;
		AnimSetup.RotOffset = FRotator::ZeroRotator;
		AnimSetup.LocOffset = FVector::ZeroVector;
		AnimSetup.CombinedRotation = FQuat::Identity;
	}

	for (auto I = 0; I < NumItems; ++I)
	{
		Chaos::FSimOutputData* ModuleOutput = VehiclePhysicsOutput->SimTreeOutputData[I];
		if (!ModuleOutput) continue;

		const int32 Guid = ModuleOutput->ModuleGuid;

		// 拓扑变更帧的在飞输出仍描述旧模块，其动画槽位索引会落在新模块的槽位上。
		// 动画槽位的标志只增不减，错配的旧数据（尤其是悬挂位移）会被永久钉在新模块上，
		// 表现为部件可视位姿被持续偏移。模块 GUID 全局唯一且永不复用，
		// 故以「GUID 仍在本帧模块表中」为当前模块的判据（手动注册路径同样入表）
		if (!GuidToCoreModule.Contains(Guid)) continue;

		if (const TWeakObjectPtr<UActorComponent>* Component = PhysicsGuidToComponent.Find(Guid))
		{
			if (auto* BaseSU = Cast<ISingularisMorphVehicleSUInterface>(
				Component->Get()
			))
				BaseSU->OnOutputReady(ModuleOutput);
		}

		// 3a) 缓存常用状态（多变速箱/多引擎时取最后一个）
		if (ModuleOutput->IsSimType<Chaos::FTransmissionSimModule>())
		{
			CurrentGear = static_cast<Chaos::FTransmissionOutputData*>(ModuleOutput)->CurrentGear;
			bHasGearData = true;
		}
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
		if (AnimIndex >= 0 && AnimIndex < AnimSetupUpdates.Num())
		{
			// 同一骨骼可能由多个模块共享（如车轮旋转与悬挂位移），按通道累加标志，
			// 但仅写入该模块真正驱动的通道，避免未驱动通道被单位值覆盖
			if (AnimData.AnimFlags & Chaos::EAnimationFlags::AnimateRotation)
			{
				AnimSetupUpdates[AnimIndex].AnimFlags |= Chaos::EAnimationFlags::AnimateRotation;
				AnimSetupUpdates[AnimIndex].CombinedRotation = AnimData.CombinedRotation;
				AnimSetupUpdates[AnimIndex].RotOffset = AnimData.AnimationRotOffset;
			}

			if (AnimData.AnimFlags & Chaos::EAnimationFlags::AnimatePosition)
			{
				AnimSetupUpdates[AnimIndex].AnimFlags |= Chaos::EAnimationFlags::AnimatePosition;
				AnimSetupUpdates[AnimIndex].LocOffset = AnimData.AnimationLocOffset;
			}
		}
	}

	// 3c) 回写本帧动画数据：本地累积期间不持锁，避免持锁执行模块输出回调。
	//     槽位表可能在回调期间被增删（手动注册、重建、销毁），故按 ModuleGUID 定位目标槽位
	//     逐项回写而非整表覆盖，避免覆盖期间的变更丢失、已移除的槽位被复制品复活
	if (!AnimSetupUpdates.IsEmpty())
	{
		FScopeLock AnimationLock(&ModuleAnimationSetupsLock);

		for (const FSingularisMorphModuleAnimationSetup& Updated : AnimSetupUpdates)
		{
			const int32 Slot = ModuleAnimationSetups.IndexOfByPredicate(
				[Guid = Updated.ModuleGUID](const FSingularisMorphModuleAnimationSetup& Setup)
				{
					return Setup.ModuleGUID == Guid;
				}
			);
			if (Slot == INDEX_NONE) continue;

			FSingularisMorphModuleAnimationSetup& Target = ModuleAnimationSetups[Slot];
			Target.AnimFlags = Updated.AnimFlags;
			Target.RotOffset = Updated.RotOffset;
			Target.LocOffset = Updated.LocOffset;
			Target.CombinedRotation = Updated.CombinedRotation;
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

	// 1) 输入生产者产出本物理步控制输入（GT 频率缓冲合并 → PT 频率拷出并复位）。
	//    必须早于代理守卫：代理未解析（启动、物理重同步）期间不消费缓冲，
	//    会在恢复后把多次脉冲叠加成一次输入
	if (InputProducer)
		InputProducer->ProduceInput(PhysicsStep, NumSteps, InputNameMap, InputsContainer);

	IPhysicsProxyBase* Proxy = GetPhysicsProxy();
	if (!Proxy) return;

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

	// 6) 无物理材质命中面的摩擦回退值（物理线程不访问 UObject，故参数在游戏线程取出）
	AsyncInput->PhysicsInputs.SurfaceFrictionFallback = DefaultSurfaceFriction;
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
	// 请求的挡位先按变速箱的实际挡数钳制：变速箱会把目标挡位夹到
	// [-倒挡数, 前进挡数]，越界请求会让“当前挡位 ≠ 目标挡位”恒成立而逐帧持续发脉冲
	auto MinGear = -1;
	auto MaxGear = 1;

	if (const AActor* Owner = GetOwner())
	{
		if (const USingularisTransmissionSUComponent* TransmissionSU =
			Owner->FindComponentByClass<USingularisTransmissionSUComponent>())
		{
			MaxGear = FMath::Max(1, TransmissionSU->ForwardRatios.Num());
			MinGear = -FMath::Max(1, TransmissionSU->ReverseRatios.Num());
		}
	}

	// 仅记录目标挡位，换挡脉冲由 Update 逐帧产出：变速箱一次只接受一挡（目标挡位 ±1）
	TargetGearInput = FMath::Clamp(Gear, MinGear, MaxGear);
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
	//    模块对象在终止回调中释放其外部资源，必须先于删除调用
	for (const Chaos::FPendingModuleAdds& Pending : StoredTreeUpdates.GetNewModules())
	{
		Chaos::ISimulationModuleBase* Module = Pending.NewSimModule;
		if (!Module) continue;

		// 已被 RemoveSimulationModule 终止过的模块不重复终止：终止回调会释放模块的
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
	{
		FScopeLock AnimationLock(&ModuleAnimationSetupsLock);
		ModuleAnimationSetups.Empty();
	}
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
	//    残留句柄会使新一轮模块全部绑定到已销毁的物理对象（模块外部资源失效）
	VehicleSimulationPT.Reset();
	SimulationModuleTree.Reset();
	VehiclePhysicsOutput.Reset();
	RootPhysicsObject = nullptr;
	CachedPhysicsProxy = nullptr;
}

void USingularisMorphVehicleSimulationComponent::ClearAllSimulationModules()
{
	// 1) 逐个移除旧模块（内部会释放模块外部资源并广播移除事件）。
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
	{
		FScopeLock AnimationLock(&ModuleAnimationSetupsLock);
		ModuleAnimationSetups.Empty();
	}
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
		if (FModuleInputSetup* Existing = OutCombinedInputs.FindByPredicate(
			[&Config](const FModuleInputSetup& Candidate)
			{
				return Candidate.Name == Config.Name;
			}
		))
		{
			// 同名不同类型的声明无法共存于同一容器（容器按名称单槽位），保留首条并告警
			if (Existing->Type != Config.Type)
			{
				UE_LOG(
					LogSingularisMorphVehicle,
					Warning,
					TEXT(
						"[AssimilateComponentInputs] Input %s is declared with conflicting types (%d vs %d) - keeping the first declaration"
					),
					*Config.Name.ToString(),
					static_cast<int32>(Existing->Type),
					static_cast<int32>(Config.Type)
				);
			}
			return;
		}

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

	// 2) 配置与生产者类型均未变化时跳过重建（运行时模块增删会反复进入此函数）。
	//    位置必须早于 CombinedInputConfiguration 的覆盖：下面的比较需要旧值
	const bool bProducerClassMatches = InputProducer && InputProducer->GetClass() == InputProducerClass.Get();
	if (!bForceReinitialize && bProducerClassMatches &&
		IsInputConfigurationEqual(NewCombinedConfig, CombinedInputConfiguration))
		return;

	// 3) 生产者实例：类不匹配时重建实例（否则换类后仍沿用旧实例）
	if (InputProducer && InputProducer->GetClass() != InputProducerClass.Get())
		InputProducer = nullptr;

	if (!InputProducer && InputProducerClass)
		InputProducer = NewObject<UVehicleInputProducerBase>(this, InputProducerClass);

	CombinedInputConfiguration = MoveTemp(NewCombinedConfig);

	// 4) 初始化控制/状态输入容器，并迁移重建前已缓冲的输入值：
	//    容器重建会清零全部输入，否则拓扑变更帧会丢失玩家已缓冲的油门/转向
	{
		const FInputInterface::FInputNameMap PreviousInputMap = InputNameMap;
		const TArray<FModuleInputValue> PreviousValues = InputsContainer.AccessInputValues();

		InputsContainer.Initialize(CombinedInputConfiguration, InputNameMap);
		SingularisMorphVehicleInputUtils::CarryOverValues(
			InputsContainer,
			InputNameMap,
			PreviousInputMap,
			PreviousValues
		);
	}
	{
		const FInputInterface::FInputNameMap PreviousStateMap = StateNameMap;
		const TArray<FModuleInputValue> PreviousStateValues = StateInputContainer.AccessInputValues();

		StateInputContainer.Initialize(StateInputConfiguration, StateNameMap);
		SingularisMorphVehicleInputUtils::CarryOverValues(
			StateInputContainer,
			StateNameMap,
			PreviousStateMap,
			PreviousStateValues
		);
	}

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
	// 仅游戏线程调用，且在 ParallelUpdate 提交本帧动画数据之后：
	// 同线程读取无需持锁，跨线程读取（动画代理）必须持有 GetModuleAnimationSetupsLock()
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

		// 模块的静止基准与动画位移均表达在参考空间（适配器参考变换/载具空间），
		// 而驱动组件可位于其它 Actor（被停靠、动态生成的部件），其父空间与参考空间并不一致，
		// 故先换算到参考空间叠加动画，再换算回父空间写入。
		// 适配器缺失（手动注册路径）时以父组件为参考空间，模块变换即按调用方约定提供
		const USceneComponent* AttachParent = ComponentToAnimate->GetAttachParent();
		const FTransform ReferenceTransform = PhysicsAdapter
			                                      ? PhysicsAdapter->GetReferenceTransform()
			                                      : AttachParent
			                                      ? AttachParent->GetComponentTransform()
			                                      : FTransform::Identity;

		// 1) 以部件当前变换为基准（未参与动画的通道保持现状），叠加动画数据
		FTransform TargetTransform = ComponentToAnimate->GetComponentTransform().GetRelativeTransform(
			ReferenceTransform
		);

		if (AnimSetup.AnimFlags & Chaos::EAnimationFlags::AnimateRotation)
		{
			TargetTransform.SetRotation(
				AnimSetup.InitialRotOffset * AnimSetup.CombinedRotation
			);
		}
		if (AnimSetup.AnimFlags & Chaos::EAnimationFlags::AnimatePosition)
		{
			TargetTransform.SetLocation(
				AnimSetup.InitialLocOffset + AnimSetup.LocOffset
			);
		}

		// 2) 参考空间 → 父空间：父组件即参考组件时该换算退化为单位变换
		const FTransform ReferenceToParent = AttachParent
			                                     ? ReferenceTransform.GetRelativeTransform(
				                                     AttachParent->GetComponentTransform()
			                                     )
			                                     : ReferenceTransform;

		ComponentToAnimate->SetRelativeTransform(
			TargetTransform * ReferenceToParent,
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

	// 5) 设置物理变换（模块静止基准，不含编辑器偏移）。
	//    以单位变换作「未提供」哨兵：部件恰位于参考原点时会被判为未提供并退回相对组件变换。
	//    该判定在重复重建间结果一致，不产生累积漂移
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

	// 6b) 通知模块物理对象已就绪（模块在物理对象上建立外部资源）。
	//    手动注册路径没有重建流程，模块首次入队时根物理对象可能尚未绑定，
	//    缺绑会让 OnConstruction_External 不执行
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
	//    施加多次变换；骨骼名为空表示非骨骼动画，各模块独占槽位以对应各自的驱动组件。
	//    槽位表由动画线程共享，登记期间持锁
	int32 SetupIndex = INDEX_NONE;
	{
		FScopeLock AnimationLock(&ModuleAnimationSetupsLock);

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
	}

	CoreModule->SetAnimationData(BoneName, AnimationOffset, SetupIndex);

	return SetupIndex;
}

const FSingularisMorphModuleRestPose& USingularisMorphVehicleSimulationComponent::AcquireModuleRestPose(
	UPrimitiveComponent* Component,
	const int32 ParticleIndex,
	const FTransform& RestTransform,
	const FTransform& ComponentTransform
)
{
	const FSingularisMorphModuleRestPoseKey Key{Component, ParticleIndex};

	// 已捕获的基准直接复用：重建读到的 ChildToParent 已含引擎写回的动画位移，
	// 重新捕获会把当帧姿态固化为新基准，逐次重建累积为可视轮位与悬挂行程漂移
	if (const FSingularisMorphModuleRestPose* Existing = ModuleRestPoses.Find(Key))
		return *Existing;

	FSingularisMorphModuleRestPose& Added = ModuleRestPoses.Add(Key);
	Added.RestTransform = RestTransform;
	Added.ComponentTransform = ComponentTransform;

	return Added;
}

void USingularisMorphVehicleSimulationComponent::PruneModuleRestPoses(
	const TSet<FSingularisMorphModuleRestPoseKey>& LiveKeys
)
{
	if (ModuleRestPoses.IsEmpty()) return;

	TArray<FSingularisMorphModuleRestPoseKey> StaleKeys;
	for (const auto& Pair : ModuleRestPoses)
	{
		if (!LiveKeys.Contains(Pair.Key))
			StaleKeys.Add(Pair.Key);
	}

	for (const FSingularisMorphModuleRestPoseKey& Key : StaleKeys)
		ModuleRestPoses.Remove(Key);
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
