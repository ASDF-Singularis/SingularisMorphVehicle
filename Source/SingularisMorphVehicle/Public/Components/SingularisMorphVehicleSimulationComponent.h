#pragma once

#include <CoreMinimal.h>
#include <Chaos/Framework/PhysicsSolverBase.h>
#include <Components/ActorComponent.h>
#include <SimModule/ModuleInput.h>
#include <SimModule/SimModuleTree.h>

#include "Types/SingularisMorphSimModuleManagerAsyncCallback.h"
#include "Types/SingularisMorphVehiclePhysicsAdapterType.h"
#include "Types/SingularisMorphVehicleSimType.h"
#include "SingularisMorphVehicleSimulationComponent.generated.h"

namespace Chaos
{
	struct FSimOutputData;
	struct FCreatedModules;
	class FSimModuleTree;
	class FSimTreeUpdates;
	class ISimulationModuleBase;
}

class UNetworkPhysicsComponent;
class UVehicleInputProducerBase;
struct FSingularisMorphVehicleAsyncInput;
struct FSingularisMorphVehicleAsyncOutput;
struct FSingularisMorphVehiclePhysicsOutput;
struct FSingularisMorphChaosSimModuleManagerAsyncInput;
struct FSingularisMorphChaosSimModuleManagerAsyncOutput;
class FDebugDisplayInfo;
class FSingularisMorphVehicleSimulation;
class USingularisMorphVehiclePhysicsAdapter;
class IPhysicsProxyBase;
class AHUD;
class UCanvas;
class USingularisMorphVehicleSUComponent;
class USingularisSuspensionSUComponent;
class USingularisWheelSUComponent;

#pragma region 委托签名

DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FOnSimulationModuleAddedNative,
	const FName& /*ModuleType*/,
	int32 /*ModuleGuid*/,
	int32 /*TreeIndex*/
);

DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FOnSimulationModuleRemovedNative,
	const FName& /*ModuleType*/,
	int32 /*ModuleGuid*/,
	int32 /*TreeIndex*/
);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FOnSimulationModuleAdded,
	const FName&,
	ModuleType,
	int32,
	ModuleGuid,
	int32,
	TreeIndex
);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FOnSimulationModuleRemoved,
	const FName&,
	ModuleType,
	int32,
	ModuleGuid,
	int32,
	TreeIndex
);

#pragma endregion

