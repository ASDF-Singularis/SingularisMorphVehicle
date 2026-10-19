#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include <VehicleUtility.h>
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
#include "Components/SingularisEngineComponent.h"
#include "Components/SingularisMorphVehicleBaseComponent.h"
#include "Components/SingularisMotorComponent.h"
#include "Components/SingularisTransmissionComponent.h"
#include "Components/SingularisUprightComponent.h"
#include "Components/SingularisWheelComponent.h"
#include "Core/SingularisMorphVehicleSimulationCU.h"
#include "Core/SingularisMorphVehicleSuspensionSimModule.h"
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

	/**
	 * 逐通道读取组件的碰撞响应，构造完整响应容器。
	 */
	FCollisionResponseContainer CaptureCollisionResponses(const UPrimitiveComponent& Component)
	{
		FCollisionResponseContainer Responses{};
		for (auto ChannelIndex = 0; ChannelIndex < ECC_MAX; ++ChannelIndex)
		{
			const auto Channel = static_cast<ECollisionChannel>(ChannelIndex);
			Responses.SetResponse(Channel, Component.GetCollisionResponseToChannel(Channel));
		}
		return Responses;
	}

	/**
	 * 将完整响应容器逐通道写入组件。
	 */
	void ApplyCollisionResponses(UPrimitiveComponent& Component, const FCollisionResponseContainer& Responses)
	{
		for (auto ChannelIndex = 0; ChannelIndex < ECC_MAX; ++ChannelIndex)
		{
			const auto Channel = static_cast<ECollisionChannel>(ChannelIndex);
			Component.SetCollisionResponseToChannel(Channel, Responses.GetResponse(Channel));
		}
	}

	/**
	 * 从最新异步输出读取变速箱当前挡位。
	 *
	 * 游戏线程用于插值显示的 CurrentGear 来自较旧的输出帧（FTransmissionOutputData::Lerp
	 * 固定取 InCurrent 的挡位），不能用于换挡脉冲决策；否则换挡进行中仍会读到旧挡位，
	 * 重复脉冲会把目标挡位推过空挡（例如 1→N 被推成 -1）。
	 */
	int32 GetLatestTransmissionGear(
		const FSingularisMorphVehicleAsyncOutput* Output,
		const ESingularisMorphChaosAsyncVehicleDataType DataType,
		const TMap<int32, Chaos::ISimulationModuleBase*>& GuidToCoreModule
	)
	{
		if (!Output || !Output->bValid || Output->Type != DataType) return INDEX_NONE;

		int32 Gear = INDEX_NONE;

		for (const Chaos::FSimOutputData* ModuleOutput : Output->VehicleSimOutput.SimTreeOutputData)
		{
			if (!ModuleOutput || !ModuleOutput->IsSimType<Chaos::FTransmissionSimModule>()) continue;

			// 拓扑变更帧的在飞输出可能仍描述已移除的模块，忽略未纳管的 GUID
			if (!GuidToCoreModule.Contains(ModuleOutput->ModuleGuid)) continue;

			Gear = static_cast<const Chaos::FTransmissionOutputData*>(ModuleOutput)->CurrentGear;
		}

		return Gear;
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
	PendingGearPulseTarget = INDEX_NONE;
	PendingGearPulseTimeout = 0.0f;
	LastGearPulseOutputTimestamp = INDEX_NONE;
	LastGearPulseOutput = nullptr;
	bWarnedAutomaticGearInput = false;
	LastEngagedGear = 0;
	bHasGearShiftBaseline = false;
	SlippingWheelGuids.Empty();
	BottomedOutSuspensionGuids.Empty();
	EngineRPM = 0.0f;
	EngineTorque = 0.0f;
	bIsLocallyControlled = false;
	bPendingReplicationStructureRebuild = false;
	CurrentAsyncDataType = AsyncInvalid;
	CurrentAsyncInput = nullptr;
	CurrentAsyncOutput = nullptr;
	NextAsyncOutput = nullptr;
	LatestAsyncOutputTimestamp = INDEX_NONE;
	OutputInterpolationAlpha = 0.0f;

	// 6) 恢复全部被抑制的车轮碰撞：物理状态拆除（含对象池回收）后不得残留抑制状态
	RestoreAllWheelCollisionSuppression();

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
		if (auto* SUComp = Cast<USingularisMorphVehicleBaseComponent>(Component->Get()))
		{
			SUComp->SetModuleGuid(INDEX_NONE);
			SUComp->SetTreeIndex(INDEX_NONE);
		}

		ComponentToPhysicsObjects.Remove(Component->Get());
		PhysicsGuidToComponent.Remove(ModuleGuid);
	}

	// 3) 事件状态清理：打滑中的车轮被移除时广播停止事件，防止音效等观察者驻留；
	//    悬挂触底为一次性冲击事件，仅清理记录不产生配对广播
	if (SlippingWheelGuids.Contains(ModuleGuid))
	{
		SlippingWheelGuids.Remove(ModuleGuid);
		OnWheelSlipStoppedEvent.Broadcast(ModuleGuid);
	}
	BottomedOutSuspensionGuids.Remove(ModuleGuid);

	// 4) 维护动画槽位：删除指向被移除模块的槽位，被多个模块共享的槽位由仍存活的模块接管，
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

	// 5) 排队删除树节点
	StoredTreeUpdates.RemoveNode(ModuleGuid);

	// 6) 拓扑已变更：标记待重建网络复制结构
	bPendingReplicationStructureRebuild = true;
}

