#pragma once

#include <CoreMinimal.h>
#include <Components/ActorComponent.h>
#include <Engine/EngineTypes.h>

#include "SingularisMorphVehicleIgniteComponent.generated.h"

class USingularisMorphVehicleSimulationComponent;

#pragma region 委托签名

/** 开始点火时广播（熄火 → 点火中） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnIgnitionStartedSignature);

/** 点火成功时广播（点火中 → 运行中） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnIgnitionSucceededSignature);

/** 点火取消时广播（点火中 → 熄火） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnIgnitionCancelledSignature);

/** 熄火时广播（运行中 → 熄火） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnIgnitionShutdownSignature);

#pragma endregion

/** 点火状态 */
UENUM(BlueprintType)
enum class ESingularisMorphVehicleIgnitionState : uint8
{
	/** 熄火：油门禁用 */
	Off UMETA(DisplayName = "熄火"),

	/** 点火中：油门禁用，等待点火成功 */
	Igniting UMETA(DisplayName = "点火中"),

	/** 运行中：油门放行 */
	Running UMETA(DisplayName = "运行中")
};

/**
 * 引力奇点变形载具点火组件。
 *
 * 服务器权威维护点火状态（熄火/点火中/运行中），状态复制到各端。
 * 组件在 BeginPlay 时直接通过仿真组件的控制通道闸门禁用油门；
 * 点火开始后经过 IgnitionDuration 秒进入运行中并放行油门，取消或熄火后再次禁用。
 * 计时只在服务器推进，客户端由状态复制驱动同一套油门闸门与事件。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变形载具点火组件")
)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleIgniteComponent : public UActorComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/**
	 * 关联的仿真组件引用。
	 *
	 * 点火组件通过其控制通道闸门接管油门输入；引用无效时油门闸门不可用。
	 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "引力奇点变形载具点火组件",
		meta = (
			DisplayName = "仿真组件引用",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisMorphVehicleSimulationComponent"
		)
	)
	FComponentReference SimulationComponentReference{};

	/** 点火时长（秒）：从开始点火到点火成功的等待时间 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "引力奇点变形载具点火组件",
		meta = (DisplayName = "点火时长", ClampMin = "0.0", UIMin = "0.0")
	)
	float IgnitionDuration = 1.5f;

#pragma endregion

#pragma region Event Dispatcher

	/** 开始点火时广播 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变形载具点火组件|事件分发器",
		meta = (DisplayName = "点火开始")
	)
	FOnIgnitionStartedSignature OnIgnitionStarted{};

	/** 点火成功时广播 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变形载具点火组件|事件分发器",
		meta = (DisplayName = "点火成功")
	)
	FOnIgnitionSucceededSignature OnIgnitionSucceeded{};

	/** 点火取消时广播 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变形载具点火组件|事件分发器",
		meta = (DisplayName = "点火取消")
	)
	FOnIgnitionCancelledSignature OnIgnitionCancelled{};

	/** 熄火时广播 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变形载具点火组件|事件分发器",
		meta = (DisplayName = "熄火")
	)
	FOnIgnitionShutdownSignature OnIgnitionShutdown{};

#pragma endregion

private:
#pragma region State

	/** 缓存的仿真组件，由 SimulationComponentReference 解析填充 */
	TWeakObjectPtr<USingularisMorphVehicleSimulationComponent> CachedSimulationComponent = nullptr;

	/** 当前点火状态（复制） */
	UPROPERTY(ReplicatedUsing = OnRep_IgnitionState)
	ESingularisMorphVehicleIgnitionState IgnitionState = ESingularisMorphVehicleIgnitionState::Off;

	/** 点火计时句柄（仅服务器） */
	FTimerHandle IgnitionTimer{};

#pragma endregion

public:
#pragma region Constructors

	/** 默认构造函数，启用组件复制并保持 Tick 关闭（计时驱动） */
	USingularisMorphVehicleIgniteComponent();

