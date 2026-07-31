#include "Core/SingularisMorphVehicleSimulationCU.h"

#include "Chaos/ClusterUnionManager.h"
#include "Chaos/DebugDrawQueue.h"
#include "Chaos/PhysicsObjectCollisionInterface.h"
#include "Chaos/PhysicsObjectInternalInterface.h"
#include "Components/PrimitiveComponent.h"
#include "Core/SingularisMorphVehicleDebug.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsObjectExternalInterface.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsProxy/ClusterUnionPhysicsProxy.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Runtime/Experimental/Chaos/Private/Chaos/PhysicsObjectInternal.h"
#include "SimModule/ModuleInput.h"
#include "SimModule/SimModulesInclude.h"
#include "Types/SingularisMorphVehicleDefaultAsyncInput.h"

FSingularisMorphVehicleDebugParams GSingularisMorphVehicleDebugParams;
DEFINE_LOG_CATEGORY(LogSingularisMorphVehicleSim);

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
	UE_LOGF(LogSingularisMorphVehicleSim, Log, "FSingularisMorphVehicleSimulation::Initialize");

	SimModuleTree = MoveTemp(InSimModuleTree);
}

void FSingularisMorphVehicleSimulation::Terminate()
{
	UE_LOGF(LogSingularisMorphVehicleSim, Log, "FSingularisMorphVehicleSimulation::Terminate");

	RootParticle = nullptr;
	SimModuleTree.Reset(nullptr);
}

void FSingularisMorphVehicleSimulation::AppendTreeUpdates(Chaos::FSimTreeUpdates* InNextTreeUpdatesInternal)
{
	Chaos::EnsureIsInGameThreadContext();

	if (InNextTreeUpdatesInternal == nullptr)
		return;

	UE::TWriteScopeLock InputConfigLock(TreeConfigurationLock);
	NextTreeUpdatesInternal.Add(*InNextTreeUpdatesInternal);
}

