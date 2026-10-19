#pragma once

#include <CoreMinimal.h>
#include <Chaos/Framework/PhysicsSolverBase.h>
#include <Components/ActorComponent.h>
#include <HAL/CriticalSection.h>
#include <SimModule/ModuleInput.h>
#include <SimModule/SimModuleTree.h>

#include "Types/SingularisMorphSimModuleManagerAsyncCallback.h"
#include "Types/SingularisMorphVehiclePhysicsAdapterType.h"
#include "Types/SingularisMorphVehicleSimType.h"
#include "Types/SingularisMorphVehicleTreeUpdate.h"
#include "SingularisMorphVehicleSimulationComponent.generated.h"

namespace Chaos
{
	struct FSimOutputData;
	struct FCreatedModules;
	struct FWheelOutputData;
	class FSimModuleTree;
	class FSimTreeUpdates;
	class FTransmissionSimModule;
	class ISimulationModuleBase;
}

struct FSingularisMorphVehicleAsyncInput;
struct FSingularisMorphVehicleAsyncOutput;
struct FSingularisMorphVehiclePhysicsOutput;
struct FSingularisMorphChaosSimModuleManagerAsyncInput;
struct FSingularisMorphChaosSimModuleManagerAsyncOutput;
struct FSingularisMorphSuspensionOutputData;
class UNetworkPhysicsComponent;
class UVehicleInputProducerBase;
class FDebugDisplayInfo;
class FSingularisMorphVehicleSimulation;
class USingularisMorphVehiclePhysicsAdapter;
class IPhysicsProxyBase;
class AHUD;
class UCanvas;
class USingularisMorphVehicleBaseComponent;

/**
 * 车轮模拟期状态抑制的原始状态快照。
 *
 * 记录抑制前组件的碰撞启用模式、逐通道响应与独立物理模拟状态，供轮胎分离或载具拆除时
 * 逐项恢复；抑制过程不改变碰撞物体类型与移动性，故不记录这些字段。
 */
struct FSingularisMorphWheelCollisionSnapshot
{
	ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::QueryAndPhysics;
	FCollisionResponseContainer CollisionResponses{};
	bool bSimulatePhysics = false;
};

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

/** 换挡事件：非空挡位咬合时广播，携带换挡前后的挡位 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnGearShiftedSignature,
	int32,
	OldGear,
	int32,
	NewGear
);

/** 车轮开始打滑事件：侧滑角越过起始阈值且车轮接地时广播，携带车轮模块 GUID */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnWheelSlipStartedSignature,
	int32,
	WheelModuleGuid
);

/** 车轮停止打滑事件：侧滑角回落到解除阈值以下或车轮离地时广播，携带车轮模块 GUID */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnWheelSlipStoppedSignature,
	int32,
	WheelModuleGuid
);

/** 悬挂触底事件：弹簧压缩量达到最大压缩行程时广播（仅上升沿一次），携带悬挂模块 GUID */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnSuspensionBottomedOutSignature,
	int32,
	SuspensionModuleGuid
);

#pragma endregion