/**
 * 引力奇点变型载具仿真组件
 *
 * 挂载于变型载具 Actor/Pawn 上，作为整个模块化模拟系统的核心调度器。
 * 管理模拟模块树、异步物理回调、物理适配器、输入生产者与动力学输出，
 * 通过 FSingularisMorphVehicleSimulation 在物理线程上执行动力学计算。
 *
 * 与集群联合组件完全正交——不持有、不引用、不调用任何集群联合相关逻辑。
 * 物理来源由用户在编辑器中配置的 PhysicsAdapter 实例负责桥接。
 *
 * GT: 游戏线程  PT: 物理线程
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具仿真组件")
)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleSimulationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 仿真树处理顺序 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "仿真树处理顺序")
	)
	TEnumAsByte<ESimTreeProcessingOrder> SimulationTreeProcessingOrder = LeafFirst;

	/** 输入量化类型 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "输入量化类型")
	)
	EModuleInputQuantizationType InputQuantizationType = EModuleInputQuantizationType::Default_16Bits;

	/** 输入生产者类，控制输入的采集与产出策略（真人/回放/随机） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "输入生产者类")
	)
	TSubclassOf<UVehicleInputProducerBase> InputProducerClass{};

	/** 状态输入配置（区别于控制输入，供模块读取非瞬态状态） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "状态输入配置")
	)
	TArray<FModuleInputSetup> StateInputConfiguration{};
	/** 组件级控制输入配置（与 SU 组件的输入配置合并去重） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "控制输入配置")
	)
	TArray<FModuleInputSetup> InputConfig{};

	/**
	 * 物理适配器实例。
	 *
	 * 用户在编辑器中创建并配置适配器子对象（如 ClusterUnionAdapter），
	 * 由适配器负责从特定物理来源获取代理并将组件增删事件翻译为标准 API 调用。
	 * 若留空则为手动模式，外部代码直接调用 AddSimulationModule 等 API。
	 */
	UPROPERTY(
		Instanced,
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "物理适配器")
	)
	TObjectPtr<USingularisMorphVehiclePhysicsAdapter> PhysicsAdapter = nullptr;

	/** 保持载具唤醒（禁止休眠） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "禁止休眠")
	)
	bool bKeepVehicleAwake = true;

	/** 线性阻尼（空气阻力） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|阻尼",
		meta = (DisplayName = "线性阻尼")
	)
	float LinearDamping = 0.01f;

	/** 角阻尼（旋转阻力） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|阻尼",
		meta = (DisplayName = "角阻尼")
	)
	float AngularDamping = 0.5f;

	/** 悬挂射线检测通道 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|悬挂",
		meta = (DisplayName = "悬挂射线通道")
	)
	TEnumAsByte<ECollisionChannel> SuspensionCollisionChannel = ECC_WorldDynamic;

	/** 悬挂射线碰撞响应 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|悬挂",
		meta = (DisplayName = "悬挂射线碰撞响应")
	)
	FCollisionResponseContainer SuspensionTraceCollisionResponses{};

	/** 悬挂射线是否使用复杂碰撞 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|悬挂",
		meta = (DisplayName = "悬挂射线复杂碰撞")
	)
	bool bSuspensionTraceComplex = true;

	/** 悬挂射线类型 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|悬挂",
		meta = (DisplayName = "悬挂射线类型")
	)
	ESingularisMorphTraceType TraceType = ESingularisMorphTraceType::Raycast;

#pragma endregion

#pragma region 事件分发器

	/** 模拟模块添加事件（原生，低开销） */
	FOnSimulationModuleAddedNative OnSimulationModuleAddedNativeEvent{};

	/** 模拟模块移除事件（原生，低开销） */
	FOnSimulationModuleRemovedNative OnSimulationModuleRemovedNativeEvent{};

	/** 模拟模块添加事件（Blueprint） */
	UPROPERTY(
		BlueprintAssignable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|事件分发器",
		meta = (DisplayName = "模拟模块添加")
	)
	FOnSimulationModuleAdded OnSimulationModuleAddedEvent{};

	/** 模拟模块移除事件（Blueprint） */
	UPROPERTY(
		BlueprintAssignable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|事件分发器",
		meta = (DisplayName = "模拟模块移除")
	)
	FOnSimulationModuleRemoved OnSimulationModuleRemovedEvent{};

#pragma endregion