void FSingularisMorphVehicleSimulation::ActionTreeUpdates()
{
	Chaos::EnsureIsInPhysicsThreadContext();

	if (NextTreeUpdatesInternal.IsEmpty())
		return;

	UE::TReadScopeLock InputConfigLock(TreeConfigurationLock);

	if (SimModuleTree.IsValid())
	{
		for (Chaos::FSimTreeUpdates& TreeUpdate : NextTreeUpdatesInternal)
		{
			SimModuleTree->AppendTreeUpdates(TreeUpdate);
			FSingularisMorphVehicleBuilder::FixupTreeLinks(SimModuleTree);

			// Diagnostic: dump tree structure after fixup
			UE_LOG(
				LogSingularisMorphVehicleSim,
				Log,
				TEXT("=== PT Tree after Fixup: %d nodes ==="),
				SimModuleTree->GetNumNodes()
			);
			for (int32 N = 0; N < SimModuleTree->GetNumNodes(); N++)
			{
				Chaos::ISimulationModuleBase* Mod = SimModuleTree->GetNode(N).SimModule;
				if (Mod)
				{
					const int32 ParentIdx = SimModuleTree->GetNode(N).Parent;
					const int32 NumChildren = SimModuleTree->GetNode(N).Children.Num();
					FString DebugStr;
					Mod->GetDebugString(DebugStr);
					UE_LOG(
						LogSingularisMorphVehicleSim,
						Log,
						TEXT("  Node[%d]: %s | Parent=%d Children=%d | GUID=%d TransformIdx=%d"),
						N,
						*DebugStr,
						ParentIdx,
						NumChildren,
						Mod->GetGuid(),
						Mod->GetTransformIndex()
					);

					// Log suspension-wheel links
					if (Mod->IsSimType<Chaos::FSuspensionBaseInterface>())
					{
						auto* Susp = Mod->Cast<Chaos::FSuspensionBaseInterface>();
						UE_LOG(
							LogSingularisMorphVehicleSim,
							Log,
							TEXT("    Suspension: WheelSimTreeIdx=%d, MaxLength=%.1f"),
							Susp->GetWheelSimTreeIndex(),
							Susp->GetMaxSpringLength()
						);
					}
					if (Mod->IsSimType<Chaos::FWheelBaseInterface>())
					{
						auto* Wheel = Mod->Cast<Chaos::FWheelBaseInterface>();
						UE_LOG(
							LogSingularisMorphVehicleSim,
							Log,
							TEXT("    Wheel: SuspSimTreeIdx=%d, Radius=%.1f"),
							Wheel->GetSuspensionSimTreeIndex(),
							Wheel->GetWheelRadius()
						);
					}
				}
			}

			// NewlyCreatedModuleGuids will be passed back to GT to inform that these now exist
			for (const Chaos::FPendingModuleAdds& ModuleAdd : TreeUpdate.GetNewModules())
			{
				if (ModuleAdd.NewSimModule)
				{
					NewlyCreatedModuleGuids.Add(
						Chaos::FCreatedModule(
							ModuleAdd.NewSimModule->GetSimType(),
							ModuleAdd.NewSimModule->GetGuid(),
							ModuleAdd.NewSimModule->GetTreeIndex()
						)
					);
				}
			}
		}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		if (bSingularisMorphVehicle_DumpModuleTreeStructure_Enabled)
		{
			UE_LOGF(LogSingularisMorphVehicleSim, Warning, "SimTreeModules:");
			for (auto I = 0; I < SimModuleTree->GetNumNodes(); I++)
			{
				if (Chaos::ISimulationModuleBase* Module = SimModuleTree->GetNode(I).SimModule)
				{
					FString String;
					Module->GetDebugString(String);
					UE_LOGF(LogSingularisMorphVehicleSim, Warning, "..%ls", *String);
				}
			}
		}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	}

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
			UE_LOGF(LogSingularisMorphVehicleSim, Error, "Unsupported Particle type");
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

	auto RigidsSolver = Proxy->GetSolver<Chaos::FPhysicsSolver>();
	int CurrentFrame = -1;
	if (RigidsSolver != nullptr)
	{
		Chaos::FRewindData* RewindData = RigidsSolver->GetRewindData();
		if (RewindData != nullptr)
			CurrentFrame = RewindData->CurrentFrame();
	}

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
		//if (InWorld)
		//{
		//	WriteNetReport(InWorld->IsNetMode(NM_Client), FString::Printf(TEXT("X %s,  R %s,  V %s,  W %s")
		//		, *Proxy->GetParticle_Internal()->X().ToString()
		//		, *Proxy->GetParticle_Internal()->R().ToString()
		//		, *Proxy->GetParticle_Internal()->V().ToString()
		//		, *Proxy->GetParticle_Internal()->W().ToString()));
		//}

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
				if (Node.SimModule->IsClustered() && Node.SimModule->IsBehaviourType(Raycast))
				{
					FSpringTrace OutTrace;
					auto Suspension = static_cast<FSuspensionBaseInterface*>(Node.SimModule);
					// would be cleaner an faster to just store radius in suspension also
					float WheelRadius = 0;
					const int32 WheelLinkIdx = Suspension->GetWheelSimTreeIndex();
					if (WheelLinkIdx != ISimulationModuleBase::INVALID_IDX)
					{
						auto Wheel = static_cast<FWheelBaseInterface*>(ModuleArray[WheelLinkIdx].
							SimModule);
						if (Wheel)
							WheelRadius = Wheel->GetWheelRadius();
					}
					else
					{
						UE_LOG(
							LogSingularisMorphVehicleSim,
							Warning,
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
				if (Node.SimModule->IsClustered() && Node.SimModule->IsBehaviourType(Raycast))
				{
					QUICK_SCOPE_CYCLE_COUNTER(STAT_VehicleSim_SuspenssionRaycast);
					FSpringTrace OutTrace;
					auto Suspension = static_cast<FSuspensionBaseInterface*>(Node.SimModule);

					// would be cleaner an faster to just store radius in suspension also
					float WheelRadius = 0;
					if (Suspension->GetWheelSimTreeIndex() != ISimulationModuleBase::INVALID_IDX)
					{
						auto Wheel = static_cast<FWheelBaseInterface*>(ModuleArray[Suspension->GetWheelSimTreeIndex()].
							SimModule);
						if (Wheel)
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

					float Offset = Suspension->GetMaxSpringLength();
					if (bHasActualHit && GSingularisMorphVehicleDebugParams.SuspensionRaycastsEnabled)
					{
						Offset = HitResult.Distance - WheelRadius;

						if (Suspension->GetWheelSimTreeIndex() != ISimulationModuleBase::INVALID_IDX)
						{
							const FSimModuleTree::FSimModuleNode& WheelNode = ModuleArray[Suspension->
								GetWheelSimTreeIndex()];

							auto Wheel = static_cast<FWheelBaseInterface*>(WheelNode.SimModule);
							if (Wheel)
							{
								if (GSingularisMorphVehicleDebugParams.FrictionOverride > 0)
									Wheel->SetSurfaceFriction(GSingularisMorphVehicleDebugParams.FrictionOverride);
								else
								{
									if (HitResult.Shape && HitResult.Actor && HitResult.Actor->PhysicsProxy() &&
										HitResult.FaceIndex >= 0)
									{
										if (const FPhysicsMaterial* Material =
											GetMaterialFromInternalFaceIndex_InternalHelper(
												*HitResult.Shape,
												*HitResult.Actor,
												HitResult.FaceIndex
											))
											Wheel->SetSurfaceFriction(Material->Friction);
									}
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

						if (Suspension->GetWheelSimTreeIndex() != ISimulationModuleBase::INVALID_IDX)
						{
							auto Wheel = static_cast<FWheelBaseInterface*>(ModuleArray[Suspension->
								GetWheelSimTreeIndex()].SimModule);
							if (Wheel)
							{
								if (GSingularisMorphVehicleDebugParams.ShowWheelData)
								{
									FString TextOut = FString::Format(TEXT("{0}"), {Wheel->GetForceIntoSurface()});
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
							}
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
