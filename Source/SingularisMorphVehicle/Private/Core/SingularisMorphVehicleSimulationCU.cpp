#include "Core/SingularisMorphVehicleSimulationCU.h"

#include "SingularisMorphVehicle.h"
#include "Chaos/ClusterUnionManager.h"
#include "Chaos/DebugDrawQueue.h"
#include "Chaos/PhysicsObjectCollisionInterface.h"
#include "Chaos/PhysicsObjectInternalInterface.h"
#include "Components/PrimitiveComponent.h"
#include "Core/SingularisMorphVehicleBuilder.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsProxy/ClusterUnionPhysicsProxy.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Runtime/Experimental/Chaos/Private/Chaos/PhysicsObjectInternal.h"
#include "SimModule/ModuleInput.h"
#include "SimModule/SimModulesInclude.h"

FSingularisMorphVehicleDebugParams GSingularisMorphVehicleDebugParams;

auto bSingularisMorphVehicle_DumpModuleTreeStructure_Enabled = false;
FAutoConsoleVariableRef CVarSingularisMorphVehicleDumpModuleTreeStructureEnabled(
	TEXT("p.ModularVehicle.DumpModuleTreeStructure.Enabled"),
	bSingularisMorphVehicle_DumpModuleTreeStructure_Enabled,
	TEXT("Enable/Disable logging of module tree structure every time there is a change.")
);

FAutoConsoleVariableRef CVarSingularisMorphVehiclesRaycastsEnabled(
	TEXT("p.ModularVehicle.SuspensionRaycastsEnabled"),
	GSingularisMorphVehicleDebugParams.SuspensionRaycastsEnabled,
	TEXT("Enable/Disable Suspension Raycasts.")
);

#if CHAOS_DEBUG_DRAW
FAutoConsoleVariableRef CVarSingularisMorphVehiclesShowRaycasts(
	TEXT("p.ModularVehicle.ShowSuspensionRaycasts"),
	GSingularisMorphVehicleDebugParams.ShowSuspensionRaycasts,
	TEXT("Enable/Disable Suspension Raycast Visualisation.")
);
FAutoConsoleVariableRef CVarSingularisMorphVehiclesShowWheelData(
	TEXT("p.ModularVehicle.ShowWheelData"),
	GSingularisMorphVehicleDebugParams.ShowWheelData,
	TEXT("Enable/Disable Displaying Wheel Simulation Data.")
);
FAutoConsoleVariableRef CVarSingularisMorphVehiclesShowRaycastMaterial(
	TEXT("p.ModularVehicle.ShowRaycastMaterial"),
	GSingularisMorphVehicleDebugParams.ShowRaycastMaterial,
	TEXT("Enable/Disable Raycast Material Hit Visualisation.")
);
FAutoConsoleVariableRef CVarSingularisMorphVehiclesShowWheelCollisionNormal(
	TEXT("p.ModularVehicle.ShowWheelCollisionNormal"),
	GSingularisMorphVehicleDebugParams.ShowWheelCollisionNormal,
	TEXT("Enable/Disable Wheel Collision Normal Visualisation.")
);
FAutoConsoleVariableRef CVarSingularisMorphVehiclesFrictionOverride(
	TEXT("p.ModularVehicle.FrictionOverride"),
	GSingularisMorphVehicleDebugParams.FrictionOverride,
	TEXT("Override the physics material friction value..")
);
FAutoConsoleVariableRef CVarSingularisMorphVehiclesDisableAnim(
	TEXT("p.ModularVehicle.DisableAnim"),
	GSingularisMorphVehicleDebugParams.DisableAnim,
	TEXT("Disable animating wheels, etc")
);
#endif

void FSingularisMorphVehicleSimulation::Initialize(TUniquePtr<Chaos::FSimModuleTree>& InSimModuleTree)
{
	UE_LOGF(LogSingularisMorphVehicle, Log, "FSingularisMorphVehicleSimulation::Initialize");

	SimModuleTree = MoveTemp(InSimModuleTree);
}

void FSingularisMorphVehicleSimulation::Terminate()
{
	UE_LOGF(LogSingularisMorphVehicle, Log, "FSingularisMorphVehicleSimulation::Terminate");

	RootParticle = nullptr;
	SimModuleTree.Reset(nullptr);

	// 树已销毁：待提交的增删/重挂/限速与新建事件不再有意义，
	// 残留会在同一对象重新 Initialize 时被应用到新树
	NextTreeUpdatesInternal.Empty();
	NewlyCreatedModuleGuids.Empty();
}

void FSingularisMorphVehicleSimulation::AppendTreeUpdates(FSingularisMorphTreeUpdate* InUpdate)
{
	Chaos::EnsureIsInGameThreadContext();

	if (!InUpdate)
		return;

	// 增删、重挂与运行期设置必须同批入队：物理线程在全部增删完成后才解析重挂，
	// 新父模块即使与本批新增处于同帧也能被正确解析
	UE::TWriteScopeLock InputConfigLock(TreeConfigurationLock);
	NextTreeUpdatesInternal.Add(*InUpdate);
}

