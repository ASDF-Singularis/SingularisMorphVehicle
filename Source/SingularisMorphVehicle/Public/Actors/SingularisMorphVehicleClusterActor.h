#pragma once

#include <CoreMinimal.h>
#include <GameFramework/Actor.h>

#include "SingularisMorphVehicleClusterActor.generated.h"

class USingularisMorphVehicleClusterUnionComponent;
class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具集群 Actor。
 *
 * 作为变型载具的物理集群根节点，承载 ClusterUnionComponent 进行群体物理模拟，
 * 并通过 VehicleMovementComponent 管理完整模拟管线。
 */
UCLASS(Abstract, Blueprintable)
class SINGULARISMORPHVEHICLE_API ASingularisMorphVehicleClusterActor : public AActor
{
	GENERATED_BODY()

public:
#pragma region Instantiation

	/** 载具集群联合组件 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly)
	TObjectPtr<USingularisMorphVehicleClusterUnionComponent> VehicleClusterUnionComponent = nullptr;

	/** 变型载具模拟组件 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly)
	TObjectPtr<USingularisMorphVehicleSimulationComponent> VehicleMovementComponent = nullptr;

#pragma endregion

#pragma region Constructors

	ASingularisMorphVehicleClusterActor();

#pragma endregion

#pragma region Actor Interface

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

#pragma endregion
};
