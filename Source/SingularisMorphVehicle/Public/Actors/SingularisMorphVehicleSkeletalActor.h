#pragma once

#include <CoreMinimal.h>
#include <GameFramework/Actor.h>

#include "SingularisMorphVehicleSkeletalActor.generated.h"

class USkeletalMeshComponent;
class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具骨骼 Actor。
 *
 * 使用 SkeletalMeshComponent 作为根组件并提供动画驱动的变型载具 Actor，
 * 适用于需要模块动画（如悬挂动画）的载具配置。
 */
UCLASS(Abstract, Blueprintable)
class SINGULARISMORPHVEHICLE_API ASingularisMorphVehicleSkeletalActor : public AActor
{
	GENERATED_BODY()

public:
#pragma region Instantiation

	/** 骨骼网格体组件 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly)
	TObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent = nullptr;

	/** 载具模拟组件 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly)
	TObjectPtr<USingularisMorphVehicleSimulationComponent> VehicleMovementComponent = nullptr;

#pragma endregion

#pragma region Constructors

	ASingularisMorphVehicleSkeletalActor();

#pragma endregion

#pragma region Actor Interface

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

#pragma endregion
};
