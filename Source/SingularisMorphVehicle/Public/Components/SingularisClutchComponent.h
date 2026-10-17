#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleBaseComponent.h"
#include "SingularisClutchComponent.generated.h"

/**
 * 引力奇点离合器组件
 *
 * 限制引擎与变速箱之间的扭矩传递量，允许连接的轴以不同转速旋转。
 * 离合器强度决定最大可传递扭矩。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点离合器组件")
)
class SINGULARISMORPHVEHICLE_API USingularisClutchComponent : public USingularisMorphVehicleBaseComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 离合器接合强度（线性传递系数，1 = 完全接合直传扭矩） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点离合器组件",
		meta = (DisplayName = "离合器强度", ClampMin = "0", ClampMax = "1")
	)
	float ClutchStrength = 1.0f;

#pragma endregion

#pragma region Constructors

	USingularisClutchComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU InterfaceS

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Clutch;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