void USingularisMorphVehicleSimulationComponent::FinalizeModuleUpdates()
{
	UE_LOG(
		LogSingularisMorphVehicle,
		Display,
		TEXT(
			"[FinalizeModuleUpdates] VehicleSimulationPT=%s, pending adds=%d, reparents=%d, wheel limits=%d"
		),
		VehicleSimulationPT.IsValid() ? TEXT("valid") : TEXT("null"),
		StoredTreeUpdates.GetNewModules().Num(),
		StoredTreeReparents.Num(),
		StoredWheelSpeedLimits.Num()
	);

	const auto bHasStructureChanges = !StoredTreeUpdates.GetNewModules().IsEmpty()
		|| !StoredTreeUpdates.GetDeletedModules().IsEmpty()
		|| !StoredTreeReparents.IsEmpty();
	const auto bHasWheelSpeedLimitChanges = !StoredWheelSpeedLimits.IsEmpty();

	if ((bHasStructureChanges || bHasWheelSpeedLimitChanges) && VehicleSimulationPT)
	{
		// 增删、重挂与运行期设置作为一次原子更新提交：物理线程先应用全部增删，
		// 再解析父 GUID，最后把限速写入已存在的车轮模块
		FSingularisMorphTreeUpdate Update;
		Update.TreeUpdates = MoveTemp(StoredTreeUpdates);
		Update.Reparents = MoveTemp(StoredTreeReparents);
		Update.WheelSpeedLimits = MoveTemp(StoredWheelSpeedLimits);

		VehicleSimulationPT->AppendTreeUpdates(&Update);

		// 仅增删或重挂会改变网络状态历史的模块布局；车轮限速是运行期设置，
		// 不改变模块集合与层级，无需重建复制结构
		if (bHasStructureChanges)
			bPendingReplicationStructureRebuild = true;

		StoredTreeUpdates = Chaos::FSimTreeUpdates();
		StoredTreeReparents.Reset();
		StoredWheelSpeedLimits.Reset();
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
	TArray<USingularisMorphVehicleBaseComponent*> AllSUComponents;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;
		for (USingularisMorphVehicleBaseComponent* SUComp :
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
			if (auto* SUComp = Cast<USingularisMorphVehicleBaseComponent>(ActorComponent))
				AllSUComponents.AddUnique(SUComp);
		}
	}

	// 5) 单动力链分拣：底盘/引擎/离合/变速箱/轮轴各取首个实例，多余实例不入树。
	//    原动机槽由首个引擎占据，无引擎时由首个电机顶替（引擎与电机同为扭矩源）。
	//
	//    物理装配存在性：驱动组件为物理部件（UPrimitiveComponent）但不在本次快照中时，
	//    该部件已脱离装配（轮胎断裂、部件被摧毁等），继续建入树会让模块以无效粒子索引
	//    绑定到装配根粒子，力仍施加到整车（断轮后照常行驶）。故仅纯仿真模块（无驱动组件）
	//    或驱动组件在快照中者参与建树；脱离装配的 SU 不入计划，既有模块随差量移除，
	//    部件回到装配后重新建入
	const auto IsInPhysicsAssembly = [&EntityMap](const USingularisMorphVehicleBaseComponent* SUComp) -> bool
	{
		if (!SUComp) return false;

		UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
			SUComp->DrivenComponent.GetComponent(SUComp->GetOwner())
		);
		return !ProxyComp || EntityMap.Contains(ProxyComp);
	};

	USingularisMorphVehicleBaseComponent* ChassisSU = nullptr;
	USingularisMorphVehicleBaseComponent* PrimeMoverSU = nullptr;
	USingularisMorphVehicleBaseComponent* ClutchSU = nullptr;
	USingularisMorphVehicleBaseComponent* TransmissionSU = nullptr;
	USingularisMorphVehicleBaseComponent* AxleSU = nullptr;

	TArray<USingularisMorphVehicleBaseComponent*> Wheels;
	TArray<USingularisMorphVehicleBaseComponent*> Suspensions;
	TArray<USingularisMorphVehicleBaseComponent*> AuxiliaryModules;
	TArray<USingularisMorphVehicleBaseComponent*> Uprights;

	auto NumChassis = 0;
	auto NumEngine = 0;
	auto NumClutch = 0;
	auto NumTransmission = 0;
	auto NumAxle = 0;

	for (USingularisMorphVehicleBaseComponent* SUComp : AllSUComponents)
	{
		if (!SUComp) continue;

		switch (SUComp->GetModuleType())
		{
		case ESingularisMorphVehicleModuleType::Chassis:
			++NumChassis;
			if (!ChassisSU && IsInPhysicsAssembly(SUComp))
				ChassisSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Engine:
			// 引擎优先占据原动机槽，后续引擎实例不入树
			++NumEngine;
			if (!PrimeMoverSU && IsInPhysicsAssembly(SUComp))
				PrimeMoverSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Motor:
			// 无引擎时首个电机顶替原动机槽，其余电机作为辅助模块挂到底盘之下
			if (!IsInPhysicsAssembly(SUComp)) break;
			if (!PrimeMoverSU)
				PrimeMoverSU = SUComp;
			else
				AuxiliaryModules.Add(SUComp);
			break;

		case ESingularisMorphVehicleModuleType::Clutch:
			++NumClutch;
			if (!ClutchSU && IsInPhysicsAssembly(SUComp))
				ClutchSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Transmission:
			++NumTransmission;
			if (!TransmissionSU && IsInPhysicsAssembly(SUComp))
				TransmissionSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Axle:
			++NumAxle;
			if (!AxleSU && IsInPhysicsAssembly(SUComp))
				AxleSU = SUComp;
			break;

		case ESingularisMorphVehicleModuleType::Wheel:
			if (IsInPhysicsAssembly(SUComp))
				Wheels.Add(SUComp);
			break;

		case ESingularisMorphVehicleModuleType::Suspension:
			if (IsInPhysicsAssembly(SUComp))
				Suspensions.Add(SUComp);
			break;

		case ESingularisMorphVehicleModuleType::Upright:
			// 立轴为纯配置载体，不创建模块不入树，仅在槽位匹配阶段参与；
			// 槽位标记是定位参照（通常非物理部件），不适用装配存在性过滤：
			// 过滤掉会让仍存活的轮胎失去槽位配置（转向/轴向/反转）
			Uprights.Add(SUComp);
			break;

		default:
			if (IsInPhysicsAssembly(SUComp))
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

	// 7b) 车轮有效转速上限：车轮自带的 MaxRotationVel 是机械安全上限，若它低于
	//     引擎最高转速经最高挡（最小传动比）折算的轮速，车轮会先于引擎到达限速，
	//     引擎转速停在低于 MaxRPM 的位置，转速限制器永远无法介入，而轮胎仍按扭矩
	//     继续驱动车辆。此处把有效上限提升到动力链可达轮速，让引擎转速限制器成为
	//     最终约束；无传动链/无原动机时保持车轮配置值。
	{
		// 先清除上一次重建写入的有效上限，动力链缺失时回退到车轮自身配置
		for (USingularisMorphVehicleBaseComponent* WheelSU : Wheels)
		{
			if (auto* Wheel = Cast<USingularisWheelComponent>(WheelSU))
				Wheel->SetEffectiveMaxRotationVel(0.0f);
		}

		auto DrivelineMaxOmega = 0.0f;
		if (const auto* EngineSU = Cast<USingularisEngineComponent>(PrimeMoverSU))
		{
			DrivelineMaxOmega = Chaos::RPMToOmega(
				FMath::Clamp(EngineSU->MaxRPM, 1, static_cast<int32>(MAX_uint16))
			);
		}
		else if (const auto* MotorSU = Cast<USingularisMotorComponent>(PrimeMoverSU))
			DrivelineMaxOmega = Chaos::RPMToOmega(FMath::Max(MotorSU->MaxRPM, 0.0f));

		auto TopGearRatio = 0.0f;
		if (const auto* Transmission = Cast<USingularisTransmissionComponent>(TransmissionSU))
		{
			auto MinGearRatio = TNumericLimits<float>::Max();
			for (const float Ratio : Transmission->ForwardRatios)
			{
				if (FMath::Abs(Ratio) > KINDA_SMALL_NUMBER)
					MinGearRatio = FMath::Min(MinGearRatio, FMath::Abs(Ratio));
			}
			for (const float Ratio : Transmission->ReverseRatios)
			{
				if (FMath::Abs(Ratio) > KINDA_SMALL_NUMBER)
					MinGearRatio = FMath::Min(MinGearRatio, FMath::Abs(Ratio));
			}

			if (MinGearRatio < TNumericLimits<float>::Max())
				TopGearRatio = MinGearRatio * FMath::Abs(Transmission->FinalDriveRatio);
		}
		else
		{
			// 无变速箱：原动机与车轮直连
			TopGearRatio = 1.0f;
		}

		if (DrivelineMaxOmega > 0.0f && TopGearRatio > KINDA_SMALL_NUMBER)
		{
			// 10% 余量：确保引擎能进入限速区间，由转速限制器而非车轮上限截止
			const float MaxWheelOmega = DrivelineMaxOmega / TopGearRatio * 1.1f;
			for (USingularisMorphVehicleBaseComponent* WheelSU : Wheels)
			{
				if (auto* Wheel = Cast<USingularisWheelComponent>(WheelSU))
					Wheel->SetEffectiveMaxRotationVel(MaxWheelOmega);
			}
		}
	}

	// 8) 轮位槽邻近匹配：轮胎 ↔ 立轴一对一配对。
	//    立轴的驱动组件为车架上的槽位标记场景组件，其世界位置即槽位位置；
	//    轮胎驱动组件落在某槽位匹配半径内即挂载该槽位（多候选取最近者，
	//    每个槽位至多消费一个轮胎）。匹配结果决定车轮的树内位置与槽位配置注入；
	//    未匹配的轮胎为自由轮：挂底盘之下，无驱动、无转向，仅提供滚动。
	//    相对几何随车体整体运动保持不变，匹配仅在拓扑变更时重算即可保持稳定
	TMap<USingularisMorphVehicleBaseComponent*, USingularisUprightComponent*> WheelToUpright;
	{
		struct FUprightSlot
		{
			USingularisUprightComponent* UprightSU = nullptr;
			FVector SlotLocation = FVector::ZeroVector;
		};
		TArray<FUprightSlot> Slots;
		Slots.Reserve(Uprights.Num());

		for (USingularisMorphVehicleBaseComponent* SUComp : Uprights)
		{
			auto* UprightSU = Cast<USingularisUprightComponent>(SUComp);
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

		TSet<USingularisMorphVehicleBaseComponent*> ConsumedUprights;
		TSet<USingularisMorphVehicleBaseComponent*> HandledWheels;

		// 1) 优先沿用上次匹配：集群子件顺序可能随引擎安装等拓扑变更而改变，
		//    重新做贪心最近匹配会让相邻轮位互换槽位配置，进而触发不必要的车轮模块替换。
		//    只要原槽位仍存在且轮胎仍在其匹配半径内，就保持原配对。
		for (USingularisMorphVehicleBaseComponent* SUComp : Wheels)
		{
			const FSingularisMorphVehicleComponentData* Existing = ComponentToPhysicsObjects.Find(SUComp);
			USingularisUprightComponent* PreviousUpright = Existing ? Existing->MatchedUpright.Get() : nullptr;
			if (!PreviousUpright || ConsumedUprights.Contains(PreviousUpright)) continue;

			const UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
				SUComp->DrivenComponent.GetComponent(SUComp->GetOwner())
			);
			if (!ProxyComp) continue;

			const int32 SlotIndex = Slots.IndexOfByPredicate(
				[PreviousUpright](const FUprightSlot& Slot)
				{
					return Slot.UprightSU == PreviousUpright;
				}
			);
			if (SlotIndex == INDEX_NONE) continue;

			const float Radius = FMath::Max(Slots[SlotIndex].UprightSU->MatchRadius, 0.0f);
			const float DistanceSq = FVector::DistSquared(
				ProxyComp->GetComponentTransform().GetLocation(),
				Slots[SlotIndex].SlotLocation
			);
			if (DistanceSq > Radius * Radius) continue;

			WheelToUpright.Add(SUComp, PreviousUpright);
			ConsumedUprights.Add(PreviousUpright);
			HandledWheels.Add(SUComp);
		}

		// 2) 未沿用的车轮再做最近槽位匹配
		for (USingularisMorphVehicleBaseComponent* SUComp : Wheels)
		{
			if (HandledWheels.Contains(SUComp)) continue;

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
	//    计划是差量比对与拓扑同步的唯一事实来源；父节点以 SU 指针表达，
	//    全部模块创建后再按模块 GUID 统一重挂，不依赖树索引或批次顺序
	enum class EPlanStatus : uint8
	{
		Keep, // 保留：模块与粒子绑定均未变化，不重建
		Add, // 新建：此前无模块
		Replace, // 替换：模块存在但属性变化，先移除再新建
	};

	struct FPlanEntry
	{
		USingularisMorphVehicleBaseComponent* SU = nullptr;
		USingularisMorphVehicleBaseComponent* ParentSU = nullptr; // nullptr = 根（底盘）
		USingularisUprightComponent* MatchedUpright = nullptr; // 车轮：槽位配置注入源
		UPrimitiveComponent* ProxyComp = nullptr; // 驱动组件解析结果
		const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity* Entity = nullptr;
		EPlanStatus Status = EPlanStatus::Add;

		/** 替换前模块的通用状态；同类型新模块创建后回填，避免动态状态重置 */
		TSharedPtr<Chaos::FModuleNetData> PreservedState;

		/** 被替换模块的模拟类型；类型变化时不做状态移植，避免跨类型写入 */
		FName PreservedSimType = NAME_None;
	};

	TArray<FPlanEntry> Plan;
	{
		auto FirstValidSU = [](
			const std::initializer_list<USingularisMorphVehicleBaseComponent*> Candidates
		)
			-> USingularisMorphVehicleBaseComponent*
		{
			for (USingularisMorphVehicleBaseComponent* Candidate : Candidates)
				if (Candidate) return Candidate;
			return nullptr;
		};

		auto AddPlanEntry = [&](
			USingularisMorphVehicleBaseComponent* SUComp,
			USingularisMorphVehicleBaseComponent* ParentSU,
			USingularisUprightComponent* MatchedUpright
		) -> void
		{
			FPlanEntry Entry;
			Entry.SU = SUComp;
			Entry.ParentSU = ParentSU;
			Entry.MatchedUpright = MatchedUpright;
			Entry.ProxyComp = Cast<UPrimitiveComponent>(SUComp->DrivenComponent.GetComponent(SUComp->GetOwner()));
			Entry.Entity = Entry.ProxyComp ? EntityMap.FindRef(Entry.ProxyComp) : nullptr;
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
		USingularisMorphVehicleBaseComponent* WheelParentSU = FirstValidSU(
			{AxleSU, TransmissionSU, ClutchSU, PrimeMoverSU, ChassisSU}
		);
		for (USingularisMorphVehicleBaseComponent* SUComp : Wheels)
		{
			if (USingularisUprightComponent* const* MatchedUpright = WheelToUpright.Find(SUComp))
				AddPlanEntry(SUComp, WheelParentSU, *MatchedUpright);
			else
				AddPlanEntry(SUComp, ChassisSU, nullptr);
		}

		// 悬挂 → 同一驱动组件上的车轮之下；配对失败挂底盘并告警
		TMap<TObjectPtr<UPrimitiveComponent>, USingularisMorphVehicleBaseComponent*> WheelSUByComponent;
		for (USingularisMorphVehicleBaseComponent* SUComp : Wheels)
		{
			if (UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
				SUComp->DrivenComponent.GetComponent(SUComp->GetOwner())
			))
				WheelSUByComponent.FindOrAdd(ProxyComp) = SUComp;
		}

		for (USingularisMorphVehicleBaseComponent* SUComp : Suspensions)
		{
			USingularisMorphVehicleBaseComponent* PairedWheelSU = nullptr;
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

		for (USingularisMorphVehicleBaseComponent* SUComp : AuxiliaryModules)
			AddPlanEntry(SUComp, ChassisSU, nullptr);
	}

	// 10) 差量分派：与现存模块逐一比对。
	//     保留 = 模块存活且驱动组件、槽位配置源、物理粒子索引、动画开关均未变化；
	//     新建 = 此前无模块；替换 = 任一属性变化（模块设置与粒子绑定在构建期固化，
	//     无法原位修改，故「修改」表现为「先移除再新建」）；父节点变化不触发替换，
	//     由重挂阶段处理。计划外的现存模块、失效映射键与未纳管模块一并移除
	TArray<USingularisMorphVehicleBaseComponent*> RemovedSUs;
	TArray<TObjectPtr<UActorComponent>> StaleMapKeys;
	TArray<int32> UnmanagedGuids;
	{
		TSet<USingularisMorphVehicleBaseComponent*> PlannedSUs;
		PlannedSUs.Reserve(Plan.Num());
		for (const FPlanEntry& Entry : Plan)
			PlannedSUs.Add(Entry.SU);

		for (const auto& Pair : ComponentToPhysicsObjects)
		{
			auto* SUComp = Cast<USingularisMorphVehicleBaseComponent>(Pair.Key.Get());
			if (!IsValid(SUComp))
			{
				// 键已失效（SU 随部件 Actor 销毁）：其模块仍须移除，
				// 键无法经弱引用定位，收录键值稍后手工清除
				StaleMapKeys.Add(Pair.Key);
				continue;
			}

			if (!PlannedSUs.Contains(SUComp))
			{
				RemovedSUs.Add(SUComp);

				// 驱动组件仍在但已不在快照中：部件脱离装配（如轮胎断裂）导致的移除，
				// 记录以便与「其它计划外模块」（重复实例、配置变更）区分
				UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
					SUComp->DrivenComponent.GetComponent(SUComp->GetOwner())
				);
				if (ProxyComp && !EntityMap.Contains(ProxyComp))
				{
					UE_LOG(
						LogSingularisMorphVehicle,
						Display,
						TEXT(
							"[RebuildFromSnapshot] %s has left the physics assembly - removing its module (the part is no longer in the snapshot)"
						),
						*SUComp->GetName()
					);
				}
			}
		}

		for (FPlanEntry& Entry : Plan)
		{
			const FSingularisMorphVehicleComponentData* Existing = ComponentToPhysicsObjects.Find(Entry.SU);
			Chaos::ISimulationModuleBase* const* ExistingModule =
				Existing ? GuidToCoreModule.Find(Existing->Guid) : nullptr;

			// 曾匹配的槽位已销毁且本次未找到新槽位：替换以清除旧槽位配置
			const bool bLostMatchedUpright = Existing
				&& Existing->bHadMatchedUpright
				&& !Existing->MatchedUpright.IsValid();

			if (!Existing || !ExistingModule || !*ExistingModule)
				Entry.Status = EPlanStatus::Add;
			else if (Existing->ProxyComponentToAnimate.Get() != static_cast<USceneComponent*>(Entry.ProxyComp)
				|| Existing->MatchedUpright.Get() != Entry.MatchedUpright
				|| bLostMatchedUpright
				|| (*ExistingModule)->GetParticleIndex().Idx !=
				(Entry.Entity ? Entry.Entity->ParticleIndex : Chaos::FUniqueIdx().Idx)
				|| (*ExistingModule)->IsAnimationEnabled() != Entry.SU->GetAnimationEnabled())
			{
				// 驱动组件/槽位配置/粒子绑定/动画开关变化：替换以重建模块与动画槽位
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

	// 10b) 保留车轮的有效转速上限原位更新。新建/替换模块在 CreateNewCoreModule 中
	//      已读取最新上限；保留模块的设置已固化，只有直接写回模块设置才能生效。
	//      若靠替换模块更新，会把角位置与动画槽位清零，重新引入轮胎闪烁。
	for (const FPlanEntry& Entry : Plan)
	{
		if (Entry.Status != EPlanStatus::Keep) continue;

		const auto* WheelSU = Cast<USingularisWheelComponent>(Entry.SU);
		const FSingularisMorphVehicleComponentData* Data = ComponentToPhysicsObjects.Find(Entry.SU);
		if (!WheelSU || !Data) continue;

		StoredWheelSpeedLimits.Add({Data->Guid, WheelSU->GetResolvedMaxRotationVel()});
	}

	// 车轮碰撞抑制对账（活动车轮集合 → 抑制/恢复）。
	// 与拓扑是否变更无关：任何一帧都必须对账，否则外部（组件物理状态重建、载具逻辑）
	// 恢复过的轮胎碰撞会在「无拓扑差异」帧被静默保留，重新成为解析悬挂之外的第二支撑
	const auto ReconcileActiveWheelCollision = [this, &Wheels]()
	{
		TSet<UPrimitiveComponent*> ActiveWheelComponents;
		ActiveWheelComponents.Reserve(Wheels.Num());

		for (USingularisMorphVehicleBaseComponent* WheelSU : Wheels)
		{
			if (!WheelSU) continue;

			if (UPrimitiveComponent* WheelComponent = Cast<UPrimitiveComponent>(
				WheelSU->DrivenComponent.GetComponent(WheelSU->GetOwner())
			))
				ActiveWheelComponents.Add(WheelComponent);
		}

		ReconcileWheelCollisionSuppression(ActiveWheelComponents);
	};

	// 11) 替换项状态移植：模块被替换时保留角速度、角位置、挡位与悬挂行程等动态状态。
	//      新模块若从默认值起步，会在首帧输出把轮胎转角/悬挂压缩量拉回基准，表现为闪烁。
	for (FPlanEntry& Entry : Plan)
	{
		if (Entry.Status != EPlanStatus::Replace) continue;

		const FSingularisMorphVehicleComponentData* Existing = ComponentToPhysicsObjects.Find(Entry.SU);
		if (!Existing) continue;

		Chaos::ISimulationModuleBase* const* OldModule = GuidToCoreModule.Find(Existing->Guid);
		if (!OldModule || !*OldModule) continue;

		TSharedPtr<Chaos::FModuleNetData> State = (*OldModule)->GenerateNetData(0);
		if (!State.IsValid()) continue;

		State->FillNetState(*OldModule);
		Entry.PreservedState = MoveTemp(State);
		Entry.PreservedSimType = (*OldModule)->GetSimType();
	}

	// 12) 裁剪静止基准缓存：仅保留本次快照仍存活的（驱动组件, 粒子）键。
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

	// 13) 模块集合可能已变化（变形增删部件），刷新输入配置。
	//     配置未变更时该调用为空操作，不会清空输入容器。
	SetupInputConfiguration();

	const FTransform ReferenceTransform = PhysicsAdapter
		                                      ? PhysicsAdapter->GetReferenceTransform()
		                                      : FTransform::Identity;

	// 14) 缓存根物理对象（首次或重建时刷新）
	//     模块构造期需要 RootPhysicsObject 进行 OnConstruction_External 初始化
	if (RootPhysicsObject == nullptr)
		CacheRootPhysicsObject(GetPhysicsProxy());

	// 15) 执行移除：计划外模块、替换项旧模块、失效映射键模块与未纳管模块。
	//     逐个移除（终止回调释放外部资源、动画槽位让渡、映射清理）；
	//     删除与重挂均以 GUID 解析，不依赖节点索引
	TArray<int32> GuidsToRemove;
	GuidsToRemove.Reserve(RemovedSUs.Num() + Plan.Num() + UnmanagedGuids.Num());
	for (USingularisMorphVehicleBaseComponent* SUComp : RemovedSUs)
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

	// 16) 注册 Lambda：构建并登记单个计划条目对应的模块。
	//     驱动组件/实体/槽位配置源已在计划阶段解析；新模块一律以根入树，
	//     父关系由构建完成后的统一重挂阶段按 GUID 建立。
	auto AddEntity = [&](const FPlanEntry& Entry) -> int32
	{
		USingularisMorphVehicleBaseComponent* SUComp = Entry.SU;
		if (!SUComp) return INDEX_NONE;

		Chaos::ISimulationModuleBase* CoreModule = SUComp->CreateNewCoreModule();
		if (!CoreModule)
		{
			// 计划条目静默不入树会让「计划数」与「树内模块数」出现无从对账的缺口，必须可见
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT("[RebuildFromSnapshot] %s failed to create a core module - module skipped"),
				*GetNameSafe(SUComp)
			);
			return INDEX_NONE;
		}

		// SU 的动画开关是模块动画的总开关：部分模块类型（引擎、变速箱等）
		// 不自带动画而不会在 CreateNewCoreModule 中同步该标志，此处统一同步
		CoreModule->SetAnimationEnabled(SUComp->GetAnimationEnabled());

		// 替换项：同类型模块间移植动态状态（角速度/角位置/挡位/悬挂行程）。
		// 类型不一致时放弃移植，避免向不兼容的模块写入状态
		if (Entry.PreservedState.IsValid() && Entry.PreservedSimType == CoreModule->GetSimType())
			Entry.PreservedState->FillSimState(CoreModule);

		// 挂载立轴槽位的车轮：注入槽位配置（转向/轴向/反转/制动/辅助/力偏移）。
		// 各项均由核心物理路径从模块设置读取，注入必须在入树前一次性完成
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
			INDEX_NONE,
			TransformIndex,
			Entry.Entity ? Chaos::FUniqueIdx(Entry.Entity->ParticleIndex) : Chaos::FUniqueIdx(),
			RestTransform,
			SUComp->GetBoneName(),
			SUComp->GetAnimationOffset()
		);

		if (ModuleIdx == INDEX_NONE)
		{
			// 模块已创建（悬挂类模块此时已建立物理约束），入树失败必须自行释放，
			// 否则模块对象与其外部资源（悬挂约束）泄漏且约束会持续施力
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
		CompData.MatchedUpright = Entry.MatchedUpright;
		CompData.bHadMatchedUpright = Entry.MatchedUpright != nullptr;

		// 树内实际父节点为根；真实父 GUID 由重挂阶段写入
		CompData.ParentGuid = INDEX_NONE;
		ComponentToPhysicsObjects.Add(SUComp, CompData);
		PhysicsGuidToComponent.Add(CoreModule->GetGuid(), SUComp);

		return ModuleIdx;
	};

	// 17) 构建新增/替换模块。所有新模块先以根节点入树：
	//     父子关系不再依赖批次局部索引，统一在全部模块创建后按 GUID 重挂。
	for (FPlanEntry& Entry : Plan)
	{
		if (Entry.Status == EPlanStatus::Keep) continue;
		AddEntity(Entry);
	}

	// 18) 统一重挂父节点。
	//     新增模块必须从根移动到目标父节点；保留模块仅在父节点实际变化时重挂。
	//     这样安装引擎只会新增引擎并移动变速箱，轮轴/车轮/悬挂模块及其动态状态、
	//     动画槽位全部原样保留，不再触发轮胎转角复位闪烁。
	for (const FPlanEntry& Entry : Plan)
	{
		if (!Entry.SU || !Entry.ParentSU) continue;

		FSingularisMorphVehicleComponentData* Data = ComponentToPhysicsObjects.Find(Entry.SU);
		if (!Data) continue;

		const int32 DesiredParentGuid = Entry.ParentSU->GetModuleGuid();
		if (DesiredParentGuid == INDEX_NONE)
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT("[RebuildFromSnapshot] %s has no valid parent module GUID - reparent skipped"),
				*GetNameSafe(Entry.SU)
			);
			continue;
		}

		// 新模块在树中的父节点是根，必须重挂；保留模块只在父 GUID 变化时重挂
		const bool bRequiresReparent = Entry.Status != EPlanStatus::Keep
			|| Data->ParentGuid != DesiredParentGuid;
		if (bRequiresReparent)
			StoredTreeReparents.Add({Data->Guid, DesiredParentGuid});

		Data->ParentGuid = DesiredParentGuid;
	}

	// 19) 车轮碰撞对账：抑制活动轮胎的碰撞（解除其与地面的物理接触，避免成为解析悬挂之外的
	//     第二支撑），恢复已分离轮胎的原始碰撞。以「活动集合 vs 已记录集合」对账，
	//     天然覆盖共享组件（轮胎与悬挂常共用同一网格）、替换与组件销毁
	ReconcileActiveWheelCollision();

	// 20) 收尾：提交本帧全部增删与重挂，刷新物理属性与复制结构标记
	FinalizeModuleUpdates();
}

void USingularisMorphVehicleSimulationComponent::UpdatePhysicalProperties() const
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

void USingularisMorphVehicleSimulationComponent::SetInputBool(
	const FName Name,
	const bool Value,
	const EModuleInputBufferActionType BufferAction
) const
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInputInteger(
	const FName Name,
	const int32 Value,
	const EModuleInputBufferActionType BufferAction
) const
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInputAxis1D(
	const FName Name,
	const double Value,
	const EModuleInputBufferActionType BufferAction
) const
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInputAxis2D(
	const FName Name,
	const FVector2D Value,
	const EModuleInputBufferActionType BufferAction
) const
{
	if (InputProducer)
		InputProducer->BufferInput(InputNameMap, Name, Value, BufferAction);
}

void USingularisMorphVehicleSimulationComponent::SetInputAxis3D(
	const FName Name,
	const FVector Value,
	const EModuleInputBufferActionType BufferAction
) const
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
		if (const USingularisTransmissionComponent* TransmissionSU =
			Owner->FindComponentByClass<USingularisTransmissionComponent>())
		{
			// 自动变速箱不消费升降挡脉冲：写入目标会让 Update 每帧发脉冲却无人处理。
			// 尤其是目标 0（空挡）永远无法到达，载具实际仍在前进挡
			if (TransmissionSU->TransmissionType == ESingularisMorphVehicleTransmissionType::Automatic)
			{
				// 每个实例只告警一次，避免上层逐帧调用刷屏
				if (!bWarnedAutomaticGearInput)
				{
					bWarnedAutomaticGearInput = true;

					UE_LOG(
						LogSingularisMorphVehicle,
						Warning,
						TEXT(
							"[%s] SetGearInput is ignored: automatic transmission does not consume shift pulses"
						),
						*GetName()
					);
				}

				TargetGearInput = INDEX_NONE;
				return;
			}

			// 与变速箱的 CorrectGearInputRange 使用同一上限：空列表时只能请求空挡，
			// 否则 Update 会为永远无法到达的挡位逐帧发送脉冲
			MaxGear = FMath::Max(TransmissionSU->ForwardRatios.Num(), 0);
			MinGear = -FMath::Max(TransmissionSU->ReverseRatios.Num(), 0);
		}
	}

	// 仅记录目标挡位，换挡脉冲由 Update 逐帧产出：变速箱一次只接受一挡（目标挡位 ±1）
	TargetGearInput = FMath::Clamp(Gear, MinGear, MaxGear);
}

void USingularisMorphVehicleSimulationComponent::SetControlEnabled(
	const FName ControlName,
	const bool bEnabled
)
{
	if (ControlName.IsNone()) return;

	// 配置已建立时校验通道名，避免拼写错误静默失效；配置尚未建立时允许预登记
	if (!InputNameMap.IsEmpty() && !InputNameMap.Contains(ControlName))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("SetControlEnabled: 未配置的控制输入通道 '%s'，请求已忽略"),
			*ControlName.ToString()
		);
		return;
	}

	if (bEnabled)
		DisabledControlNames.Remove(ControlName);
	else
		DisabledControlNames.Add(ControlName);
}