/**
 * 引力奇点变型载具仿真组件
 *
 * 挂载于变型载具 Actor/Pawn 上，作为整个模块化模拟系统的核心调度器。
 * 管理模拟模块树、异步物理回调、物理适配器、输入生产者与动力学输出，
 * 通过 FSingularisMorphVehicleSimulation 在物理线程上执行动力学计算。
 *
 * 输出侧数据访问采用推拉混合模型：
 * 连续状态（转速/扭矩/车速/挡位）由观察者在 Tick 中拉取（State 区域的 Getter），
 * 离散事件（模块增删/换挡/打滑/触底）由本组件在游戏线程边沿检测后经事件分发器推送。
 *
 * 与任何具体物理后端完全正交——不持有、不引用、不调用特定物理实现的相关逻辑。
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
		Category = "引力奇点变型载具仿真组件",
		meta = (DisplayName = "仿真树处理顺序")
	)
	TEnumAsByte<ESimTreeProcessingOrder> SimulationTreeProcessingOrder = LeafFirst;

	/** 输入量化类型 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件",
		meta = (DisplayName = "输入量化类型")
	)
	EModuleInputQuantizationType InputQuantizationType = EModuleInputQuantizationType::Default_16Bits;

	/** 输入生产者类，控制输入的采集与产出策略（真人/回放/随机） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件",
		meta = (DisplayName = "输入生产者类")
	)
	TSubclassOf<UVehicleInputProducerBase> InputProducerClass{};

	/** 状态输入配置（区别于控制输入，供模块读取非瞬态状态） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件",
		meta = (DisplayName = "状态输入配置")
	)
	TArray<FModuleInputSetup> StateInputConfiguration{};

	/** 组件级控制输入配置（与 SU 组件的输入配置合并去重） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件",
		meta = (DisplayName = "控制输入配置")
	)
	TArray<FModuleInputSetup> InputConfig{};

	/**
	 * 物理适配器实例。
	 *
	 * 用户在编辑器中创建并配置适配器子对象，
	 * 由适配器负责从特定物理来源获取代理并将组件增删事件翻译为标准 API 调用。
	 * 若留空则为手动模式，外部代码直接调用 AddSimulationModule 等 API。
	 */
	UPROPERTY(
		Instanced,
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件",
		meta = (DisplayName = "物理适配器")
	)
	TObjectPtr<USingularisMorphVehiclePhysicsAdapter> PhysicsAdapter = nullptr;

	/** 保持载具唤醒（禁止休眠） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件",
		meta = (DisplayName = "禁止休眠")
	)
	bool bKeepVehicleAwake = true;

	/** 线性阻尼（空气阻力） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|阻尼",
		meta = (DisplayName = "线性阻尼")
	)
	float LinearDamping = 0.01f;

	/** 角阻尼（旋转阻力） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|阻尼",
		meta = (DisplayName = "角阻尼")
	)
	float AngularDamping = 0.5f;

	/** 悬挂射线检测通道 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|悬挂",
		meta = (DisplayName = "悬挂射线通道")
	)
	TEnumAsByte<ECollisionChannel> SuspensionCollisionChannel = ECC_WorldDynamic;

	/** 悬挂射线碰撞响应 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|悬挂",
		meta = (DisplayName = "悬挂射线碰撞响应")
	)
	FCollisionResponseContainer SuspensionTraceCollisionResponses{};

	/** 悬挂射线是否使用复杂碰撞 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|悬挂",
		meta = (DisplayName = "悬挂射线复杂碰撞")
	)
	bool bSuspensionTraceComplex = true;

	/** 悬挂射线类型 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|悬挂",
		meta = (DisplayName = "悬挂射线类型")
	)
	ESingularisMorphTraceType TraceType = ESingularisMorphTraceType::Raycast;

	/**
	 * 默认路面摩擦（抓地力）。
	 *
	 * 仅当悬挂射线命中面未提供物理材质时生效；命中面有物理材质时始终以材质摩擦为准。
	 * 默认 0.7 与 UPhysicalMaterial 的默认摩擦系数一致。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|悬挂",
		meta = (DisplayName = "默认路面摩擦", ClampMin = "0.0", UIMin = "0.0")
	)
	float DefaultSurfaceFriction = 0.7f;

	/**
	 * 模拟期间施加到车轮驱动组件的碰撞启用模式。
	 *
	 * 默认 QueryOnly：保留集群联合所需的查询物理表示（NoCollision 会销毁物理状态
	 * 并使车轮脱离集群），但不参与物理接触，也不会成为解析悬挂之外的第二支撑。
	 * 查询交互由 SimulatedWheelCollisionResponses 默认全部忽略而阻断；
	 * QueryAndPhysics/PhysicsOnly 会重新引入物理接触，不应使用。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|车轮碰撞",
		meta = (DisplayName = "车轮模拟期碰撞启用")
	)
	TEnumAsByte<ECollisionEnabled::Type> SimulatedWheelCollisionEnabled = ECollisionEnabled::QueryOnly;

	/**
	 * 模拟期间施加到车轮驱动组件的逐通道碰撞响应。
	 *
	 * 默认忽略全部通道：QueryOnly 仍保留查询表示，必须由本配置阻断悬挂射线、
	 * 交互射线与重叠检测命中轮胎自身；默认忽略可视为对 NoCollision 的行为等价。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|车轮碰撞",
		meta = (DisplayName = "车轮模拟期碰撞响应")
	)
	FCollisionResponseContainer SimulatedWheelCollisionResponses{};

	/** 打滑起始侧滑角阈值（度）：车轮接地且侧滑角达到该值时广播打滑开始事件 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|事件判定",
		meta = (DisplayName = "打滑起始侧滑角(度)", ClampMin = "0.0", UIMin = "0.0")
	)
	float SlipStartAngleThreshold = 6.0f;

	/** 打滑解除侧滑角阈值（度）：侧滑角回落到该值以下或车轮离地时广播打滑停止事件；配置高于起始阈值时按起始阈值执行 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|事件判定",
		meta = (DisplayName = "打滑解除侧滑角(度)", ClampMin = "0.0", UIMin = "0.0")
	)
	float SlipStopAngleThreshold = 3.0f;

	/** 触底判定容差（cm）：悬挂压缩量达到 最大上升减容差 时视为触底 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具仿真组件|事件判定",
		meta = (DisplayName = "触底判定容差(cm)", ClampMin = "0.0", UIMin = "0.0")
	)
	float SuspensionBottomOutTolerance = 0.5f;

#pragma endregion

#pragma region Event Dispatcher

	/** 模拟模块添加事件（原生，低开销） */
	FOnSimulationModuleAddedNative OnSimulationModuleAddedNativeEvent{};

	/** 模拟模块移除事件（原生，低开销） */
	FOnSimulationModuleRemovedNative OnSimulationModuleRemovedNativeEvent{};

	/** 模拟模块添加事件（Blueprint） */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变型载具仿真组件|事件分发器",
		meta = (DisplayName = "模拟模块添加")
	)
	FOnSimulationModuleAdded OnSimulationModuleAddedEvent{};

	/** 模拟模块移除事件（Blueprint） */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变型载具仿真组件|事件分发器",
		meta = (DisplayName = "模拟模块移除")
	)
	FOnSimulationModuleRemoved OnSimulationModuleRemovedEvent{};

	/** 换挡事件：非空挡位咬合时广播（空挡为换挡过渡态不广播，首次挡位输出仅建立基准） */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变型载具仿真组件|事件分发器",
		meta = (DisplayName = "换挡")
	)
	FOnGearShiftedSignature OnGearShiftedEvent{};

	/** 车轮开始打滑事件 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变型载具仿真组件|事件分发器",
		meta = (DisplayName = "车轮开始打滑")
	)
	FOnWheelSlipStartedSignature OnWheelSlipStartedEvent{};

	/** 车轮停止打滑事件（车轮模块被移除时同样广播，供观察者解除驻留状态） */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变型载具仿真组件|事件分发器",
		meta = (DisplayName = "车轮停止打滑")
	)
	FOnWheelSlipStoppedSignature OnWheelSlipStoppedEvent{};

	/** 悬挂触底事件（一次性冲击事件，仅在触底上升沿广播） */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变型载具仿真组件|事件分发器",
		meta = (DisplayName = "悬挂触底")
	)
	FOnSuspensionBottomedOutSignature OnSuspensionBottomedOutEvent{};

