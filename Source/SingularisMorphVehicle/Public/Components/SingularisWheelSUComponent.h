#pragma once

#include <CoreMinimal.h>
#include <Curves/CurveFloat.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisWheelSUComponent.generated.h"

#pragma region 委托签名

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnWheelTouchChangeNative, int32, bool);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnWheelTouchChange, int32, Guid, bool, IsInContact);

#pragma endregion

/**
 * 引力奇点车轮转向设置。
 *
 * 转向角是模拟中的积分状态量：目标角由控制输入、最大转角与车速敏感曲线决定，
 * 实际角以有限的上升/回落角速度逐步逼近，逼近过程与轮胎回正力矩共同构成
 * 转向的动力学过程（ChaosVehicles 的原始实现为输入直接乘最大转角的瞬时转向）。
 *
 * 参考角指外侧轮的目标角，由 MaxSteeringAngle 限幅；几何修正只放大内侧轮。
 */
USTRUCT(BlueprintType)
struct SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleSteeringSetup
{
	GENERATED_BODY()

	/** 转向几何类型（仅决定内侧轮的目标角分配方式） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (DisplayName = "转向几何类型")
	)
	ESingularisMorphVehicleSteeringType SteeringType = ESingularisMorphVehicleSteeringType::SingleAngle;

	/** 转向角上升速率（度/秒），越低转向越缓慢 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (DisplayName = "转向上升速率", ClampMin = "0.0", UIMin = "0.0")
	)
	float SteeringRiseRate = 120.0f;

	/** 转向角回落速率（度/秒），通常大于上升速率以便更快回正 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (DisplayName = "转向回落速率", ClampMin = "0.0", UIMin = "0.0")
	)
	float SteeringFallRate = 240.0f;

	/** 角度比例（角度比例几何用）：内侧轮 = 参考角 / 比例，取值 (0, 1] */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (
			DisplayName = "角度比例",
			ClampMin = "0.01",
			ClampMax = "1.0",
			UIMin = "0.01",
			UIMax = "1.0",
			EditCondition = "SteeringType == ESingularisMorphVehicleSteeringType::AngleRatio"
		)
	)
	float AngleRatio = 0.7f;

	/** 轴距（厘米，阿克曼几何用） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (
			DisplayName = "轴距",
			ClampMin = "0.0",
			UIMin = "0.0",
			EditCondition = "SteeringType == ESingularisMorphVehicleSteeringType::Ackermann"
		)
	)
	float WheelBase = 280.0f;

	/** 轮距（厘米，阿克曼几何用） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (
			DisplayName = "轮距",
			ClampMin = "0.0",
			UIMin = "0.0",
			EditCondition = "SteeringType == ESingularisMorphVehicleSteeringType::Ackermann"
		)
	)
	float TrackWidth = 160.0f;

	/**
	 * 轮胎回正效应增益（度/(牛顿·秒)），0 表示关闭。
	 *
	 * 开启后以轮胎侧向力大小为比例持续削减转向角幅值，使稳态转角小于目标角
	 * （需驾驶员持续输入以保持转角），并在附着突变时产生转向反馈。
	 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (DisplayName = "回正效应增益", ClampMin = "0.0", UIMin = "0.0")
	)
	float SelfAligningTorqueGain = 0.0f;

	/**
	 * 车速敏感转向曲线：X 为轮心相对地面的前进速度（km/h），Y 为转向倍率。
	 *
	 * 曲线按峰值归一化后等距采样进模拟，为空表示转向角不随速度衰减。
	 * 默认值取自经典载具插件的速度敏感曲线（0/32/97/193 km/h → 1.0/0.8/0.4/0.3）。
	 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (DisplayName = "车速敏感转向曲线")
	)
	FRuntimeFloatCurve SpeedSteeringCurve{};
};

/**
 * 引力奇点车轮仿单元组件
 *
 * 模拟车轮的转动、摩擦、转向与制动行为。支持 ABS、牵引力控制、
 * 手刹等高级特性。通过物理线程与悬挂模块协同工作，
 * 输出数据通过 OnWheelTouchChange 事件回调至游戏线程。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点车轮仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API USingularisWheelSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 链接的悬挂组件（单向引用） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元",
		meta = (
			DisplayName = "链接悬挂",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisSuspensionSUComponent"
		)
	)
	FComponentReference LinkedSuspension{};

	/** 链接的轮轴组件（单向引用，声明后车轮的扭矩父节点为该轮轴） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元",
		meta = (
			DisplayName = "链接轮轴",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisAxleSUComponent"
		)
	)
	FComponentReference LinkedAxle{};

	/** 车轮半径（厘米） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|几何",
		meta = (DisplayName = "车轮半径")
	)
	float WheelRadius = 30.0f;

	/** 车轮宽度（厘米） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|几何",
		meta = (DisplayName = "车轮宽度")
	)
	float WheelWidth = 20.0f;

	/** 轴向类型 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|几何",
		meta = (DisplayName = "轴向类型")
	)
	ESingularisMorphVehicleWheelAxisType AxisType = ESingularisMorphVehicleWheelAxisType::X;

	/** 车轮转动惯性 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|几何",
		meta = (DisplayName = "车轮惯性")
	)
	float WheelInertia = 10.0f;

	/** 最大旋转角速度（弧度/秒） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|几何",
		meta = (DisplayName = "最大旋转角速度")
	)
	float MaxRotationVel = 100.0f;

	/** 摩擦系数倍率 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|摩擦",
		meta = (DisplayName = "摩擦倍率")
	)
	float FrictionMultiplier = 2.0f;

	/** 侧向滑移图倍率 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|摩擦",
		meta = (DisplayName = "侧滑图倍率")
	)
	float LateralSlipGraphMultiplier = 1.0f;

	/** 侧偏刚度 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|摩擦",
		meta = (DisplayName = "侧偏刚度")
	)
	float CorneringStiffness = 1000.0f;

	/** 侧向滑移图采样点（X 为侧滑角（度），Y 为侧向力，须按 X 等间距排列；为空时改用侧偏刚度） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|摩擦",
		meta = (DisplayName = "侧滑图")
	)
	TArray<FVector2D> LateralSlipGraph{};

	/** 侧滑角上限 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|摩擦",
		meta = (DisplayName = "侧滑角上限")
	)
	float SlipAngleLimit = 8.0f;

	/** 轮胎合力受附着极限削减时的衰减系数 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|摩擦",
		meta = (DisplayName = "打滑衰减")
	)
	float SlipModifier = 0.9f;

	/** 是否启用 ABS */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|辅助",
		meta = (DisplayName = "启用ABS")
	)
	bool bABSEnabled = true;

	/** 是否启用牵引力控制 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|辅助",
		meta = (DisplayName = "启用牵引力控制")
	)
	bool bTractionControlEnabled = true;

	/** 最大制动扭矩 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|制动",
		meta = (DisplayName = "最大制动扭矩")
	)
	float MaxBrakeTorque = 2000.0f;

	/** 是否启用手刹 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|制动",
		meta = (DisplayName = "启用手刹")
	)
	bool bHandbrakeEnabled = false;

	/** 手刹扭矩 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|制动",
		meta = (DisplayName = "手刹扭矩", EditCondition = "bHandbrakeEnabled")
	)
	float HandbrakeTorque = 2000.0f;

	/** 是否启用自动手刹 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|制动",
		meta = (DisplayName = "启用自动手刹")
	)
	bool bAutoHandbrakeEnabled = false;

	/** 自动手刹触发速度阈值（厘米/秒） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|制动",
		meta = (DisplayName = "自动手刹速度阈值", EditCondition = "bAutoHandbrakeEnabled")
	)
	float AutoHandbrakeVelocityThreshold = 10.0f;

	/** 是否启用转向 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (DisplayName = "启用转向")
	)
	bool bSteeringEnabled = false;

	/** 最大转向角度 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (DisplayName = "最大转向角度", EditCondition = "bSteeringEnabled", ClampMin = "0.0", UIMin = "0.0")
	)
	float MaxSteeringAngle = 35.0f;

	/**
	 * 转向动力学设置（仅启用转向时生效）。
	 *
	 * 转向角是被积分的状态量：目标角由输入、最大转角与车速敏感曲线确定，
	 * 实际角按上升/回落速率限幅逼近目标角，由此获得经典载具插件的转向手感。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|转向",
		meta = (DisplayName = "转向设置", EditCondition = "bSteeringEnabled")
	)
	FSingularisMorphVehicleSteeringSetup SteeringSetup{};

	/** 力作用点偏移 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|偏移",
		meta = (DisplayName = "力偏移")
	)
	FVector ForceOffset = FVector::ZeroVector;

	/** 反转旋转方向 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|偏移",
		meta = (DisplayName = "反转方向")
	)
	bool ReverseDirection = false;

#pragma endregion

#pragma region 事件分发器

	/** 快速（低开销）原生版本的车轮触地变化委托 */
	FOnWheelTouchChangeNative OnWheelTouchChangeNativeEvent{};

	/** 蓝图表单的车轮触地变化事件 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点车轮仿真单元|事件分发器",
		meta = (DisplayName = "触地变化")
	)
	FOnWheelTouchChange OnWheelTouchChangeEvent{};

#pragma endregion

private:
#pragma region Internal Variable

	/**
	 * 最近一次输出的实际转向角（度）。
	 *
	 * 拓扑重建会销毁并按几何重建车轮模块，而转向角是积分状态；
	 * 以最近输出为初值可使重建后的车轮从当前转角继续，而非瞬间回正。
	 */
	float CachedSteeringAngleDegrees = 0.0f;

#pragma endregion

public:
	/** 获取最近一次输出的实际转向角（度） */
	float GetCachedSteeringAngleDegrees() const { return CachedSteeringAngleDegrees; }

#pragma region Constructors

	USingularisWheelSUComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Wheel;
	}

	virtual void OnOutputReady(const Chaos::FSimOutputData* OutputData) override;

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