void USingularisMorphVehicleSimulationComponent::SetControlsEnabled(
	const TArray<FName>& ControlNames,
	const bool bEnabled
)
{
	for (const FName& ControlName : ControlNames)
		SetControlEnabled(ControlName, bEnabled);
}

void USingularisMorphVehicleSimulationComponent::SetAllControlsEnabled(const bool bEnabled)
{
	if (bEnabled)
	{
		DisabledControlNames.Reset();
		return;
	}

	for (const auto& Pair : InputNameMap)
		DisabledControlNames.Add(Pair.Key);
}

bool USingularisMorphVehicleSimulationComponent::IsControlEnabled(const FName ControlName) const
{
	return !DisabledControlNames.Contains(ControlName);
}

TArray<FName> USingularisMorphVehicleSimulationComponent::GetDisabledControls() const
{
	return DisabledControlNames.Array();
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
	// 显式声明本地控制：SimulatedProxy 的输入来自服务器，无论传入何值均不标记本地
	const AActor* Owner = GetOwner();
	const bool bCanBeLocal = Owner && Owner->GetLocalRole() != ROLE_SimulatedProxy;
	bIsLocallyControlled = bInLocallyControlled && bCanBeLocal;

	// 启用网络物理预测时同步本地输入中继标记
	if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent)
		NetworkPhysicsComponent->SetIsRelayingLocalInputs(bIsLocallyControlled);
}

