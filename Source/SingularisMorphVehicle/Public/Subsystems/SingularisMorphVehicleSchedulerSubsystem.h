#pragma once

#include <CoreMinimal.h>
#include <Engine/WorldInitializationValues.h>
#include <Physics/Experimental/PhysScene_Chaos.h>
#include <Subsystems/WorldSubsystem.h>

#include "Core/SingularisMorphSimModuleManager.h"
#include "Types/SingularisMorphSimModuleManagerAsyncCallback.h"
#include "SingularisMorphVehicleSchedulerSubsystem.generated.h"

/**
 * 引力奇点变形载具调度器子系统。
 *
 * 作为整个载具模拟系统的游戏线程中枢，负责：
 * - 统一调度所有已注册载具组件的 PreTick / Update / PostUpdate 生命周期。
 * - 管理游戏线程与 Chaos 物理线程之间的双向异步数据编组：
 *   输入方向（游戏线程 → 物理线程）：InjectInputs() 将载具控制输入写入共享缓冲区。
 *   输出方向（物理线程 → 游戏线程）：ParallelUpdateVehicles() 拉取模拟结果并通过双缓冲插值。
 * - 注册和管理网络令牌数据存储（Net Token Store），支持载具状态的网络复制。
 * - 作为 WorldSubsystem，生命周期与 UWorld 绑定：每个 World 独立拥有一个实例，
 *   PIE（Play In Editor）多实例场景下各自维护独立的调度状态。
 *
 * 调用链概览：
 *   PostInitialize()  → 获取物理场景、调用 BindEvent()
 *   OnPhysScenePreTick → 载具 PreTickGT → 载具 Update → FinalizeSimCallbackData
 *   [物理线程异步模拟] → Chaos Solver 消费 AsyncInput、产出 AsyncOutput
 *   OnPhysScenePostTick → ParallelUpdateVehicles() → 载具 PostUpdate
 *   Deinitialize()     → OutputRecord 清理 → UnbindEvent()
 */
UCLASS(NotBlueprintable, BlueprintType)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleSchedulerSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

#pragma region Internal Variable

	/**
	 * 全局一次性初始化标记。
	 * 构造函数中通过此标记确保全局副作用（如静态注册）只执行一次，
	 * 而非每次创建子系统实例时重复执行。
	 */
	bool bGInitialized = false;

	/**
	 * 单调递增的帧序号计数器。
	 * 每帧在 InjectInputs() 中递增，注入到 AsyncInput 供物理线程判断输入是否过期。
	 */
	int32 Timestamp = 0;

	/**
	 * 当前帧内已完成的物理子步计数。
	 * 由 OnPhysScenePreTick 归零，供载具组件在子步插值中参考。
	 */
	int32 SubStepCount = 0;

	/** 已注册的载具模拟组件列表（弱指针，不阻止 GC 回收）。 */
	TArray<TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>> VehicleSimulationComponents{};

	/** 所属 World 的 Chaos 物理场景指针。 */
	FPhysScene_Chaos* PhysicsScene = nullptr;

	/**
	 * 异步输出双缓冲记录器。
	 * 缓存最近两帧物理线程产出的异步输出，供 ParallelUpdateVehicles() 插值使用。
	 */
	FSingularisMorphSimModuleOutputRecord OutputRecord{};

	/**
	 * 注册在 Chaos Solver 上的异步回调对象。
	 * 负责物理线程侧的 PreSimulate / Rewind / ContactModification 回调。
	 * 游戏线程通过它读写 AsyncInput / AsyncOutput 完成数据编组。
	 */
	FSingularisMorphSimModuleManagerAsyncCallback* AsyncCallback = nullptr;

	FDelegateHandle OnPostWorldInitializationHandle{};
	FDelegateHandle OnWorldCleanupHandle{};

	/** 网络驱动创建回调句柄，用于注册 NetTokenStore。 */
	FDelegateHandle OnNetDriverCreatedHandle{};

	/** 物理场景 PreTick 回调句柄（每帧物理步进前）。 */
	FDelegateHandle OnPhysScenePreTickHandle{};

	/** 物理场景 PostTick 回调句柄（每帧物理步进后）。 */
	FDelegateHandle OnPhysScenePostTickHandle{};

#pragma endregion

public:
#pragma region Constructors

	USingularisMorphVehicleSchedulerSubsystem();

#pragma endregion

#pragma region Subsystem Interface

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void PostInitialize() override;
	virtual void Deinitialize() override;

#pragma endregion

