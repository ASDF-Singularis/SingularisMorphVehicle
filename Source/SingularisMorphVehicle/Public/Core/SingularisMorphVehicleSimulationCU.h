#pragma once

#include <Delegates/Delegate.h>
#include <Misc/ScopeRWLock.h>
#include <Misc/TransactionallySafeRWLock.h>

#include "SingularisMorphVehicleBuilder.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/ParticleHandleFwd.h"
#include "SimModule/SimModuleTree.h"
#include "SimModule/SimModulesInclude.h"
#include "Types/SingularisMorphSimModuleManagerAsyncCallback.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSingularisMorphVehicleSim, Log, All);

struct FSingularisMorphVehicleAsyncInput;
struct FSingularisMorphChaosSimModuleManagerAsyncOutput;
struct FModuleInputContainer;

/**
 * 引力奇点变型载具调试参数。
 */
struct FSingularisMorphVehicleDebugParams
{
	bool ShowDebug = false;
	bool SuspensionRaycastsEnabled = true;
	bool ShowSuspensionRaycasts = false;
	bool ShowWheelData = false;
	bool ShowRaycastMaterial = false;
	bool ShowWheelCollisionNormal = false;

	bool DisableAnim = false;
	float FrictionOverride = 1.0f;
};

namespace Chaos
{
	class FClusterUnionPhysicsProxy;
	class FSingleParticlePhysicsProxy;
}

class SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleSimulation;
DECLARE_MULTICAST_DELEGATE_EightParams(
	FSingularisMorphVehicleSimulationCallback,
	FSingularisMorphVehicleSimulation*,
	UWorld*,
	float,
	const FSingularisMorphVehicleAsyncInput&,
	Chaos::FAllInputs&,
	IPhysicsProxyBase*,
	Chaos::FPBDRigidParticleHandle*,
	Chaos::FSimModuleTree*
);

/**
 * 引力奇点变型载具模拟（物理线程端）。
 *
 * 在物理线程异步回调中运行，管理模拟模块树的构建、销毁与执行。
 * 包含线程安全的输入配置、模块更新队列与复制结构生成。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleSimulation
{
public:
	using FInputNameMap = FInputInterface::FInputNameMap;

	FSingularisMorphVehicleSimulation(bool InUsingNetworkPhysicsPrediction, int8 InNetMode)
		: bUsingNetworkPhysicsPrediction(InUsingNetworkPhysicsPrediction), NetMode(InNetMode) {}

	virtual ~FSingularisMorphVehicleSimulation() { Terminate(); }

	/** 初始化模拟模块树 */
	void Initialize(TUniquePtr<Chaos::FSimModuleTree>& InSimModuleTree);

	/** 终止模拟并清理资源 */
	void Terminate();

	void SetInputQuantizationType(EModuleInputQuantizationType InInputQuantizationType)
	{
		UE::TWriteScopeLock InputConfigLock(InputConfigurationLock);
		InputQuantizationType = InInputQuantizationType;
	}

	void SetInputMappings(const FInputNameMap& InNameMap)
	{
		UE::TWriteScopeLock InputConfigLock(InputConfigurationLock);
		InputNameMap = InNameMap;
	}

	void SetStateMappings(const FInputNameMap& InNameMap)
	{
		UE::TWriteScopeLock InputConfigLock(InputConfigurationLock);
		StateNameMap = InNameMap;
	}

	void SetTestInputBuffer(
		TArray<FModuleInputContainer>& InTestInputBuffer,
		bool bInIsLoopBuffer,
		int32 InTestInputBufferStartFrame
	)
	{
		UE::TWriteScopeLock InputConfigLock(InputConfigurationLock);
		bIsLoopingTestInputBuffer = bInIsLoopBuffer;
		TestInputBuffer = InTestInputBuffer;
		TestInputBufferStartFrame = InTestInputBufferStartFrame;
	}

	void AppendTreeUpdates(Chaos::FSimTreeUpdates* InNextTreeUpdatesInternal);
	void ActionTreeUpdates();

	/** 生成网络复制结构 */
	void GenerateReplicationStructure(FNetworkSingularisMorphVehicleStates& State);

	/** 缓存根粒子句柄 */
	void CacheRootParticle(IPhysicsProxyBase* Proxy);

	/** 物理线程端主模拟入口 */
	virtual void Simulate(
		UWorld* InWorld,
		float DeltaSeconds,
		const FSingularisMorphVehicleAsyncInput& InputData,
		FSingularisMorphVehicleAsyncOutput& OutputData,
		IPhysicsProxyBase* Proxy
	);

	/** 模拟模块树的核心逻辑 */
	virtual void SimulateModuleTree(
		UWorld* InWorld,
		float DeltaSeconds,
		const FSingularisMorphVehicleAsyncInput& InputData,
		FSingularisMorphVehicleAsyncOutput& OutputData,
		IPhysicsProxyBase* Proxy
	);

	/** 碰撞修正回调 */
	virtual void OnContactModification(Chaos::FCollisionContactModifier& Modifier, IPhysicsProxyBase* Proxy);

	/** 应用延迟力到物理代理 */
	void ApplyDeferredForces(IPhysicsProxyBase* Proxy);

	/** 执行额外的模拟工作 */
	void PerformAdditionalSimWork(
		float DeltaSeconds,
		UWorld* InWorld,
		const FSingularisMorphVehicleAsyncInput& InputData,
		IPhysicsProxyBase* Proxy,
		Chaos::FAllInputs& AllInputs
	);

	/** 填充输出状态 */
	void FillOutputState(FSingularisMorphVehicleAsyncOutput& Output);

	/** 获取模拟组件树（仅物理线程上下文） */
	const TUniquePtr<Chaos::FSimModuleTree>& GetSimComponentTree() const
	{
		Chaos::EnsureIsInPhysicsThreadContext();
		return SimModuleTree;
	}

	TUniquePtr<Chaos::FSimModuleTree>& AccessSimComponentTree() { return SimModuleTree; }

	TUniquePtr<Chaos::FSimModuleTree> SimModuleTree;
	Chaos::FAllInputs SimInputData;
	bool bUsingNetworkPhysicsPrediction = false;

	/** 物理线程当前使用的控制输入 */
	FSingularisMorphVehicleInputs VehicleInputs;
	EModuleInputQuantizationType InputQuantizationType = EModuleInputQuantizationType::Default_16Bits;

	FInputNameMap InputNameMap;
	FInputNameMap StateNameMap;
	FTransactionallySafeRWLock InputConfigurationLock;
	FTransactionallySafeRWLock TreeConfigurationLock;

	int8 NetMode = 0;

	int32 TestInputBufferStartFrame = -1;
	bool bIsLoopingTestInputBuffer = false;
	bool ImplementsTestBuffer() { return TestInputBuffer.Num() > 0; }
	bool ImplementsLoopingTestBuffer() { return bIsLoopingTestInputBuffer; }

	TArray<FModuleInputContainer> TestInputBuffer;
	TArray<Chaos::FSimTreeUpdates> NextTreeUpdatesInternal;
	TArray<Chaos::FCreatedModule> NewlyCreatedModuleGuids;

	/** 缓存的根粒子句柄 */
	Chaos::FPBDRigidParticleHandle* RootParticle = nullptr;

	/** 模拟回调委托 */
	FSingularisMorphVehicleSimulationCallback VehicleSimulationCallback;
};