void USingularisMorphVehicleSimulationComponent::Update(const float DeltaTime)
{
	if (TargetGearInput == INDEX_NONE) return;

	// 1) 换挡决策只读取最新物理输出，而非插值显示的 CurrentGear：
	//    插值挡位固定取较旧输出帧的挡位，换挡期间会滞后；以它驱动脉冲会在变速箱
	//    已进入空挡后再发一次，把目标挡位推过空挡（例如 1→N 被推成 -1）
	const auto* DecisionOutput = NextAsyncOutput ? NextAsyncOutput : CurrentAsyncOutput;
	const auto LatestGear = GetLatestTransmissionGear(
		DecisionOutput,
		CurrentAsyncDataType,
		GuidToCoreModule
	);

	// 未收到变速箱输出时挡位未知：此时发脉冲会把变速箱的初始挡位误判为需升降挡
	if (LatestGear == INDEX_NONE) return;

	// 2) 解析变速箱模块：挡位上限、换挡时间与手动/自动类型均以模拟模块设置为准
	const Chaos::FTransmissionSimModule* Transmission = nullptr;
	for (const TPair<int32, Chaos::ISimulationModuleBase*>& Pair : GuidToCoreModule)
	{
		if (!Pair.Value || !Pair.Value->IsSimType<Chaos::FTransmissionSimModule>()) continue;

		Transmission = Pair.Value->Cast<Chaos::FTransmissionSimModule>();
		if (Transmission) break;
	}
	if (!Transmission) return;

	if (Transmission->Setup().TransmissionType != Chaos::FTransmissionSettings::ETransType::ManualType)
	{
		PendingGearPulseTarget = INDEX_NONE;
		PendingGearPulseTimeout = 0.0f;
		return;
	}

	// 3) 物理输出未推进时不做重复决策：异步求解器可能暂停或落后于游戏线程，
	//    同一份输出上反复发脉冲会把目标挡位推过空挡或目标挡
	if (PendingGearPulseTarget != INDEX_NONE &&
		DecisionOutput == LastGearPulseOutput &&
		LatestAsyncOutputTimestamp == LastGearPulseOutputTimestamp)
		return;

	// 4) 待确认脉冲：输出到达待确认挡位即完成；超时后重新评估，
	//    避免指令被其它换挡逻辑覆盖后永久等待
	const auto SafeDeltaTime = FMath::IsFinite(DeltaTime) ? FMath::Max(DeltaTime, 0.0f) : 0.0f;

	if (PendingGearPulseTarget != INDEX_NONE)
	{
		if (LatestGear == PendingGearPulseTarget)
		{
			PendingGearPulseTarget = INDEX_NONE;
			PendingGearPulseTimeout = 0.0f;
		}
		else
		{
			PendingGearPulseTimeout -= SafeDeltaTime;
			if (PendingGearPulseTimeout > 0.0f) return;

			PendingGearPulseTarget = INDEX_NONE;
			PendingGearPulseTimeout = 0.0f;
		}
	}

	// 5) 已到达请求挡位
	if (LatestGear == TargetGearInput) return;

	// 6) 一次只推进一挡，并记录待确认目标：变速箱以空挡过渡，
	//    输出确认前不再发送，避免 1→2 被推成 1→3、1→N 被推成 1→R
	const auto MinGear = -Transmission->Setup().ReverseRatios.Num();
	const auto MaxGear = Transmission->Setup().ForwardRatios.Num();
	const auto Step = LatestGear < TargetGearInput ? 1 : -1;
	const auto PulseTarget = FMath::Clamp(LatestGear + Step, MinGear, MaxGear);
	if (PulseTarget == LatestGear) return;

	const auto GearChangeTime = FMath::IsFinite(Transmission->Setup().GearChangeTime)
		                            ? FMath::Max(Transmission->Setup().GearChangeTime, 0.0f)
		                            : 0.0f;

	SetInputBool(
		Step > 0 ? Chaos::ChangeUpControlName : Chaos::ChangeDownControlName,
		true
	);

	PendingGearPulseTarget = PulseTarget;
	PendingGearPulseTimeout = FMath::Max(GearChangeTime * 2.0f + 0.5f, 0.5f);
	LastGearPulseOutputTimestamp = LatestAsyncOutputTimestamp;
	LastGearPulseOutput = DecisionOutput;
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

	// 0) 记录最新输出的物理帧时间戳：换挡脉冲只在输出推进后继续决策，
	//    异步求解器暂停时不会对同一份状态重复发送
	LatestAsyncOutputTimestamp = NextOutput
		                             ? NextOutput->Timestamp
		                             : CurOutput
		                             ? CurOutput->Timestamp
		                             : INDEX_NONE;

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
			EngineRPM = FMath::IsFinite(EngineOutput->RPM) ? EngineOutput->RPM : 0.0f;

			// 模拟模块内部使用 CGS 单位，对外的引擎扭矩统一为 N·m（负值为发动机制动）
			EngineTorque = FMath::IsFinite(EngineOutput->Torque)
				               ? Chaos::TorqueCmToM(EngineOutput->Torque)
				               : 0.0f;
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

	// 控制通道闸门：在生产者产出后、写入物理线程前统一压平被禁通道，
	// 使闸门对所有输入生产者一致生效，禁用后的输入容器自然随网络复制
	ApplyControlGates();

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

	// 1) 广播本帧新建模块事件（物理线程 ActionTreeUpdates 收集的 NewlyCreatedModuleGuids）。
	//    广播后清空列表：模块新建是一次性事件，物理停摆帧（ParallelUpdate 未运行、
	//    输出容器保持上帧残留）不得重复广播
	for (const Chaos::FCreatedModule& AddedModule : VehiclePhysicsOutput->NewlyCreatedModuleGuids)
		BroadcastModuleAddedEvent(AddedModule.SimType, AddedModule.Guid, AddedModule.TreeIndex);
	VehiclePhysicsOutput->NewlyCreatedModuleGuids.Empty();

	// 2) 广播换挡/打滑/触底离散输出事件（游戏线程边沿检测，天然幂等）
	BroadcastDiscreteOutputEvents();

	// 3) 模块布局发生增删后，为网络状态历史的全部帧重建复制结构，
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
			TEXT("Gear %d  RPM %3.0f  Torque %4.0f N.m  Speed(km/h) %3.1f"),
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

const Chaos::FSimOutputData* USingularisMorphVehicleSimulationComponent::GetOutputData(const int32 ModuleGuid) const
{
	return VehiclePhysicsOutput
		       ? VehiclePhysicsOutput->GetOutputData(ModuleGuid)
		       : nullptr;
}

bool USingularisMorphVehicleSimulationComponent::IsLocallyControlled() const
{
	// 1) 显式声明优先：Control/Release 等外部流程通过 SetLocallyControlled 声明本地控制，
	//    启用网络物理预测时该声明同样必须生效（否则物理线程会把本地输入覆盖为零）
	if (bIsLocallyControlled)
		return true;

	// 2) NetworkPhysicsComponent 已判定为本地控制
	if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent && NetworkPhysicsComponent->IsLocallyControlled())
		return true;

	const AActor* Owner = GetOwner();
	if (!Owner) return false;

	// 3) SimulatedProxy 的输入来自服务器，绝不判定为本地
	if (Owner->GetLocalRole() == ROLE_SimulatedProxy)
		return false;

	// 4) 单机世界不存在远端权威，本机即本地控制
	if (const UWorld* World = GetWorld())
	{
		if (World->GetNetMode() == NM_Standalone)
			return true;
	}

	// 5) 本地玩家控制的 Pawn
	if (const APawn* Pawn = Cast<APawn>(Owner))
	{
		if (const APlayerController* PlayerController = Cast<APlayerController>(Pawn->GetController()))
			return PlayerController->IsLocalController();
	}

	return false;
}

