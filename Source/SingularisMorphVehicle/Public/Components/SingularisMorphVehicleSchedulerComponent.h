#pragma once

#include <CoreMinimal.h>
#include <Components/ActorComponent.h>

#include "SingularisMorphVehicleSchedulerComponent.generated.h"

class USingularisMorphVehicleSimulationComponent;

UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具调度器组件")
)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleSchedulerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具调度器|引用",
		meta = (
			DisplayName = "调度目标",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisMorphVehicleSimulationComponent"
		)
	)
	FComponentReference TargetComponent{};

#pragma endregion

#pragma region Constructors

	USingularisMorphVehicleSchedulerComponent();

#pragma endregion

#pragma region ActorComponent Interface

	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction
	) override;

	virtual void Activate(bool bReset = false) override;
	virtual void Deactivate() override;

#pragma endregion
};
