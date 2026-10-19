#pragma once

#include <CoreMinimal.h>

/**
 * 引力奇点变速箱自动换挡参数。
 *
 * 换挡点由原动机可用转速区间与相邻挡位齿比步长推导，不提供固定 RPM 配置：
 * 升挡点取可用转速带上部的比例，降挡点受「降挡后转速不得超过升挡点」约束。
 * 这些比例只决定同一方向内的挡位选择；方向切换由自动倒挡（仅自动变速箱）
 * 或手动升降挡脉冲处理，与 RPM 阈值无关。
 */
struct SINGULARISMORPHVEHICLE_API FSingularisMorphTransmissionShiftSettings
{
	/** 升挡转速占原动机可用转速带（怠速 → 最大转速）的比例，取值 [0.1, 1.0] */
	float ShiftUpRPMRatio = 0.9f;

	/** 降挡转速占升挡转速的比例上限，实际值还会被相邻挡位齿比步长进一步压低 */
	float ShiftDownRPMRatio = 0.55f;
};

/** 自动换挡计算结果（RPM） */
struct SINGULARISMORPHVEHICLE_API FSingularisMorphTransmissionShiftPoints
{
	/** 升挡转速（RPM） */
	float ChangeUpRPM = 0.0f;

	/** 降挡转速（RPM） */
	float ChangeDownRPM = 0.0f;

	/** 是否完成计算；false 表示原动机转速区间无效（非有限或最大转速为 0），调用方应保留现有阈值 */
	bool bAutoCalculated = false;
};

/**
 * 引力奇点变速箱换挡点计算器。
 *
 * 仅执行纯计算，不读取模拟状态；同一组输入始终得到同一组输出。
 * 升挡点按可用转速带比例推导，不依赖扭矩曲线形状，保持街机手感可预测；
 * 需要更激进或更保守的换挡时调整 ShiftUpRPMRatio / ShiftDownRPMRatio。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphTransmissionShiftCalculator
{
public:
	/**
	 * 计算自动换挡转速。
	 *
	 * @param ShiftSettings 自动换挡参数
	 * @param ForwardRatios 前进挡齿比（低挡 → 高挡）
	 * @param ReverseRatios 倒挡齿比（低挡 → 高挡）
	 * @param MaxRPM 原动机最大转速
	 * @param IdleRPM 原动机怠速转速
	 * @return 计算结果；bAutoCalculated 为 false 时输出值无效
	 */
	static FSingularisMorphTransmissionShiftPoints Calculate(
		const FSingularisMorphTransmissionShiftSettings& ShiftSettings,
		const TArray<float>& ForwardRatios,
		const TArray<float>& ReverseRatios,
		float MaxRPM,
		float IdleRPM
	);

private:
	/** 相邻挡位中的最大齿比步长（低挡齿比 / 高挡齿比）；无有效步长时返回 1 */
	static float CalculateMaxGearStep(const TArray<float>& Ratios);
};
