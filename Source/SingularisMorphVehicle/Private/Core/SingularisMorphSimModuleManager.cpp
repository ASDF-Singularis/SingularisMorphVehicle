#include "Core/SingularisMorphSimModuleManager.h"

#include "PBDRigidsSolver.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "GameFramework/HUD.h" // for ShowDebugInfo
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "Types/SingularisMorphModuleInputTokenStore.h"
TMap<FPhysScene*, FSingularisMorphSimModuleManager*> FSingularisMorphSimModuleManager::SceneToModuleManagerMap;

FDelegateHandle FSingularisMorphSimModuleManager::OnPostWorldInitializationHandle;
FDelegateHandle FSingularisMorphSimModuleManager::OnWorldCleanupHandle;

extern FSingularisMorphSimModuleDebugParams GSingularisMorphSimModuleDebugParams;

void FSingularisMorphSimModuleOutputRecord::ConsumeOutput(
	Chaos::TSimCallbackOutputHandle<FSingularisMorphChaosSimModuleManagerAsyncOutput>&& Output
)
{
	// 1) 优先填充空槽位
	if (!CachedOutput_0)
		CachedOutput_0 = MoveTemp(Output);
	else if (!CachedOutput_1)
		CachedOutput_1 = MoveTemp(Output);
	// 2) 双槽位就绪后按交替策略覆盖旧输出
	else
	{
		(bPreviousOutputIs0 ? CachedOutput_0 : CachedOutput_1) = MoveTemp(Output);
		bPreviousOutputIs0 = !bPreviousOutputIs0;
	}
}


const FSingularisMorphChaosSimModuleManagerAsyncOutput* FSingularisMorphSimModuleOutputRecord::GetPreviousOutput() const
{
	if (CachedOutput_0 && bPreviousOutputIs0)
		return CachedOutput_0.Get();
	if (CachedOutput_1 && !bPreviousOutputIs0)
		return CachedOutput_1.Get();
	return nullptr;
}

FSingularisMorphChaosSimModuleManagerAsyncOutput* FSingularisMorphSimModuleOutputRecord::GetPreviousOutput()
{
	if (CachedOutput_0 && bPreviousOutputIs0)
		return CachedOutput_0.Get();
	if (CachedOutput_1 && !bPreviousOutputIs0)
		return CachedOutput_1.Get();
	return nullptr;
}

const FSingularisMorphChaosSimModuleManagerAsyncOutput* FSingularisMorphSimModuleOutputRecord::GetNextOutput() const
{
	if (CachedOutput_1 && bPreviousOutputIs0)
		return CachedOutput_1.Get();
	if (CachedOutput_0 && !bPreviousOutputIs0)
		return CachedOutput_0.Get();
	return nullptr;
}

FSingularisMorphChaosSimModuleManagerAsyncOutput* FSingularisMorphSimModuleOutputRecord::GetNextOutput()
{
	if (CachedOutput_1 && bPreviousOutputIs0)
		return CachedOutput_1.Get();
	if (CachedOutput_0 && !bPreviousOutputIs0)
		return CachedOutput_0.Get();
	return nullptr;
}

double FSingularisMorphSimModuleOutputRecord::GetLatestOutputStartTime() const
{
	double LatestOutputStartTime = -DBL_MAX;
	if (CachedOutput_0 && CachedOutput_1)
		LatestOutputStartTime = (bPreviousOutputIs0 ? CachedOutput_1 : CachedOutput_0)->InternalTime;
	else if (CachedOutput_0)
		LatestOutputStartTime = CachedOutput_0->InternalTime;
	return LatestOutputStartTime;
}

