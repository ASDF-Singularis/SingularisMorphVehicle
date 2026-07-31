#pragma once

#include <CoreMinimal.h>
#include <PhysicsPublic.h>
#include <Chaos/GeometryParticlesfwd.h>
#include <Chaos/SimCallbackInput.h>
#include <Chaos/SimCallbackObject.h>
#include <Iris/ReplicationSystem/NetTokenStructDefines.h>
#include <Iris/ReplicationSystem/StructNetTokenDataStore.h>
#include <Net/Iris/ReplicationSystem/StructNetTokenDataStoreHelper.h>
#include <Physics/NetworkPhysicsComponent.h>
#include <SimModule/ModuleInput.h>
#include <SimModule/SimulationModuleBase.h>

#include "SingularisMorphSimModuleManagerAsyncCallback.generated.h"

class USingularisMorphVehicleSimulationComponent;
class FGeometryCollectionPhysicsProxy;
class UPackageMap;
struct FSingularisMorphVehicleAsyncOutput;

DECLARE_STATS_GROUP(
	TEXT("SingularisMorphSimModuleManager"),
	STATGROUP_SingularisMorphSimModuleManager,
	STATGROUP_Advanced
);

/** 引力奇点变型模拟模块调试参数 */
struct FSingularisMorphSimModuleDebugParams
{
	bool EnableMultithreading = false;
	bool EnableNetworkStateData = true;
};

/** 引力奇点变型异步载具数据类型枚举 */
UENUM()
enum ESingularisMorphChaosAsyncVehicleDataType : int8
{
	AsyncInvalid UMETA(DisplayName = "无效"),
	AsyncDefault UMETA(DisplayName = "默认"),
};

/** 引力奇点变型模块变换数据 */
struct FSingularisMorphModuleTransform
{
	int32 TransforIndex = 0;
	FTransform Transform;
};

/**
 * 引力奇点变型载具输入（来自玩家控制器）。
 * 包含反转标志、保持唤醒标志及量化后的模块输入容器。
 */
USTRUCT()
struct FSingularisMorphVehicleInputs
{
	GENERATED_USTRUCT_BODY()

	FSingularisMorphVehicleInputs()
		: Reverse(false), KeepAwake(false) {}

	/** 反转状态 */
	UPROPERTY()
	bool Reverse;

	/** 保持载具唤醒 */
	UPROPERTY()
	bool KeepAwake;

	/** 量化后的模块输入容器 */
	UPROPERTY()
	FModuleInputContainer Container;
};

/**
 * 引力奇点变型载具网络输入数据。
 *
 * 存储来自本地客户端的控制输入列表，通过网络物理组件同步至服务器，
 * 在服务器端模拟历史中应用以驱动确定性重播。
 */
USTRUCT()
struct FNetworkSingularisMorphVehicleInputs : public FNetworkPhysicsData
{
	GENERATED_USTRUCT_BODY()

	/** 本地客户端传入的控制输入 */
	UPROPERTY()
	FSingularisMorphVehicleInputs VehicleInputs;

	SINGULARISMORPHVEHICLE_API virtual void ApplyData(UActorComponent* NetworkComponent) const override;
	SINGULARISMORPHVEHICLE_API virtual void BuildData(const UActorComponent* NetworkComponent) override;
	SINGULARISMORPHVEHICLE_API bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
	SINGULARISMORPHVEHICLE_API virtual void InterpolateData(
		const FNetworkPhysicsData& MinData,
		const FNetworkPhysicsData& MaxData
	) override;
	SINGULARISMORPHVEHICLE_API virtual void MergeData(const FNetworkPhysicsData& FromData) override;
	SINGULARISMORPHVEHICLE_API virtual void DecayData(float DecayAmount) override;
};

template <>
struct TStructOpsTypeTraits<
		FNetworkSingularisMorphVehicleInputs> : TStructOpsTypeTraitsBase2<FNetworkSingularisMorphVehicleInputs>
{
	enum { WithNetSerializer = true };
};

