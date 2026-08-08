#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include <Engine/World.h>
#include <GameFramework/WorldSettings.h>
#include <PhysicsEngine/BodyInstance.h>
#include <PhysicsProxy/ClusterUnionPhysicsProxy.h>
#include <PhysicsProxy/SingleParticlePhysicsProxy.h>
#include <SimModule/SimulationModuleBase.h>

#include "Components/SingularisMorphVehicleClutchSUComponent.h"
#include "Components/SingularisMorphVehicleEngineSUComponent.h"
#include "Components/SingularisMorphVehicleSUComponent.h"
#include "Components/SingularisMorphVehicleTransmissionSUComponent.h"
#include "Core/SingularisMorphVehicleSimulationCU.h"
#include "Interfaces/SingularisMorphVehicleBaseInterface.h"
#include "Objects/SingularisMorphVehiclePhysicsAdapter.h"
#include "Subsystems/SingularisMorphVehicleMappingSubsystem.h"
#include "Subsystems/SingularisMorphVehicleSchedulerSubsystem.h"

DEFINE_LOG_CATEGORY(LogSingularisMorphBase);

USingularisMorphVehicleSimulationComponent::USingularisMorphVehicleSimulationComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.bCanEverTick = true;

	bAutoActivate = true;

	SuspensionTraceCollisionResponses.SetAllChannels(ECR_Block);
	SuspensionTraceCollisionResponses.SetResponse(ECC_Vehicle, ECR_Ignore);
	SuspensionTraceCollisionResponses.SetResponse(ECC_EngineTraceChannel1, ECR_Ignore);
	
	bUsingNetworkPhysicsPrediction = Chaos::FPhysicsSolverBase::IsNetworkPhysicsPredictionEnabled();
	CurrentAsyncDataType = AsyncInvalid;
}

void USingularisMorphVehicleSimulationComponent::BeginPlay()
{
	Super::BeginPlay();
}

void USingularisMorphVehicleSimulationComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction
)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

bool USingularisMorphVehicleSimulationComponent::ShouldCreatePhysicsState() const
{
	return true;
}

void USingularisMorphVehicleSimulationComponent::OnCreatePhysicsState()
{
	Super::OnCreatePhysicsState();

	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("=== OnCreatePhysicsState: PhysicsAdapter=%s ==="),
		PhysicsAdapter ? *PhysicsAdapter->GetClass()->GetName() : TEXT("null")
	);

	// 1) 构造上下文并初始化适配器（若用户在编辑器中配置了适配器实例）
	if (PhysicsAdapter)
	{
		const FSingularisMorphVehiclePhysicsAdapterContext Context{this};
		PhysicsAdapter->Initialize(Context);
	}

	// 2) 创建物理线程端模拟
	CreateVehicleSimulation();
	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("=== OnCreatePhysicsState: VehicleSimulationPT=%s, Proxy=%s ==="),
		VehicleSimulationPT.IsValid() ? TEXT("valid") : TEXT("null"),
		GetPhysicsProxy() ? TEXT("valid") : TEXT("null")
	);

	// 2a) 若需禁止休眠则设置集群粒子为永不睡眠
	if (bKeepVehicleAwake)
	{
		if (auto* Proxy = static_cast<Chaos::FClusterUnionPhysicsProxy*>(GetPhysicsProxy()))
		{
			if (auto* Particle = Proxy->GetParticle_External())
				Particle->SetSleepType(Chaos::ESleepType::NeverSleep);
		}
	}

	// 3) 自忽略（悬挂射线不命中自身）
	if (AActor* Owner = GetOwner()) ActorsToIgnore.Add(Owner);
}

