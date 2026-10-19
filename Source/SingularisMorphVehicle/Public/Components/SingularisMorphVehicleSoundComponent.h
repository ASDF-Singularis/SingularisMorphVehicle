#pragma once

#include <CoreMinimal.h>
#include <Components/ActorComponent.h>
#include <Engine/EngineTypes.h>
#include <Engine/HitResult.h>
#include <Math/NumericLimits.h>

#include "SingularisMorphVehicleSoundComponent.generated.h"

class UMetaSoundSource;
class UAudioComponent;
class UPrimitiveComponent;
class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变形载具声音组件。
 *
 * 世界空间音效观察者组件设计模式典范：
 * 挂载于载具，以显式组件引用装配仿真组件与音频组件，Tick 拉取物理输出连续状态、
 * 订阅离散事件，归一化后经参数与触发直写 MetaSound；世界作用域观察者，
 * 不做本地玩家检查（听觉相关性由音频引擎按监听者处理）。
 *
 * 推拉混合模型分工：
 * 拉路线——Tick 轮询仿真组件的连续状态（引擎转速/扭矩/车速/挡位），归一化后写入音频参数；
 * 引擎转速按「0 → 最大转速」归一化（0 RPM 为 0、最大转速为 1）；
 * 推路线——订阅仿真组件的离散事件（换挡/车轮打滑开始与停止/悬挂触底）
 * 与载具根组件的碰撞事件，经触发参数一次性推送。
 * 仿真组件与音频组件均为显式引用装配（组件选择器指定），引用无效时组件保持静默，
 * 不做隐式发现与运行时兜底创建；组件只负责传递数值，全部音频逻辑（采样交叉淡化、
 * 门限触发、包络整形）由 MetaSound 图承担，观察对象始终为物理输出（载具的真相），
 * 不接触玩家输入，天然兼容 AI 载具与网络同步的远端载具。
 *
 * 与本地作用域观察者（USingularisInteractionWidgetComponent 等）的关键差异——不做本地玩家检查：
 * 交互控件是视口作用域的观察者，屏幕控件与操作提示仅对本地玩家有意义，须以
 * IsLocalController 过滤视口归属；本组件是世界作用域的观察者，AI 载具与
 * 网络同步的远端载具同样须发声，任何本地门控都会使非本地载具静默。
 * 听觉相关性（距离衰减、监听者裁剪、远距虚拟化）由音频引擎按监听者处理，
 * 专用服务器无音频设备而天然静默，组件层不应重复过滤。
 * 世界作用域观察者以本组件为典范，特此记录差异，防止后续实现误套本地玩家检查。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变形载具声音组件")
)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleSoundComponent : public UActorComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 关联的仿真组件引用（观察对象：拉取连续状态、订阅离散事件） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件",
		meta = (
			DisplayName = "仿真组件引用",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisMorphVehicleSimulationComponent"
		)
	)
	FComponentReference SimulationComponentReference{};

	/** 承载发声的音频组件引用（须启用 Space 混音的空间化需求由该组件配置承担） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件",
		meta = (DisplayName = "音频组件引用", UseComponentPicker, AllowedClasses = "/Script/Engine.AudioComponent")
	)
	FComponentReference AudioComponentReference{};

	/** MetaSound 音源资产（持续声音的宿主：引擎/路噪等由其参数驱动；留空时组件静默） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件",
		meta = (DisplayName = "MetaSound 音源")
	)
	TObjectPtr<UMetaSoundSource> MetaSoundSource = nullptr;

	/** BeginPlay 时自动开始播放 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件",
		meta = (DisplayName = "自动播放")
	)
	bool bAutoPlay = true;

	/** RPM 归一化上限回退值：仿真组件取不到引擎最大转速（手动注册/无原动机）时使用 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件",
		meta = (DisplayName = "RPM 归一回退上限", ClampMin = "1.0", UIMin = "1.0")
	)
	float FallbackMaxEngineRPM = 6000.0f;

	/** 车速归一化上限（km/h） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件",
		meta = (DisplayName = "车速归一化上限", ClampMin = "1.0", UIMin = "1.0")
	)
	float MaxSpeedKmh = 120.0f;

	/** 碰撞触发阈值：法向冲量模长低于该值的接触（刮蹭/沉降）不广播碰撞事件 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件",
		meta = (DisplayName = "碰撞触发阈值", ClampMin = "0.0", UIMin = "0.0")
	)
	float CollisionImpulseThreshold = 1000.0f;

	/** RPM 参数名：归一化引擎转速 [0..1]（0 = 0 RPM，1 = 最大转速） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "RPM 参数名")
	)
	FName RPMParameterName = TEXT("RPM");

	/** 引擎净扭矩参数名：N·m 原始值直传（负值为发动机制动方向，归一化由 MetaSound 图决定） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "引擎扭矩参数名")
	)
	FName EngineTorqueParameterName = TEXT("EngineTorque");

	/** 车速参数名：归一化车速 [0..1] */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "车速参数名")
	)
	FName SpeedParameterName = TEXT("Speed");

	/** 挡位参数名：整数直传（0 为空挡，负值为倒挡） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "挡位参数名")
	)
	FName GearParameterName = TEXT("Gear");

	/** 换挡触发参数名 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "换挡触发参数名")
	)
	FName GearShiftedTriggerName = TEXT("GearShifted");

	/** 车轮打滑开始触发参数名 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "打滑开始触发参数名")
	)
	FName WheelSlipStartedTriggerName = TEXT("WheelSlipStarted");

	/** 车轮打滑停止触发参数名 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "打滑停止触发参数名")
	)
	FName WheelSlipStoppedTriggerName = TEXT("WheelSlipStopped");

	/** 悬挂触底触发参数名 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "悬挂触底触发参数名")
	)
	FName SuspensionBottomedOutTriggerName = TEXT("SuspensionBottomedOut");

	/** 碰撞触发参数名 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "碰撞触发参数名")
	)
	FName CollisionImpactTriggerName = TEXT("CollisionImpact");

	/** 碰撞强度参数名：法向冲量模长原始值直传 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变形载具声音组件|参数名",
		meta = (DisplayName = "碰撞强度参数名")
	)
	FName ImpactStrengthParameterName = TEXT("ImpactStrength");

#pragma endregion

private:
#pragma region State

	/** 运行时解析的仿真组件缓存（来自仿真组件引用） */
	TWeakObjectPtr<USingularisMorphVehicleSimulationComponent> SimulationComponent = nullptr;

	/** 运行时解析的音频组件缓存（来自音频组件引用，外部装配不持有所有权） */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> AudioComponent = nullptr;

	/** 最近一次写入的 RPM 参数缓存（变更检测，避免冗余音频线程流量） */
	float LastPushedRPM = -1.0f;

	/** 最近一次写入的车速参数缓存 */
	float LastPushedSpeed = -1.0f;

	/** 最近一次写入的扭矩参数缓存（可为负值，故使用浮点下限作哨兵） */
	float LastPushedTorque = TNumericLimits<float>::Lowest();

	/** 最近一次写入的挡位参数缓存 */
	int32 LastPushedGear = INDEX_NONE;