/**
 * 引力奇点变型载具网络状态令牌数据。
 * 存储模块数据的哈希值、索引与序列化标志，用于 Iris 增量复制。
 */
USTRUCT()
struct FNetworkSingularisMorphVehicleStateNetTokenData
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<uint32> Hashes;

	UPROPERTY()
	TArray<int32> Indexes;

	UPROPERTY()
	TArray<bool> ModuleShouldSerialize;

	UE_NET_NETTOKEN_GENERATED_BODY(NetworkSingularisMorphVehicleStateNetTokenData, SINGULARISMORPHVEHICLE_API);

	SINGULARISMORPHVEHICLE_API uint64 GetUniqueKey() const;
	SINGULARISMORPHVEHICLE_API void Init(const Chaos::FModuleNetDataArray& ModuleData);
};

UE_NET_DECLARE_NAMED_NETTOKEN_STRUCT_SERIALIZERS(
	NetworkSingularisMorphVehicleStateNetTokenData,
	SINGULARISMORPHVEHICLE_API
);

/**
 * 引力奇点变型载具网络状态数据。
 *
 * 存储模拟模块的网络复制状态数组，用于服务器状态历史中的回滚/重放。
 * 支持完整序列化与增量（Delta）序列化。
 */
USTRUCT()
struct FNetworkSingularisMorphVehicleStates : public FNetworkPhysicsData
{
	GENERATED_BODY()

	inline static auto StashServerFrameKey = FName("ServerFrame");
	Chaos::FModuleNetDataArray ModuleData;

	SINGULARISMORPHVEHICLE_API virtual void ApplyData(UActorComponent* NetworkComponent) const override;
	SINGULARISMORPHVEHICLE_API virtual void BuildData(const UActorComponent* NetworkComponent) override;
	SINGULARISMORPHVEHICLE_API bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
	SINGULARISMORPHVEHICLE_API bool DeltaNetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
	SINGULARISMORPHVEHICLE_API virtual void InterpolateData(
		const FNetworkPhysicsData& MinData,
		const FNetworkPhysicsData& MaxData
	) override;
};

template <>
struct TStructOpsTypeTraits<
		FNetworkSingularisMorphVehicleStates> : TStructOpsTypeTraitsBase2<FNetworkSingularisMorphVehicleStates>
{
	enum { WithNetSerializer = true };
};

/**
 * 引力奇点变型载具物理输出（物理线程→游戏线程）。
 *
 * 每个载具每帧由物理线程生成，包含模拟树输出数据与新创建模块 GUID 列表。
 */
struct FSingularisMorphVehiclePhysicsOutput
{
	FSingularisMorphVehiclePhysicsOutput() {}
	~FSingularisMorphVehiclePhysicsOutput() { Clean(); }

	/** 清理并释放所有模拟树输出数据 */
	void Clean()
	{
		for (Chaos::FSimOutputData* Data : SimTreeOutputData)
			if (Data) delete Data;
		SimTreeOutputData.Empty();
	}

	/** 根据模块 GUID 查找对应的模拟输出数据 */
	const Chaos::FSimOutputData* GetOutputData(int InModuleGuid)
	{
		if (SimTreeOutputData.IsEmpty()) return nullptr;

		for (auto I = 0; I < SimTreeOutputData.Num(); I++)
		{
			if (SimTreeOutputData[I] && InModuleGuid == SimTreeOutputData[I]->ModuleGuid)
				return SimTreeOutputData[I];
		}
		return nullptr;
	}

	TArray<Chaos::FSimOutputData*> SimTreeOutputData;
	TArray<Chaos::FCreatedModule> NewlyCreatedModuleGuids;
};

/** 物理载具输入/状态类型别名 */
struct FPhysicsSingularisMorphVehicleTraits
{
	using InputsType = FNetworkSingularisMorphVehicleInputs;
	using StatesType = FNetworkSingularisMorphVehicleStates;
};

/** 游戏线程端状态输入容器 */
struct FSingularisMorphGameStateInputs
{
	FModuleInputContainer StateInputContainer;
};

