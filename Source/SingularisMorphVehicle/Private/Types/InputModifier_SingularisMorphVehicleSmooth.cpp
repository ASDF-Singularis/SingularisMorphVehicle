#include "Types/InputModifier_SingularisMorphVehicleSmooth.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(InputModifier_SingularisMorphVehicleSmooth)

FInputActionValue UInputModifier_SingularisMorphVehicleSmooth::ModifyRaw_Implementation(
	const UEnhancedPlayerInput* PlayerInput,
	FInputActionValue InCurrentValue,
	const float DeltaTime
)
{
	// 仅处理标量（float）输入动作：其它类型无速率平滑语义，原样透传
	// （FInputActionValue::Get<T> 对类型不符的取值会断言失败）
	if (InCurrentValue.GetValueType() != EInputActionValueType::Axis1D)
		return InCurrentValue;

	// DeltaTime 为零（同一帧多次求值）时，下方的速率限幅自然退化为保持上次结果，无需单独分支
	const float Target = CalcControlFunction(InCurrentValue.Get<float>());

	return InterpInputValue(DeltaTime, Target);
}

float UInputModifier_SingularisMorphVehicleSmooth::InterpInputValue(const float DeltaTime, const float NewValue)
{
	const float DeltaValue = NewValue - CurrentValue;

	// 绝对值增大即为上升；从零起步且增量非零时同为上升
	const bool bRising = DeltaValue > 0.0f == CurrentValue > 0.0f ||
		(DeltaValue != 0.0f && CurrentValue == 0.0f);

	const float MaxDeltaValue = DeltaTime * (bRising ? RiseRate : FallRate);
	CurrentValue += FMath::Clamp(DeltaValue, -MaxDeltaValue, MaxDeltaValue);

	return CurrentValue;
}

float UInputModifier_SingularisMorphVehicleSmooth::CalcControlFunction(const float InputValue) const
{
	switch (InputCurveFunction)
	{
	case EFunctionType::CustomCurve:
		{
			const FRichCurve* RichCurve = UserCurve.GetRichCurveConst();
			if (RichCurve && !RichCurve->IsEmpty())
			{
				const float Output = FMath::Clamp(
					RichCurve->Eval(FMath::Abs(InputValue)),
					0.0f,
					1.0f
				);
				return InputValue < 0.0f ? -Output : Output;
			}

			return InputValue;
		}

	case EFunctionType::SquaredFunction:
		return InputValue < 0.0f ? -InputValue * InputValue : InputValue * InputValue;

	default:
	case EFunctionType::LinearFunction:
		return InputValue;
	}
}