#pragma endregion

public:
#pragma region Constructors

	USingularisMorphVehicleSoundComponent();

#pragma endregion

#pragma region ActorComponent Interface

	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction
	) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#pragma endregion

#pragma region API

	/** 开始播放 MetaSound（幂等：重复调用与播放中调用均不重启声音） */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变形载具声音组件|API",
		meta = (DisplayName = "播放")
	)
	void Play();

	/** 停止播放（幂等） */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变形载具声音组件|API",
		meta = (DisplayName = "停止")
	)
	void Stop() const;

#pragma endregion

private:
#pragma region Callback

	/** 换挡事件响应：推送一次性换挡触发参数 */
	UFUNCTION()
	void HandleGearShifted(int32 OldGear, int32 NewGear) const;

	/** 车轮打滑开始事件响应：推送打滑开始触发参数 */
	UFUNCTION()
	void HandleWheelSlipStarted(int32 WheelModuleGuid) const;

	/** 车轮打滑停止事件响应：推送打滑停止触发参数 */
	UFUNCTION()
	void HandleWheelSlipStopped(int32 WheelModuleGuid) const;

	/** 悬挂触底事件响应：推送一次性触底触发参数 */
	UFUNCTION()
	void HandleSuspensionBottomedOut(int32 SuspensionModuleGuid) const;

	/** 载具碰撞事件响应：推送冲量强度参数与一次性碰撞触发参数 */
	UFUNCTION()
	void HandleCollisionHit(
		UPrimitiveComponent* HitComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		FVector NormalImpulse,
		const FHitResult& Hit
	) const;

#pragma endregion

#pragma region Internal Function

	/** 装配音频组件：解析音频组件引用并指派 MetaSound 音源 */
	void ProxyAudioComponent();

	/** 订阅仿真组件：解析仿真组件引用并绑定全部离散事件 */
	void ObserveSimulationComponent();

	/** 装配载具根组件的碰撞回调 */
	void ProxyCollisionEvents();

	/** 解绑仿真组件的全部事件订阅 */
	void UnbindSimulationEvents() const;

	/** 拉路线：轮询仿真组件连续状态，变更检测后写入音频参数 */
	void PushContinuousParameters();

	/** 复位参数写入缓存（播放重启后音频侧参数复位，须强制重推） */
	void ResetPushCaches();

#pragma endregion
};
