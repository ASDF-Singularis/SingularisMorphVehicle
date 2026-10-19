#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleBaseComponent.h"
#include "SingularisWheelComponent.generated.h"

#pragma region 委托签名

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnWheelTouchChangeNative, int32, bool);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnWheelTouchChange, int32, Guid, bool, IsInContact);

#pragma endregion

class USingularisUprightComponent;

/**
 * 引力奇点车轮组件
 *
 * 模拟车轮的转动与摩擦行为，仅持有轮胎本体的属性：几何（半径/宽度）、
 * 惯性、转速上限与摩擦特性（摩擦倍率/侧滑图/侧偏刚度/打滑衰减）。
 * 通过物理线程与悬挂模块协同工作，输出数据通过 OnWheelTouchChange 事件回调至游戏线程。
 *
 * 其余均为轮位属性而非轮胎属性：转向、轴向、反转、制动系统（行车制动/手刹/
 * 自动手刹）、辅助系统（ABS/牵引力控制）与力作用点偏移全部由立轴槽位持有，
 * 挂载槽位时在建树期注入车轮模块（见 ApplySlotConfig）。本组件不持有这些配置：
 * 未挂载的轮胎以中性默认（无转向、无制动、轴向 X、不反转）作为简单滚动体，
 * 其地面交互按该默认结算。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点车轮组件")
)
class SINGULARISMORPHVEHICLE_API USingularisWheelComponent : public USingularisMorphVehicleBaseComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 车轮半径（cm） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|几何",
		meta = (DisplayName = "车轮半径", ClampMin = "1")
	)
	float WheelRadius = 30.0f;

	/** 车轮宽度（cm） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|几何",
		meta = (DisplayName = "车轮宽度", ClampMin = "0")
	)
	float WheelWidth = 20.0f;

	/** 车轮转动惯性（kg·m²） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|几何",
		meta = (DisplayName = "车轮惯性", ClampMin = "0.001")
	)
	float WheelInertia = 10.0f;

	/**
	 * 最大旋转角速度（rad/s）
	 *
	 * 车轮自身的安全上限；若低于引擎最高转速经最高挡传动链折算的轮速，
	 * 建树及后续拓扑同步时会自动提升到该轮速，避免车轮限速先于引擎限速生效。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|几何",
		meta = (DisplayName = "最大旋转角速度", ClampMin = "0")
	)
	float MaxRotationVel = 100.0f;

	/** 摩擦系数倍率 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|摩擦",
		meta = (DisplayName = "摩擦倍率", ClampMin = "0")
	)
	float FrictionMultiplier = 2.0f;

	/** 侧向滑移图倍率 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|摩擦",
		meta = (DisplayName = "侧滑图倍率")
	)
	float LateralSlipGraphMultiplier = 1.0f;

	/** 侧偏刚度 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|摩擦",
		meta = (DisplayName = "侧偏刚度", ClampMin = "0")
	)
	float CorneringStiffness = 1000.0f;

	/** 侧向滑移图采样点（X 为侧滑角（度），Y 为侧向力，须按 X 等间距排列；为空时改用侧偏刚度） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|摩擦",
		meta = (DisplayName = "侧滑图")
	)
	TArray<FVector2D> LateralSlipGraph{};

	/** 侧滑角上限 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|摩擦",
		meta = (DisplayName = "侧滑角上限", ClampMin = "0")
	)
	float SlipAngleLimit = 8.0f;

	/** 轮胎合力受附着极限削减时的衰减系数 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮组件|摩擦",
		meta = (DisplayName = "打滑衰减")
	)
	float SlipModifier = 0.9f;

#pragma endregion

#pragma region 事件分发器

	/** 快速（低开销）原生版本的车轮触地变化委托 */
	FOnWheelTouchChangeNative OnWheelTouchChangeNativeEvent{};

	/** 蓝图表单的车轮触地变化事件 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点车轮组件|事件分发器",
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

	/** 动力链允许的最高轮速（rad/s），0 表示未提供；建树期由 SimulationComponent 写入 */
	float EffectiveMaxRotationVel = 0.0f;

#pragma endregion

public:
	/** 获取最近一次输出的实际转向角（度） */
	float GetCachedSteeringAngleDegrees() const { return CachedSteeringAngleDegrees; }

	/** 设置建树期有效轮速上限（动力链最高转速折算值） */
	void SetEffectiveMaxRotationVel(const float InValue) { EffectiveMaxRotationVel = FMath::Max(InValue, 0.0f); }

	/** 获取建树期有效轮速上限（0 表示未由动力链提供） */
	float GetEffectiveMaxRotationVel() const { return EffectiveMaxRotationVel; }

	/** 车轮模块最终使用的转速上限：自身安全上限与动力链折算上限取较大者 */
	float GetResolvedMaxRotationVel() const
	{
		const float Base = FMath::IsFinite(MaxRotationVel) ? FMath::Max(MaxRotationVel, 0.0f) : 0.0f;
		const float Effective = FMath::IsFinite(EffectiveMaxRotationVel)
			                        ? FMath::Max(EffectiveMaxRotationVel, 0.0f)
			                        : 0.0f;
		return FMath::Max(Base, Effective);
	}

#pragma region Constructors

	USingularisWheelComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Wheel;
	}

	virtual void OnOutputReady(const Chaos::FSimOutputData* OutputData) override;

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

	/** 建树期立轴槽位配置注入：转向/轴向/反转/制动/辅助/力偏移整体写入本组件创建的车轮模块（见 ApplyUprightSlotConfig） */
	virtual void ApplySlotConfig(
		Chaos::ISimulationModuleBase* CoreModule,
		const USingularisUprightComponent* UprightSU
	) override;

#pragma endregion
};