/** 引力奇点悬挂追踪类型 */
UENUM()
enum class ESingularisMorphTraceType : uint8
{
	Raycast UMETA(DisplayName = "射线检测"),
	Spherecast UMETA(DisplayName = "球体检测"),
};

/**
 * 物理载具输入（游戏线程→物理线程）。
 * 封装网络输入、碰撞参数与时间膨胀状态。
 */
struct FPhysicsSingularisMorphVehicleInputs
{
	FPhysicsSingularisMorphVehicleInputs()
		: CollisionChannel(ECC_WorldDynamic),
		  TraceParams(),
		  TraceCollisionResponse(),
		  TraceType(ESingularisMorphTraceType::Raycast),
		  CurrentTimeDilation(1.0f) {}

	mutable FNetworkSingularisMorphVehicleInputs NetworkInputs;
	mutable ECollisionChannel CollisionChannel;
	mutable FCollisionQueryParams TraceParams;
	mutable FCollisionResponseContainer TraceCollisionResponse;
	mutable ESingularisMorphTraceType TraceType;
	mutable FSingularisMorphGameStateInputs StateInputs;
	mutable float CurrentTimeDilation = 1.0f;
};

/**
 * 载具异步输入（游戏线程→物理线程）。
 *
 * 每个载具每帧由游戏线程构建，传递至物理线程异步回调的 Simulate 方法。
 * 携带物理代理指针、控制标志与输入数据。
 */
struct FSingularisMorphVehicleAsyncInput
{
	FSingularisMorphVehicleAsyncInput(ESingularisMorphChaosAsyncVehicleDataType InType = AsyncInvalid)
		: Type(InType), Vehicle(nullptr)
	{
		Proxy = nullptr;
	}

	virtual ~FSingularisMorphVehicleAsyncInput() = default;

	SINGULARISMORPHVEHICLE_API virtual TUniquePtr<FSingularisMorphVehicleAsyncOutput> Simulate(
		UWorld* World,
		float DeltaSeconds,
		float TotalSeconds,
		bool& bWakeOut
	) const;
	SINGULARISMORPHVEHICLE_API virtual void OnContactModification(Chaos::FCollisionContactModifier& Modifier) const;
	SINGULARISMORPHVEHICLE_API virtual void ApplyDeferredForces() const;
	SINGULARISMORPHVEHICLE_API virtual void ProcessInputs();

	void SetVehicle(USingularisMorphVehicleSimulationComponent* VehicleIn) { Vehicle = VehicleIn; }
	USingularisMorphVehicleSimulationComponent* GetVehicle() const { return Vehicle; }

	const ESingularisMorphChaosAsyncVehicleDataType Type;
	IPhysicsProxyBase* Proxy = nullptr;
	bool bIsLocallyControlled = false;

	FPhysicsSingularisMorphVehicleInputs PhysicsInputs;

private:
	USingularisMorphVehicleSimulationComponent* Vehicle;
};

/**
 * 模拟模块管理器异步输入。
 * 聚合所有需要处理的载具异步输入。
 */
struct FSingularisMorphChaosSimModuleManagerAsyncInput : Chaos::FSimCallbackInput
{
	TArray<TUniquePtr<FSingularisMorphVehicleAsyncInput>> VehicleInputs;
	TWeakObjectPtr<UWorld> World;
	int32 Timestamp = INDEX_NONE;

	void Reset()
	{
		VehicleInputs.Reset();
		World.Reset();
	}
};

/**
 * 载具异步输出（物理线程→游戏线程）。
 * 包含模拟运行结果、有效标志与关联载具引用。
 */
struct FSingularisMorphVehicleAsyncOutput
{
	const ESingularisMorphChaosAsyncVehicleDataType Type;
	bool bValid;
	FSingularisMorphVehiclePhysicsOutput VehicleSimOutput;
	USingularisMorphVehicleSimulationComponent* Vehicle;

	FSingularisMorphVehicleAsyncOutput(ESingularisMorphChaosAsyncVehicleDataType InType = AsyncInvalid)
		: Type(InType), bValid(false), Vehicle(nullptr) {}

