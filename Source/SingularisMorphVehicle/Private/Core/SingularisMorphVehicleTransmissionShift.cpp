#include "Core/SingularisMorphVehicleTransmissionShift.h"

namespace
{
	/** 可用转速带下限（RPM）；低于该值时退回安全边界（升挡=最大转速，降挡=怠速） */
	constexpr auto MinUsableRPMBand = 100.0f;

	/** 升挡转速比例下限：避免升挡点贴近怠速导致换挡点重叠 */
	constexpr auto MinShiftUpRPMRatio = 0.1f;

	/** 降挡后转速相对升挡点的安全余量（降挡后转速不高于升挡点的 90%） */
	constexpr auto DownshiftSafetyFactor = 0.9f;

	/** 升降挡点之间的最小间隔比例（相对可用转速带） */
	constexpr auto MinShiftHysteresisRatio = 0.05f;
}

FSingularisMorphTransmissionShiftPoints FSingularisMorphTransmissionShiftCalculator::Calculate(
	const FSingularisMorphTransmissionShiftSettings& ShiftSettings,
	const TArray<float>& ForwardRatios,
	const TArray<float>& ReverseRatios,
	const float MaxRPM,
	const float IdleRPM
)
{
	FSingularisMorphTransmissionShiftPoints Points;

	// 1) 原动机转速区间不可信时返回失败，由调用方保留现有阈值
	if (!FMath::IsFinite(MaxRPM) || !FMath::IsFinite(IdleRPM))
		return Points;

	// 2) 归一化转速区间；怠速被钳制在最大转速内
	const float ClampedMaxRPM = FMath::Max(MaxRPM, 0.0f);
	const float ClampedIdleRPM = FMath::Clamp(IdleRPM, 0.0f, ClampedMaxRPM);
	const float UsableBand = ClampedMaxRPM - ClampedIdleRPM;

	// 区间不足以划分有意义的手感时，仍按实际转速范围给出安全边界，
	// 避免沿用高于原动机最大转速的固定阈值而导致永不换挡
	if (UsableBand <= MinUsableRPMBand)
	{
		Points.ChangeUpRPM = ClampedMaxRPM;
		Points.ChangeDownRPM = ClampedIdleRPM;
		Points.bAutoCalculated = ClampedMaxRPM > KINDA_SMALL_NUMBER;
		return Points;
	}

	// 3) 升挡点位于可用转速带上部
	const float UpRatio = FMath::Clamp(
		FMath::IsFinite(ShiftSettings.ShiftUpRPMRatio) ? ShiftSettings.ShiftUpRPMRatio : 1.0f,
		MinShiftUpRPMRatio,
		1.0f
	);
	const float UpRPM = FMath::Lerp(ClampedIdleRPM, ClampedMaxRPM, UpRatio);

	// 4) 降挡点必须保证降挡后的转速仍低于升挡点：
	//    降挡后转速 = 降挡点 × (低挡齿比 / 高挡齿比)，故按前进/倒挡中的最大齿比步长
	//    压低降挡比例，再与配置的比例上限取较小值
	const float MaxGearStep = FMath::Max(
		CalculateMaxGearStep(ForwardRatios),
		CalculateMaxGearStep(ReverseRatios)
	);
	const float RatioBound = MaxGearStep > 1.0f ? DownshiftSafetyFactor / MaxGearStep : 1.0f;
	const float RequestedDownRatio =
		FMath::IsFinite(ShiftSettings.ShiftDownRPMRatio) ? ShiftSettings.ShiftDownRPMRatio : 1.0f;
	const float DownRatio = FMath::Clamp(FMath::Min(RequestedDownRatio, RatioBound), 0.0f, 1.0f);

	// 5) 保证升降挡点之间存在可辨识的迟滞，且降挡点不低于怠速
	const float MinHysteresis = FMath::Max(UsableBand * MinShiftHysteresisRatio, 1.0f);
	const float DownRPMUpperBound = FMath::Max(UpRPM - MinHysteresis, ClampedIdleRPM);

	Points.ChangeUpRPM = UpRPM;
	Points.ChangeDownRPM = FMath::Clamp(UpRPM * DownRatio, ClampedIdleRPM, DownRPMUpperBound);
	Points.bAutoCalculated = true;

	return Points;
}

float FSingularisMorphTransmissionShiftCalculator::CalculateMaxGearStep(const TArray<float>& Ratios)
{
	auto MaxStep = 1.0f;
	auto PreviousMagnitude = 0.0f;

	for (const float Ratio : Ratios)
	{
		const float Magnitude = FMath::Abs(Ratio);
		if (!FMath::IsFinite(Magnitude) || Magnitude <= KINDA_SMALL_NUMBER)
			continue;

		// 仅在「当前挡位高于上一挡位」时累计步长；乱序或递增的配置视为无效步长
		if (PreviousMagnitude > KINDA_SMALL_NUMBER && Magnitude < PreviousMagnitude)
			MaxStep = FMath::Max(MaxStep, PreviousMagnitude / Magnitude);

		PreviousMagnitude = Magnitude;
	}

	return MaxStep;
}
