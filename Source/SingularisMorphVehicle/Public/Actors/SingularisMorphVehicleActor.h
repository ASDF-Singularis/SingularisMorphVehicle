#pragma once

#include <CoreMinimal.h>
#include <GameFramework/Actor.h>

#include "SingularisMorphVehicleActor.generated.h"

class UStaticMeshComponent;
class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具 Actor。
 */
UCLASS(Abstract, Blueprintable)
class SINGULARISMORPHVEHICLE_API ASingularisMorphVehicleActor : public AActor
{
	GENERATED_BODY()

public:
#pragma region Instantiation

	/** 静态网格体组件 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly)
	TObjectPtr<UStaticMeshComponent> StaticMeshComponent = nullptr;

	/** 载具模拟组件 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly)
	TObjectPtr<USingularisMorphVehicleSimulationComponent> VehicleMovementComponent = nullptr;

#pragma endregion

#pragma region Constructors

	ASingularisMorphVehicleActor();

#pragma endregion

#pragma region Actor Interface

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

#pragma endregion
};