float USingularisMorphVehicleSimulationComponent::GetEngineMaxRPM() const
{
	// 1) 遍历参与仿真的 SU 组件映射，按模块类型聚合引擎与电机的最大转速。
	//    手动注册路径（无 SU 映射）取不到来源，返回 0 由调用方自行回退
	auto EngineMaxRPM = 0.0f;
	auto MotorMaxRPM = 0.0f;

	for (const auto& Pair : ComponentToPhysicsObjects)
	{
		const USingularisMorphVehicleBaseComponent* SUComp = Cast<USingularisMorphVehicleBaseComponent>(
			Pair.Key.Get()
		);
		if (!SUComp) continue;

		switch (SUComp->GetModuleType())
		{
		case ESingularisMorphVehicleModuleType::Engine:
			if (const USingularisEngineComponent* EngineSU = Cast<USingularisEngineComponent>(SUComp))
			{
				// 与建模块时的 uint16 上限钳制保持一致，避免归一化分母大于实际可达转速
				const int32 ClampedEngineMaxRPM = FMath::Clamp(
					EngineSU->MaxRPM,
					0,
					static_cast<int32>(MAX_uint16)
				);
				EngineMaxRPM = FMath::Max(EngineMaxRPM, static_cast<float>(ClampedEngineMaxRPM));
			}
			break;

		case ESingularisMorphVehicleModuleType::Motor:
			if (const USingularisMotorComponent* MotorSU = Cast<USingularisMotorComponent>(SUComp))
			{
				const float SanitizedMotorMaxRPM = FMath::IsFinite(MotorSU->MaxRPM)
					                                   ? FMath::Max(MotorSU->MaxRPM, 0.0f)
					                                   : 0.0f;
				MotorMaxRPM = FMath::Max(MotorMaxRPM, SanitizedMotorMaxRPM);
			}
			break;

		default: break;
		}
	}

	// 2) 引擎占据原动机槽，无引擎时电机顶替（与建树的原动机槽策略一致），引擎优先返回
	return EngineMaxRPM > 0.0f ? EngineMaxRPM : MotorMaxRPM;
}

