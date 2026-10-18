#pragma once

#include <CoreMinimal.h>
#include <SimModule/TransmissionModule.h>

#include "Core/SingularisMorphVehicleTransmissionShift.h"

/**
 * 引力奇点变速箱模拟模块。
 *
 * 在官方自动换挡状态机之上，按动力链上游原动机的实际转速区间动态校准换挡点。
 * 固定 RPM 阈值只适配单一原动机，换装不同最大转速的引擎后会出现升挡过早/过晚、
 * 降挡后立即升挡等抖动；本模块在模拟前从模拟树向上解析原动机设置，
 * 以最大/怠速转速与相邻挡位齿比步长重算阈值，再交由官方状态机执行换挡。
 *
 * 无法解析原动机或转速区间不足时保留组件传入的手动阈值。
 *
 * 官方模块在空挡（齿比为 0）时跳过 TransmitTorque，直接扭矩子节点会保留上一挡的
 * DriveTorque；本模块在每步模拟前清零直接扭矩子节点，并在本步未传递有效扭矩时
 * 递归清零下游残留，避免空挡持续加速与倒挡反向失控。
 *
 * 不声明新的模拟类型名：输出/网络数据与工厂均以 FTransmissionSimModule 类型识别该模块。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphTransmissionSimModule : public Chaos::FTransmissionSimModule
{
public:
	explicit FSingularisMorphTransmissionSimModule(
		const Chaos::FTransmissionSettings& Settings,
		const FSingularisMorphTransmissionShiftSettings& InShiftSettings
	);

	virtual void Simulate(
		float DeltaTime,
		const Chaos::FAllInputs& Inputs,
		Chaos::FSimModuleTree& VehicleModuleSystem
	) override;

	virtual bool GetDebugString(FString& StringOut) const override;

private:
	/** 从模拟树向上查找原动机并刷新换挡点；转速区间未变化时复用缓存 */
	void UpdateShiftPoints(const Chaos::FSimModuleTree& VehicleModuleSystem);

	/** 沿父链查找原动机（引擎或电机） */
	const ISimulationModuleBase* FindPrimeMover(
		const Chaos::FSimModuleTree& VehicleModuleSystem
	) const;

	/** 递归清除下游残留驱动力矩（空挡/零扭矩时调用） */
	void ClearDownstreamDriveTorque(
		const Chaos::FSimModuleTree& VehicleModuleSystem,
		int32 ParentIndex,
		int32 Depth
	);

	FSingularisMorphTransmissionShiftSettings ShiftSettings;

	/** 组件传入的手动阈值；自动计算不可用时回退到该值，不受已写入的自动值影响 */
	float ManualChangeUpRPM = 0.0f;
	float ManualChangeDownRPM = 0.0f;

	float CachedMaxRPM = -1.0f;
	float CachedIdleRPM = -1.0f;

	/** 是否已成功解析原动机并写入自动换挡点 */
	bool bShiftPointsResolved = false;
};
