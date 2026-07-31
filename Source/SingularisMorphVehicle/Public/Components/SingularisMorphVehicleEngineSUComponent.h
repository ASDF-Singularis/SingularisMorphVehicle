#pragma once

#include <CoreMinimal.h>
#include <Curves/CurveFloat.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisMorphVehicleEngineSUComponent.generated.h"

/**
 * 引力奇点变型载具引擎仿真单元组件
 *
 * 通过扭矩曲线定义引擎输出特性，向变速箱/离合器输送扭矩。
 * 模拟怠速、引擎制动、惯性等内燃机行为，输出数据通过 OnOutputReady 传递至游戏线程。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具引擎仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API
	USingularisMorphVehicleEngineSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 引擎扭矩曲线（归一化） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具引擎仿真单元|扭矩",
		meta = (DisplayName = "扭矩曲线")
	)
	FRuntimeFloatCurve TorqueCurve{};

	/** 峰值扭矩（牛顿·米） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具引擎仿真单元|扭矩",
		meta = (DisplayName = "最大扭矩")
	)
	float MaxTorque = 200.0f;

	/** 最大转速（RPM） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具引擎仿真单元|转速",
		meta = (DisplayName = "最大RPM")
	)
	int32 MaxRPM = 5000;

	/** 怠速转速（RPM） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具引擎仿真单元|转速",
		meta = (DisplayName = "怠速RPM")
	)
	int32 EngineIdleRPM = 1200;

	/** 引擎制动效应 [0..1] */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具引擎仿真单元|动力学",
		meta = (DisplayName = "引擎制动效应")
	)
	float EngineBrakeEffect = 150.0f;

	/** 引擎惯性（kg·m²） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具引擎仿真单元|动力学",
		meta = (DisplayName = "引擎惯性")
	)
	float EngineInertia = 1000.0f;

	/** 链接的离合器组件（单向引用） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具引擎仿真单元|链接",
		meta = (
			DisplayName = "链接离合器",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisMorphVehicleClutchComponent"
		)
	)
	FComponentReference LinkedClutch{};

#pragma endregion

#pragma region Constructors

	USingularisMorphVehicleEngineSUComponent();

#pragma endregion

#pragma region ISingularisMorphVehicleBaseInterface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Engine;
	}

	virtual void OnOutputReady(const Chaos::FSimOutputData* OutputData) override;
	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