void USingularisMorphVehicleSimulationComponent::OnDestroyPhysicsState()
{
	// 1) 销毁物理线程端模拟
	DestroyVehicleSimulation();

	// 2) 终止适配器（解绑事件）
	if (PhysicsAdapter) PhysicsAdapter->Terminate();

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

	// 通知旧模块终止：释放悬挂约束等外部资源。
	// FSimModuleTree::DeleteNode 只 delete 模块对象，不会调用 OnTermination_External，
	// 若不在此释放，每次重建都会泄漏物理悬挂约束，
	// 旧约束会持续对集群根粒子施加力，导致抖动与车轮飞散。
	if (VehicleSimulationPT)
	{
		if (Chaos::FSimModuleTree* SimTree = VehicleSimulationPT->AccessSimComponentTree().Get())
		{
			for (int32 N = 0; N < SimTree->GetNumNodes(); N++)
			{
				if (Chaos::ISimulationModuleBase* Mod = SimTree->GetNode(N).SimModule)
				{
					if (Mod->GetGuid() == ModuleGuid)
					{
						Mod->SetAnimationEnabled(false);
						Mod->SetStateFlags(Chaos::eSimModuleState::Disabled);
						Mod->OnTermination_External();
						break;
					}
				}
			}
		}
	}

	StoredTreeUpdates.RemoveNode(ModuleGuid);
}

void USingularisMorphVehicleSimulationComponent::FinalizeModuleUpdates()
{
	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[FinalizeModuleUpdates] VehicleSimulationPT=%s, StoredTreeUpdates pending adds=%d"),
		VehicleSimulationPT.IsValid() ? TEXT("valid") : TEXT("null"),
		StoredTreeUpdates.GetNewModules().Num()
	);

	if (VehicleSimulationPT)
		VehicleSimulationPT->AppendTreeUpdates(&StoredTreeUpdates);

	StoredTreeUpdates = Chaos::FSimTreeUpdates();

	// 设置物理阻尼（与旧代码 UpdatePhysicalProperties 一致，抑制悬挂振荡）
	UpdatePhysicalProperties();
}

