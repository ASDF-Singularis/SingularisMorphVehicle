#pragma once

#include <CoreMinimal.h>
#include <PhysicsInterfaceDeclaresCore.h>
#include <Engine/World.h>

#include "Chaos/SimCallbackInput.h"
#include "Chaos/SimCallbackObject.h"

class FSingularisMorphSimModuleManagerAsyncCallback;
class USingularisMorphVehicleSimulationComponent;
class AHUD;
class FPhysScene_Chaos;
struct FSingularisMorphChaosSimModuleManagerAsyncOutput;

/**
 * 引力奇点变型模块管理器异步输出记录。
 *
 * 存储两个最近的物理线程异步输出，支持游戏线程在帧间进行时间插值。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphSimModuleOutputRecord
{
public:
	/** 消费一个异步输出（假定为新于之前消费的输出） */
	void ConsumeOutput(Chaos::TSimCallbackOutputHandle<FSingularisMorphChaosSimModuleManagerAsyncOutput>&& Output);

	const FSingularisMorphChaosSimModuleManagerAsyncOutput* GetPreviousOutput() const;
	FSingularisMorphChaosSimModuleManagerAsyncOutput* GetPreviousOutput();

	const FSingularisMorphChaosSimModuleManagerAsyncOutput* GetNextOutput() const;
	FSingularisMorphChaosSimModuleManagerAsyncOutput* GetNextOutput();

	double GetLatestOutputStartTime() const;
	double GetInterpolationFactor(double AtInternalTime) const;

	/** 清理并删除所有存储的输出 */
	void Clear();

private:
	Chaos::TSimCallbackOutputHandle<FSingularisMorphChaosSimModuleManagerAsyncOutput> CachedOutput_0;
	Chaos::TSimCallbackOutputHandle<FSingularisMorphChaosSimModuleManagerAsyncOutput> CachedOutput_1;
	bool bPreviousOutputIs0 = true;
};

/**
 * 引力奇点变型模拟模块管理器。
 *
 * 每个物理场景创建一个实例，负责注册/注销变型载具组件，
 * 在每个物理帧驱动载具模拟管线（输入注入、并行更新、后处理），
 * 管理异步回调与网络复制令牌存储。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphSimModuleManager
{
public:
	FSingularisMorphSimModuleManager(FPhysScene* PhysScene);
	~FSingularisMorphSimModuleManager();

	static void OnPostWorldInitialization(UWorld* InWorld, const UWorld::InitializationValues);
	static void OnWorldCleanup(UWorld* InWorld, bool bSessionEnded, bool bCleanupResources);
	static void OnShowDebugInfo(
		AHUD* HUD,
		UCanvas* Canvas,
		const FDebugDisplayInfo& DisplayInfo,
		float& YL,
		float& YPos
	);

	/** 获取关联的物理场景 */
	FPhysScene_Chaos& GetScene() const { return Scene; }

	/** 注册一个变型载具组件以进行处理 */
	void AddVehicle(TWeakObjectPtr<USingularisMorphVehicleSimulationComponent> Vehicle);

	/** 注销一个变型载具组件 */
	void RemoveVehicle(TWeakObjectPtr<USingularisMorphVehicleSimulationComponent> Vehicle);

	/** 更新载具调校与其他状态（如输入） */
	void ScenePreTick(FPhysScene* PhysScene, float DeltaTime);

	/** 从此物理场景分离管理器 */
	void DetachFromPhysScene(FPhysScene* PhysScene);

	/** 更新已注册载具的模拟 */
	void Update(FPhysScene* PhysScene, float DeltaTime);

	/** 后更新步骤 */
	void PostUpdate(FChaosScene* PhysScene);

	void OnNetDriverCreated(UWorld* InWorld, UNetDriver* InNetDriver);
	void RegisterNetTokenDataStores(UNetDriver* InNetDriver);

	/** 在游戏线程调用，但在物理线程运行之前注入输入 */
	void InjectInputs_External(int32 PhysicsStep, int32 NumSteps);

	void ParallelUpdateVehicles();

	/** 从物理场景查找载具管理器 */
	static FSingularisMorphSimModuleManager* GetManagerFromScene(FPhysScene* PhysScene);

protected:
	void RegisterCallbacks(UWorld* InWorld);
	void UnregisterCallbacks();

private:
	/** 物理场景到对应载具管理器的静态映射 */
	static TMap<FPhysScene*, FSingularisMorphSimModuleManager*> SceneToModuleManagerMap;

	/** 关联的 Chaos 物理场景 */
	FPhysScene_Chaos& Scene;

	static bool GInitialized;

	TArray<TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>> CUVehicles;

	// 回调委托句柄
	FDelegateHandle OnNetDriverCreatedHandle;
	FDelegateHandle OnPhysScenePreTickHandle;
	FDelegateHandle OnPhysScenePostTickHandle;

	static FDelegateHandle OnPostWorldInitializationHandle;
	static FDelegateHandle OnWorldCleanupHandle;

	/** 物理引擎的异步回调 */
	FSingularisMorphSimModuleManagerAsyncCallback* AsyncCallback;
	int32 Timestamp = 0;
	int32 SubStepCount = 0;

	/** 用于插值的异步输出记录 */
	FSingularisMorphSimModuleOutputRecord OutputRecord;
};