#pragma endregion

private:
#pragma region State

	/** 模拟模块树（游戏线程创建，物理线程接管所有权） */
	TUniquePtr<Chaos::FSimModuleTree> SimulationModuleTree = nullptr;

	/**
	 * 动画槽位表访问锁。
	 *
	 * 槽位表由游戏线程写入（重建、输出分发），而动画线程在 PreUpdate 中读取同一份数据；
	 * 并行动画求值开启时两者可同时执行，TArray 重分配期间读取会命中已释放内存。
	 */
	mutable FCriticalSection ModuleAnimationSetupsLock{};

	/** 物理线程输出的插值数据 */
	TUniquePtr<FSingularisMorphVehiclePhysicsOutput> VehiclePhysicsOutput = nullptr;

	/** 组件到模拟对象的映射（Key = SU 组件，避免多模块共享同一代理时的覆盖） */
	TMap<TObjectPtr<UActorComponent>, FSingularisMorphVehicleComponentData> ComponentToPhysicsObjects{};

	/** 模拟 GUID 到 SU 组件的反向映射（供 ParallelUpdate 回调 OnOutputReady） */
	TMap<int32, TWeakObjectPtr<UActorComponent>> PhysicsGuidToComponent{};

	/** 根物理对象句柄（装配根粒子，模块构造期初始化的锚点） */
	Chaos::FPhysicsObjectHandle RootPhysicsObject = nullptr;

	/** 模块动画配置列表 */
	TArray<FSingularisMorphModuleAnimationSetup> ModuleAnimationSetups{};

	/**
	 * 模块静止基准缓存（仅游戏线程）。
	 *
	 * 键为「驱动组件 + 物理粒子唯一索引」，值为首次注册时捕获的静止位姿。
	 * 拓扑重建复用既有条目而不重读实时 ChildToParent：后者携带引擎写回的动画位移，
	 * 重读会把当帧姿态固化为新基准，逐次重建累积为可视轮位与悬挂行程漂移。
	 *
	 * 缓存不随物理状态重建清除：粒子未变时既有基准仍然有效，清除反而会把
	 * 动画写回值当作新基准；粒子已重建时键不匹配，自动重新捕获。
	 * 条目在每次重建后按当前快照裁剪，不随部件反复增删无界累积。
	 */
	TMap<FSingularisMorphModuleRestPoseKey, FSingularisMorphModuleRestPose> ModuleRestPoses{};

	/**
	 * 已抑制的车轮驱动组件 → 原始状态快照（仅游戏线程）。
	 *
	 * 键为弱引用，组件销毁后条目在下一次对账时裁剪。用于轮胎分离、载具解体或
	 * 物理状态拆除时精确恢复碰撞与独立物理模拟，避免被回收复用的组件残留「被抑制」状态。
	 */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, FSingularisMorphWheelCollisionSnapshot> SuppressedWheelCollisions{};

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

	/** 已禁用的控制输入通道（仅游戏线程；按名称保存，输入配置重建后仍有效） */
	TSet<FName> DisabledControlNames{};

	/** 当前合并的输入配置（用于变更检测） */
	TArray<FModuleInputSetup> CombinedInputConfiguration{};

	/** 本地控制回退标记（无 NetworkPhysicsComponent 时生效） */
	bool bIsLocallyControlled = false;

	/** 当前挡位（由变速箱输出缓存；0 表示空挡、换挡过渡或尚未收到输出） */
	int32 CurrentGear = 0;

	/** 是否已收到过变速箱输出：挡位未知时不产出换挡脉冲（否则会把变速箱的初始挡位误判为需要升降挡） */
	bool bHasGearData = false;

	/**
	 * 目标挡位请求（SetGearInput 写入，仅手动变速箱）。
	 *
	 * INDEX_NONE 表示无请求。变速箱一次只接受一挡（目标挡位 ±1），
	 * Update 按当前物理输出逐挡推进；已发出但尚未在输出中确认的脉冲
	 * 记录在 PendingGearPulseTarget 中，避免异步输出滞后时重复发送。
	 */
	int32 TargetGearInput = INDEX_NONE;

	/** 已发出但尚未在物理输出中确认的脉冲目标挡位；INDEX_NONE 表示无待确认脉冲 */
	int32 PendingGearPulseTarget = INDEX_NONE;

	/** 待确认脉冲的超时计时（秒）；超时后重新评估，避免指令被其它换挡逻辑覆盖后永久卡住 */
	float PendingGearPulseTimeout = 0.0f;

	/** 上一次脉冲所依据的异步输出时间戳与输出对象；同一输出帧不重复发脉冲 */
	int32 LastGearPulseOutputTimestamp = INDEX_NONE;
	const FSingularisMorphVehicleAsyncOutput* LastGearPulseOutput = nullptr;

	/** 自动变速拒绝手动换挡请求的告警是否已发出（避免逐帧调用刷屏） */
	bool bWarnedAutomaticGearInput = false;

	/** 强制空挡请求尚未被变速箱输出确认；Update 期间逐帧补发直到确认 */
	bool bNeutralGearRequested = false;

	/** 引擎转速（由引擎输出缓存） */
	float EngineRPM = 0.0f;

	/** 引擎净扭矩（由引擎输出缓存，N·m；负值为发动机制动） */
	float EngineTorque = 0.0f;

	/** GUID 到核心模块的映射（仅游戏线程访问，避免读取物理线程拥有的模拟树） */
	TMap<int32, Chaos::ISimulationModuleBase*> GuidToCoreModule{};

	/** 待重建网络复制结构标记（模块增删后置位，PostUpdate 消费） */
	bool bPendingReplicationStructureRebuild = false;

	/** 构建索引计数器 */
	int32 NextConstructionIndex = 0;

	/** 悬挂射线忽略的 Actor 列表 */
	TArray<AActor*> ActorsToIgnore{};

	/** 上一次已咬合的非空挡位（换挡事件的旧挡位来源） */
	int32 LastEngagedGear = 0;

	/** 换挡事件基准已建立：首次收到挡位输出时只记录不广播，避免初始挡位误判为换挡 */
	bool bHasGearShiftBaseline = false;

	/** 打滑中的车轮模块 GUID 集合（打滑事件边沿检测状态） */
	TSet<int32> SlippingWheelGuids{};

	/** 触底中的悬挂模块 GUID 集合（触底事件边沿检测状态） */
	TSet<int32> BottomedOutSuspensionGuids{};

