#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisAxleSUComponent.generated.h"

/**
 * 引力奇点轮轴仿真单元组件
 *
 * 带惯性的扭矩传递节点，可在变速箱与车轮之间插入以构成差速器节点，
 * 扭矩沿模拟树的父子方向向下游传递。树内位置由 SimulationComponent 的
 * RebuildFromSnapshot 按 LinkedTransmission 引用决定，无引用时挂到 Chassis 之下。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点轮轴仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API USingularisAxleSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 轮轴惯性（kg·m²） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点轮轴仿真单元|动力学",
		meta = (DisplayName = "轮轴惯性")
	)
	float AxleInertia = 1.0f;

	/** 链接的变速箱组件（单向引用，声明后轮轴的扭矩父节点为该变速箱） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点轮轴仿真单元|链接",
		meta = (
			DisplayName = "链接变速箱",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisTransmissionSUComponent"
		)
	)
	FComponentReference LinkedTransmission{};

#pragma endregion

#pragma region Constructors

	USingularisAxleSUComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Axle;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