float USingularisMorphVehicleSimulationComponent::GetVehicleSpeed() const
{
	const AActor* Owner = GetOwner();
	if (!Owner) return 0.0f;

	// cm/s → km/h
	return FMath::Abs(Owner->GetVelocity().Size() * 0.036f);
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
	SlippingWheelGuids.Empty();
	BottomedOutSuspensionGuids.Empty();
	{
		FScopeLock AnimationLock(&ModuleAnimationSetupsLock);
		ModuleAnimationSetups.Empty();
	}
	StoredTreeUpdates = Chaos::FSimTreeUpdates();
	StoredTreeReparents.Reset();
	StoredWheelSpeedLimits.Reset();

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
	{
		FScopeLock AnimationLock(&ModuleAnimationSetupsLock);
		ModuleAnimationSetups.Empty();
	}
	NextConstructionIndex = 0;

	// 3) 清除待提交的重挂与限速请求：模块已全部移除，二者已无意义；
	//    删除请求仍与残余增删一并提交
	StoredTreeReparents.Reset();
	StoredWheelSpeedLimits.Reset();
	FinalizeModuleUpdates();

	// 4) 恢复全部被抑制的车轮碰撞：模块已全部清除，无车轮处于模拟状态
	RestoreAllWheelCollisionSuppression();
}