public:
	/**
	 * 物理线程端载具模拟。
	 *
	 * 降级为公共可变成员的原因：异步回调在物理线程上下文直接访问该指针
	 * （Simulate / OnContactModification / ApplyDeferredForces / 网络状态注入），
	 * 以访问器封装会误导其跨线程所有权语义；命名保留 PT 后缀作线程警示。
	 */
	TUniquePtr<FSingularisMorphVehicleSimulation> VehicleSimulationPT;

	/** 网络物理预测开关。降级为公共成员的原因同 VehicleSimulationPT：物理线程回调直接读取 */
	bool bUsingNetworkPhysicsPrediction = false;

private:
	/** 当前异步数据类型 */
	ESingularisMorphChaosAsyncVehicleDataType CurrentAsyncDataType = AsyncInvalid;

	/** 当前帧异步输入（游戏线程→物理线程） */
	const FSingularisMorphVehicleAsyncInput* CurrentAsyncInput = nullptr;

	/** 当前帧异步输出（物理线程→游戏线程） */
	const FSingularisMorphVehicleAsyncOutput* CurrentAsyncOutput = nullptr;

	/** 下一帧异步输出（用于插值） */
	const FSingularisMorphVehicleAsyncOutput* NextAsyncOutput = nullptr;

	/** 最近一次异步输出的物理帧时间戳；换挡脉冲以新输出帧为推进节拍 */
	int32 LatestAsyncOutputTimestamp = INDEX_NONE;

	/** 异步输出插值系数 [0..1] */
	float OutputInterpolationAlpha = 0.0f;

	/** 待提交到物理线程的模块树增删队列（仅游戏线程） */
	Chaos::FSimTreeUpdates StoredTreeUpdates{};

	/** 与 StoredTreeUpdates 同批提交的父节点重挂队列（仅游戏线程） */
	TArray<FSingularisMorphModuleReparent> StoredTreeReparents{};

	/** 与 StoredTreeUpdates 同批提交的车轮转速上限更新（仅游戏线程） */
	TArray<FSingularisMorphWheelSpeedLimit> StoredWheelSpeedLimits{};

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
	 * 提交本帧累积的全部模块增删、父节点重挂与车轮限速更新到物理线程。
	 *
	 * 三者作为一次原子更新提交；无变更时为空操作。
	 * 仅增删或重挂会标记网络复制结构待重建。
	 */
	void FinalizeModuleUpdates();

	/**
	 * 基于快照增量同步物理模拟树（剔除缺失-添加新建-替换变动）。
	 *
	 * 统一收集快照实体与 Owner 上的全部 SU 组件后按类型自动关联：
	 * 底盘/引擎/离合/变速箱/轮轴各取首个实例（多余实例忽略并告警），
	 * 无引擎时原动机槽由首个电机顶替；车轮与车架上的立轴槽位按邻近
	 * 匹配一对一配对（匹配半径内取最近者），挂载槽位的车轮挂到动力链
	 * 最末端（轮轴→变速箱→离合→原动机→底盘，缺项时取最近的现存上游）
	 * 并注入槽位的转向/轴向/反转配置，未匹配的自由轮挂底盘之下。
	 * 与现存模块逐一比对后仅处理差异：计划外模块移除、缺失模块新建、
	 * 驱动组件/槽位配置/粒子索引/动画开关变化时替换（先移除再新建并移植动态状态）；
	 * 父节点变化的保留模块仅按 GUID 重挂，不重建，积分状态与动画槽位原样延续。
	 * 处理完毕后调用 FinalizeModuleUpdates 批量提交到物理线程。
	 * 空快照且当前无模块时无操作；空快照且存在模块（载具解体）时清除全部模块。
	 */
	void RebuildFromSnapshot(const FSingularisMorphVehiclePhysicsAdapterSnapshot& Snapshot);

	/** 更新物理阻尼属性 */
	void UpdatePhysicalProperties() const;

	/** 设置输入生产者类并重建输入配置 */
	void SetInputProducerClass(
		TSubclassOf<UVehicleInputProducerBase> InInputProducerClass,
		bool bForceNewInstance = false
	);

	/** 追加一条控制输入配置并重建输入容器 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "添加控制输入配置")
	)
	void AddInput(const FModuleInputSetup& InputSetup);

	/** 添加悬挂射线忽略的 Actor */
	void AddActorsToIgnore(TArray<AActor*>& ActorsIn);

	/** 移除悬挂射线忽略的 Actor（Owner 自忽略始终保留） */
	void RemoveActorsToIgnore(TArray<AActor*>& ActorsIn);

	/** 写入布尔控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "设置布尔输入")
	)
	void SetInputBool(
		const FName Name,
		const bool Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	) const;

	/** 写入整数控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "设置整数输入")
	)
	void SetInputInteger(
		const FName Name,
		const int32 Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	) const;

	/** 写入一维轴控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "设置一维轴输入")
	)
	void SetInputAxis1D(
		const FName Name,
		const double Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	) const;

	/** 写入二维轴控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "设置二维轴输入")
	)
	void SetInputAxis2D(
		const FName Name,
		const FVector2D Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	) const;

	/** 写入三维轴控制输入 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "设置三维轴输入")
	)
	void SetInputAxis3D(
		const FName Name,
		const FVector Value,
		EModuleInputBufferActionType BufferAction = EModuleInputBufferActionType::Override
	) const;

	/**
	 * 设置目标挡位（仅手动变速箱），越界请求按变速箱实际挡数钳制。
	 *
	 * 自动变速箱不消费升降挡脉冲：除 0（空挡）外调用会被忽略并告警，
	 * 其余挡位由自动换挡状态机在全挡位范围内决定。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "设置挡位输入")
	)
	void SetGearInput(int32 Gear);

	/**
	 * 立即请求变速箱回到空挡。
	 *
	 * 通过系统控制通道发送一次性脉冲，物理线程在下一步把当前/目标挡位置为 0
	 * 并清除下游残留扭矩。由外部在需要时显式调用（例如停车）；
	 * 请求会持续发送直到变速箱输出确认空挡，避免异步物理输出滞后导致请求丢失。
	 * 同时取消尚未完成的挡位请求。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "重置变速箱为空挡")
	)
	void ResetTransmissionToNeutral();

	/**
	 * 禁用/启用单个控制输入通道。
	 *
	 * 通道名与 SetInput* 使用的输入名一致（如 Throttle、Steering、Brake），
	 * 通过 AddInput 扩展的通道同样适用。
	 * 禁用期间该通道在 ProduceInput 产出后统一按中性值处理，
	 * 不受玩家、AI 或自定义输入生产者的写入影响；重新启用后下一物理步恢复。
	 * 闸门在输入生产端生效：本地控制方禁用后的输入会随网络物理复制到其它端；
	 * 服务器要单方面覆盖远端客户端已产出的输入，需在输入同步层另行处理。
	 * 配置建立后传入未配置的通道名会被忽略并告警。
	 * 系统空挡复位通道（TransmissionNeutral）不受闸门影响。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "禁用/启用控制通道")
	)
	void SetControlEnabled(const FName ControlName, const bool bEnabled);

	/** 批量禁用/启用控制输入通道（逐项等价于 SetControlEnabled） */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "批量禁用/启用控制通道")
	)
	void SetControlsEnabled(const TArray<FName>& ControlNames, const bool bEnabled);

	/** 禁用/启用调用时已配置的全部控制输入通道 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "禁用/启用全部控制通道")
	)
	void SetAllControlsEnabled(const bool bEnabled);

	/** 查询控制输入通道是否启用（未禁用的通道均返回 true） */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "查询控制通道启用")
	)
	bool IsControlEnabled(const FName ControlName) const;

	/** 获取当前被禁用的控制输入通道 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|API",
		meta = (DisplayName = "获取禁用的控制通道")
	)
	TArray<FName> GetDisabledControls() const;

	/** 写入布尔状态输入 */
	void SetState(const FName& Name, const bool Value);

	/** 写入整数状态输入 */
	void SetState(const FName& Name, const int32 Value);

	/** 写入浮点状态输入 */
	void SetState(const FName& Name, const double Value);

	/** 写入二维向量状态输入 */
	void SetState(const FName& Name, const FVector2D& Value);

	/** 写入三维向量状态输入 */
	void SetState(const FName& Name, const FVector& Value);

	/** 声明本地控制状态（网络预测；SimulatedProxy 的输入来自服务器，无论传入何值均不标记本地） */
	void SetLocallyControlled(bool bInLocallyControlled);

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

	/** 后更新（广播离散事件、重建网络复制结构） */
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

	/** 获取模块动画配置（只读）；跨线程读取前必须持有 GetModuleAnimationSetupsLock() */
	const TArray<FSingularisMorphModuleAnimationSetup>& GetModuleAnimationSetups() const
	{
		return ModuleAnimationSetups;
	}

	/** 获取动画槽位表的访问锁（动画线程与游戏线程共享该表） */
	FCriticalSection& GetModuleAnimationSetupsLock() const { return ModuleAnimationSetupsLock; }

	/** 按模块 GUID 获取最近的输出数据 */
	const Chaos::FSimOutputData* GetOutputData(int32 ModuleGuid) const;

	/** 获取当前物理适配器（可能为 nullptr = 手动模式） */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|State",
		meta = (DisplayName = "获取物理适配器")
	)
	USingularisMorphVehiclePhysicsAdapter* GetPhysicsAdapter() const { return PhysicsAdapter; }

	/** 是否本地控制 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|State",
		meta = (DisplayName = "是否本地控制")
	)
	bool IsLocallyControlled() const;

	/** 获取当前挡位 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|State",
		meta = (DisplayName = "获取当前挡位")
	)
	int32 GetCurrentGear() const { return CurrentGear; }

	/** 是否正在倒车 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|State",
		meta = (DisplayName = "是否倒车")
	)
	bool IsReversing() const { return CurrentGear < 0; }

	/** 获取引擎转速（RPM） */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|State",
		meta = (DisplayName = "获取引擎转速")
	)
	float GetEngineRPM() const { return EngineRPM; }

	/** 获取引擎净扭矩（N·m；负值为发动机制动） */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|State",
		meta = (DisplayName = "获取引擎扭矩")
	)
	float GetEngineTorque() const { return EngineTorque; }

	/**
	 * 获取引擎最大转速（RPM）。
	 *
	 * 从参与仿真的引擎/电机 SU 组件聚合：引擎占据原动机槽时返回引擎最大值，
	 * 无引擎时返回电机最大值；手动注册路径（无 SU 映射）与无原动机时返回 0，
	 * 调用方需按 0 自行回退。每次调用实时解析，模块增删后取值自动更新。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|State",
		meta = (DisplayName = "获取引擎最大转速")
	)
	float GetEngineMaxRPM() const;

	/** 获取车速（km/h） */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具仿真组件|State",
		meta = (DisplayName = "获取车速(km/h)")
	)
	float GetVehicleSpeed() const;