void USingularisMorphVehicleSimulationComponent::RebuildFromSnapshot(
	const FSingularisMorphVehiclePhysicsAdapterSnapshot& Snapshot
)
{
	if (!VehicleSimulationPT) return;

	// 解析映射子系统：SU 组件的查询统一由映射完成
	const UWorld* World = GetWorld();
	const USingularisMorphVehicleMappingSubsystem* Subsystem =
		World ? World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>() : nullptr;
	if (!Subsystem) return;

	// 0) 底盘守卫：映射结果中必须存在 Chassis 类型的 SU 才能重建。
	//    车身件脱离集群（载具解体）后，直接清除全部模拟模块：
	//    释放悬挂约束、移除树节点、清空缓存，剩余部件作为独立碎片由物理引擎接管。
	bool bHasChassis = false;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;
		for (USingularisMorphVehicleSUComponent* SUComp :
			Subsystem->FindSUComponents(Entity.PrimitiveComponent))
		{
			if (SUComp && SUComp->GetModuleType() == ESingularisMorphVehicleModuleType::Chassis)
			{
				bHasChassis = true;
				break;
			}
		}
		if (bHasChassis) break;
	}
	if (!bHasChassis)
	{
		UE_LOG(
			LogSingularisMorphBase,
			Warning,
			TEXT("[RebuildFromSnapshot] Chassis not found in snapshot (%d entities) - clearing all simulation modules"),
			Snapshot.Entities.Num()
		);

		// 1) 逐个移除旧模块（内部会释放悬挂约束等外部资源）
		TArray<int32> ExistingGuids;
		for (const auto& Pair : ComponentToPhysicsObjects)
			ExistingGuids.Add(Pair.Value.Guid);
		for (int32 Guid : ExistingGuids)
			RemoveSimulationModule(Guid);

		// 2) 清空所有缓存，保证幂等
		ComponentToPhysicsObjects.Empty();
		PhysicsGuidToComponent.Empty();
		ModuleAnimationSetups.Empty();
		NextConstructionIndex = 0;

		// 3) 提交删除到物理线程
		UpdatePhysicalProperties();
		FinalizeModuleUpdates();
		return;
	}

	AActor* Owner = GetOwner();
	if (!IsValid(Owner)) return;

	const FTransform ReferenceTransform = PhysicsAdapter
		                                      ? PhysicsAdapter->GetReferenceTransform()
		                                      : FTransform::Identity;

	// 1) 移除旧模块：收集已有 GUID 并全部标记删除
	TArray<int32> ExistingGuids;
	for (const auto& Pair : ComponentToPhysicsObjects)
		ExistingGuids.Add(Pair.Value.Guid);

	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[RebuildFromSnapshot] Removing %d old modules, adding %d from snapshot"),
		ExistingGuids.Num(),
		Snapshot.Entities.Num()
	);

	for (int32 Guid : ExistingGuids)
		RemoveSimulationModule(Guid);

	// 2) 清空所有缓存，保证幂等
	ComponentToPhysicsObjects.Empty();
	PhysicsGuidToComponent.Empty();
	ModuleAnimationSetups.Empty();
	NextConstructionIndex = 0;

	// 2a) 缓存根物理对象（首次或重建时刷新）
	//     模块构造期需要 RootPhysicsObject 进行 OnConstruction_External 初始化
	if (RootPhysicsObject == nullptr)
		CacheRootPhysicsObject(GetPhysicsProxy());

	// 3) 构建 物理组件 → Entity 快速查找表
	//    实体只描述物理信息；SU 组件由各 Pass 通过映射子系统在实体上展开。
	TMap<TObjectPtr<UPrimitiveComponent>, const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity*>
		EntityMap;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (Entity.PrimitiveComponent)
			EntityMap.Add(Entity.PrimitiveComponent, &Entity);
	}

	// 4) 核心 Lambda：将单个 SU 注册到模拟树。
	//    统一通过 SU 的 DrivenComponent 解析物理组件：
	//    - 物理组件在快照实体中（集群子粒子）→ 复用其粒子索引与集群变换；
	//    - 不在快照中（纯仿真模块）→ 粒子无效，变换取自身相对变换。
	//    悬挂的 DrivenComponent 由资产配置决定：无悬挂实体时指向车轮网格
	//    （复用车轮的粒子/位置），有悬挂实体时指向悬挂自身。
	TMap<ESingularisMorphVehicleModuleType, int32> TypeToTreeIndex;
	TSet<USingularisMorphVehicleSUComponent*> ProcessedComponents;

	auto AddEntity = [&](USingularisMorphVehicleSUComponent* SUComp, const int32 ParentIndex) -> int32
	{
		if (!SUComp || ProcessedComponents.Contains(SUComp)) return INDEX_NONE;

		Chaos::ISimulationModuleBase* CoreModule = SUComp->CreateNewCoreModule();
		if (!CoreModule) return INDEX_NONE;

		UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
			SUComp->DrivenComponent.GetComponent(Owner)
		);
		if (!ProxyComp)
		{
			delete CoreModule;
			return INDEX_NONE;
		}

		// 物理组件若在快照实体中，复用其粒子索引与 ChildToParent；否则为纯仿真模块
		const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity* Entity = EntityMap[ProxyComp];

		FTransform CompTransform = ProxyComp->GetComponentTransform().GetRelativeTransform(
			ReferenceTransform
		);
		CompTransform = SUComp->TransformOffset * CompTransform;

		const int32 TransformIndex = NextConstructionIndex++;

		const int32 ModuleIdx = AddModuleToTree(
			CoreModule,
			CompTransform,
			ParentIndex,
			TransformIndex,
			Entity ? Chaos::FUniqueIdx(Entity->ParticleIndex) : Chaos::FUniqueIdx(),
			Entity ? Entity->ChildToParent : FTransform::Identity
		);

		if (ModuleIdx != INDEX_NONE)
		{
			SUComp->SetModuleGuid(CoreModule->GetGuid());
			FSingularisMorphVehicleComponentData CompData;
			CompData.Guid = CoreModule->GetGuid();
			CompData.ProxyComponentToAnimate = ProxyComp;
			ComponentToPhysicsObjects.Add(SUComp, CompData);
			PhysicsGuidToComponent.Add(CoreModule->GetGuid(), SUComp);
			TypeToTreeIndex.Add(SUComp->GetModuleType(), ModuleIdx);
			ProcessedComponents.Add(SUComp);
		}

		return ModuleIdx;
	};

	// 5) 按确定性顺序重建物理模拟树
	//    同一物理组件可展开出多个 SU（映射列表），各 Pass 独立按类型过滤。

	// 5a) Pass 1: Chassis → root
	int32 ChassisIndex = INDEX_NONE;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;
		for (USingularisMorphVehicleSUComponent* SUComp :
			Subsystem->FindSUComponents(Entity.PrimitiveComponent))
		{
			if (SUComp && SUComp->GetModuleType() == ESingularisMorphVehicleModuleType::Chassis)
			{
				ChassisIndex = AddEntity(SUComp, INDEX_NONE);
				break;
			}
		}
		if (ChassisIndex != INDEX_NONE) break;
	}

	// 5b) Pass 2: Engine → Clutch → Transmission 动力链（通过 Linked 引用串联）
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;
		for (USingularisMorphVehicleSUComponent* SUComp :
			Subsystem->FindSUComponents(Entity.PrimitiveComponent))
		{
			if (!SUComp || SUComp->GetModuleType() != ESingularisMorphVehicleModuleType::Engine) continue;

			const int32 EngineIndex = AddEntity(SUComp, ChassisIndex);
			if (EngineIndex == INDEX_NONE) continue;

			auto* EngineSU = Cast<USingularisMorphVehicleEngineSUComponent>(SUComp);
			if (!EngineSU) continue;

			if (auto* ClutchSU = Cast<USingularisMorphVehicleClutchSUComponent>(
				EngineSU->LinkedClutch.GetComponent(Owner)
			))
			{
				const int32 ClutchIndex = AddEntity(ClutchSU, EngineIndex);

				if (auto* TransSU = Cast<USingularisMorphVehicleTransmissionSUComponent>(
					ClutchSU->LinkedTransmission.GetComponent(Owner)
				))
				{
					AddEntity(TransSU, ClutchIndex);
				}
			}
		}
	}

	// 5c) Pass 3: Suspension → Chassis
	//     记录 物理组件 → 悬挂索引，供同一物理组件上的轮子查找父节点
	TMap<TObjectPtr<UPrimitiveComponent>, int32> SuspensionIndexByComponent;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;
		for (USingularisMorphVehicleSUComponent* SUComp :
			Subsystem->FindSUComponents(Entity.PrimitiveComponent))
		{
			if (SUComp && SUComp->GetModuleType() == ESingularisMorphVehicleModuleType::Suspension)
			{
				const int32 SuspIndex = AddEntity(SUComp, ChassisIndex);
				if (SuspIndex != INDEX_NONE)
					SuspensionIndexByComponent.FindOrAdd(Entity.PrimitiveComponent) = SuspIndex;
			}
		}
	}

	// 5d) Pass 4: Wheel → 同一物理组件上的悬挂下（无则挂 Chassis 下）
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;
		for (USingularisMorphVehicleSUComponent* SUComp :
			Subsystem->FindSUComponents(Entity.PrimitiveComponent))
		{
			if (!SUComp || SUComp->GetModuleType() != ESingularisMorphVehicleModuleType::Wheel) continue;

			int32 ParentIdx = ChassisIndex;
			if (const int32* FoundIdx = SuspensionIndexByComponent.Find(Entity.PrimitiveComponent))
				ParentIdx = *FoundIdx;

			AddEntity(SUComp, ParentIdx);
		}
	}

	// 5e) Pass 5: 其余模块（Aerofoil、Thruster 等）→ Chassis
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.PrimitiveComponent) continue;
		for (USingularisMorphVehicleSUComponent* SUComp :
			Subsystem->FindSUComponents(Entity.PrimitiveComponent))
		{
			if (!SUComp) continue;
			const ESingularisMorphVehicleModuleType Type = SUComp->GetModuleType();
			if (Type == ESingularisMorphVehicleModuleType::Chassis ||
				Type == ESingularisMorphVehicleModuleType::Engine ||
				Type == ESingularisMorphVehicleModuleType::Clutch ||
				Type == ESingularisMorphVehicleModuleType::Transmission ||
				Type == ESingularisMorphVehicleModuleType::Suspension ||
				Type == ESingularisMorphVehicleModuleType::Wheel)
				continue;

			AddEntity(SUComp, ChassisIndex);
		}
	}

	// 6) 批量提交到物理线程
	UpdatePhysicalProperties();
	FinalizeModuleUpdates();
}