	virtual ~FSingularisMorphVehicleAsyncOutput() { VehicleSimOutput.Clean(); }
};

/**
 * 模拟模块管理器异步输出。
 * 聚合所有载具的异步输出结果。
 */
struct FSingularisMorphChaosSimModuleManagerAsyncOutput : Chaos::FSimCallbackOutput
{
	TArray<TUniquePtr<FSingularisMorphVehicleAsyncOutput>> VehicleOutputs;
	int32 Timestamp = INDEX_NONE;

	void Reset() { VehicleOutputs.Reset(); }
};

/**
 * 引力奇点变型模拟模块管理器异步回调。
 *
 * 这是整个载具模拟系统在物理线程侧的入口枢纽，继承自 Chaos::TSimCallbackObject，
 * 注册在 Chaos Solver 上并在物理管线的特定阶段被回调。
 *
 * 模板参数说明：
 * - FSingularisMorphChaosSimModuleManagerAsyncInput：输入类型，由游戏线程写入，物理线程消费。
 * - FSingularisMorphChaosSimModuleManagerAsyncOutput：输出类型，由物理线程产出，游戏线程拉取。
 * - ESimCallbackOptions 组合标志，决定在哪些物理管线阶段触发回调：
 *   Presimulate         → OnPreSimulate_Internal()，在物理步进前执行载具模拟。
 *   Rewind              → 支持网络物理回放（服务器复现历史步骤）。
 *   ContactModification → OnContactModification_Internal()，在碰撞解算阶段修改碰撞响应。
 *
 * 核心回调流程（物理线程）：
 *   ProcessInputs_Internal()  → 分发 ProcessInputs() 到各载具，处理输入方向（本地 vs 远程）。
 *   OnPreSimulate_Internal()  → 并行调用各载具的 Simulate()，产出异步输出数据。
 *                              → 串行调用 ApplyDeferredForces()（力的施加不能多线程）。
 *   OnContactModification_Internal() → 并行调用各载具的碰撞修正回调。
 *
 * 数据编组机制：
 * - GetConsumerInput_Internal() 读取游戏线程通过 AsyncCallback->GetProducerInputData_External() 写入的输入。
 * - GetProducerOutputData_Internal() 写入输出，游戏线程通过 AsyncCallback->PopOutputData_External() 取出。
 * - Chaos Solver 的 TSimCallbackObject 框架自动管理输入/输出缓冲区的双缓冲切换。
 */
class FSingularisMorphSimModuleManagerAsyncCallback : public Chaos::TSimCallbackObject<
		FSingularisMorphChaosSimModuleManagerAsyncInput, FSingularisMorphChaosSimModuleManagerAsyncOutput
		, Chaos::ESimCallbackOptions::Presimulate | Chaos::ESimCallbackOptions::Rewind |
		Chaos::ESimCallbackOptions::ContactModification>
{
public:
	/** 返回统计 ID 名称，供性能分析器（Unreal Insights）识别。 */
	SINGULARISMORPHVEHICLE_API virtual FName GetFNameForStatId() const override;

private:
	/**
	 * 输入预处理回调（每个物理子步调用一次）。
	 * 遍历所有载具的异步输入，调用 ProcessInputs() 处理输入方向。
	 */
	SINGULARISMORPHVEHICLE_API virtual void ProcessInputs_Internal(int32 PhysicsStep) override;

	/**
	 * 预模拟回调（每个物理子步调用一次）。
	 * 并行执行所有载具的模拟计算，产出异步输出数据。
	 * 模拟完成后串行施加延迟力（力的施加不能多线程执行）。
	 */
	SINGULARISMORPHVEHICLE_API virtual void OnPreSimulate_Internal() override;

	/**
	 * 碰撞修正回调（每个物理子步调用一次）。
	 * 并行调用各载具的碰撞响应处理，允许修改碰撞解算结果。
	 */
	SINGULARISMORPHVEHICLE_API virtual void OnContactModification_Internal(
		Chaos::FCollisionContactModifier& Modifications
	) override;
};