private:
#pragma region Internal Variable

	/** 模拟模块树（游戏线程创建，物理线程接管所有权） */
	TUniquePtr<Chaos::FSimModuleTree> SimulationModuleTree = nullptr;

	/** 物理线程输出的插值数据 */
	TUniquePtr<FSingularisMorphVehiclePhysicsOutput> VehiclePhysicsOutput = nullptr;

	/** 组件到模拟对象的映射（Key = SU 组件，避免多模块共享同一代理时的覆盖） */
	TMap<TObjectPtr<UActorComponent>, FSingularisMorphVehicleComponentData> ComponentToPhysicsObjects{};

	/** 模拟 GUID 到 SU 组件的反向映射（供 ParallelUpdate 回调 OnOutputReady） */
	TMap<int32, TWeakObjectPtr<UActorComponent>> PhysicsGuidToComponent{};

	Chaos::FPhysicsObjectHandle RootPhysicsObject = nullptr;

	/** 模块动画配置列表 */
	TArray<FSingularisMorphModuleAnimationSetup> ModuleAnimationSetups{};

	/** 缓存的物理代理（仅游戏线程） */
	mutable IPhysicsProxyBase* CachedPhysicsProxy = nullptr;

	/** 网络物理组件（网络预测开启时于构造函数创建） */
	UPROPERTY(Transient)
	TObjectPtr<UNetworkPhysicsComponent> NetworkPhysicsComponent = nullptr;

	/** 输入生产者实例（由 InputProducerClass 实例化） */
	UPROPERTY(Transient)
	TObjectPtr<UVehicleInputProducerBase> InputProducer = nullptr;

	/** 控制输入名到索引的映射 */
	FInputInterface::FInputNameMap InputNameMap{};

	/** 状态输入名到索引的映射 */
	FInputInterface::FInputNameMap StateNameMap{};

	/** 控制输入容器（GT 侧，ProduceInput 时拷出） */
	FModuleInputContainer InputsContainer{};

	/** 状态输入容器（GT 侧） */
	FModuleInputContainer StateInputContainer{};

	/** 当前合并的输入配置（用于变更检测） */
	TArray<FModuleInputSetup> CombinedInputConfiguration{};

	/** 本地控制回退标记（无 NetworkPhysicsComponent 时生效） */
	bool bIsLocallyControlled = false;

	/** 当前挡位（由变速箱输出缓存） */
	int32 CurrentGear = 0;

	/** 引擎转速（由引擎输出缓存） */
	float EngineRPM = 0.0f;

	/** 引擎扭矩（由引擎输出缓存） */
	float EngineTorque = 0.0f;

	/** GUID 到核心模块的映射（仅游戏线程访问，避免读取物理线程拥有的模拟树） */
	TMap<int32, Chaos::ISimulationModuleBase*> GuidToCoreModule{};

	/** 待重建网络复制结构标记（模块增删后置位，PostUpdate 消费） */
	bool bPendingReplicationStructureRebuild = false;

	/** 构建索引计数器 */
	int32 NextConstructionIndex = 0;

	/** 悬挂射线忽略的 Actor 列表 */
	TArray<AActor*> ActorsToIgnore{};

public:
	/** 存储的树更新队列 */
	Chaos::FSimTreeUpdates StoredTreeUpdates{};

	/** 网络物理预测开关 */
	bool bUsingNetworkPhysicsPrediction = false;

	/** 当前异步数据类型 */
	ESingularisMorphChaosAsyncVehicleDataType CurrentAsyncDataType = AsyncInvalid;

	/** 当前帧异步输入（游戏线程→物理线程） */
	const FSingularisMorphVehicleAsyncInput* CurrentAsyncInput = nullptr;

	/** 当前帧异步输出（物理线程→游戏线程） */
	const FSingularisMorphVehicleAsyncOutput* CurrentAsyncOutput = nullptr;

	/** 下一帧异步输出（用于插值） */
	const FSingularisMorphVehicleAsyncOutput* NextAsyncOutput = nullptr;

	/** 异步输出插值系数 [0..1] */
	float OutputInterpolationAlpha = 0.0f;

private:
#pragma endregion

public:
#pragma region Constructors

	USingularisMorphVehicleSimulationComponent();

#pragma endregion

#pragma region ActorComponent Interface

protected:
	virtual bool ShouldCreatePhysicsState() const override;
	virtual void OnCreatePhysicsState() override;
	virtual void OnDestroyPhysicsState() override;

public:
#pragma endregion

