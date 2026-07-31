#pragma once

#include <CoreMinimal.h>
#include <GameFramework/Pawn.h>

#include "SingularisMorphVehicleSkeletalPawn.generated.h"

class USkeletalMeshComponent;
class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具骨骼 Pawn。
 *
 * 使用 SkeletalMeshComponent 作为根组件并提供动画驱动的变型载具 Pawn，
 * 适用于需要模块动画（如悬挂动画）的载具配置。
 */
UCLASS(Abstract, Blueprintable)
class SINGULARISMORPHVEHICLE_API ASingularisMorphVehicleSkeletalPawn : public APawn
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

	ASingularisMorphVehicleSkeletalPawn();

#pragma endregion

#pragma region Actor Interface

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

#pragma endregion
};