double FSingularisMorphSimModuleOutputRecord::GetInterpolationFactor(double AtInternalTime) const
{
	const FSingularisMorphChaosSimModuleManagerAsyncOutput* PreviousOutput = GetPreviousOutput();
	const FSingularisMorphChaosSimModuleManagerAsyncOutput* NextOutput = GetNextOutput();
	double Alpha = 0.f;
	if (NextOutput)
	{
		const double Denom = NextOutput->InternalTime - PreviousOutput->InternalTime;
		if (Denom > SMALL_NUMBER)
			Alpha = FMath::Clamp((AtInternalTime - PreviousOutput->InternalTime) / Denom, 0.0f, 1.0f);
	}
	return Alpha;
}

void FSingularisMorphSimModuleOutputRecord::Clear()
{
	CachedOutput_0 = Chaos::TSimCallbackOutputHandle<FSingularisMorphChaosSimModuleManagerAsyncOutput>();
	CachedOutput_1 = Chaos::TSimCallbackOutputHandle<FSingularisMorphChaosSimModuleManagerAsyncOutput>();
	bPreviousOutputIs0 = true;
}

FSingularisMorphSimModuleManager::FSingularisMorphSimModuleManager(FPhysScene* PhysScene)
	: Scene(*PhysScene), AsyncCallback(nullptr), Timestamp(0)

{
	check(PhysScene);

	if (!GInitialized)
	{
		GInitialized = true;
		// PhysScene->GetOwningWorld() is always null here, the world is being setup too late to be of use
		// therefore setup these global world delegates that will callback when everything is setup so registering
		// the physics solver Async Callback will succeed
		OnPostWorldInitializationHandle = FWorldDelegates::OnPostWorldInitialization.AddStatic(
			&FSingularisMorphSimModuleManager::OnPostWorldInitialization
		);
		OnWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(
			&FSingularisMorphSimModuleManager::OnWorldCleanup
		);

		if (!IsRunningDedicatedServer())
			AHUD::OnShowDebugInfo.AddStatic(&FSingularisMorphSimModuleManager::OnShowDebugInfo);
	}

	ensure(FSingularisMorphSimModuleManager::SceneToModuleManagerMap.Find(PhysScene) == nullptr);
	// double registration with same scene, will cause a leak

	// Add to Scene-To-Manager map
	SceneToModuleManagerMap.Add(PhysScene, this);
}

FSingularisMorphSimModuleManager::~FSingularisMorphSimModuleManager()
{
	while (CUVehicles.Num() > 0)
		RemoveVehicle(CUVehicles.Last());
}

void FSingularisMorphSimModuleManager::OnPostWorldInitialization(UWorld* InWorld, const UWorld::InitializationValues)
{
	FSingularisMorphSimModuleManager* Manager = GetManagerFromScene(InWorld->GetPhysicsScene());
	if (Manager)
		Manager->RegisterCallbacks(InWorld);
}

void FSingularisMorphSimModuleManager::OnWorldCleanup(UWorld* InWorld, bool bSessionEnded, bool bCleanupResources)
{
	FSingularisMorphSimModuleManager* Manager = GetManagerFromScene(InWorld->GetPhysicsScene());
	if (Manager)
		Manager->UnregisterCallbacks();
}

void FSingularisMorphSimModuleManager::OnShowDebugInfo(
	AHUD* HUD,
	UCanvas* Canvas,
	const FDebugDisplayInfo& DisplayInfo,
	float& YL,
	float& YPos
)
{
	static const FName NAME_SingularisMorphVehicle("SingularisMorphVehicle");
	if (Canvas && HUD->ShouldDisplayDebug(NAME_SingularisMorphVehicle))
	{
		if (FSingularisMorphSimModuleManager* Manager = GetManagerFromScene(HUD->GetWorld()->GetPhysicsScene()))
		{
			int32 ShowVehicleIndex = 0;
			if (!Manager->CUVehicles.IsEmpty())
			{
				TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Manager->CUVehicles[
						ShowVehicleIndex]
					.Pin();
				if (StrongPtr.IsValid())
					StrongPtr->ShowDebugInfo(HUD, Canvas, DisplayInfo, YL, YPos);
			}
		}
	}
}