#pragma region API

	/**
	 * 注册载具模拟组件到调度器。
	 * 注册后该载具将参与每帧的 PreTick / Update / PostUpdate 及异步模拟管线。
	 * 同一组件多次注册会被忽略（AddUnique）。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具调度器|API",
		meta = (DisplayName = "注册变型载具组件")
	)
	void RegisterVehicleComponent(USingularisMorphVehicleSimulationComponent* Vehicle);

	/**
	 * 从调度器注销载具模拟组件。
	 * 注销后该载具不再参与模拟管线。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具调度器|API",
		meta = (DisplayName = "注销变型载具组件")
	)
	void UnregisterVehicleComponent(USingularisMorphVehicleSimulationComponent* Vehicle);

#pragma endregion

private:
#pragma region Callback

	/** World 初始化完成回调（当前未被使用，生命周期由 PostInitialize 替代）。 */
	void OnPostWorldInitialization(UWorld* World, FWorldInitializationValues WorldInitializationValues);

	/** World 清理回调（当前未被使用，生命周期由 Deinitialize 替代）。 */
	void OnWorldCleanup(UWorld* World, bool bArg, bool bCond);

	/**
	 * 网络驱动创建回调。
	 * 在新 NetDriver 创建时注册网络令牌数据存储，用于载具状态的网络复制。
	 * 若 NetTokenStore 尚未就绪，则注册延迟回调等待。
	 */
	void OnNetDriverCreated(UWorld* World, UNetDriver* NetDriver);

	/** NetTokenStore 就绪的延迟回调，实际执行令牌数据存储的注册。 */
	void OnNetTokenStoreReady(UNetDriver* NetDriver) const;

	/**
	 * 物理场景 PreTick 回调（每帧物理步进前，游戏线程）。
	 *
	 * 执行两个阶段：
	 * 1) PreTickGT：各载具组件在物理更新前刷新游戏线程状态（如动画、输入）。
	 * 2) Update + FinalizeSimCallbackData：各载具执行逻辑更新并将控制输入序列化到 AsyncInput。
	 */
	void OnPhysScenePreTick(FPhysScene_Chaos* PhysScene_Chaos, float DeltaTime);

	/**
	 * 物理场景 PostTick 回调（每帧物理步进后，游戏线程）。
	 *
	 * 先通过 ParallelUpdateVehicles() 拉取并插值物理模拟结果，
	 * 再调用各载具的 PostUpdate() 完成本帧收尾（如更新渲染变换）。
	 */
	void OnPhysScenePostTick(FChaosScene* ChaosScene);

	/**
	 * 注入载具控制输入到物理线程。
	 *
	 * 由 Chaos Solver 在每次物理步进前通过 FNetworkPhysicsCallback 回调。
	 * 正常 Tick 与网络回放（Rewind）两种场景均会触发。
	 *
	 * @param PhysicsStep 当前子步索引。
	 * @param NumSteps    本帧需要的总物理子步数。
	 */
	void InjectInputs(int PhysicsStep, int NumSteps);

#pragma endregion

#pragma region Internal Function

	/**
	 * 注册所有事件回调与异步回调对象。
	 *
	 * 注册项包括：
	 * 1) 网络驱动创建回调（用于网络复制初始化）。
	 * 2) 物理场景 PreTick / PostTick 回调。
	 * 3) Chaos Solver 异步回调对象（负责物理线程侧模拟）。
	 * 4) 网络物理回放回调（InjectInputs 注册点）。
	 *
	 * 调用时机：PostInitialize() 中，物理场景就绪后。
	 */
	void BindEvent();

	/**
	 * 解绑所有事件回调并注销异步回调对象。
	 *
	 * 调用时机：Deinitialize() 中，子系统销毁前。
	 * 注意：编辑器关闭时 PhysicsScene 可能已提前释放，因此需做空指针守卫。
	 */
	void UnbindEvent();

	/**
	 * 向 NetDriver 的 NetTokenStore 注册载具相关的网络令牌数据存储类型。
	 *
	 * 注册两种数据类型：
	 * - FSingularisMorphModuleInputNetTokenData：模块输入数据的网络同步令牌。
	 * - FNetworkSingularisMorphVehicleStateNetTokenData：载具状态的网络同步令牌。
	 *
	 * 采用幂等注册：若已存在则跳过。
	 */
	void RegisterNetTokenDataStores(UNetDriver* NetDriver) const;

	/**
	 * 从物理线程拉取异步模拟结果、插值并分发给所有载具组件进行并行更新。
	 *
	 * 调用时机：OnPhysScenePostTick 每帧调用一次。
	 */
	void ParallelUpdateVehicles();

#pragma endregion
};
