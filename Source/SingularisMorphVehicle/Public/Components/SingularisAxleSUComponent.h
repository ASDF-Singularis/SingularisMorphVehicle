#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisAxleSUComponent.generated.h"

/**
 * 引力奇点轮轴仿真单元组件
 *
 * 带惯性的扭矩传递节点，插入变速箱与车轮之间构成差速器节点，
 * 扭矩沿模拟树的父子方向向下游传递。单动力链约定下由 SimulationComponent
 * 的 RebuildFromSnapshot 自动挂到首个变速箱之下，无需配置引用。
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
		Category = "引力奇点轮轴仿真单元",
		meta = (DisplayName = "轮轴惯性")
	)
	float AxleInertia = 1.0f;

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
