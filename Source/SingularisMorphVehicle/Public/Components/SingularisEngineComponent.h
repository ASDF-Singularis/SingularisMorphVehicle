#pragma once

#include <CoreMinimal.h>
#include <Curves/CurveFloat.h>

#include "SingularisMorphVehicleBaseComponent.h"
#include "SingularisEngineComponent.generated.h"

/**
 * 引力奇点引擎组件
 *
 * 通过扭矩曲线定义引擎输出特性，向变速箱/离合器输送扭矩。
 * 模拟怠速、引擎制动、惯性等内燃机行为，输出数据通过 OnOutputReady 传递至游戏线程。
 * 空挡或离合分离时引擎按自身扭矩与惯性自由升速，松油门后按引擎制动扭矩回落至怠速。
 * 挂挡且未踩制动/油门时按怠速蠕行设置向传动系输出扭矩，使载具能够自行加速到怠速行驶速度。
 * 离合接合时引擎转速直接反映传动系输入轴转速，允许被拖到怠速以下并随车速回升。
 * 制动踏板或手刹有效时怠速蠕行扭矩归零。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点引擎组件")
)
class SINGULARISMORPHVEHICLE_API USingularisEngineComponent : public USingularisMorphVehicleBaseComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 引擎扭矩曲线（横轴为 0..1 归一化转速，纵轴为归一化扭矩） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|扭矩",
		meta = (DisplayName = "扭矩曲线")
	)
	FRuntimeFloatCurve TorqueCurve{};

	/** 峰值扭矩（N·m） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|扭矩",
		meta = (DisplayName = "最大扭矩", ClampMin = "0")
	)
	float MaxTorque = 200.0f;

	/**
	 * 最大转速（RPM，上限受引擎设置的 uint16 存储限制）
	 *
	 * 既是引擎自身角速度上限，也是传动系经齿比映射后的转速上限：
	 * 传动系超速时引擎断油并按超速量施加反拖制动。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|转速",
		meta = (DisplayName = "最大RPM", ClampMin = "1", UIMin = "1", ClampMax = "65535", UIMax = "65535")
	)
	int32 MaxRPM = 5000;

	/** 怠速转速（RPM），运行时会被钳制到最大转速以下 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|转速",
		meta = (DisplayName = "怠速RPM", ClampMin = "0", UIMin = "0")
	)
	int32 EngineIdleRPM = 1200;

	/**
	 * 最大引擎制动扭矩（N·m）
	 *
	 * 释放油门时按转速从怠速向最大转速线性增强；挂挡滑行时经传动系制动车辆，
	 * 空挡时只降低引擎自身转速。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|动力学",
		meta = (DisplayName = "引擎制动扭矩", ClampMin = "0")
	)
	float EngineBrakeEffect = 150.0f;

	/**
	 * 怠速蠕行扭矩比例（相对最大扭矩）
	 *
	 * 挂挡且未踩制动/油门时，按该比例输出蠕行扭矩推动载具，目标为怠速对应的车速；
	 * 扭矩随转速差成比例衰减，并随油门线性淡出；制动踏板或手刹有效时归零。
	 * 0 表示关闭蠕行。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|动力学",
		meta = (DisplayName = "怠速蠕行扭矩比例", ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1")
	)
	float IdleCreepTorqueRatio = 0.15f;

	/**
	 * 空挡/离合分离时的最大升速速率（RPM/s）
	 *
	 * 仅约束无负载自由转速；直连接合时转速由传动系决定。0 表示不限制。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|动力学",
		meta = (DisplayName = "空挡升速速率(RPM/s)", ClampMin = "0", UIMin = "0", UIMax = "20000")
	)
	float EngineRevUpRate = 5000.0f;

	/** 空挡/离合分离时的最大降速速率（RPM/s）；0 表示不限制 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|动力学",
		meta = (DisplayName = "空挡降速速率(RPM/s)", ClampMin = "0", UIMin = "0", UIMax = "20000")
	)
	float EngineRevDownRate = 4000.0f;

	/**
	 * 引擎转动惯量（kg·cm²，1 kg·m² = 10000 kg·cm²）
	 *
	 * 与 Chaos 模拟模块的 CGS 单位一致，直接参与角速度积分；
	 * 1000 kg·cm² ≈ 0.1 kg·m²，为轻量赛车飞轮量级。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点引擎组件|动力学",
		meta = (DisplayName = "引擎惯性(kg·cm²)", ClampMin = "1", UIMin = "1")
	)
	float EngineInertia = 1000.0f;

#pragma endregion

#pragma region Constructors

	USingularisEngineComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Engine;
	}

	virtual void OnOutputReady(const Chaos::FSimOutputData* OutputData) override;

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
