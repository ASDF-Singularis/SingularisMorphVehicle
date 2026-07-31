#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisMorphVehicleClutchSUComponent.generated.h"

/**
 * 引力奇点变型载具离合器仿真单元组件
 *
 * 限制引擎与变速箱之间的扭矩传递量，允许连接的轴以不同转速旋转。
 * 离合器强度决定最大可传递扭矩。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具离合器仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API
	USingularisMorphVehicleClutchSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 离合器接合强度 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具离合器仿真单元|参数",
		meta = (DisplayName = "离合器强度")
	)
	float ClutchStrength = 1.0f;

	/** 链接的变速箱组件（单向引用） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具离合器仿真单元|链接",
		meta = (
			DisplayName = "链接变速箱",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisMorphVehicleTransmissionComponent"
		)
	)
	FComponentReference LinkedTransmission{};

#pragma endregion

#pragma region Constructors

	USingularisMorphVehicleClutchSUComponent();

#pragma endregion

#pragma region ISingularisMorphVehicleBaseInterface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Clutch;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
