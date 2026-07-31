#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisMorphVehicleThrusterSUComponent.generated.h"

/**
 * 引力奇点变型载具推进器仿真单元组件
 *
 * 沿指定的力轴和力偏移位置施加推进力，支持推力转向（矢量推力）。
 * 可选启用推力倍率（Boost）以在运行时动态增强推进输出。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具推进器仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API
	USingularisMorphVehicleThrusterSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 最大推进力 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具推进器仿真单元|推力",
		meta = (DisplayName = "最大推进力")
	)
	float MaxThrustForce = 10000.0f;

	/** 推力作用轴（本地空间） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具推进器仿真单元|推力",
		meta = (DisplayName = "力轴")
	)
	FVector ForceAxis = FVector(1.0f, 0.0f, 0.0f);

	/** 推力作用点偏移 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具推进器仿真单元|推力",
		meta = (DisplayName = "力偏移")
	)
	FVector ForceOffset = FVector::ZeroVector;

	/** 推力倍率效应（Boost） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具推进器仿真单元|推力",
		meta = (DisplayName = "推力倍率效应")
	)
	float BoostMultiplierEffect = 2.0f;

	/** 是否启用推力转向 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具推进器仿真单元|转向",
		meta = (DisplayName = "启用转向")
	)
	bool bSteeringEnabled = false;

	/** 转向轴 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具推进器仿真单元|转向",
		meta = (DisplayName = "转向轴")
	)
	FVector SteeringAxis = FVector(0.0f, 0.0f, 1.0f);

	/** 最大转向角度 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具推进器仿真单元|转向",
		meta = (DisplayName = "最大转向角度")
	)
	float MaxSteeringAngle = 0.0f;

	/** 转向力效应 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具推进器仿真单元|转向",
		meta = (DisplayName = "转向力效应")
	)
	float SteeringForceEffect = 0.5f;

#pragma endregion

#pragma region Constructors

	USingularisMorphVehicleThrusterSUComponent();

#pragma endregion

#pragma region ISingularisMorphVehicleBaseInterface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Thruster;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
