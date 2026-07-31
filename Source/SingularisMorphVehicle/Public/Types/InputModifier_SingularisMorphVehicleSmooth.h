#pragma once

#include <CoreMinimal.h>
#include <InputActionValue.h>
#include <InputModifiers.h>

#include "InputModifier_SingularisMorphVehicleSmooth.generated.h"

/**
 * 引力奇点变型载具平滑输入修饰器。
 *
 * 将 float 型输入动作添加延迟/平滑效果，模拟传统 Chaos 载具的转向手感。
 * 通过对当前值向目标值渐进逼近实现平滑过渡。
 */
UCLASS(BlueprintType, meta = (DisplayName = "引力奇点变型载具平滑修饰器"))
class SINGULARISMORPHVEHICLE_API UInputModifier_SingularisMorphVehicleSmooth : public UInputModifier
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 追赶原始输入的速率。数值越高响应越快；越低则滞后/平滑效果越强 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|平滑输入修饰器|参数",
		meta = (DisplayName = "上升速率")
	)
	float RiseRate = 10.0f;

#pragma endregion

protected:
	virtual FInputActionValue ModifyRaw_Implementation(
		const UEnhancedPlayerInput* PlayerInput,
		FInputActionValue CurrentValue,
		float DeltaTime
	) override;

private:
#pragma region Internal Variable

	float PreviousValue = 0.0f;

#pragma endregion
};
