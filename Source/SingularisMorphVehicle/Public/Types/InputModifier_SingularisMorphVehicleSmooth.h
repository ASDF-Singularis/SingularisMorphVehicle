#pragma once

#include <CoreMinimal.h>
#include <InputActionValue.h>
#include <InputModifiers.h>
#include <Curves/CurveFloat.h>
#include <SimModule/ModuleInput.h>

#include "InputModifier_SingularisMorphVehicleSmooth.generated.h"

/**
 * 引力奇点变型载具平滑输入修饰器。
 *
 * 将 float 型输入动作按独立可控的上升/回落速率逼近目标值，并可叠加输入曲线，
 * 对应经典载具插件的 FVehicleInputRateConfig：转向手感来自「输入渐进 + 输入曲线」，
 * 与物理线程侧的转向角速率限幅共同构成完整的转向过程。
 *
 * 本修饰器作用于游戏线程的输入采集，仅影响本地玩家手感；不参与物理线程的
 * 确定性重演，联机一致性由物理线程侧的转向动力学保证。
 */
UCLASS(BlueprintType, meta = (DisplayName = "引力奇点变型载具平滑修饰器"))
class SINGULARISMORPHVEHICLE_API UInputModifier_SingularisMorphVehicleSmooth : public UInputModifier
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 上升速率（输入单位/秒）：向目标值绝对值增大的方向逼近的速率上限 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|平滑输入修饰器|参数",
		meta = (DisplayName = "上升速率", ClampMin = "0.0", UIMin = "0.0")
	)
	float RiseRate = 2.5f;

	/** 回落速率（输入单位/秒）：绝对值减小的速率上限，通常大于上升速率以便快速回正 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|平滑输入修饰器|参数",
		meta = (DisplayName = "回落速率", ClampMin = "0.0", UIMin = "0.0")
	)
	float FallRate = 5.0f;

	/** 输入曲线：线性、平方（低速段更平缓）或自定义曲线 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|平滑输入修饰器|参数",
		meta = (DisplayName = "输入曲线")
	)
	EFunctionType InputCurveFunction = EFunctionType::SquaredFunction;

	/** 自定义曲线：横轴为输入绝对值 [0,1]，纵轴为输出绝对值 [0,1]；仅在输入曲线为自定义时生效 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|平滑输入修饰器|参数",
		meta = (DisplayName = "自定义曲线", EditCondition = "InputCurveFunction == EFunctionType::CustomCurve")
	)
	FRuntimeFloatCurve UserCurve{};

#pragma endregion

protected:
	virtual FInputActionValue ModifyRaw_Implementation(
		const UEnhancedPlayerInput* PlayerInput,
		FInputActionValue InCurrentValue,
		float DeltaTime
	) override;

private:
#pragma region Internal Function

	/** 按上升/回落速率限幅逼近目标值 */
	float InterpInputValue(float DeltaTime, float NewValue);

	/** 按配置的曲线整形输入，保持符号 */
	float CalcControlFunction(float InputValue) const;

#pragma endregion

#pragma region Internal Variable

	/**
	 * 上一次的平滑结果，修饰器的积分状态。
	 *
	 * 修饰器实例存在于输入动作/映射上下文资源上，状态由所有使用者共享；
	 * 分屏多本地玩家、或同一 IMC 被多个玩家复用时不要共用同一实例。
	 */
	float CurrentValue = 0.0f;

#pragma endregion
};
