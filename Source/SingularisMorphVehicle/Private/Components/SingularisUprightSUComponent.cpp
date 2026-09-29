#include "Components/SingularisUprightSUComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisUprightSUComponent)

namespace
{
	/** 车速敏感曲线的等距采样段数（采样点数为段数 + 1） */
	constexpr auto SpeedSteeringCurveSamples = 20;

	/**
	 * 将编辑器曲线烘培为物理线程可用的等距采样图。
	 *
	 * 曲线按峰值归一化（Y 峰值映射为 1），横轴取曲线最后一个关键帧的时间；
	 * 空曲线不产生采样点，模拟侧据此跳过车速衰减。
	 */
	void BakeSpeedSteeringCurve(const FRuntimeFloatCurve& Curve, Chaos::FGraph& OutGraph)
	{
		const FRichCurve* RichCurve = Curve.GetRichCurveConst();
		if (!RichCurve || RichCurve->IsEmpty()) return;

		auto MinValue = 0.0f;
		auto MaxValue = 1.0f;
		RichCurve->GetValueRange(MinValue, MaxValue);
		const float NormalizeScale = FMath::IsNearlyZero(MaxValue) ? 1.0f : 1.0f / MaxValue;

		const float MaxX = RichCurve->GetLastKey().Time;
		for (auto I = 0; I <= SpeedSteeringCurveSamples; ++I)
		{
			const float X = MaxX * static_cast<float>(I) / static_cast<float>(SpeedSteeringCurveSamples);
			OutGraph.Add(Chaos::FVec2(X, RichCurve->Eval(X) * NormalizeScale));
		}
	}
}

USingularisUprightSUComponent::USingularisUprightSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;

	// 速度敏感转向曲线的默认值取自经典载具插件：车速越高可用转角越小
	FRichCurve* SteeringCurve = SteeringSetup.SpeedSteeringCurve.GetRichCurve();
	SteeringCurve->AddKey(0.0f, 1.0f);
	SteeringCurve->AddKey(32.0f, 0.8f);
	SteeringCurve->AddKey(97.0f, 0.4f);
	SteeringCurve->AddKey(193.0f, 0.3f);
}

FSingularisMorphWheelSteeringSettings USingularisUprightSUComponent::BuildSteeringSettings(
	const float InitialSteeringAngleDegrees
) const
{
	// 负的最大转角（蓝图/C++ 可写，编辑器 ClampMin 仅约束 UI）会让轮胎设置得到
	// 反向转向角，故统一钳制到非负
	const float EffectiveMaxSteeringAngle = bSteeringEnabled ? FMath::Max(0.0f, MaxSteeringAngle) : 0.0f;

	FSingularisMorphWheelSteeringSettings Settings;
	Settings.bEnabled = bSteeringEnabled && EffectiveMaxSteeringAngle > KINDA_SMALL_NUMBER;
	Settings.SteeringType = SteeringSetup.SteeringType;
	Settings.MaxSteeringAngle = EffectiveMaxSteeringAngle;
	Settings.SteeringRiseRate = SteeringSetup.SteeringRiseRate;
	Settings.SteeringFallRate = SteeringSetup.SteeringFallRate;
	Settings.AngleRatio = SteeringSetup.AngleRatio;
	Settings.WheelBase = SteeringSetup.WheelBase;
	Settings.TrackWidth = SteeringSetup.TrackWidth;
	Settings.SelfAligningTorqueGain = SteeringSetup.SelfAligningTorqueGain;
	Settings.InitialSteeringAngle = InitialSteeringAngleDegrees;

	BakeSpeedSteeringCurve(SteeringSetup.SpeedSteeringCurve, Settings.SpeedSteeringCurve);

	return Settings;
}
