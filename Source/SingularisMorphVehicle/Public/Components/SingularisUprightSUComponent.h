#pragma once

#include <CoreMinimal.h>
#include <Curves/CurveFloat.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "Core/SingularisMorphVehicleWheelSimModule.h"
#include "SingularisUprightSUComponent.generated.h"

/**
 * 引力奇点立轴转向设置。
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
		Category = "引力奇点立轴仿真单元|转向",
		meta = (DisplayName = "转向几何类型")
	)
	ESingularisMorphVehicleSteeringType SteeringType = ESingularisMorphVehicleSteeringType::SingleAngle;

	/** 转向角上升速率（度/秒），越低转向越缓慢 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元|转向",
		meta = (DisplayName = "转向上升速率", ClampMin = "0.0", UIMin = "0.0")
	)
	float SteeringRiseRate = 120.0f;

	/** 转向角回落速率（度/秒），通常大于上升速率以便更快回正 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元|转向",
		meta = (DisplayName = "转向回落速率", ClampMin = "0.0", UIMin = "0.0")
	)
	float SteeringFallRate = 240.0f;

	/** 角度比例（角度比例几何用）：内侧轮 = 参考角 / 比例，取值 (0, 1] */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元|转向",
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
		Category = "引力奇点立轴仿真单元|转向",
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
		Category = "引力奇点立轴仿真单元|转向",
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
		Category = "引力奇点立轴仿真单元|转向",
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
		Category = "引力奇点立轴仿真单元|转向",
		meta = (DisplayName = "车速敏感转向曲线")
	)
	FRuntimeFloatCurve SpeedSteeringCurve{};
};

/**
 * 引力奇点立轴仿真单元组件
 *
 * 车架上的轮位槽配置载体：持有该轮位的转向、轴向与反转方向配置，
 * 不创建模拟模块、不进入模拟树。轮胎安装进槽位匹配半径内时，
 * SimulationComponent 在建树期将本组件配置注入车轮模块
 * （见 WheelSU 的 ApplySlotConfig），轮胎模块代码本身零改动。
 *
 * 驱动组件指向车架上的槽位标记场景组件，其世界位置即槽位位置；
 * 邻近匹配按轮胎驱动组件与槽位标记的距离取匹配半径内的最近者，
 * 每个槽位至多消费一个轮胎。未匹配任何立轴的轮胎为自由轮：
 * 挂到底盘之下，无驱动、无转向，仅提供滚动。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点立轴仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API USingularisUprightSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 槽位匹配半径（厘米）：轮胎驱动组件距槽位标记小于该值时可挂载本槽位 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元",
		meta = (DisplayName = "匹配半径", ClampMin = "0.0", UIMin = "0.0")
	)
	float MatchRadius = 50.0f;

	/** 轴向类型：挂载轮胎的滚动轴（建树期注入车轮模块） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元",
		meta = (DisplayName = "轴向类型")
	)
	ESingularisMorphVehicleWheelAxisType AxisType = ESingularisMorphVehicleWheelAxisType::X;

	/** 反转方向：车辆两侧轮胎的镜像差异由槽位声明（建树期注入车轮模块） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元",
		meta = (DisplayName = "反转方向")
	)
	bool ReverseDirection = false;

	/** 是否启用转向 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元|转向",
		meta = (DisplayName = "启用转向")
	)
	bool bSteeringEnabled = false;

	/** 最大转向角度（度，同时作为外侧轮的参考角上限） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元|转向",
		meta = (DisplayName = "最大转向角度", EditCondition = "bSteeringEnabled", ClampMin = "0.0", UIMin = "0.0")
	)
	float MaxSteeringAngle = 35.0f;

	/** 转向动力学设置 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "引力奇点立轴仿真单元|转向",
		meta = (DisplayName = "转向设置", EditCondition = "bSteeringEnabled")
	)
	FSingularisMorphVehicleSteeringSetup SteeringSetup{};

#pragma endregion

#pragma region Constructors

	USingularisUprightSUComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Upright;
	}

#pragma endregion

#pragma region 槽位配置

	/**
	 * 构建物理线程侧转向动力学设置（含车速敏感曲线烘培）。
	 *
	 * @param InitialSteeringAngleDegrees 转向角初值（度），取槽位当前车轮的缓存转向角
	 * @return 物理线程侧转向设置，供车轮模块建树期注入
	 */
	FSingularisMorphWheelSteeringSettings BuildSteeringSettings(float InitialSteeringAngleDegrees) const;

#pragma endregion
};
