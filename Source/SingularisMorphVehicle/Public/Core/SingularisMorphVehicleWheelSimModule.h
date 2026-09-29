#pragma once

#include <CoreMinimal.h>

#include "VehicleUtility.h"
#include "SimModule/WheelModule.h"
#include "Types/SingularisMorphVehicleType.h"

/**
 * 引力奇点变型车轮转向设置（物理线程端）。
 *
 * 转向角是被积分的状态量：目标角由控制输入、最大转角与速度敏感曲线决定，
 * 实际角以有限角速度逐步逼近目标角，构成转向的执行过程；目标角到实际角的
 * 传递速率与轮胎回正力矩共同决定转向手感。
 */
struct FSingularisMorphWheelSteeringSettings
{
	/** 是否启用转向动力学；关闭时车轮保持 ChaosVehicles 的瞬时转向行为 */
	bool bEnabled = false;

	/** 转向几何类型（仅影响内侧轮的目标角分配） */
	ESingularisMorphVehicleSteeringType SteeringType = ESingularisMorphVehicleSteeringType::SingleAngle;

	/** 本轮的机械最大转角（度），同时作为外侧轮的参考角上限 */
	float MaxSteeringAngle = 35.0f;

	/** 转向角上升速率（度/秒） */
	float SteeringRiseRate = 120.0f;

	/** 转向角回落速率（度/秒），通常大于上升速率以便更快回正 */
	float SteeringFallRate = 240.0f;

	/** 角度比例（AngleRatio 用）：内侧轮 = 参考角 / AngleRatio，取值 (0, 1] */
	float AngleRatio = 0.7f;

	/** 轴距（厘米，Ackermann 用） */
	float WheelBase = 280.0f;

	/** 轮距（厘米，Ackermann 用） */
	float TrackWidth = 160.0f;

	/**
	 * 轮胎回正效应增益（度/(牛顿·秒)），0 表示关闭。
	 *
	 * 开启后以轮胎侧向力大小为比例持续削减转向角幅值，使稳态转角小于目标角
	 * （需驾驶员持续输入转向力矩以维持转角），并在附着突变时产生转向反馈。
	 */
	float SelfAligningTorqueGain = 0.0f;

	/** 模块创建时的初始转向角（度），用于拓扑重建后延续转向状态 */
	float InitialSteeringAngle = 0.0f;

	/** 速度敏感曲线：X 为轮心相对地面的前进速度（km/h，静止地面时即车速），Y 为转向倍率；为空时不随速度衰减 */
	Chaos::FGraph SpeedSteeringCurve;
};

/**
 * 引力奇点变型车轮模拟模块。
 *
 * 在 ChaosVehicles 官方车轮模块之上补齐转向动力学：ChaosVehicles 的转向角
 * 直接由控制输入乘以最大转角得出，没有任何过程；本模块把转向角变为被积分
 * 的状态量，按上升/回落速率限幅逼近目标角，并按配置施加车速衰减、内外轮
 * 几何修正与轮胎回正力矩。
 *
 * 转向角仍经官方模块的力学路径生效（轮胎摩擦按转向后的轮坐标系解算），
 * 因此本模块不复制任何轮胎公式：计算出的实际转向角以归一化形式写回输入
 * 容器的 Steering 项，调用基类后立即还原，其余模块仍读到原始控制输入。
 *
 * 保持 ChaosVehicles 的模拟类型名（不新增 GetSimType），使扭矩传递、树构建
 * 与网络复制路径与官方车轮模块完全一致。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphWheelSimModule : public Chaos::FWheelSimModule
{
public:
	FSingularisMorphWheelSimModule(
		const Chaos::FWheelSettings& InSettings,
		const FSingularisMorphWheelSteeringSettings& InSteeringSettings
	);

	virtual void Simulate(
		float DeltaTime,
		const Chaos::FAllInputs& Inputs,
		Chaos::FSimModuleTree& VehicleModuleSystem
	) override;

	/** 当前实际转向角（度） */
	float GetCurrentSteeringAngleDegrees() const { return CurrentSteeringAngleDegrees; }

	/**
	 * 应用立轴槽位配置（建树期注入，模块入树前调用）。
	 *
	 * 轴向与反转方向由核心物理路径（速度轴交换、摩擦力轴映射、动画旋转轴）
	 * 从模块设置读取，无法按帧从父模块查询，必须在此一次性写入设置本体；
	 * 转向设置整体替换（含转向角初值，供槽位换胎后延续转向状态）。
	 */
	void ApplyUprightSlotConfig(
		const FSingularisMorphWheelSteeringSettings& InSteeringSettings,
		const Chaos::EWheelAxis InAxis,
		const bool bInReverseDirection
	);

private:
	/** 计算本帧的目标转向角（含车速衰减与内外轮几何修正） */
	float ComputeTargetSteeringAngle(const Chaos::FAllInputs& Inputs) const;

	/** 以有限角速度逼近目标角，并叠加回正力矩与机械限位 */
	void UpdateSteeringAngle(float DeltaTime, const Chaos::FAllInputs& Inputs);

	/** 车轮纵向速度（cm/s，模块局部坐标系，已按 ReverseDirection 校正） */
	float GetLongitudinalSpeed() const;

	/** 车轮横向位置符号（+1 = +Y 侧，-1 = -Y 侧，0 = 位置未知） */
	float GetLateralSideSign() const;

	FSingularisMorphWheelSteeringSettings SteeringSettings;

	/** 实际转向角（度），转向动力学的积分状态 */
	float CurrentSteeringAngleDegrees = 0.0f;

	/** 上一物理步的轮胎侧向力（牛顿），供回正力矩使用 */
	float LastLateralForce = 0.0f;
};