#pragma region API

	/**
	 * 添加模拟模块（手动模式 API，适配器留空时由外部代码调用）。
	 *
	 * ParentIndex 语义与 FSimTreeUpdates 一致：本批次首条为绝对树索引，
	 * 其后为批次内局部索引（由前序 AddSimulationModule 的返回值指定）。
	 *
	 * 增删同样遵循分批提交语义：调用不会立即生效，须以 FinalizeModuleUpdates() 收尾。
	 */
	int32 AddSimulationModule(
		Chaos::ISimulationModuleBase* CoreModule,
		const FTransform& ComponentTransform,
		int32 ParentIndex,
		int32 TransformIndex = INDEX_NONE,
		Chaos::FUniqueIdx ParticleIndex = Chaos::FUniqueIdx(),
		const FTransform& PhysicalTransform = FTransform::Identity
	);

	/**
	 * 移除模拟模块（手动模式 API）。
	 *
	 * 仅入队待删除：模块的终止回调与树节点删除均在 FinalizeModuleUpdates() 提交后生效。
	 */
	void RemoveSimulationModule(int32 ModuleGuid);

	/**
	 * 提交本帧累积的全部模块增删到物理线程。
	 *
	 * 无参数变更时为空操作；提交后重新应用物理阻尼并标记网络复制结构待重建。
	 */
	void FinalizeModuleUpdates();

	/**
	 * 基于快照全量重建物理模拟树。
	 *
	 * 清空现有模拟树缓存，按确定性模块类型层级重建整棵模拟树；
	 * 模块来源为快照实体上映射的 SU 组件加 Owner 上其余 SU 组件（纯仿真模块）。
	 * 处理完毕后调用 FinalizeModuleUpdates 批量提交到物理线程。
	 * 空快照且当前无模块时无操作；空快照且存在模块（载具解体）时清除全部模块。
	 */
	void RebuildFromSnapshot(const FSingularisMorphVehiclePhysicsAdapterSnapshot& Snapshot);

	/** 更新物理阻尼属性 */
	void UpdatePhysicalProperties();

	/** 获取模块动画配置（可修改） */
	TArray<FSingularisMorphModuleAnimationSetup>& AccessModuleAnimationSetups()
	{
		return ModuleAnimationSetups;
	}

	/** 获取模块动画配置（只读） */
	const TArray<FSingularisMorphModuleAnimationSetup>& GetModuleAnimationSetups() const
	{
		return ModuleAnimationSetups;
	}

	/** 获取仿真树处理顺序 */
	ESimTreeProcessingOrder GetSimulationTreeProcessingOrder() const { return SimulationTreeProcessingOrder; }

	/** 获取仿真树处理顺序（旧名称，兼容已有代码） */
	ESimTreeProcessingOrder GetTreeProcessingOrder() const { return SimulationTreeProcessingOrder; }

	/** 物理线程端载具模拟（保留旧名称以兼容异步回调） */
	TUniquePtr<FSingularisMorphVehicleSimulation> VehicleSimulationPT;

	/** 缓存根物理对象 */
	void CacheRootPhysicsObject(IPhysicsProxyBase* Proxy);

	/** 设置输入生产者类并重建输入配置 */
	void SetInputProducerClass(
		TSubclassOf<UVehicleInputProducerBase> InInputProducerClass,
		bool bForceNewInstance = false
	);

	/** 追加一条控制输入配置并重建输入容器 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|API",
		meta = (DisplayName = "添加控制输入配置")
	)
	void AddInput(const FModuleInputSetup& InputSetup);

	/** 添加悬挂射线忽略的 Actor */
	void AddActorsToIgnore(TArray<AActor*>& ActorsIn);

	/** 移除悬挂射线忽略的 Actor */
	void RemoveActorsToIgnore(TArray<AActor*>& ActorsIn);

#pragma endregion

