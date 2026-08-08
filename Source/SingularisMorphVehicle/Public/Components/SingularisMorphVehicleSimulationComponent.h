#pragma once

#include <CoreMinimal.h>
#include <Chaos/Framework/PhysicsSolverBase.h>
#include <Components/ActorComponent.h>
#include <SimModule/SimModuleTree.h>

#include "Types/SingularisMorphSimModuleManagerAsyncCallback.h"
#include "Types/SingularisMorphVehiclePhysicsAdapterType.h"
#include "Types/SingularisMorphVehicleSimType.h"
#include "SingularisMorphVehicleSimulationComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSingularisMorphBase, Log, All);

namespace Chaos
{
	struct FSimOutputData;
	struct FCreatedModules;
	class FSimModuleTree;
	class FSimTreeUpdates;
	class ISimulationModuleBase;
}

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
class USingularisMorphVehicleSuspensionSUComponent;
class USingularisMorphVehicleWheelSUComponent;

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
 * 管理模拟模块树、异步物理回调、物理适配器与动力学输出，
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
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具仿真组件")
)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleSimulationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 仿真树处理顺序 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "仿真树处理顺序")
	)
	TEnumAsByte<ESimTreeProcessingOrder> SimulationTreeProcessingOrder = LeafFirst;

	/** 输入量化类型 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "输入量化类型")
	)
	EModuleInputQuantizationType InputQuantizationType = EModuleInputQuantizationType::Default_16Bits;

	/**
	 * 物理适配器实例。
	 *
	 * 用户在编辑器中创建并配置适配器子对象（如 ClusterUnionAdapter），
	 * 由适配器负责从特定物理来源获取代理并将组件增删事件翻译为标准 API 调用。
	 * 若留空则为手动模式，外部代码直接调用 AddSimulationModule 等 API。
	 */
	UPROPERTY(
		Instanced,
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "物理适配器")
	)
	TObjectPtr<USingularisMorphVehiclePhysicsAdapter> PhysicsAdapter = nullptr;

	/** 线性阻尼（空气阻力） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|阻尼",
		meta = (DisplayName = "线性阻尼")
	)
	float LinearDamping = 0.01f;

	/** 角阻尼（旋转阻力） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|阻尼",
		meta = (DisplayName = "角阻尼")
	)
	float AngularDamping = 0.0f;

	/** 保持载具唤醒（禁止休眠） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|参数",
		meta = (DisplayName = "禁止休眠")
	)
	bool bKeepVehicleAwake = true;

	/** 悬挂射线检测通道 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|悬挂",
		meta = (DisplayName = "悬挂射线通道")
	)
	TEnumAsByte<ECollisionChannel> SuspensionCollisionChannel = ECC_WorldDynamic;

	/** 悬挂射线碰撞响应 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|悬挂",
		meta = (DisplayName = "悬挂射线碰撞响应")
	)
	FCollisionResponseContainer SuspensionTraceCollisionResponses{};

	/** 悬挂射线是否使用复杂碰撞 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|悬挂",
		meta = (DisplayName = "悬挂射线复杂碰撞")
	)
	bool bSuspensionTraceComplex = false;

	/** 悬挂射线类型 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
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

	/** 变换索引计数器 */
	int32 NextTransformIndex = 0;

	/** 构建索引计数器 */
	int32 NextConstructionIndex = 0;

	/** 模拟树是否已构建 */
	bool bSimTreeBuilt = false;

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

	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction
	) override;

protected:
	virtual bool ShouldCreatePhysicsState() const override;
	virtual void OnCreatePhysicsState() override;
	virtual void OnDestroyPhysicsState() override;

public:
#pragma endregion

#pragma region API

	int32 AddSimulationModule(
		Chaos::ISimulationModuleBase* CoreModule,
		const FTransform& ComponentTransform,
		int32 ParentIndex,
		int32 TransformIndex = INDEX_NONE,
		Chaos::FUniqueIdx ParticleIndex = Chaos::FUniqueIdx(),
		const FTransform& PhysicalTransform = FTransform::Identity
	);

	void RemoveSimulationModule(int32 ModuleGuid);

	void FinalizeModuleUpdates();

	/**
	 * 基于快照全量重建物理模拟树。
	 *
	 * 清空现有模拟树缓存，按确定性模块类型层级重建整棵模拟树。
	 * 处理完毕后调用 FinalizeModuleUpdates 批量提交到物理线程。
	 * 空快照无操作，保证幂等。
	 */
	void RebuildFromSnapshot(const FSingularisMorphVehiclePhysicsAdapterSnapshot& Snapshot);

	/** 更新物理阻尼属性 */
	void UpdatePhysicalProperties();

	/** 设置被更新的物理组件（兼容 PawnMovementComponent 接口） */
	void SetUpdatedComponent(USceneComponent* InUpdatedComponent = nullptr);

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

#pragma endregion

#pragma region State

	/** 获取当前物理适配器（可能为 nullptr = 手动模式） */
	UFUNCTION(
		BlueprintPure,
		Category = "SingularisMorphVehicle|引力奇点变型载具仿真|State",
		meta = (DisplayName = "获取物理适配器")
	)
	USingularisMorphVehiclePhysicsAdapter* GetPhysicsAdapter() const { return PhysicsAdapter; }

#pragma endregion

private:
#pragma region Internal Function

	void CreateVehicleSimulation();
	void DestroyVehicleSimulation();
	void ActionTreeUpdates(Chaos::FSimTreeUpdates* NextTreeUpdates);
	IPhysicsProxyBase* GetPhysicsProxy() const;
	int32 GenerateNewGuid();

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
	 * 与旧代码 AddModuleToTree lambda 行为一致。
	 *
	 * @return TreeIndex，失败返回 INDEX_NONE
	 */
	int32 AddModuleToTree(
		Chaos::ISimulationModuleBase* CoreModule,
		const FTransform& ComponentTransform,
		int32 ParentIndex,
		int32 TransformIndex,
		Chaos::FUniqueIdx ParticleIndex,
		const FTransform& PhysicalTransform
	);

#pragma endregion

#pragma region Callback

	void OnSimulationModuleInitialized(const FName& ModuleType, int32 Guid, int32 TreeIndex);
	void OnSimulationModuleRemovedCallback(const FName& ModuleType, int32 Guid, int32 TreeIndex);

#pragma endregion
};