IPhysicsProxyBase* USingularisMorphVehicleSimulationComponent::GetPhysicsProxy() const
{
	// 1) 优先从适配器获取
	if (PhysicsAdapter)
	{
		if (IPhysicsProxyBase* Proxy = PhysicsAdapter->GetPhysicsProxy())
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
) const
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

void USingularisMorphVehicleSimulationComponent::ApplyControlGates()
{
	if (DisabledControlNames.IsEmpty()) return;

	TArray<FModuleInputValue>& InputValues = InputsContainer.AccessInputValues();

	for (const FName& ControlName : DisabledControlNames)
	{
		const int32* Index = InputNameMap.Find(ControlName);
		if (!Index || !InputValues.IsValidIndex(*Index)) continue;

		FModuleInputValue& InputValue = InputValues[*Index];

		// 按通道自身类型写入中性值，避免类型不匹配破坏下游模块读取
		switch (InputValue.GetValueType())
		{
		case EModuleInputValueType::MBoolean:
			InputValue.Set(FModuleInputValue(false));
			break;
		case EModuleInputValueType::MInteger:
			InputValue.Set(FModuleInputValue(0));
			break;
		case EModuleInputValueType::MAxis1D:
			InputValue.Set(FModuleInputValue(0.0));
			break;
		case EModuleInputValueType::MAxis2D:
			InputValue.Set(FModuleInputValue(FVector2D::ZeroVector));
			break;
		case EModuleInputValueType::MAxis3D:
			InputValue.Set(FModuleInputValue(FVector::ZeroVector));
			break;
		}
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
) const
{
	OnSimulationModuleAddedNativeEvent.Broadcast(ModuleType, Guid, TreeIndex);
	OnSimulationModuleAddedEvent.Broadcast(ModuleType, Guid, TreeIndex);
}

void USingularisMorphVehicleSimulationComponent::BroadcastModuleRemovedEvent(
	const FName& ModuleType,
	const int32 Guid,
	const int32 TreeIndex
) const
{
	OnSimulationModuleRemovedNativeEvent.Broadcast(ModuleType, Guid, TreeIndex);
	OnSimulationModuleRemovedEvent.Broadcast(ModuleType, Guid, TreeIndex);
}

void USingularisMorphVehicleSimulationComponent::BroadcastDiscreteOutputEvents()
{
	// 1) 换挡事件：挡位缓存咬合非空挡位且与上次事件挡位不同时广播。
	//    a) 空挡（0）为换挡过渡态，不构成换挡事件
	//    b) 首次收到挡位输出只建立基准不广播，避免变速箱初始挡位被误判为换挡
	//    c) 挡位缓存在本帧 ParallelUpdate 更新，边沿检测天然幂等，物理停摆帧不重复广播
	if (bHasGearData && CurrentGear != 0)
	{
		if (bHasGearShiftBaseline && LastEngagedGear != CurrentGear)
			OnGearShiftedEvent.Broadcast(LastEngagedGear, CurrentGear);

		LastEngagedGear = CurrentGear;
		bHasGearShiftBaseline = true;
	}

	// 2) 车轮打滑与悬挂触底：按模块输出做状态边沿检测
	if (!VehiclePhysicsOutput) return;

	for (Chaos::FSimOutputData* Output : VehiclePhysicsOutput->SimTreeOutputData)
	{
		if (!Output) continue;

		if (Output->IsSimType<Chaos::FWheelSimModule>())
			ProcessWheelSlipState(*static_cast<const Chaos::FWheelOutputData*>(Output));
		else if (Output->IsSimType<FSingularisMorphVehicleSuspensionSimModule>())
			ProcessSuspensionBottomOutState(*static_cast<const FSingularisMorphSuspensionOutputData*>(Output));
	}
}

void USingularisMorphVehicleSimulationComponent::ProcessWheelSlipState(const Chaos::FWheelOutputData& OutputData)
{
	const int32 Guid = OutputData.ModuleGuid;

	// 1) 双阈值滞回：未打滑时按起始阈值进入，打滑中按较低的解除阈值退出，
	//    避免侧滑角在单一阈值附近抖动导致事件反复翻转；
	//    解除阈值配置高于起始阈值时按起始阈值执行，防止打滑状态永久滞留。
	//    腾空车轮不判打滑（侧滑角失去物理意义）
	const float StartThreshold = FMath::Max(FMath::DegreesToRadians(SlipStartAngleThreshold), 0.0f);
	const float StopThreshold = FMath::Min(FMath::DegreesToRadians(SlipStopAngleThreshold), StartThreshold);

	const float AbsSlipAngle = FMath::Abs(OutputData.SlipAngle);
	const bool bWasSlipping = SlippingWheelGuids.Contains(Guid);
	const bool bIsSlipping = OutputData.bTouchingGround
		&& AbsSlipAngle >= (bWasSlipping ? StopThreshold : StartThreshold);

	// 2) 状态翻转时更新记录并广播对应事件；无翻转时静默返回
	if (bIsSlipping == bWasSlipping) return;

	if (bIsSlipping)
	{
		SlippingWheelGuids.Add(Guid);
		OnWheelSlipStartedEvent.Broadcast(Guid);
	}
	else
	{
		SlippingWheelGuids.Remove(Guid);
		OnWheelSlipStoppedEvent.Broadcast(Guid);
	}
}

void USingularisMorphVehicleSimulationComponent::ProcessSuspensionBottomOutState(
	const FSingularisMorphSuspensionOutputData& OutputData
)
{
	const int32 Guid = OutputData.ModuleGuid;

	// 1) 触底判定：弹簧压缩量达到最大压缩行程（MaxRaise）减去判定容差。
	//    压缩量以完全伸展为 0、MaxRaise + MaxDrop 为上限，腾空无命中时退回 0 不会误判；
	//    MaxRaise 非正（未配置行程）或不超过容差时不判定
	const float Tolerance = FMath::Max(SuspensionBottomOutTolerance, 0.0f);
	const bool bIsBottomedOut = OutputData.bWheelHit
		&& OutputData.MaxRaise > Tolerance
		&& OutputData.SpringDisplacement >= OutputData.MaxRaise - Tolerance;

	// 2) 触底为一次性冲击事件：仅上升沿广播，脱离触底只复位记录不产生配对事件
	if (!bIsBottomedOut)
	{
		BottomedOutSuspensionGuids.Remove(Guid);
		return;
	}

	if (BottomedOutSuspensionGuids.Contains(Guid)) return;

	BottomedOutSuspensionGuids.Add(Guid);
	OnSuspensionBottomedOutEvent.Broadcast(Guid);
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

void USingularisMorphVehicleSimulationComponent::SuppressWheelCollision(UPrimitiveComponent* WheelComponent)
{
	// 1) 无效组件或已抑制的目标直接跳过（幂等）
	if (!IsValid(WheelComponent)) return;

	const TWeakObjectPtr<UPrimitiveComponent> Key{WheelComponent};
	if (SuppressedWheelCollisions.Contains(Key)) return;

	// 2) 记录原始碰撞配置，供分离或拆除时精确恢复（只记录本方法会改动的字段）
	FSingularisMorphWheelCollisionSnapshot Snapshot;
	Snapshot.CollisionEnabled = WheelComponent->GetCollisionEnabled();
	Snapshot.CollisionResponses = CaptureCollisionResponses(*WheelComponent);
	SuppressedWheelCollisions.Add(Key, Snapshot);

	// 3) 施加模拟期配置：启用模式解除物理接触，逐通道响应保留所需查询
	ApplyCollisionResponses(*WheelComponent, SimulatedWheelCollisionResponses);
	WheelComponent->SetCollisionEnabled(SimulatedWheelCollisionEnabled);
}

void USingularisMorphVehicleSimulationComponent::RestoreWheelCollision(
	const TWeakObjectPtr<UPrimitiveComponent>& WheelComponent
)
{
	// 1) 未记录的目标不需要恢复（从未被抑制）
	FSingularisMorphWheelCollisionSnapshot Snapshot;
	if (!SuppressedWheelCollisions.RemoveAndCopyValue(WheelComponent, Snapshot)) return;

	// 2) 组件已销毁：无法恢复，仅移除记录
	UPrimitiveComponent* Component = WheelComponent.Get();
	if (!IsValid(Component)) return;

	// 3) 逐项写回原始配置
	ApplyCollisionResponses(*Component, Snapshot.CollisionResponses);
	Component->SetCollisionEnabled(Snapshot.CollisionEnabled);
}

void USingularisMorphVehicleSimulationComponent::ReconcileWheelCollisionSuppression(
	const TSet<UPrimitiveComponent*>& ActiveWheelComponents
)
{
	// 1) 抑制当前活动但尚未抑制的车轮组件
	for (UPrimitiveComponent* WheelComponent : ActiveWheelComponents)
		SuppressWheelCollision(WheelComponent);

	// 2) 恢复已记录但不再活动（或已销毁）的车轮组件
	TArray<TWeakObjectPtr<UPrimitiveComponent>> StaleEntries;
	StaleEntries.Reserve(SuppressedWheelCollisions.Num());
	for (const auto& Pair : SuppressedWheelCollisions)
	{
		UPrimitiveComponent* Tracked = Pair.Key.Get();
		if (!Tracked || !ActiveWheelComponents.Contains(Tracked))
			StaleEntries.Add(Pair.Key);
	}

	for (const TWeakObjectPtr<UPrimitiveComponent>& Entry : StaleEntries)
		RestoreWheelCollision(Entry);
}

void USingularisMorphVehicleSimulationComponent::RestoreAllWheelCollisionSuppression()
{
	if (SuppressedWheelCollisions.IsEmpty()) return;

	for (const auto& Pair : SuppressedWheelCollisions)
	{
		if (UPrimitiveComponent* Component = Pair.Key.Get())
		{
			ApplyCollisionResponses(*Component, Pair.Value.CollisionResponses);
			Component->SetCollisionEnabled(Pair.Value.CollisionEnabled);
		}
	}

	SuppressedWheelCollisions.Empty();
}