void FSingularisMorphVehicleSimulation::ActionTreeUpdates()
{
	Chaos::EnsureIsInPhysicsThreadContext();

	// 快速路径：绝大多数物理步没有树更新。该判断与游戏线程的 AppendTreeUpdates 存在
	// 理论数据竞争（仅读一个已对齐的计数），故在锁内重新复核后再决定是否处理
	if (NextTreeUpdatesInternal.IsEmpty())
		return;

	// 树结构变更必须以写锁保护：AppendTreeUpdates 会新增/删除节点并可能重分配节点数组，
	// 而 GenerateReplicationStructure 在游戏线程以读锁遍历同一棵树。
	// 若此处用读锁，两个读者互不排斥，游戏线程会读到半更新的树（悬垂模块指针、越界索引）。
	UE::TWriteScopeLock InputConfigLock(TreeConfigurationLock);

	if (NextTreeUpdatesInternal.IsEmpty())
		return;

	if (!SimModuleTree.IsValid())
	{
		NextTreeUpdatesInternal.Empty();
		return;
	}

	TArray<TPair<FName, int32>> RequestedModules;
	TArray<FSingularisMorphModuleReparent> PendingReparents;
	TArray<FSingularisMorphWheelSpeedLimit> PendingWheelSpeedLimits;

	// 1) 先应用全部新增/删除。FSimTreeUpdates::AppendTreeUpdates 在单批内先增后删，
	//    因此待新增指针在删除后可能悬垂；只记录类型与 GUID，提交后统一从树中取值。
	for (const FSingularisMorphTreeUpdate& Update : NextTreeUpdatesInternal)
	{
		RequestedModules.Reserve(RequestedModules.Num() + Update.TreeUpdates.GetNewModules().Num());
		for (const Chaos::FPendingModuleAdds& ModuleAdd : Update.TreeUpdates.GetNewModules())
		{
			if (ModuleAdd.NewSimModule)
				RequestedModules.Emplace(ModuleAdd.NewSimModule->GetSimType(), ModuleAdd.NewSimModule->GetGuid());
		}

		SimModuleTree->AppendTreeUpdates(Update.TreeUpdates);
		PendingReparents.Append(Update.Reparents);
		PendingWheelSpeedLimits.Append(Update.WheelSpeedLimits);
	}

	// 2) 建立 GUID → 树索引表。增删已全部完成，本批新增模块与既有模块都在表中。
	const int32 NumNodes = SimModuleTree->GetNumNodes();
	TMap<int32, int32> TreeIndexByGuid;
	TreeIndexByGuid.Reserve(NumNodes);
	for (auto Index = 0; Index < NumNodes; ++Index)
	{
		const Chaos::ISimulationModuleBase* Module = SimModuleTree->GetNode(Index).SimModule;
		if (Module)
			TreeIndexByGuid.Add(Module->GetGuid(), Index);
	}

	// 3) 统一重挂父节点。父/子缺失时跳过并告警，绝不以错误索引重挂。
	for (const FSingularisMorphModuleReparent& Reparent : PendingReparents)
	{
		if (Reparent.ChildGuid == INDEX_NONE || Reparent.ParentGuid == INDEX_NONE)
			continue;

		const int32* ChildIndex = TreeIndexByGuid.Find(Reparent.ChildGuid);
		const int32* ParentIndex = TreeIndexByGuid.Find(Reparent.ParentGuid);
		if (!ChildIndex || !ParentIndex)
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT("[ActionTreeUpdates] Reparent skipped: child GUID %d (%s), parent GUID %d (%s)"),
				Reparent.ChildGuid,
				ChildIndex ? TEXT("found") : TEXT("missing"),
				Reparent.ParentGuid,
				ParentIndex ? TEXT("found") : TEXT("missing")
			);
			continue;
		}

		if (*ChildIndex == *ParentIndex || SimModuleTree->GetParent(*ChildIndex) == *ParentIndex)
			continue;

		// 防御损坏计划造成的环：父链上出现子节点时拒绝重挂
		auto bCreatesCycle = false;
		for (auto Cursor = *ParentIndex, Depth = 0;
		     Cursor >= 0 && Depth < NumNodes;
		     ++Depth)
		{
			if (Cursor == *ChildIndex)
			{
				bCreatesCycle = true;
				break;
			}

			if (!SimModuleTree->IsValidNode(Cursor))
				break;

			Cursor = SimModuleTree->GetParent(Cursor);
		}

		if (bCreatesCycle)
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Error,
				TEXT("[ActionTreeUpdates] Reparent rejected (cycle): child GUID %d, parent GUID %d"),
				Reparent.ChildGuid,
				Reparent.ParentGuid
			);
			continue;
		}

		SimModuleTree->Reparent(*ChildIndex, *ParentIndex);
	}

	// 4) 拓扑稳定后再建立悬挂↔车轮交叉链接，并输出一次结构快照
	FSingularisMorphVehicleBuilder::FixupTreeLinks(SimModuleTree);

	// 5) 保留车轮的转速上限原位更新。车轮模块在 CreateNewCoreModule 中读取该设置，
	//    仅重建模块才能生效；而重建会把角位置与动画槽位清零，造成轮胎闪烁。
	//    因此动力链变化导致的上限变化必须直接写入已存在的模块设置。
	for (const FSingularisMorphWheelSpeedLimit& Limit : PendingWheelSpeedLimits)
	{
		if (!FMath::IsFinite(Limit.MaxRotationVel)) continue;

		const int32* TreeIndex = TreeIndexByGuid.Find(Limit.ModuleGuid);
		if (!TreeIndex) continue;

		Chaos::ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(*TreeIndex);
		Chaos::FWheelSimModule* Wheel = Module ? Module->Cast<Chaos::FWheelSimModule>() : nullptr;
		if (!Wheel) continue;

		Wheel->AccessSetup().MaxRotationVel = FMath::Max(Limit.MaxRotationVel, 0.0f);

		UE_LOG(
			LogSingularisMorphVehicle,
			Verbose,
			TEXT("[ActionTreeUpdates] Wheel speed limit updated: GUID %d -> %.1f rad/s"),
			Limit.ModuleGuid,
			Limit.MaxRotationVel
		);
	}

	UE_LOG(
		LogSingularisMorphVehicle,
		Display,
		TEXT("=== PT Tree after sync: %d nodes, %d reparents, %d wheel limits ==="),
		NumNodes,
		PendingReparents.Num(),
		PendingWheelSpeedLimits.Num()
	);
	for (auto Index = 0; Index < NumNodes; ++Index)
	{
		Chaos::ISimulationModuleBase* Module = SimModuleTree->GetNode(Index).SimModule;
		if (!Module)
			continue;

		const int32 ParentIdx = SimModuleTree->GetNode(Index).Parent;
		const int32 NumChildren = SimModuleTree->GetNode(Index).Children.Num();
		FString DebugStr;
		Module->GetDebugString(DebugStr);
		UE_LOG(
			LogSingularisMorphVehicle,
			Display,
			TEXT("  Node[%d]: %s | Parent=%d Children=%d | GUID=%d TransformIdx=%d"),
			Index,
			*DebugStr,
			ParentIdx,
			NumChildren,
			Module->GetGuid(),
			Module->GetTransformIndex()
		);

		if (Module->IsSimType<Chaos::FSuspensionBaseInterface>())
		{
			auto* Suspension = Module->Cast<Chaos::FSuspensionBaseInterface>();
			UE_LOG(
				LogSingularisMorphVehicle,
				Display,
				TEXT("    Suspension: WheelSimTreeIdx=%d, MaxLength=%.1f"),
				Suspension->GetWheelSimTreeIndex(),
				Suspension->GetMaxSpringLength()
			);
		}
		if (Module->IsSimType<Chaos::FWheelBaseInterface>())
		{
			auto* Wheel = Module->Cast<Chaos::FWheelBaseInterface>();
			UE_LOG(
				LogSingularisMorphVehicle,
				Display,
				TEXT("    Wheel: SuspSimTreeIdx=%d, Radius=%.1f"),
				Wheel->GetSuspensionSimTreeIndex(),
				Wheel->GetWheelRadius()
			);
		}
	}

	// 6) 本批成功进入树中的新增模块上报游戏线程；同批自增自删的模块不在表中，自然跳过
	for (const TPair<FName, int32>& Requested : RequestedModules)
	{
		if (const int32* FoundTreeIndex = TreeIndexByGuid.Find(Requested.Value))
			NewlyCreatedModuleGuids.Add(Chaos::FCreatedModule(Requested.Key, Requested.Value, *FoundTreeIndex));
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (bSingularisMorphVehicle_DumpModuleTreeStructure_Enabled)
	{
		UE_LOGF(LogSingularisMorphVehicle, Warning, "SimTreeModules:");
		for (auto Index = 0; Index < NumNodes; Index++)
		{
			if (Chaos::ISimulationModuleBase* Module = SimModuleTree->GetNode(Index).SimModule)
			{
				FString String;
				Module->GetDebugString(String);
				UE_LOGF(LogSingularisMorphVehicle, Warning, "..%ls", *String);
			}
		}
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	NextTreeUpdatesInternal.Empty();
}

void FSingularisMorphVehicleSimulation::GenerateReplicationStructure(FNetworkSingularisMorphVehicleStates& State)
{
	Chaos::EnsureIsInGameThreadContext();

	// ensure tree resizing from ActionTreeUpdates doesn't run at the same time as this
	UE::TReadScopeLock InputConfigLock(TreeConfigurationLock);

	State.ModuleData.Empty();
	if (SimModuleTree.IsValid())
		SimModuleTree->GenerateReplicationStructure(State.ModuleData);
}

void FSingularisMorphVehicleSimulation::ApplySimStateFromNetwork(
	const Chaos::FModuleNetDataArray& ModuleData
)
{
	// SetSimState 会把数据写回各模块（FillSimState），因此必须是**写锁**：
	// 若只要读锁，会与同样持读锁读模块状态的 GenerateReplicationStructure（游戏线程）
	// 互不排斥，两边同时访问同一模块产生撕裂读
	UE::TWriteScopeLock TreeLock(TreeConfigurationLock);

	if (SimModuleTree.IsValid())
		SimModuleTree->SetSimState(ModuleData);
}

void FSingularisMorphVehicleSimulation::BuildNetStateForNetwork(Chaos::FModuleNetDataArray& ModuleData)
{
	UE::TReadScopeLock TreeLock(TreeConfigurationLock);

	if (SimModuleTree.IsValid())
		SimModuleTree->SetNetState(ModuleData);
}

void FSingularisMorphVehicleSimulation::CacheRootParticle(IPhysicsProxyBase* Proxy)
{
	Chaos::EnsureIsInPhysicsThreadContext();
	using namespace Chaos;
	RootParticle = nullptr;

	if (Proxy == nullptr)
		return;

	switch (Proxy->GetType())
	{
	case EPhysicsProxyType::ClusterUnionProxy:
		{
			if (auto CUProxy = static_cast<FClusterUnionPhysicsProxy*>(Proxy))
			{
				FPBDRigidsEvolutionGBF& Evolution = *CUProxy->GetSolver<FPBDRigidsSolver>()->GetEvolution();
				FClusterUnionManager& ClusterUnionManager = Evolution.GetRigidClustering().GetClusterUnionManager();
				const FClusterUnionIndex& CUI = CUProxy->GetClusterUnionIndex();
				if (FClusterUnion* ClusterUnion = ClusterUnionManager.FindClusterUnion(CUI))
				{
					if (FPBDRigidClusteredParticleHandle* ClusterHandle = ClusterUnion->InternalCluster)
						RootParticle = ClusterHandle;
				}
			}
		}
		break;

	case EPhysicsProxyType::SingleParticleProxy:
		{
			if (auto ParticleProxy = static_cast<FSingleParticlePhysicsProxy*>(Proxy))
			{
				RootParticle = ParticleProxy->GetHandle_LowLevel()
					               ? ParticleProxy->GetHandle_LowLevel()->CastToRigidParticle()
					               : nullptr;
			}
		}
		break;

	default:
		{
			UE_LOGF(LogSingularisMorphVehicle, Error, "Unsupported Particle type");
		}
		break;
	}
}

void FSingularisMorphVehicleSimulation::Simulate(
	UWorld* InWorld,
	float DeltaSeconds,
	const FSingularisMorphVehicleAsyncInput& InputData,
	FSingularisMorphVehicleAsyncOutput& OutputData,
	IPhysicsProxyBase* Proxy
)
{
	CacheRootParticle(Proxy);

	ActionTreeUpdates();

	SimulateModuleTree(InWorld, DeltaSeconds, InputData, OutputData, Proxy);
}

void FSingularisMorphVehicleSimulation::SimulateModuleTree(
	UWorld* InWorld,
	float DeltaSeconds,
	const FSingularisMorphVehicleAsyncInput& InputData,
	FSingularisMorphVehicleAsyncOutput& OutputData,
	IPhysicsProxyBase* Proxy
)
{
	Chaos::EnsureIsInPhysicsThreadContext();

	if (Proxy && SimModuleTree.IsValid())
	{
		int InitialNum = SimModuleTree->GetSimulationModuleTree().Num();
		if (InitialNum == 0)
			return;
		UE::TReadScopeLock InputConfigLock(InputConfigurationLock);

		FModuleInputContainer Container = InputData.PhysicsInputs.NetworkInputs.VehicleInputs.Container;

		if (ImplementsTestBuffer())
		{
			auto RigidsSolver = Proxy->GetSolver<Chaos::FPhysicsSolver>();
			check(RigidsSolver);
			const int32 CurrentPhysicsFrame = RigidsSolver->GetCurrentFrame();

			if (TestInputBufferStartFrame < 0)
				TestInputBufferStartFrame = CurrentPhysicsFrame;

			if (TestInputBufferStartFrame <= CurrentPhysicsFrame)
			{
				int32 InputFrame = CurrentPhysicsFrame - TestInputBufferStartFrame;

				if (ImplementsLoopingTestBuffer() && !TestInputBuffer.IsValidIndex(InputFrame))
				{
					TestInputBufferStartFrame = CurrentPhysicsFrame;
					InputFrame = 0;
				}

				if (TestInputBuffer.IsValidIndex(InputFrame))
					Container = TestInputBuffer[InputFrame];
			}
		}

		FInputInterface InputInterface(InputNameMap, Container, InputQuantizationType);

		FModuleInputContainer StateInputContainer = InputData.PhysicsInputs.StateInputs.StateInputContainer;
		FInputInterface StateInterface(StateNameMap, StateInputContainer, InputQuantizationType);

		// 输入集合的生命周期限定在本函数内：两个 FInputInterface 是局部对象，
		// 若把指针存入成员，函数返回后 ControlInputs/StateInputs 即为悬垂指针
		Chaos::FAllInputs SimInputData;
		SimInputData.ControlInputs = &InputInterface;
		SimInputData.StateInputs = &StateInterface;
		SimInputData.bKeepVehicleAwake = InputData.PhysicsInputs.NetworkInputs.VehicleInputs.KeepAwake;
		SimInputData.CurrentTimeDilation = InputData.PhysicsInputs.CurrentTimeDilation;


		if (SimModuleTree->GetSimTreeProcessingOrder() != ManualOverride)
			PerformAdditionalSimWork(DeltaSeconds, InWorld, InputData, Proxy, SimInputData);
		// run the dynamics simulation, engine, suspension, wheels, aerofoils etc.
		SimModuleTree->Simulate(DeltaSeconds, SimInputData, Proxy, RootParticle);

		if (SimModuleTree->GetSimTreeProcessingOrder() == ManualOverride)
		{
			VehicleSimulationCallback.Broadcast(
				this,
				InWorld,
				DeltaSeconds,
				InputData,
				SimInputData,
				Proxy,
				RootParticle,
				SimModuleTree.Get()
			);
		}

		// Clear those Inputs that we don't want to remain set if the physics simulation thread ticks more frames than the GT
		InputData.PhysicsInputs.NetworkInputs.VehicleInputs.Container.ClearConsumedInputs();
		InputData.PhysicsInputs.StateInputs.StateInputContainer.ClearConsumedInputs();
	}
}

void FSingularisMorphVehicleSimulation::OnContactModification(
	Chaos::FCollisionContactModifier& Modifier,
	IPhysicsProxyBase* Proxy
)
{
	using namespace Chaos;
	EnsureIsInPhysicsThreadContext();

	if (SimModuleTree.IsValid())
		SimModuleTree->OnContactModification(Modifier, Proxy);
}


void FSingularisMorphVehicleSimulation::ApplyDeferredForces(IPhysicsProxyBase* Proxy)
{
	using namespace Chaos;

	EnsureIsInPhysicsThreadContext();

	if (SimModuleTree && Proxy)
		SimModuleTree->AccessDeferredForces().Apply(RootParticle);
}

void FSingularisMorphVehicleSimulation::PerformAdditionalSimWork(
	float DeltaSeconds,
	UWorld* InWorld,
	const FSingularisMorphVehicleAsyncInput& InputData,
	IPhysicsProxyBase* Proxy,
	Chaos::FAllInputs& AllInputs
)
{
	using namespace Chaos;
	check(Proxy);
	EnsureIsInPhysicsThreadContext();


	// clear all ground interactions
	if (SimModuleTree)
	{
		const TArray<FSimModuleTree::FSimModuleNode>& ModuleArray = SimModuleTree->GetSimulationModuleTree();

		for (const FSimModuleTree::FSimModuleNode& Node : ModuleArray)
		{
			if (Node.IsValid() && Node.SimModule && Node.SimModule->IsEnabled())
			{
				if (FWheelBaseInterface* Wheel = Node.SimModule->Cast<FWheelBaseInterface>())
					Wheel->ClearGroundBody();
			}
		}
	}

	QUICK_SCOPE_CYCLE_COUNTER(STAT_VehicleSim_PerformAdditionalSimWork);
	if (SimModuleTree && RootParticle)
	{
		auto ClusterWorldTM = FRigidTransform3(RootParticle->GetX(), RootParticle->GetR());
		AllInputs.VehicleWorldTransform = ClusterWorldTM;
		FVector ClusterVelocity = RootParticle->GetV();

		const TArray<FSimModuleTree::FSimModuleNode>& ModuleArray = SimModuleTree->GetSimulationModuleTree();


		//Collect all rays, find intersecting objects via aabb box of rays.
		FBox Box;
		Box.Init();
		for (const FSimModuleTree::FSimModuleNode& Node : ModuleArray)
		{
			if (Node.IsValid() && Node.SimModule && Node.SimModule->IsEnabled())
			{
				// 按类型容器判定而非 IsBehaviourType(Raycast) 行为标志：
				// 行为标志不构成类型保证，若存在其它返回 Raycast 的模块，static_cast 即为未定义行为
				if (FSuspensionBaseInterface* Suspension = Node.SimModule->Cast<FSuspensionBaseInterface>();
					Suspension && Node.SimModule->IsClustered())
				{
					FSpringTrace OutTrace;
					// would be cleaner an faster to just store radius in suspension also
					float WheelRadius = 0;
					const int32 WheelLinkIdx = Suspension->GetWheelSimTreeIndex();
					if (ModuleArray.IsValidIndex(WheelLinkIdx))
					{
						if (const FWheelBaseInterface* Wheel = ModuleArray[WheelLinkIdx].SimModule
							                                       ? ModuleArray[WheelLinkIdx].SimModule->Cast<
								                                       FWheelBaseInterface>()
							                                       : nullptr)
							WheelRadius = Wheel->GetWheelRadius();
					}
					else
					{
						UE_LOG(
							LogSingularisMorphVehicle,
							Display,
							TEXT("Suspension[GUID=%d]: WheelSimTreeIndex=INVALID! WheelRadius=0."),
							Suspension->GetGuid()
						);
					}
					Suspension->GetWorldTraceEndpoints(
						DeltaSeconds,
						ClusterWorldTM,
						ClusterVelocity,
						WheelRadius,
						OutTrace
					);
					FVector TraceStart = OutTrace.Start;
					FVector TraceEnd = OutTrace.End;
					Box += TraceStart;
					Box += TraceEnd;
				}
			}
		}
		FCollisionQueryParams& TraceParams = InputData.PhysicsInputs.TraceParams;
		ECollisionChannel SpringCollisionChannel = InputData.PhysicsInputs.CollisionChannel;
		FCollisionResponseParams ResponseParams = InputData.PhysicsInputs.TraceCollisionResponse;
		TraceParams.bIgnoreTouches = true;
		TraceParams.bSkipNarrowPhase = true;
		TraceParams.bTraceComplex = true;

		TArray<FOverlapResult> Overlaps;
		auto bHasOverlap = false;
		if (InWorld && Box.IsValid && !Box.ContainsNaN() && Box.GetSize().Length() > 0)
		{
			FCollisionShape Shape = FCollisionShape::MakeBox(Box.GetExtent());
			bHasOverlap = FGenericPhysicsInterface::GeomOverlapMulti(
				InWorld,
				Shape,
				Box.GetCenter(),
				FQuat::Identity,
				Overlaps,
				SpringCollisionChannel,
				TraceParams,
				ResponseParams,
				FCollisionObjectQueryParams::DefaultObjectQueryParam
			);
		}

		TArray<FConstPhysicsObjectHandle> BlockingPhysicsObjects;
		TArray<FOverlapResult> BlockingOverlaps;
		if (bHasOverlap)
		{
			for (FOverlapResult& Overlap : Overlaps)
			{
				if (Overlap.PhysicsObject && Overlap.bBlockingHit)
				{
					if (TThreadParticle<EThreadContext::Internal>* Part = Overlap.PhysicsObject->GetParticle<
						EThreadContext::Internal>())
					{
						BlockingPhysicsObjects.Add(Overlap.PhysicsObject);
						BlockingOverlaps.Add(Overlap);
					}
				}
			}
		}
		FReadPhysicsObjectInterface_Internal Interface = FPhysicsObjectInternalInterface::GetRead();
		FPhysicsObjectCollisionInterface_Internal CollisionInterface{Interface};

		auto GetMaterialFromInternalFaceIndex_InternalHelper = [](
			const FPhysicsShape& Shape,
			const FGeometryParticleHandle& PTActor,
			uint32 InternalFaceIndex
		)-> FChaosPhysicsMaterial*
		{
			const auto& Materials = Shape.GetMaterials();
			if (Materials.Num() > 0 && PTActor.PhysicsProxy())
			{
				FPBDRigidsSolver* Solver = PTActor.PhysicsProxy()->GetSolver<FPBDRigidsSolver>();

				if (ensure(Solver))
				{
					if (Materials.Num() == 1)
						return Solver->GetSimMaterials().Get(Materials[0].InnerHandle);

					uint8 Index = Shape.GetGeometry()->GetMaterialIndex(InternalFaceIndex);

					if (Materials.IsValidIndex(Index))
						return Solver->GetSimMaterials().Get(Materials[Index].InnerHandle);
				}
			}

			return nullptr;
		};

		auto GetUserDataHelper = [](const FChaosPhysicsMaterial& Material) -> UPhysicalMaterial*
		{
			void* UserData = Material.UserData;
			return UserData ? FChaosUserData::Get<UPhysicalMaterial>(UserData) : nullptr;
		};

		for (const FSimModuleTree::FSimModuleNode& Node : ModuleArray)
		{
			if (Node.IsValid() && Node.SimModule && Node.SimModule->IsEnabled())
			{
				if (FSuspensionBaseInterface* Suspension = Node.SimModule->Cast<FSuspensionBaseInterface>();
					Suspension && Node.SimModule->IsClustered())
				{
					QUICK_SCOPE_CYCLE_COUNTER(STAT_VehicleSim_SuspenssionRaycast);
					FSpringTrace OutTrace;

					// would be cleaner an faster to just store radius in suspension also
					float WheelRadius = 0;
					if (const int32 WheelLinkIdx = Suspension->GetWheelSimTreeIndex();
						ModuleArray.IsValidIndex(WheelLinkIdx))
					{
						if (const FWheelBaseInterface* Wheel = ModuleArray[WheelLinkIdx].SimModule
							                                       ? ModuleArray[WheelLinkIdx].SimModule->Cast<
								                                       FWheelBaseInterface>()
							                                       : nullptr)
							WheelRadius = Wheel->GetWheelRadius();
					}

					Suspension->GetWorldTraceEndpoints(
						DeltaSeconds,
						ClusterWorldTM,
						ClusterVelocity,
						WheelRadius,
						OutTrace
					);

					FVector TraceStart = OutTrace.Start;
					FVector TraceEnd = OutTrace.End;

					FVector TraceVector(TraceStart - TraceEnd);
					FVector TraceNormal = TraceVector.GetSafeNormal();

					auto HitResult = ChaosInterface::FPTLocationHit();
					auto bHasActualHit = false;
					if (InWorld && bHasOverlap && BlockingPhysicsObjects.Num() > 0)
					{
						switch (InputData.PhysicsInputs.TraceType)
						{
						case ESingularisMorphTraceType::Spherecast:
							{
								QUICK_SCOPE_CYCLE_COUNTER(STAT_VehicleSim_ShapeSweep);
								auto SweepResult = ChaosInterface::FPTSweepHit();
								FSweepParameters SweepParams;
								SweepParams.bComputeMTD = false;
								SweepParams.bSweepComplex = TraceParams.bTraceComplex;
								FTransform SphereStart(FQuat::Identity, TraceStart);
								const FPhysicsShapeAdapter SphereShapeAdapter(
									FQuat::Identity,
									FCollisionShape::MakeSphere(WheelRadius)
								);
								bHasActualHit = CollisionInterface.ShapeSweep(
									BlockingPhysicsObjects,
									SphereShapeAdapter.GetGeometry(),
									SphereStart,
									TraceEnd,
									SweepParams,
									SweepResult
								);
								if (bHasActualHit)
								{
									HitResult.WorldPosition = SweepResult.WorldPosition;
									HitResult.WorldNormal = SweepResult.WorldNormal;
									HitResult.Distance = SweepResult.Distance;
									HitResult.Actor = SweepResult.Actor;
									HitResult.Shape = SweepResult.Shape;
									HitResult.FaceIndex = SweepResult.FaceIndex;
									HitResult.ElementIndex = SweepResult.ElementIndex;
									HitResult.Flags = SweepResult.Flags;
									HitResult.FaceNormal = SweepResult.FaceNormal;
								}
							}
							break;
						case ESingularisMorphTraceType::Raycast:
						default:
							{
								auto RayResult = ChaosInterface::FPTRaycastHit();
								QUICK_SCOPE_CYCLE_COUNTER(STAT_VehicleSim_LineTrace);
								bHasActualHit = CollisionInterface.LineTrace(
									BlockingPhysicsObjects,
									TraceStart,
									TraceEnd,
									TraceParams.bTraceComplex,
									RayResult
								);
								if (bHasActualHit)
								{
									HitResult.WorldPosition = RayResult.WorldPosition;
									HitResult.WorldNormal = RayResult.WorldNormal;
									HitResult.Distance = RayResult.Distance;
									HitResult.Actor = RayResult.Actor;
									HitResult.Shape = RayResult.Shape;
									HitResult.FaceIndex = RayResult.FaceIndex;
									HitResult.ElementIndex = RayResult.ElementIndex;
									HitResult.Flags = RayResult.Flags;
									HitResult.FaceNormal = RayResult.FaceNormal;
								}
							}
							break;
						}
					}

					FOverlapResult ActualHitOverlapResult;
					FConstPhysicsObjectHandle ActualHitObjectHandle;
					if (bHasActualHit)
					{
						if (!HitResult.Actor)
							bHasActualHit = false;
						else
						{
							for (auto Idx = 0; Idx < BlockingPhysicsObjects.Num(); ++Idx)
							{
								if (HitResult.Actor->PhysicsProxy() == BlockingPhysicsObjects[Idx]->PhysicsProxy())
								{
									ActualHitOverlapResult = BlockingOverlaps[Idx];
									ActualHitObjectHandle = BlockingPhysicsObjects[Idx];
									break;
								}
							}
							if (!ActualHitOverlapResult.bBlockingHit)
								bHasActualHit = false;
						}
					}

					// 悬挂射线开关关闭时不做落点解算、不写地面摩擦与地面交互，
					// 弹簧长度退回最大行程（与悬挂 Simulate 的兜底分支一致）：
					// 该开关改变物理结果，仅用于调试对比，不是纯显示开关。
					float Offset = Suspension->GetMaxSpringLength();
					if (bHasActualHit && GSingularisMorphVehicleDebugParams.SuspensionRaycastsEnabled)
					{
						Offset = HitResult.Distance - WheelRadius;

						FWheelBaseInterface* Wheel = nullptr;
						if (const int32 WheelLinkIdx = Suspension->GetWheelSimTreeIndex();
							ModuleArray.IsValidIndex(WheelLinkIdx))
						{
							Wheel = ModuleArray[WheelLinkIdx].SimModule
								        ? ModuleArray[WheelLinkIdx].SimModule->Cast<FWheelBaseInterface>()
								        : nullptr;
						}

						if (Wheel)
						{
							// FrictionOverride 默认 0：默认按命中面的物理材质取摩擦系数，
							// 大于 0 时以固定值覆盖路面摩擦（调试对比用）；命中面无物理材质时
							// 回退到载具配置的默认摩擦，避免沿用上一次命中的材质摩擦（粘滞抓地力）
							if (GSingularisMorphVehicleDebugParams.FrictionOverride > 0)
								Wheel->SetSurfaceFriction(GSingularisMorphVehicleDebugParams.FrictionOverride);
							else
							{
								float SurfaceFriction = InputData.PhysicsInputs.SurfaceFrictionFallback;

								if (HitResult.Shape && HitResult.Actor && HitResult.Actor->PhysicsProxy() &&
									HitResult.FaceIndex >= 0)
								{
									if (const FPhysicsMaterial* Material =
										GetMaterialFromInternalFaceIndex_InternalHelper(
											*HitResult.Shape,
											*HitResult.Actor,
											HitResult.FaceIndex
										))
										SurfaceFriction = Material->Friction;
								}

								Wheel->SetSurfaceFriction(SurfaceFriction);
							}


							IPhysicsProxyBase* HitProxy = nullptr;
							if (InWorld)
							{
								HitProxy = FPhysicsObjectInterface::GetProxy({&ActualHitObjectHandle, 1});

								if (HitProxy && HitProxy->GetType() == EPhysicsProxyType::SingleParticleProxy)
								{
									if (auto ParticleProxy = static_cast<FSingleParticlePhysicsProxy*>(HitProxy))
									{
										FPBDRigidParticleHandle* GroundParticle = HitResult.Actor->
											CastToRigidParticle();

										// 地面体即载具自身根粒子时不记录：否则车轮反力会重复施加到自身
										if (GroundParticle && GroundParticle != RootParticle)
										{
											Wheel->SetGroundInteraction(
												GroundParticle,
												HitResult.WorldPosition,
												HitResult.WorldNormal
											);
										}
									}
								}
							}
						}

#if CHAOS_DEBUG_DRAW
						if (GSingularisMorphVehicleDebugParams.ShowSuspensionRaycasts)
						{
							FDebugDrawQueue::GetInstance().DrawDebugSphere(
								HitResult.WorldPosition,
								3,
								16,
								FColor::Red,
								false,
								-1.f,
								0,
								10.f
							);
						}

						FWheelBaseInterface* DebugWheel = nullptr;
						if (const int32 DebugWheelLinkIdx = Suspension->GetWheelSimTreeIndex();
							ModuleArray.IsValidIndex(DebugWheelLinkIdx))
						{
							DebugWheel = ModuleArray[DebugWheelLinkIdx].SimModule
								             ? ModuleArray[DebugWheelLinkIdx].SimModule->Cast<FWheelBaseInterface>()
								             : nullptr;
						}

						if (DebugWheel && GSingularisMorphVehicleDebugParams.ShowWheelData)
						{
							FString TextOut = FString::Format(TEXT("{0}"), {DebugWheel->GetForceIntoSurface()});
							FColor Col = FColor::White;
							if (InWorld)
							{
								if (InWorld->GetNetMode() == NM_Client)
									Col = FColor::Blue;
								else
									Col = FColor::Red;
							}
							FDebugDrawQueue::GetInstance().DrawDebugString(
								HitResult.WorldNormal + FVec3(0, 50, 50),
								TextOut,
								nullptr,
								Col,
								-1.f,
								true,
								1.0f
							);
						}

#endif
					}

#if CHAOS_DEBUG_DRAW
					if (GSingularisMorphVehicleDebugParams.ShowSuspensionRaycasts)
					{
						FColor DrawColor = FColor::Green;
						DrawColor = bHasActualHit ? FColor::Red : FColor::Green;
						FDebugDrawQueue::GetInstance().DrawDebugLine(
							TraceStart,
							TraceEnd,
							DrawColor,
							false,
							-1.f,
							0,
							2.f
						);
						FDebugDrawQueue::GetInstance().DrawDebugSphere(
							TraceStart,
							3,
							16,
							FColor::White,
							false,
							-1.f,
							0,
							10.f
						);
						FDebugDrawQueue::GetInstance().DrawDebugSphere(
							HitResult.WorldPosition,
							1,
							16,
							FColor::Red,
							false,
							-1.f,
							0,
							10.f
						);
						FString TextOut = FString::Format(TEXT("{0}"), {HitResult.Distance});

						FColor Col = FColor::White;
						if (InWorld)
						{
							if (InWorld->GetNetMode() == NM_Client)
								Col = FColor::Blue;
							else
								Col = FColor::Red;
						}
						FDebugDrawQueue::GetInstance().DrawDebugString(
							HitResult.WorldPosition + FVector(0, 50, 50),
							TextOut,
							nullptr,
							Col,
							-1.f,
							true,
							1.0f
						);
					}

					if (GSingularisMorphVehicleDebugParams.ShowRaycastMaterial)
					{
						if (HitResult.Shape && HitResult.Actor && HitResult.Actor->PhysicsProxy() && HitResult.FaceIndex
							>= 0)
						{
							if (const FPhysicsMaterial* Material = GetMaterialFromInternalFaceIndex_InternalHelper(
								*HitResult.Shape,
								*HitResult.Actor,
								HitResult.FaceIndex
							))
							{
								if (UPhysicalMaterial* GTMaterial = GetUserDataHelper(*Material))
								{
									FDebugDrawQueue::GetInstance().DrawDebugString(
										HitResult.WorldPosition,
										GTMaterial->GetName(),
										nullptr,
										FColor::White,
										-1.f,
										true,
										1.0f
									);
								}
							}
						}
					}

					if (GSingularisMorphVehicleDebugParams.ShowWheelCollisionNormal)
					{
						FVector Pt = HitResult.WorldPosition;
						FDebugDrawQueue::GetInstance().DrawDebugLine(
							Pt,
							Pt + HitResult.WorldNormal * 20.0f,
							FColor::Yellow,
							false,
							1.0f,
							0,
							1.0f
						);
						FDebugDrawQueue::GetInstance().DrawDebugSphere(
							Pt,
							5.0f,
							4,
							FColor::White,
							false,
							1.0f,
							0,
							1.0f
						);
					}

#endif
					Suspension->SetSpringLength(Offset, WheelRadius);
					FVector Up = ClusterWorldTM.GetUnitAxis(EAxis::Z);

					FVector HitPoint;
					float HitDistance = Offset;
					if (InputData.PhysicsInputs.TraceType == ESingularisMorphTraceType::Spherecast)
						HitPoint = HitResult.WorldPosition;
					else
						HitPoint = HitResult.WorldPosition + Up * WheelRadius;

					TEnumAsByte<EPhysicalSurface> SurfaceType = SurfaceType_Default;
					if (HitResult.Shape && HitResult.Actor && HitResult.FaceIndex >= 0)
					{
						if (const FPhysicsMaterial* Material = GetMaterialFromInternalFaceIndex_InternalHelper(
							*HitResult.Shape,
							*HitResult.Actor,
							HitResult.FaceIndex
						))
						{
							if (UPhysicalMaterial* GTMaterial = GetUserDataHelper(*Material))
								SurfaceType = GTMaterial->SurfaceType;
						}
					}

					FSuspensionTargetPoint TargetPoint(
						HitPoint,
						HitResult.WorldNormal,
						HitDistance,
						bHasActualHit,
						SurfaceType,
						bHasActualHit && HitResult.Actor ? HitResult.Actor->PhysicsProxy() : nullptr
					);

					Suspension->SetTargetPoint(TargetPoint);
				}
			}
		}
	}
}

void FSingularisMorphVehicleSimulation::FillOutputState(FSingularisMorphVehicleAsyncOutput& Output)
{
	QUICK_SCOPE_CYCLE_COUNTER(Stat_FSingularisMorphVehicleSimulation_FillOutputState);

	Output.VehicleSimOutput.NewlyCreatedModuleGuids = NewlyCreatedModuleGuids;
	NewlyCreatedModuleGuids.Empty();

	if (Chaos::FSimModuleTree* SimTree = GetSimComponentTree().Get())
	{
		for (auto I = 0; I < SimTree->GetNumNodes(); I++)
		{
			if (SimTree->GetSimModule(I))
			{
				if (Chaos::FSimOutputData* OutData = SimTree->AccessSimModule(I)->GenerateOutputData())
				{
					OutData->FillOutputState(SimTree->GetSimModule(I));
					Output.VehicleSimOutput.SimTreeOutputData.Add(OutData);
				}
			}
		}
	}
}