void USingularisMorphVehicleSimulationComponent::UpdatePhysicalProperties()
{
	IPhysicsProxyBase* Proxy = GetPhysicsProxy();
	if (!Proxy || Proxy->GetType() != EPhysicsProxyType::ClusterUnionProxy) return;

	auto CUProxy = static_cast<Chaos::FClusterUnionPhysicsProxy*>(Proxy);
	Chaos::FPBDRigidsSolver* Solver = Proxy->GetSolver<Chaos::FPBDRigidsSolver>();
	if (!Solver) return;

	Solver->EnqueueCommandImmediate(
		[CUProxy, this]() mutable
		{
			if (auto* Particle = CUProxy->GetParticle_Internal())
			{
				Particle->SetLinearEtherDrag(LinearDamping);
				Particle->SetAngularEtherDrag(AngularDamping);
			}
		}
	);
}

void USingularisMorphVehicleSimulationComponent::SetUpdatedComponent(
	USceneComponent* InUpdatedComponent
)
{
	// 兼容 PawnMovementComponent 接口
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

void USingularisMorphVehicleSimulationComponent::Update(const float DeltaTime) {}

void USingularisMorphVehicleSimulationComponent::PreTickGT(const float DeltaTime)
{
	// 从适配器拉取完整快照并全量重建物理模拟树（Pull Model）
	// 仅当适配器就绪且存在待处理的拓扑变更时才执行重建
	if (PhysicsAdapter && PhysicsAdapter->IsReady() && PhysicsAdapter->IsDirty())
	{
		const FSingularisMorphVehiclePhysicsAdapterSnapshot Snapshot = PhysicsAdapter->ConsumeSnapshot();
		RebuildFromSnapshot(Snapshot);
	}
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

	// 1) 清理上一帧输出并预留空间
	VehiclePhysicsOutput->Clean();
	VehiclePhysicsOutput->NewlyCreatedModuleGuids = ModuleEvents.ModuleEvents;

	const int32 NumItems = CurrentAsyncOutput->VehicleSimOutput.SimTreeOutputData.Num();
	VehiclePhysicsOutput->SimTreeOutputData.Reserve(NumItems);

	// 2) 对每个模拟输出数据执行插值
	for (int32 I = 0; I < NumItems; ++I)
	{
		Chaos::FSimOutputData* CurrentSimData = CurrentAsyncOutput->
		                                        VehicleSimOutput.SimTreeOutputData[I];
		if (!CurrentSimData) continue;

		VehiclePhysicsOutput->SimTreeOutputData.EmplaceAt(
			I,
			CurrentSimData->MakeNewData()
		);

		if (NextAsyncOutput)
		{
			if (const Chaos::FSimOutputData* NextSimData = FindModuleOutputFromGuid(
				NextAsyncOutput->VehicleSimOutput,
				CurrentSimData->ModuleGuid
			))
			{
				VehiclePhysicsOutput->SimTreeOutputData[I]->Lerp(
					*CurrentSimData,
					*NextSimData,
					OutputInterpolationAlpha
				);
			}
		}
		else
		{
			VehiclePhysicsOutput->SimTreeOutputData[I]->Lerp(
				*CurrentSimData,
				*CurrentSimData,
				0.0f
			);
		}
	}

	// 3) 分发输出数据到各 SU Component
	int32 CallbackCount = 0;
	for (int32 I = 0; I < NumItems; ++I)
	{
		if (!VehiclePhysicsOutput->SimTreeOutputData[I]) continue;

		const int32 Guid = VehiclePhysicsOutput->SimTreeOutputData[I]->ModuleGuid;

		if (const TWeakObjectPtr<UActorComponent>* Component = PhysicsGuidToComponent.Find(
			Guid
		))
		{
			if (auto* BaseSU = Cast<ISingularisMorphVehicleBaseInterface>(
				Component->Get()
			))
			{
				BaseSU->OnOutputReady(VehiclePhysicsOutput->SimTreeOutputData[I]);
				++CallbackCount;
			}
		}

		// 3a) 将仿真动画数据从 FSimOutputData 拷贝到 ModuleAnimationSetups
		if (Chaos::FSimOutputData* ModuleOutput = VehiclePhysicsOutput->SimTreeOutputData[I])
		{
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
	}
	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[ParallelUpdate] NumItems=%d, OnOutputReady called=%d, PhysicsGuidToComponent size=%d"),
		NumItems,
		CallbackCount,
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

	AsyncInput->SetVehicle(this);
	AsyncInput->Proxy = Proxy;

	// 1) 禁止休眠
	AsyncInput->PhysicsInputs.NetworkInputs.VehicleInputs.KeepAwake = bKeepVehicleAwake;

	// 2) 时间膨胀
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

	// 3) 悬挂射线参数（地面检测必需）
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

	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[SimComp] ProduceInput: PhysicsStep=%d, Proxy=%s, KeepAwake=%d"),
		PhysicsStep,
		Proxy ? TEXT("valid") : TEXT("null"),
		bKeepVehicleAwake
	);
}

void USingularisMorphVehicleSimulationComponent::PostUpdate()
{
	/*// 将模拟输出应用到视觉组件变换（悬挂压缩、车轮旋转等）
	int32 UpdatedCount = 0;
	for (int32 AnimIndex = 0; AnimIndex < ModuleAnimationSetups.Num(); ++AnimIndex)
	{
		const FSingularisMorphModuleAnimationSetup& AnimSetup = ModuleAnimationSetups[AnimIndex];
		const int32 ModuleGuid = AnimSetup.ModuleGUID;
		if (ModuleGuid == INDEX_NONE) continue;

		USceneComponent* ComponentToAnimate = nullptr;
		for (const auto& Pair : ComponentToPhysicsObjects)
		{
			if (Pair.Value.Guid == ModuleGuid)
			{
				ComponentToAnimate = Pair.Value.ProxyComponentToAnimate;
				break;
			}
		}
		if (!ComponentToAnimate) continue;

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
		++UpdatedCount;
	}
	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[PostUpdate] ModuleAnimationSetups=%d, Updated=%d"),
		ModuleAnimationSetups.Num(),
		UpdatedCount
	);*/
}

void USingularisMorphVehicleSimulationComponent::FinalizeSimCallbackData(
	FSingularisMorphChaosSimModuleManagerAsyncInput& Input
) {}

void USingularisMorphVehicleSimulationComponent::ShowDebugInfo(
	AHUD* HUD,
	UCanvas* Canvas,
	const FDebugDisplayInfo& DisplayInfo,
	float& YL,
	float& YPos
) {}

const FTransform& USingularisMorphVehicleSimulationComponent::GetComponentTransform() const
{
	if (const AActor* Owner = GetOwner())
	{
		if (const USceneComponent* RootComp = Owner->GetRootComponent())
			return RootComp->GetComponentTransform();
	}

	return FTransform::Identity;
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

	// 4) 创建模拟模块树
	SimulationModuleTree = MakeUnique<Chaos::FSimModuleTree>();
	SimulationModuleTree->SetAnimationEnabled(true);
	SimulationModuleTree->SetSimTreeProcessingOrder(SimulationTreeProcessingOrder);

	// 5) 物理线程接管模块树所有权
	VehicleSimulationPT->Initialize(SimulationModuleTree);

	// 6) 注册到仿真管理器
	USingularisMorphVehicleSchedulerSubsystem* SchedulerSubsystem =
		GetWorld()->GetSubsystem<USingularisMorphVehicleSchedulerSubsystem>();
	if (!IsValid(SchedulerSubsystem)) return;
	SchedulerSubsystem->RegisterVehicleComponent(this);
}

void USingularisMorphVehicleSimulationComponent::DestroyVehicleSimulation()
{
	// 1) 从仿真管理器注销
	USingularisMorphVehicleSchedulerSubsystem* SchedulerSubsystem =
		GetWorld()->GetSubsystem<USingularisMorphVehicleSchedulerSubsystem>();
	if (!IsValid(SchedulerSubsystem)) return;
	SchedulerSubsystem->UnregisterVehicleComponent(this);

	// 2) 清理资源
	VehicleSimulationPT.Reset();
	SimulationModuleTree.Reset();
	VehiclePhysicsOutput.Reset();
	CachedPhysicsProxy = nullptr;
}

void USingularisMorphVehicleSimulationComponent::ActionTreeUpdates(
	Chaos::FSimTreeUpdates* NextTreeUpdates
)
{
	if (!NextTreeUpdates || !VehicleSimulationPT) return;

	VehicleSimulationPT->AppendTreeUpdates(NextTreeUpdates);
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

	return nullptr;
}

int32 USingularisMorphVehicleSimulationComponent::GenerateNewGuid()
{
	static int32 Val = 0;
	return Val++;
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
	const FTransform& PhysicalTransform
)
{
	if (!CoreModule) return INDEX_NONE;

	// 1) 从 PT 端获取模拟树引用（Initialize 后树所有权已转移）
	Chaos::FSimModuleTree* SimTree = VehicleSimulationPT
		                                 ? VehicleSimulationPT->AccessSimComponentTree().Get()
		                                 : SimulationModuleTree.Get();

	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[AddSimModule] SimTree=%s, ParentIndex=%d"),
		SimTree ? TEXT("valid") : TEXT("null"),
		ParentIndex
	);

	if (!SimTree) return INDEX_NONE;

	// 2) 设置模块到模拟树
	CoreModule->SetSimModuleTree(SimTree);

	// 3) 在树更新中注册节点
	const int32 TreeIndex = StoredTreeUpdates.AddNodeBelow(ParentIndex, CoreModule);
	UE_LOG(LogSingularisMorphBase, Log, TEXT("[AddSimModule] AddNodeBelow returned TreeIndex=%d"), TreeIndex);
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
	InitialTransform.SetLocation(InitialTransform.GetLocation());

	CoreModule->SetIntactTransform(FTransform::Identity);
	CoreModule->SetClusteredTransform(InitialTransform);
	CoreModule->SetClustered(true);
	CoreModule->SetInitialParticleTransform(InitialTransform);
	CoreModule->SetComponentTransform(ComponentTransform);

	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[AddSimModule] SetInitialParticleTransform=%s, SetComponentTransform=%s"),
		*InitialTransform.ToHumanReadableString(),
		*ComponentTransform.ToHumanReadableString()
	);

	// 覆盖 ClusteredTransform 为仅含位移的版本（与旧代码行为一致）
	FTransform ClusterredTransform(FQuat::Identity, InitialTransform.GetLocation());
	CoreModule->SetClusteredTransform(ClusterredTransform);

	// 6a) 动画绑定：为每个模块创建 AnimationSetup，供 PostUpdate 更新渲染变换
	CoreModule->SetAnimationData(
		CoreModule->GetBoneName(),
		CoreModule->GetAnimationOffset(),
		ModuleAnimationSetups.Num()
	);
	{
		FSingularisMorphModuleAnimationSetup AnimSetup(
			CoreModule->GetBoneName(),
			TransformIndex,
			CoreModule->GetGuid()
		);
		AnimSetup.InitialRotOffset = InitialTransform.GetRotation();
		AnimSetup.InitialLocOffset = InitialTransform.GetTranslation();
		ModuleAnimationSetups.Add(AnimSetup);
	}

	// 6b) 通知模块物理对象已就绪（与旧代码 AddModuleToTree 的 OnConstruction_External 一致）
	if (RootPhysicsObject)
		CoreModule->OnConstruction_External(RootPhysicsObject);

	return TreeIndex;
}

void USingularisMorphVehicleSimulationComponent::OnSimulationModuleInitialized(
	const FName& ModuleType,
	const int32 Guid,
	const int32 TreeIndex
)
{
	BroadcastModuleAddedEvent(ModuleType, Guid, TreeIndex);
}

void USingularisMorphVehicleSimulationComponent::OnSimulationModuleRemovedCallback(
	const FName& ModuleType,
	const int32 Guid,
	const int32 TreeIndex
)
{
	BroadcastModuleRemovedEvent(ModuleType, Guid, TreeIndex);
}
