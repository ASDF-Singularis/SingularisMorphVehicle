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
 * 无法解析原动机时逐物理步重试；原动机最大转速无效时保留当前设置，
 * 待原动机转速区间变化后重新计算。转速区间过小时仍按实际范围写入安全边界。
 *
 * 官方模块在空挡（齿比为 0）时跳过 TransmitTorque，直接扭矩子节点会保留上一挡的
 * DriveTorque；本模块在每步模拟前清零直接扭矩子节点，并在本步未传递有效扭矩时
 * 递归清零下游残留，避免空挡持续加速与倒挡反向失控。
 *
 * 空挡时若上游直接为原动机（无离合器直连拓扑），本模块把输入轴转速同步为原动机转速：
 * 原动机的 TransmitTorque 会以直接子节点（本模块）的转速为耦合参照，输入轴若在空挡
 * 保持旧转速，会把原动机转速钳回旧值，使空挡油门无法提升转速。
 *
 * 不改变 GetSimType()：构造时追加派生标记类型后重新写入基类类型名，
 * 输出/网络数据、工厂哈希与同类型子节点判定仍以 FTransmissionSimModule 工作。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphTransmissionSimModule : public Chaos::FTransmissionSimModule
{
public:
	DEFINE_CHAOSSIMTYPENAME(FSingularisMorphTransmissionSimModule);

	explicit FSingularisMorphTransmissionSimModule(
		const Chaos::FTransmissionSettings& Settings,
		const FSingularisMorphTransmissionShiftSettings& InShiftSettings
	);

	/** 当前挡位是否无有效齿比（空挡、零齿比或换挡过渡） */
	bool IsInNeutral()
	{
		const int32 Gear = GetCurrentGear();
		return Gear == 0 || FMath::Abs(GetGearRatio(Gear)) <= SMALL_NUMBER;
	}

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

	float CachedMaxRPM = -1.0f;
	float CachedIdleRPM = -1.0f;

	/** 是否已成功按原动机转速区间写入自动换挡点 */
	bool bShiftPointsResolved = false;
};