#pragma region SPI

	/** 游戏线程更新入口 */
	void Update(float DeltaTime);

	/** 游戏线程预更新（处理挂起的模块变更） */
	void PreTickGT(float DeltaTime);

	/** 设置当前异步输出数据并执行插值 */
	void SetCurrentAsyncData(
		FSingularisMorphChaosSimModuleManagerAsyncOutput* CurOutput,
		FSingularisMorphChaosSimModuleManagerAsyncOutput* NextOutput,
		float Alpha,
		int32 Timestamp
	);

	/** 并行更新（读取异步输出数据、分发到模块组件） */
	void ParallelUpdate(const Chaos::FCreatedModules& ModuleEvents);

	/** 产生输入数据提供给物理线程 */
	void ProduceInput(
		int32 PhysicsStep,
		int32 NumSteps,
		FSingularisMorphVehicleAsyncInput* AsyncInput
	);

	/** 后更新 */
	void PostUpdate();

	/** 完成异步回调数据处理 */
	void FinalizeSimCallbackData(FSingularisMorphChaosSimModuleManagerAsyncInput& Input);

	/** 调试信息显示 */
	void ShowDebugInfo(
		AHUD* HUD,
		UCanvas* Canvas,
		const FDebugDisplayInfo& DisplayInfo,
		float& YL,
		float& YPos
	);

	/** 物理输出容器访问 */
	TUniquePtr<FSingularisMorphVehiclePhysicsOutput>& GetVehiclePhysicsOutput()
	{
		return VehiclePhysicsOutput;
	}

	/** 模拟模块树访问 */
	TUniquePtr<Chaos::FSimModuleTree>& AccessSimulationModuleTree()
	{
		return SimulationModuleTree;
	}

	/** 获取组件变换 */
	const FTransform& GetComponentTransform() const;

	/** 忽略 Actor 列表访问 */
	TArray<AActor*>& AccessActorsToIgnore() { return ActorsToIgnore; }

	/** 获取当前异步输入 */
	const FSingularisMorphVehicleAsyncInput* GetCurrentAsyncInput() const { return CurrentAsyncInput; }

	/** 按模块 GUID 获取最近的输出数据 */
	const Chaos::FSimOutputData* GetOutputData(int32 ModuleGuid);

#pragma endregion

#pragma region 输入

	/** 写入布尔控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|输入",
		meta = (DisplayName = "设置布尔输入")
	)
	void SetInputBool(
		const FName Name,
		const bool Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);

	/** 写入整数控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|输入",
		meta = (DisplayName = "设置整数输入")
	)
	void SetInputInteger(
		const FName Name,
		const int32 Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);

	/** 写入一维轴控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|输入",
		meta = (DisplayName = "设置一维轴输入")
	)
	void SetInputAxis1D(
		const FName Name,
		const double Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);

	/** 写入二维轴控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|输入",
		meta = (DisplayName = "设置二维轴输入")
	)
	void SetInputAxis2D(
		const FName Name,
		const FVector2D Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);

	/** 写入三维轴控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|输入",
		meta = (DisplayName = "设置三维轴输入")
	)
	void SetInputAxis3D(
		const FName Name,
		const FVector Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);

	/** 设置目标挡位（手动变速箱） */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|输入",
		meta = (DisplayName = "设置挡位输入")
	)
	void SetGearInput(int32 Gear);

	/** C++ 重载：写入控制输入 */
	void SetInput(
		const FName& Name,
		const bool Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);
	void SetInput(
		const FName& Name,
		const int32 Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);
	void SetInput(
		const FName& Name,
		const double Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);
	void SetInput(
		const FName& Name,
		const FVector2D& Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);
	void SetInput(
		const FName& Name,
		const FVector& Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	);

	/** C++ 重载：写入状态输入 */
	void SetState(const FName& Name, const bool Value);
	void SetState(const FName& Name, const int32 Value);
	void SetState(const FName& Name, const double Value);
	void SetState(const FName& Name, const FVector2D& Value);
	void SetState(const FName& Name, const FVector& Value);

	/** 设置本地控制状态（网络预测） */
	void SetLocallyControlled(bool bInLocallyControlled);

#pragma endregion