void FSingularisMorphSimModuleManager::AddVehicle(TWeakObjectPtr<USingularisMorphVehicleSimulationComponent> Vehicle)
{
	// 1) 前置条件校验
	check(Vehicle != NULL);
	check(AsyncCallback);

	// 2) 注册到载具列表
	CUVehicles.Add(Vehicle);
}

void FSingularisMorphSimModuleManager::RemoveVehicle(TWeakObjectPtr<USingularisMorphVehicleSimulationComponent> Vehicle)
{
	// 1) 有效性检查并从载具列表中移除以停止后续处理
	if (Vehicle != nullptr)
		CUVehicles.Remove(Vehicle);
}

void FSingularisMorphSimModuleManager::ScenePreTick(FPhysScene* PhysScene, float DeltaTime)
{
	for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : CUVehicles)
	{
		TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
		if (StrongPtr.IsValid())
			StrongPtr->PreTickGT(DeltaTime);
	}
}


void FSingularisMorphSimModuleManager::DetachFromPhysScene(FPhysScene* PhysScene)
{
	if (AsyncCallback)
		UnregisterCallbacks();

	SceneToModuleManagerMap.Remove(PhysScene);
}

void FSingularisMorphSimModuleManager::Update(FPhysScene* PhysScene, float DeltaTime)
{
	UWorld* World = Scene.GetOwningWorld();

	SubStepCount = 0;

	ScenePreTick(PhysScene, DeltaTime);

	if (World)
	{
		FSingularisMorphChaosSimModuleManagerAsyncInput* AsyncInput = AsyncCallback->GetProducerInputData_External();

		for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : CUVehicles)
		{
			TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
			if (StrongPtr.IsValid())
			{
				StrongPtr->Update(DeltaTime);
				StrongPtr->FinalizeSimCallbackData(*AsyncInput);
			}
		}
	}
}

void FSingularisMorphSimModuleManager::PostUpdate(FChaosScene* PhysScene)
{
	ParallelUpdateVehicles();

	for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : CUVehicles)
	{
		TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
		if (StrongPtr.IsValid())
			StrongPtr->PostUpdate();
	}
}

void FSingularisMorphSimModuleManager::OnNetDriverCreated(UWorld* InWorld, UNetDriver* InNetDriver)
{
	if (InNetDriver)
	{
		if (UE::Net::FNetTokenStore* TokenStore = InNetDriver->GetNetTokenStore())
			RegisterNetTokenDataStores(InNetDriver);
		else
		{
			InNetDriver->OnNetTokenStoreReady().AddRaw(
				this,
				&FSingularisMorphSimModuleManager::RegisterNetTokenDataStores
			);
		}
	}
}

void FSingularisMorphSimModuleManager::RegisterNetTokenDataStores(UNetDriver* InNetDriver)
{
	if (InNetDriver)
	{
		if (UE::Net::FNetTokenStore* TokenStore = InNetDriver->GetNetTokenStore())
		{
			using FModuleInputNetTokenStore = UE::Net::TStructNetTokenDataStore<FSingularisMorphModuleInputNetTokenData>
				;
			using FNetworkSingularisMorphVehicleStateNetTokenStore = UE::Net::TStructNetTokenDataStore<
				FNetworkSingularisMorphVehicleStateNetTokenData>;
			if (!TokenStore->GetDataStore<FModuleInputNetTokenStore>())
				TokenStore->CreateAndRegisterDataStore<FModuleInputNetTokenStore>();
			if (!TokenStore->GetDataStore<FNetworkSingularisMorphVehicleStateNetTokenStore>())
				TokenStore->CreateAndRegisterDataStore<FNetworkSingularisMorphVehicleStateNetTokenStore>();
		}
	}
}

