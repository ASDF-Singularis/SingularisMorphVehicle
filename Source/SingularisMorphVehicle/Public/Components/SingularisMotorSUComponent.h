#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisMotorSUComponent.generated.h"

/**
 * 引力奇点电机仿真单元组件
 *
 * 以转速平方曲线输出扭矩的电驱动力单元，扭矩幅值由油门输入控制，
 * 转速接近零与最大转速时输出衰减、中段转速达到峰值。
 * 与引擎同为原动机，扭矩沿模拟树的父子方向向下传递，通过链接离合器接入动力链。
 * 输入端配置继承自基类，由上层按需声明油门等控制通道。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点电机仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API USingularisMotorSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 链接的离合器组件（单向引用，声明后电机的扭矩下游为该离合器） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点电机仿真单元组件",
		meta = (
			DisplayName = "链接离合器",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisClutchSUComponent"
		)
	)
	FComponentReference LinkedClutch{};

	/** 峰值扭矩（牛顿·米） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点电机仿真单元组件",
		meta = (DisplayName = "最大扭矩")
	)
	float MaxTorque = 200.0f;

	/** 最大转速（RPM） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点电机仿真单元组件",
		meta = (DisplayName = "最大RPM")
	)
	float MaxRPM = 6000.0f;

	/** 电机惯性（kg·m²） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点电机仿真单元组件",
		meta = (DisplayName = "电机惯性")
	)
	float EngineInertia = 100.0f;

#pragma endregion

#pragma region Constructors

	USingularisMotorSUComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Motor;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
