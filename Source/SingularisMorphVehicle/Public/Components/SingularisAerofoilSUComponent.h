#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisAerofoilSUComponent.generated.h"

/**
 * 引力奇点翼型仿真单元组件
 *
 * 模拟空气动力学翼面的升力、阻力与控制面偏转。
 * 支持固定翼、机翼、方向舵、升降舵四种类型。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点翼型仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API USingularisAerofoilSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 翼面相对于父节点的位置偏移 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|力轴",
		meta = (DisplayName = "偏移")
	)
	FVector Offset = FVector::ZeroVector;

	/** 升力作用轴（本地空间） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|力轴",
		meta = (DisplayName = "力轴")
	)
	FVector ForceAxis = FVector(0.0f, 0.0f, 1.0f);

	/** 控制面旋转轴 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|力轴",
		meta = (DisplayName = "控制旋转轴")
	)
	FVector ControlRotationAxis = FVector(0.0f, 1.0f, 0.0f);

	/** 翼面积 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|翼面",
		meta = (DisplayName = "翼面积")
	)
	float Area = 10.0f;

	/** 翼面弧度 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|翼面",
		meta = (DisplayName = "弧度")
	)
	float Camber = 10.0f;

	/** 失速角 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|翼面",
		meta = (DisplayName = "失速角")
	)
	float StallAngle = 20.0f;

	/** 翼面类型 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|翼面",
		meta = (DisplayName = "翼面类型")
	)
	ESingularisMorphVehicleAerofoilType Type = ESingularisMorphVehicleAerofoilType::Wing;

	/** 最大控制面偏转角 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|翼面",
		meta = (DisplayName = "最大控制角")
	)
	float MaxControlAngle = 30.0f;

	/** 升力倍率 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|倍率",
		meta = (DisplayName = "升力倍率")
	)
	float LiftMultiplier = 1.0f;

	/** 阻力倍率 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|倍率",
		meta = (DisplayName = "阻力倍率")
	)
	float DragMultiplier = 1.0f;

	/** 动画幅度倍率 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点翼型仿真单元组件|倍率",
		meta = (DisplayName = "动画幅度倍率")
	)
	float AnimationMagnitudeMultiplier = 1.0f;

#pragma endregion

#pragma region Constructors

	USingularisAerofoilSUComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Aerofoil;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