void FSingularisMorphSimModuleManager::InjectInputs_External(int32 PhysicsStep, int32 NumSteps)
{
	UWorld* World = Scene.GetOwningWorld();
	if (IsValid(World) == false)
		return;
	FSingularisMorphChaosSimModuleManagerAsyncInput* AsyncInput = AsyncCallback->GetProducerInputData_External();
	check(AsyncInput);

	AsyncInput->Reset(); // only want latest frame's data
	AsyncInput->VehicleInputs.Reserve(CUVehicles.Num());
	AsyncInput->World = World;
	AsyncInput->Timestamp = Timestamp;
	++Timestamp;

	for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : CUVehicles)
	{
		TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
		if (StrongPtr.IsValid())
		{
			auto CurInput = MakeUnique<FSingularisMorphVehicleAsyncInput>();
			AsyncInput->VehicleInputs.Add(MoveTemp(CurInput));
			StrongPtr->ProduceInput(PhysicsStep, NumSteps, AsyncInput->VehicleInputs.Last().Get());
		}
	}
}

void FSingularisMorphSimModuleManager::ParallelUpdateVehicles()
{
	// Friendly reminder: Results time is the time at the END of an async step
	const double ResultsTime = AsyncCallback->GetSolver()->GetPhysicsResultsTime_External();
	const double AsyncDt = AsyncCallback->GetSolver()->GetAsyncDeltaTime();

	TArray<Chaos::FCreatedModules> CombinedNewlyCreatedModuleGuids;
	Chaos::FCreatedModules ModulesEventData;
	CombinedNewlyCreatedModuleGuids.Init(ModulesEventData, CUVehicles.Num());

	// We need to pop new output data only if results time is beyond the latest cached output's step end time
	// Otherwise we can still use the ones we have stored in OutputRecord to interpolate
	// OutputRecord stores FSingularisMorphChaosSimModuleManagerAsyncOutput which "InternalTime" si the time at the BEGINNING of the step that produced that output
	const double LatestOutputResultsTime = OutputRecord.GetLatestOutputStartTime() + AsyncDt;
	if (ResultsTime > LatestOutputResultsTime)
	{
		Chaos::TSimCallbackOutputHandle<FSingularisMorphChaosSimModuleManagerAsyncOutput> NextAsyncOutput;
		while ((NextAsyncOutput = AsyncCallback->PopOutputData_External()))
		{
			// We process all events we find to never miss one
			for (auto VehicleIdx = 0; VehicleIdx < NextAsyncOutput->VehicleOutputs.Num(); ++VehicleIdx)
			{
				if (!NextAsyncOutput->VehicleOutputs[VehicleIdx]) continue;

				TArray<Chaos::FCreatedModule>& NewlyCreatedModuleGuids = NextAsyncOutput->VehicleOutputs[VehicleIdx]->
				                                                         VehicleSimOutput.NewlyCreatedModuleGuids;
				if (NewlyCreatedModuleGuids.Num() > 0 && VehicleIdx < CombinedNewlyCreatedModuleGuids.Num())
					CombinedNewlyCreatedModuleGuids[VehicleIdx].ModuleEvents.Append(NewlyCreatedModuleGuids);
			}
			OutputRecord.ConsumeOutput(MoveTemp(NextAsyncOutput));
		}
	}

	FSingularisMorphChaosSimModuleManagerAsyncOutput* PreviousOutput = OutputRecord.GetPreviousOutput();
	FSingularisMorphChaosSimModuleManagerAsyncOutput* NextOutput = OutputRecord.GetNextOutput();
	UWorld* World = Scene.GetOwningWorld();
	FSingularisMorphChaosSimModuleManagerAsyncInput* AsyncInput = AsyncCallback->GetProducerInputData_External();
	if (World && PreviousOutput && AsyncInput)
	{
		// InternalTime is the time at the beginning of the step, not at the end, whereas ResultsTime is the end time of the step (where we got to by taking the step)
		// OutputRecord deals with start times so we need to subtract AsyncDt from ResultsTime in what we pass to GetInterpolationFactor
		const double InterpolationBaseTime = ResultsTime - AsyncDt;
		float Alpha = OutputRecord.GetInterpolationFactor(InterpolationBaseTime);

		for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : CUVehicles)
		{
			TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
			if (StrongPtr.IsValid())
				StrongPtr->SetCurrentAsyncData(PreviousOutput, NextOutput, Alpha, Timestamp);
		}
	}

	bool ForceSingleThread = !GSingularisMorphSimModuleDebugParams.EnableMultithreading;
	{
		const TArray<TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>>& AwakeVehiclesBatch = CUVehicles;
		const TArray<Chaos::FCreatedModules>& ModuleEvents = CombinedNewlyCreatedModuleGuids;
		auto LambdaParallelUpdate2 = [&AwakeVehiclesBatch, &ModuleEvents](int32 Idx)
		{
			TWeakObjectPtr<USingularisMorphVehicleSimulationComponent> Vehicle = AwakeVehiclesBatch[Idx];
			TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
			if (StrongPtr.IsValid())
				StrongPtr->ParallelUpdate(ModuleEvents[Idx]); // gets output state from PT
		};

		ParallelFor(AwakeVehiclesBatch.Num(), LambdaParallelUpdate2, ForceSingleThread);
	}
}