#pragma region State

	/** 获取当前物理适配器（可能为 nullptr = 手动模式） */
	UFUNCTION(
		BlueprintPure,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|State",
		meta = (DisplayName = "获取物理适配器")
	)
	USingularisMorphVehiclePhysicsAdapter* GetPhysicsAdapter() const { return PhysicsAdapter; }

	/** 是否本地控制 */
	UFUNCTION(
		BlueprintPure,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|State",
		meta = (DisplayName = "是否本地控制")
	)
	bool IsLocallyControlled() const;

	/** 获取当前挡位 */
	UFUNCTION(
		BlueprintPure,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|State",
		meta = (DisplayName = "获取当前挡位")
	)
	int32 GetCurrentGear() const { return CurrentGear; }

	/** 是否正在倒车 */
	UFUNCTION(
		BlueprintPure,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|State",
		meta = (DisplayName = "是否倒车")
	)
	bool IsReversing() const { return CurrentGear < 0; }

	/** 获取引擎转速（RPM） */
	UFUNCTION(
		BlueprintPure,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|State",
		meta = (DisplayName = "获取引擎转速")
	)
	float GetEngineRPM() const { return EngineRPM; }

	/** 获取引擎扭矩 */
	UFUNCTION(
		BlueprintPure,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|State",
		meta = (DisplayName = "获取引擎扭矩")
	)
	float GetEngineTorque() const { return EngineTorque; }

	/** 获取车速（公里/小时） */
	UFUNCTION(
		BlueprintPure,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|State",
		meta = (DisplayName = "获取车速(km/h)")
	)
	float GetVehicleSpeed() const;

	/** 获取输入生产者实例 */
	UVehicleInputProducerBase* GetInputProducer() const { return InputProducer; }

#pragma endregion

private:
#pragma region Internal Function

	void CreateVehicleSimulation();
	void DestroyVehicleSimulation();

	/** 清除全部模拟模块（载具解体/销毁时释放资源并提交删除） */
	void ClearAllSimulationModules();
	IPhysicsProxyBase* GetPhysicsProxy() const;
	int32 GenerateNewGuid();

	/** 聚合所有 SU 组件的输入配置（去重） */
	void AssimilateComponentInputs(TArray<FModuleInputSetup>& OutCombinedInputs);

	/** 初始化输入生产者与输入容器（配置变更时幂等重建） */
	void SetupInputConfiguration(bool bForceReinitialize = false);

	/** 将动画数据应用到非骨骼可视化组件 */
	void UpdateNonSkeletalAnimations();

	Chaos::FSimOutputData* FindModuleOutputFromGuid(
		const FSingularisMorphVehiclePhysicsOutput& OutputContainer,
		int32 Guid
	) const;

	void BroadcastModuleAddedEvent(const FName& ModuleType, int32 Guid, int32 TreeIndex);
	void BroadcastModuleRemovedEvent(const FName& ModuleType, int32 Guid, int32 TreeIndex);

	/**
	 * 内部模块注册（Internal API）。
	 *
	 * 将单个 CoreModule 注册到模拟树，设置正确的粒子索引和变换。
	 * BoneName/AnimationOffset 来自模块对应的 SU 组件，用于绑定动画槽位；
	 * 留空时沿用模块自身的骨骼信息（手动注册路径）。
	 *
	 * @return TreeIndex，失败返回 INDEX_NONE
	 */
	int32 AddModuleToTree(
		Chaos::ISimulationModuleBase* CoreModule,
		const FTransform& ComponentTransform,
		int32 ParentIndex,
		int32 TransformIndex,
		Chaos::FUniqueIdx ParticleIndex,
		const FTransform& PhysicalTransform,
		const FName& BoneName = NAME_None,
		const FVector& AnimationOffset = FVector::ZeroVector
	);

	/**
	 * 注册模块的动画槽位。
	 *
	 * 骨骼名非空时复用同一骨骼既有的槽位，避免车轮、悬挂等共用骨骼的模块
	 * 对同一骨骼重复施加变换；骨骼名为空（非骨骼动画）的模块各自独占槽位，
	 * 与可视化组件一一对应。模块未启用动画时不占用槽位。
	 *
	 * @return 动画槽位索引，未注册时返回 INDEX_NONE
	 */
	int32 RegisterModuleAnimationSetup(
		Chaos::ISimulationModuleBase* CoreModule,
		const FName& BoneName,
		const FVector& AnimationOffset,
		const FTransform& InitialTransform
	);

#pragma endregion

#pragma region Callback

	void OnSimulationModuleInitialized(const FName& ModuleType, int32 Guid, int32 TreeIndex);
	void OnSimulationModuleRemovedCallback(const FName& ModuleType, int32 Guid, int32 TreeIndex);

#pragma endregion
};
