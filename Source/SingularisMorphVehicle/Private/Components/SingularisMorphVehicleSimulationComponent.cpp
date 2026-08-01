#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include <Engine/World.h>
#include <PhysicsEngine/BodyInstance.h>
#include <PhysicsProxy/ClusterUnionPhysicsProxy.h>
#include <PhysicsProxy/SingleParticlePhysicsProxy.h>

#include "Components/SingularisMorphVehicleClutchSUComponent.h"
#include "Components/SingularisMorphVehicleEngineSUComponent.h"
#include "Components/SingularisMorphVehicleSUComponent.h"
#include "Components/SingularisMorphVehicleSuspensionSUComponent.h"
#include "Components/SingularisMorphVehicleTransmissionSUComponent.h"
#include "Components/SingularisMorphVehicleWheelSUComponent.h"
#include "Core/SingularisMorphVehicleSimulationCU.h"
#include "Interfaces/SingularisMorphVehicleBaseInterface.h"
#include "Objects/SingularisMorphVehiclePhysicsAdapter.h"
#include "Subsystems/SingularisMorphVehicleSchedulerSubsystem.h"

DEFINE_LOG_CATEGORY(LogSingularisMorphBase);

USingularisMorphVehicleSimulationComponent::USingularisMorphVehicleSimulationComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.bCanEverTick = true;

	bAutoActivate = true;

	bUsingNetworkPhysicsPrediction = Chaos::FPhysicsSolverBase::IsNetworkPhysicsPredictionEnabled();
	CurrentAsyncDataType = AsyncDefault;
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
	if (Snapshot.IsEmpty()) return;
	if (!VehicleSimulationPT) return;

	AActor* Owner = GetOwner();
	if (!IsValid(Owner)) return;

	const FTransform ReferenceTransform = PhysicsAdapter
		                                      ? PhysicsAdapter->GetReferenceTransform()
		                                      : FTransform::Identity;

	// 1) 移除旧模块：收集已有 GUID 并全部标记删除
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

	// 3) 构建 SUComponent → Entity 快速查找表
	TMap<TObjectPtr<USingularisMorphVehicleSUComponent>, const FSingularisMorphVehiclePhysicsAdapterSnapshotEntity*>
		EntityMap;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (Entity.SUComponent)
			EntityMap.Add(Entity.SUComponent, &Entity);
	}

	// 4) 核心 Lambda：将快照实体注册到模拟树
	TMap<ESingularisMorphVehicleModuleType, int32> TypeToTreeIndex;
	TSet<USingularisMorphVehicleSUComponent*> ProcessedComponents;

	auto AddEntity = [&](USingularisMorphVehicleSUComponent* SUComp, const int32 ParentIndex) -> int32
	{
		if (!SUComp || ProcessedComponents.Contains(SUComp)) return INDEX_NONE;

		const auto* Found = EntityMap.Find(SUComp);
		if (!Found) return INDEX_NONE;
		const auto& Entity = **Found;

		Chaos::ISimulationModuleBase* CoreModule = SUComp->CreateNewCoreModule();
		if (!CoreModule) return INDEX_NONE;

		UPrimitiveComponent* ProxyComp = Cast<UPrimitiveComponent>(
			SUComp->ProxyComponent.GetComponent(Owner)
		);
		if (!ProxyComp)
		{
			delete CoreModule;
			return INDEX_NONE;
		}

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
			Chaos::FUniqueIdx(Entity.ParticleIndex),
			Entity.ChildToParent
		);

		if (ModuleIdx != INDEX_NONE)
		{
			SUComp->SetModuleGuid(CoreModule->GetGuid());
			TypeToTreeIndex.Add(SUComp->GetModuleType(), ModuleIdx);
			ProcessedComponents.Add(SUComp);
		}

		return ModuleIdx;
	};

	// 5) 按确定性顺序重建物理模拟树

	// 5a) Pass 1: Chassis → root
	int32 ChassisIndex = INDEX_NONE;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (Entity.SUComponent && Entity.SUComponent->GetModuleType() == ESingularisMorphVehicleModuleType::Chassis)
		{
			ChassisIndex = AddEntity(Entity.SUComponent, INDEX_NONE);
			break;
		}
	}

	// 5b) Pass 2: Engine → Clutch → Transmission 动力链（通过 Linked 引用串联）
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.SUComponent) continue;
		if (Entity.SUComponent->GetModuleType() != ESingularisMorphVehicleModuleType::Engine) continue;

		const int32 EngineIndex = AddEntity(Entity.SUComponent, ChassisIndex);

		auto* EngineSU = Cast<USingularisMorphVehicleEngineSUComponent>(Entity.SUComponent);
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

	// 5c) Pass 3: Suspension → Chassis，记录 Susp→Index 映射供 Wheel 查找
	TMap<USingularisMorphVehicleSuspensionSUComponent*, int32> SuspensionIndexMap;
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.SUComponent) continue;
		if (Entity.SUComponent->GetModuleType() != ESingularisMorphVehicleModuleType::Suspension) continue;

		auto* SuspSU = Cast<USingularisMorphVehicleSuspensionSUComponent>(Entity.SUComponent);
		if (!SuspSU) continue;

		const int32 SuspIndex = AddEntity(SuspSU, ChassisIndex);
		SuspensionIndexMap.Add(SuspSU, SuspIndex);
	}

	// 5d) Pass 4: Wheel → LinkedSuspension（有则挂悬架下，否则挂 Chassis 下）
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.SUComponent) continue;
		if (Entity.SUComponent->GetModuleType() != ESingularisMorphVehicleModuleType::Wheel) continue;

		auto* WheelSU = Cast<USingularisMorphVehicleWheelSUComponent>(Entity.SUComponent);
		if (!WheelSU) continue;

		int32 ParentIdx = ChassisIndex;
		if (auto* LinkedSusp = Cast<USingularisMorphVehicleSuspensionSUComponent>(
			WheelSU->LinkedSuspension.GetComponent(Owner)
		))
		{
			if (const int32* FoundIdx = SuspensionIndexMap.Find(LinkedSusp))
				ParentIdx = *FoundIdx;
		}

		AddEntity(WheelSU, ParentIdx);
	}

	// 5e) Pass 5: 其余模块（Aerofoil、Thruster 等）→ Chassis
	for (const auto& Entity : Snapshot.Entities)
	{
		if (!Entity.SUComponent) continue;
		const ESingularisMorphVehicleModuleType Type = Entity.SUComponent->GetModuleType();
		if (Type == ESingularisMorphVehicleModuleType::Chassis ||
			Type == ESingularisMorphVehicleModuleType::Engine ||
			Type == ESingularisMorphVehicleModuleType::Clutch ||
			Type == ESingularisMorphVehicleModuleType::Transmission ||
			Type == ESingularisMorphVehicleModuleType::Suspension ||
			Type == ESingularisMorphVehicleModuleType::Wheel)
			continue;

		AddEntity(Entity.SUComponent, ChassisIndex);
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
	if (PhysicsAdapter)
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
	for (int32 I = 0; I < NumItems; ++I)
	{
		if (!VehiclePhysicsOutput->SimTreeOutputData[I]) continue;

		const int32 Guid = VehiclePhysicsOutput->SimTreeOutputData[I]->ModuleGuid;

		if (const TWeakObjectPtr<USceneComponent>* Component = PhysicsGuidToComponent.Find(
			Guid
		))
		{
			if (auto* BaseSU = Cast<ISingularisMorphVehicleBaseInterface>(
				Component->Get()
			))
				BaseSU->OnOutputReady(VehiclePhysicsOutput->SimTreeOutputData[I]);
		}
	}
}

void USingularisMorphVehicleSimulationComponent::ProduceInput(
	const int32 PhysicsStep,
	const int32 NumSteps,
	FSingularisMorphVehicleAsyncInput* AsyncInput
)
{
	if (!AsyncInput) return;

	AsyncInput->SetVehicle(this);

	// 1) 获取物理代理
	IPhysicsProxyBase* Proxy = GetPhysicsProxy();
	if (Proxy) AsyncInput->Proxy = Proxy;

	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("[SimComp] ProduceInput: PhysicsStep=%d, Proxy=%s"),
		PhysicsStep,
		Proxy ? TEXT("valid") : TEXT("null")
	);
}

void USingularisMorphVehicleSimulationComponent::PostUpdate() {}

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

	// 6) 通知模块物理对象已就绪（与旧代码 AddModuleToTree 的 OnConstruction_External 一致）
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
