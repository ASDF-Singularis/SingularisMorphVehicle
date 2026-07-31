#pragma once

#include <CoreMinimal.h>
#include <GameFramework/Pawn.h>

#include "SingularisMorphVehiclePawn.generated.h"

class UStaticMeshComponent;
class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具 Pawn。
 *
 * 基于 StaticMeshComponent 作为根组件的变型载具 Actor 基类，
 * 通过 VehicleMovementComponent 管理完整的物理模拟管线。
 */
UCLASS(Abstract, Blueprintable)
class SINGULARISMORPHVEHICLE_API ASingularisMorphVehiclePawn : public APawn
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

	ASingularisMorphVehiclePawn();

#pragma endregion

#pragma region Actor Interface

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

#pragma endregion
};