#pragma endregion

private:
#pragma region Callback

	/** 模块初始化回调：分发生命周期到 SU 组件（广播由 BroadcastModuleAddedEvent 统一执行） */
	void OnSimulationModuleInitialized(const FName& ModuleType, int32 Guid, int32 TreeIndex);

	/** 模块移除回调：分发生命周期到 SU 组件（广播由 BroadcastModuleRemovedEvent 统一执行） */
	void OnSimulationModuleRemovedCallback(const FName& ModuleType, int32 Guid, int32 TreeIndex);

#pragma endregion

#pragma region Internal Function

	/** 创建物理线程端载具模拟并注册到仿真管理器（仅游戏世界生效） */
	void CreateVehicleSimulation();

	/** 销毁物理线程端模拟：注销管理器、终止模块、释放资源（销毁推迟到求解器命令队列） */
	void DestroyVehicleSimulation();

	/** 清除全部模拟模块（载具解体/销毁时释放资源并提交删除） */
	void ClearAllSimulationModules();

	/** 解析物理代理：适配器优先，其次缓存值，最后回退 Owner 根组件物理体 */
	IPhysicsProxyBase* GetPhysicsProxy() const;

	/** 生成全局唯一且不复用的模块 GUID */
	int32 GenerateNewGuid();

	/** 聚合所有 SU 组件的输入配置（去重，换挡脉冲置为消费即清零） */
	void AssimilateComponentInputs(TArray<FModuleInputSetup>& OutCombinedInputs) const;

	/** 初始化输入生产者与输入容器（配置变更时幂等重建并迁移已缓冲输入值） */
	void SetupInputConfiguration(bool bForceReinitialize = false);

	/** 将被禁控制通道的产出值压为对应类型的中性值（ProduceInput 收口处调用） */
	void ApplyControlGates();

	/** 发送强制空挡请求，并在变速箱输出确认空挡后结束请求 */
	void UpdateNeutralGearRequest();

	/** 查找当前变速箱模块（仅游戏线程映射表） */
	const Chaos::FTransmissionSimModule* FindTransmissionModule() const;

	/** 将动画数据应用到非骨骼可视化组件 */
	void UpdateNonSkeletalAnimations();

	/** 在输出容器中按 GUID 查找模块输出数据（支持异步输出与游戏线程插值副本） */
	Chaos::FSimOutputData* FindModuleOutputFromGuid(
		const FSingularisMorphVehiclePhysicsOutput& OutputContainer,
		int32 Guid
	) const;

	/** 广播模块添加事件（原生与 Blueprint 双轨） */
	void BroadcastModuleAddedEvent(const FName& ModuleType, int32 Guid, int32 TreeIndex) const;

	/** 广播模块移除事件（原生与 Blueprint 双轨） */
	void BroadcastModuleRemovedEvent(const FName& ModuleType, int32 Guid, int32 TreeIndex) const;

	/** 广播换挡/打滑/触底离散输出事件（游戏线程边沿检测，天然幂等） */
	void BroadcastDiscreteOutputEvents();

	/** 车轮打滑状态边沿检测：双阈值滞回 + 接地门控，状态翻转时广播开始/停止事件 */
	void ProcessWheelSlipState(const Chaos::FWheelOutputData& OutputData);

	/** 悬挂触底状态边沿检测：压缩量达最大行程减容差时广播一次性触底事件 */
	void ProcessSuspensionBottomOutState(const FSingularisMorphSuspensionOutputData& OutputData);

	/** 缓存根物理对象（按代理类型解析集群联合或单粒子句柄） */
	void CacheRootPhysicsObject(IPhysicsProxyBase* Proxy);

	/**
	 * 内部模块注册（Internal API）。
	 *
	 * 将单个 CoreModule 注册到模拟树，设置正确的粒子索引和变换。
	 * BoneName/AnimationOffset 来自模块对应的 SU 组件，用于绑定动画槽位；
	 * 留空时沿用模块自身的骨骼信息（手动注册路径）。
	 * PhysicalTransform 为模块的静止基准（手动注册路径由调用方保证其静止性），
	 * 快照路径由 RebuildFromSnapshot 从静止基准缓存取得。
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

	/**
	 * 取得模块静止基准。
	 *
	 * 首次注册时以传入的实时值捕获入库，此后同一（组件, 粒子）重建时
	 * 一律返回已捕获的基准，避免把引擎写回的动画位移固化为新的静止基准。
	 *
	 * @param Component 驱动组件（缓存键的一半）
	 * @param ParticleIndex 物理粒子唯一索引（缓存键的另一半）
	 * @param RestTransform 实时静止基准（仅首次注册时使用）
	 * @param ComponentTransform 实时驱动组件变换（仅首次注册时使用）
	 * @return 静止基准引用（缓存内部存储，随键失效而失效）
	 */
	const FSingularisMorphModuleRestPose& AcquireModuleRestPose(
		UPrimitiveComponent* Component,
		int32 ParticleIndex,
		const FTransform& RestTransform,
		const FTransform& ComponentTransform
	);

	/** 按当前快照裁剪静止基准缓存，丢弃已离开物理装配的部件（粒子不再存在）的条目 */
	void PruneModuleRestPoses(const TSet<FSingularisMorphModuleRestPoseKey>& LiveKeys);

	/**
	 * 抑制单个车轮驱动组件的独立物理与碰撞。
	 *
	 * 记录原始碰撞启用模式、逐通道响应与物理模拟状态后，先停止独立模拟，
	 * 再按 SimulatedWheelCollision* 施加模拟期配置。目标已记录时幂等跳过，
	 * 不覆盖原始快照。
	 */
	void SuppressWheelCollision(UPrimitiveComponent* WheelComponent);

	/** 恢复单个车轮组件的原始碰撞与物理模拟状态并删除记录；组件已销毁时仅删除记录 */
	void RestoreWheelCollision(const TWeakObjectPtr<UPrimitiveComponent>& WheelComponent);

	/**
	 * 按当前活动车轮集合对账抑制状态。
	 *
	 * 对活动车轮中尚未抑制者施加抑制、对已记录但不再活动者恢复、对已销毁者裁剪记录。
	 * 以「期望集合 vs 已记录集合」对账，天然覆盖共享组件（轮胎与悬挂共用网格）、
	 * 替换（先移除再新增）与组件销毁。
	 */
	void ReconcileWheelCollisionSuppression(const TSet<UPrimitiveComponent*>& ActiveWheelComponents);

	/**
	 * 将已记录车轮收敛回模拟期状态。
	 *
	 * 外部系统（装备/物品流程、物理状态重建）可能在两次拓扑重建之间恢复车轮的
	 * 独立模拟或碰撞配置；每物理帧复核并写回抑制状态，避免无拓扑差异帧残留第二支撑。
	 */
	void ReapplyWheelCollisionSuppression();

	/** 恢复全部被抑制的车轮组件（载具解体、物理状态拆除时调用） */
	void RestoreAllWheelCollisionSuppression();

#pragma endregion
};
