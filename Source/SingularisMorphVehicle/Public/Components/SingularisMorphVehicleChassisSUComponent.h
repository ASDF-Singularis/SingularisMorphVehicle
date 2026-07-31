#pragma once

#include <CoreMinimal.h>
#include <VehicleUtility.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisMorphVehicleChassisSUComponent.generated.h"

/**
 * 引力奇点变型载具底盘仿真单元组件
 *
 * 定义载具的空气动力学阻力属性，通过物理线程模拟空气阻力与角阻尼。
 * 底盘自身不产生推进力，仅影响物理系统的减速行为。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具底盘仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API
	USingularisMorphVehicleChassisSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 迎风面积（平方米） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具底盘仿真单元|阻力",
		meta = (DisplayName = "迎风面积")
	)
	float AreaMetresSquared = 0.0f;

	/** 空气阻力系数 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具底盘仿真单元|阻力",
		meta = (DisplayName = "阻力系数")
	)
	float DragCoefficient = 0.5f;

	/** 介质密度 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具底盘仿真单元|阻力",
		meta = (DisplayName = "介质密度")
	)
	float DensityOfMedium = Chaos::RealWorldConsts::AirDensity();

	/** X轴阻力倍率 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具底盘仿真单元|阻力",
		meta = (DisplayName = "X轴倍率")
	)
	float XAxisMultiplier = 1.0f;

	/** Y轴阻力倍率 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具底盘仿真单元|阻力",
		meta = (DisplayName = "Y轴倍率")
	)
	float YAxisMultiplier = 1.0f;

	/** 角阻尼系数 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具底盘仿真单元|阻尼",
		meta = (DisplayName = "角阻尼")
	)
	float AngularDamping = 0.0f;

#pragma endregion

#pragma region Constructors

	USingularisMorphVehicleChassisSUComponent();

#pragma endregion

#pragma region ISingularisMorphVehicleBaseInterface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Chassis;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