FSingularisMorphSimModuleManager* FSingularisMorphSimModuleManager::GetManagerFromScene(FPhysScene* PhysScene)
{
	FSingularisMorphSimModuleManager* Manager = nullptr;
	FSingularisMorphSimModuleManager** ManagerPtr = SceneToModuleManagerMap.Find(PhysScene);
	if (ManagerPtr != nullptr)
		Manager = *ManagerPtr;
	return Manager;
}

void FSingularisMorphSimModuleManager::RegisterCallbacks(UWorld* InWorld)
{
	// 1) 注册网络驱动创建回调
	OnNetDriverCreatedHandle = FWorldDelegates::OnNetDriverCreated.AddRaw(
		this,
		&FSingularisMorphSimModuleManager::OnNetDriverCreated
	);

	// 2) 注册物理场景前后Tick回调
	OnPhysScenePreTickHandle = Scene.OnPhysScenePreTick.AddRaw(this, &FSingularisMorphSimModuleManager::Update);
	OnPhysScenePostTickHandle = Scene.OnPhysScenePostTick.AddRaw(this, &FSingularisMorphSimModuleManager::PostUpdate);

	// 3) 创建异步回调对象以管理异步Ticking与数据编组
	check(AsyncCallback == nullptr);
	AsyncCallback = Scene.GetSolver()->CreateAndRegisterSimCallbackObject_External<
		FSingularisMorphSimModuleManagerAsyncCallback>();

	// 4) 将输入注入函数注册至网络物理回放回调
	if (auto SolverCallback = static_cast<FNetworkPhysicsCallback*>(Scene.GetSolver()->GetRewindCallback()))
		SolverCallback->InjectInputsExternal.AddRaw(this, &FSingularisMorphSimModuleManager::InjectInputs_External);
}

void FSingularisMorphSimModuleManager::UnregisterCallbacks()
{
	// 1) 移除所有物理场景Tick回调
	Scene.OnPhysScenePreTick.Remove(OnPhysScenePreTickHandle);
	Scene.OnPhysScenePostTick.Remove(OnPhysScenePostTickHandle);
	FWorldDelegates::OnNetDriverCreated.Remove(OnNetDriverCreatedHandle);

	// 2) 注销并释放异步回调对象
	if (AsyncCallback)
	{
		Scene.GetSolver()->UnregisterAndFreeSimCallbackObject_External(AsyncCallback);
		AsyncCallback = nullptr;
	}
}

bool FSingularisMorphSimModuleManager::GInitialized = false;
