#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleBaseComponent.h"
#include "SingularisAxleComponent.generated.h"

/**
 * 引力奇点轮轴组件
 *
 * 动力链中的纯扭矩中继节点（开式差速器），插入变速箱与车轮之间：
 * 扭矩沿模拟树的父子方向向下游均分，自身转速同步为下游车轮平均转速。
 * 单动力链约定下由 SimulationComponent 的 RebuildFromSnapshot 自动挂到
 * 首个变速箱之下，无需配置引用。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点轮轴组件")
)
class SINGULARISMORPHVEHICLE_API USingularisAxleComponent : public USingularisMorphVehicleBaseComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/**
	 * 轮轴惯性（kg·m²）
	 *
	 * 保留为既有资产的兼容配置：轮轴在模拟树中是纯扭矩中继，
	 * 自身转速由下游车轮同步，自惯量不参与转速链。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点轮轴组件",
		meta = (DisplayName = "轮轴惯性", ClampMin = "0.001")
	)
	float AxleInertia = 1.0f;

#pragma endregion

#pragma region Constructors

	USingularisAxleComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Axle;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
