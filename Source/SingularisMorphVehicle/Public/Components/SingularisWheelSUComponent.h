#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisWheelSUComponent.generated.h"

#pragma region 委托签名

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnWheelTouchChangeNative, int32, bool);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnWheelTouchChange, int32, Guid, bool, IsInContact);

#pragma endregion

class USingularisUprightSUComponent;

/**
 * 引力奇点车轮仿真单元组件
 *
 * 模拟车轮的转动、摩擦与制动行为，支持 ABS、牵引力控制、手刹等高级特性。
 * 通过物理线程与悬挂模块协同工作，输出数据通过 OnWheelTouchChange 事件回调至游戏线程。
 *
 * 转向、轴向与反转方向是轮位属性而非轮胎属性：挂载立轴槽位时由立轴配置
 * 在建树期注入（见 ApplySlotConfig）；未挂载的自由轮胎使用本组件的轴向与
 * 反转方向作为回退值，且始终无转向。
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

	/** 轴向类型（自由轮胎回退值：挂载立轴槽位时由槽位配置覆盖） */
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

	/** 力作用点偏移 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点车轮仿真单元|偏移",
		meta = (DisplayName = "力偏移")
	)
	FVector ForceOffset = FVector::ZeroVector;

	/** 反转旋转方向（自由轮胎回退值：挂载立轴槽位时由槽位配置覆盖） */
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

	/** 建树期立轴槽位配置注入：转向/轴向/反转写入本组件创建的车轮模块（见 ApplyUprightSlotConfig） */
	virtual void ApplySlotConfig(
		Chaos::ISimulationModuleBase* CoreModule,
		const USingularisUprightSUComponent* UprightSU
	) override;

#pragma endregion
};