#pragma endregion

#pragma region ActorComponent Interface

	/** 解析仿真组件引用并应用初始熄火状态 */
	virtual void BeginPlay() override;

	/** 清理点火计时并释放缓存 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 注册 IgnitionState 为复制属性 */
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

#pragma endregion

#pragma region API

	/**
	 * 获取当前点火状态。
	 *
	 * @return 当前点火状态。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变形载具点火组件|API",
		meta = (DisplayName = "GetIgnitionState")
	)
	ESingularisMorphVehicleIgnitionState GetIgnitionState() const { return IgnitionState; }

	/**
	 * 是否处于点火中。
	 *
	 * @return 点火计时尚未结束时返回 true。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变形载具点火组件|API",
		meta = (DisplayName = "IsIgniting")
	)
	bool IsIgniting() const { return IgnitionState == ESingularisMorphVehicleIgnitionState::Igniting; }

	/**
	 * 是否处于运行中。
	 *
	 * @return 点火成功且尚未熄火时返回 true。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变形载具点火组件|API",
		meta = (DisplayName = "IsRunning")
	)
	bool IsRunning() const { return IgnitionState == ESingularisMorphVehicleIgnitionState::Running; }

	/**
	 * 开始点火。
	 *
	 * 服务器权威，仅熄火状态可调用。进入点火中后禁用油门，
	 * 经过 IgnitionDuration 秒自动进入运行中并放行油门。
	 * 点火中可调用 CancelIgnition 取消。
	 */
	UFUNCTION(
		BlueprintCallable,
		BlueprintAuthorityOnly,
		Category = "引力奇点变形载具点火组件|API",
		meta = (DisplayName = "开始点火")
	)
	void StartIgnition();

	/**
	 * 取消点火。
	 *
	 * 服务器权威，仅点火中可调用。停止计时并回到熄火，油门保持禁用。
	 * 运行中请使用 Shutdown。
	 */
	UFUNCTION(
		BlueprintCallable,
		BlueprintAuthorityOnly,
		Category = "引力奇点变形载具点火组件|API",
		meta = (DisplayName = "取消点火")
	)
	void CancelIgnition();

	/**
	 * 熄火。
	 *
	 * 服务器权威，仅运行中可调用。回到熄火并禁用油门。
	 * 点火中调用等价于 CancelIgnition。
	 */
	UFUNCTION(
		BlueprintCallable,
		BlueprintAuthorityOnly,
		Category = "引力奇点变形载具点火组件|API",
		meta = (DisplayName = "熄火")
	)
	void Shutdown();

#pragma endregion

private:
#pragma region Response

	/**
	 * IgnitionState 复制回调。
	 *
	 * 客户端收到复制值后应用油门闸门并广播状态迁移事件。
	 *
	 * @param OldState 复制前的旧状态。
	 */
	UFUNCTION()
	void OnRep_IgnitionState(ESingularisMorphVehicleIgnitionState OldState) const;

#pragma endregion

#pragma region Callback

	/** 点火计时结束：点火中 → 运行中 */
	void FinishIgnition();

#pragma endregion

#pragma region Internal Function

	/** 解析 SimulationComponentReference 并缓存仿真组件 */
	void ResolveSimulationComponent();

	/**
	 * 设置点火状态并应用后果。
	 *
	 * 服务器权威，幂等。先快照旧值，再赋值新值，最后调用 ApplyState。
	 *
	 * @param NewState 新的点火状态。
	 */
	void SetIgnitionState(ESingularisMorphVehicleIgnitionState NewState);

	/**
	 * 应用点火状态到油门闸门与状态迁移事件。
	 *
	 * @param OldState 变化前的旧状态，用于选择事件。
	 */
	void ApplyState(ESingularisMorphVehicleIgnitionState OldState) const;

	/** 按当前状态设置油门闸门：仅运行中放行 */
	void ApplyThrottleGate() const;

#pragma endregion
};
