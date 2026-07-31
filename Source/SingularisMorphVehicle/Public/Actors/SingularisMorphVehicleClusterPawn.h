#pragma once

#include <CoreMinimal.h>
#include <GameFramework/Pawn.h>

#include "SingularisMorphVehicleClusterPawn.generated.h"

class USingularisMorphVehicleSimulationComponent;
class USingularisMorphVehicleClusterUnionComponent;

/**
 * 引力奇点变型载具集群 Pawn。
 *
 * 可用于玩家控制的变型载具 Pawn，组合集群联合与模拟组件，
 * 通过控制器输入驱动完整的物理模拟管线。
 */
UCLASS(Abstract, Blueprintable)
class SINGULARISMORPHVEHICLE_API ASingularisMorphVehicleClusterPawn : public APawn
{
	GENERATED_BODY()

public:
#pragma region Instantiation

	/** 载具集群联合组件 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly)
	TObjectPtr<USingularisMorphVehicleClusterUnionComponent> VehicleClusterUnionComponent = nullptr;

	/** 载具模拟组件 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly)
	TObjectPtr<USingularisMorphVehicleSimulationComponent> VehicleSimulationComponent = nullptr;

#pragma endregion

#pragma region Constructors

	ASingularisMorphVehicleClusterPawn();

#pragma endregion

#pragma region Actor Interface

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

#pragma endregion
};
