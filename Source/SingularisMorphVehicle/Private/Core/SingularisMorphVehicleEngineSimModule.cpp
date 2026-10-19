#include <SimModule/SimModuleTree.h>

#include "Core/SingularisMorphVehicleTorqueSimModules.h"
#include "Core/SingularisMorphVehicleTransmissionSimModule.h"

namespace
{
	/**
	 * 最大转速前的软限速带宽（占最大转速比例）。
	 *
	 * 带宽过窄会使满油门时的扭矩在最大转速附近快速通断，形成可感知的
	 * 前后顿挫；此处同时用于收油与超速制动，保留足够宽的连续过渡区。
	 */
	constexpr auto EngineSimRevLimiterRange = 0.1f;

	/** 动力链查找深度上限，避免异常树结构造成无限递归 */
	constexpr auto EngineSimMaxDrivelineSearchDepth = 32;

	/** 角速度积分的最小转动惯量（kg·cm²），避免除零 */
	constexpr auto EngineSimMinInertia = 1.0f;
}

void FSingularisMorphEngineSimModule::Simulate(
	const float DeltaTime,
	const Chaos::FAllInputs& Inputs,
	Chaos::FSimModuleTree& VehicleModuleSystem
)
{
	if (!EngineStarted || !FMath::IsFinite(MaxEngineSpeed) || MaxEngineSpeed <= KINDA_SMALL_NUMBER)
	{
		DriveTorque = 0.0f;
		LoadTorque = 0.0f;
		BrakingTorque = 0.0f;
		return;
	}

	const float Dt = FMath::IsFinite(DeltaTime) ? FMath::Max(DeltaTime, 0.0f) : 0.0f;

	// 1) 油门与转速范围钳制：负转速按 0 取扭矩曲线，输入越界或非有限值时按无油门处理
	const float RawThrottle = Inputs.GetControls().GetMagnitude(Chaos::ThrottleControlName);
	const float Throttle = FMath::IsFinite(RawThrottle) ? FMath::Clamp(RawThrottle, 0.0f, 1.0f) : 0.0f;
	const float RawBrake = Inputs.GetControls().GetMagnitude(Chaos::BrakeControlName);
	const float Brake = FMath::IsFinite(RawBrake) ? FMath::Clamp(RawBrake, 0.0f, 1.0f) : 0.0f;
	const float RawHandbrake = Inputs.GetControls().GetMagnitude(Chaos::HandbrakeControlName);
	const float Handbrake = FMath::IsFinite(RawHandbrake)
		                        ? FMath::Clamp(RawHandbrake, 0.0f, 1.0f)
		                        : 0.0f;

	// 制动优先于怠速蠕行：踏板或手刹有效时不再主动驱动车辆，避免制动被蠕行扭矩抵消
	const bool bBrakeApplied = Brake > KINDA_SMALL_NUMBER || Handbrake > KINDA_SMALL_NUMBER;

	const float MaxRPM = FMath::Max(static_cast<float>(Setup().MaxRPM), 1.0f);
	const float IdleRPM = FMath::Clamp(static_cast<float>(Setup().IdleRPM), 0.0f, MaxRPM);
	const float EffectiveIdleSpeed = FMath::Clamp(EngineIdleSpeed, 0.0f, MaxEngineSpeed);
	const float MaxTorque = FMath::IsFinite(Setup().MaxTorque)
		                        ? FMath::Max(Setup().MaxTorque, 0.0f)
		                        : 0.0f;

	// 角速度可能被异常子节点或外部状态污染，先恢复为有限值再读取转速
	if (!FMath::IsFinite(AngularVelocity))
		AngularVelocity = EffectiveIdleSpeed;

	const bool bDrivelineConnected = IsDrivelineConnected(VehicleModuleSystem);

	// 传动系输入轴转速每步只读取一次：怠速蠕行、接合转速约束与限速共用
	const float DrivelineOmega = bDrivelineConnected
		                             ? GetDrivelineAngularVelocity(VehicleModuleSystem)
		                             : 0.0f;

	const float CurrentRPM = GetRPM();
	const float CurveRPM = FMath::Clamp(CurrentRPM, 0.0f, MaxRPM);

	// 限速判断在接合时使用传动系输入轴转速，避免引擎自身状态被钳制后
	// 看不到车轮侧的真实超速；空挡时使用原动机自由转速。
	const float LimiterRPM = bDrivelineConnected
		                         ? Chaos::OmegaToRPM(FMath::Abs(DrivelineOmega))
		                         : FMath::Abs(CurrentRPM);
	const float LimiterBandRPM = FMath::Max(MaxRPM * EngineSimRevLimiterRange, 1.0f);

	// 2) 最大转速前线性收油，避免硬断油与引擎制动在高转速处交替
	const float LimiterScale = FMath::Clamp(
		(MaxRPM - LimiterRPM) / LimiterBandRPM,
		0.0f,
		1.0f
	);
	const float OverspeedBrakeScale = FMath::Clamp(
		(LimiterRPM - MaxRPM) / LimiterBandRPM,
		0.0f,
		1.0f
	);
	// 制动方向必须与传动系实际转向相反：反向滑行时不能用固定负扭矩继续推动
	const float LimiterDirection = FMath::Sign(
		bDrivelineConnected ? DrivelineOmega : CurrentRPM
	);
	const float OverspeedBrakeTorque = MaxTorque * OverspeedBrakeScale;
	// FEngineSimModule::GetTorqueFromRPM 虽在头文件声明为 inline，但定义位于
	// ChaosVehicles 的 cpp 且未导出，跨模块调用会链接失败。此处按相同语义直接求值：
	// 转速钳制到 [怠速, 最大转速]，最大转速处断油。
	const float CurveInputRPM = FMath::Clamp(CurveRPM, IdleRPM, MaxRPM);
	const float TorqueFromCurve = CurveRPM < MaxRPM
		                              ? Setup().TorqueCurve.GetValue(
			                              CurveInputRPM,
			                              MaxRPM,
			                              MaxTorque
		                              )
		                              : 0.0f;
	const float RawThrottleTorque = Throttle * TorqueFromCurve;
	const float ThrottleTorque = FMath::IsFinite(RawThrottleTorque)
		                             ? FMath::Max(RawThrottleTorque, 0.0f) * LimiterScale
		                             : 0.0f;

	// 3) 引擎制动随转速从怠速向最大转速线性增强；松油门时作为负驱动力矩下推，
	//    挂挡时由传动系承受，空挡时参与引擎自身角速度积分
	const float BrakeScale = FMath::Clamp(
		(CurrentRPM - IdleRPM) / FMath::Max(MaxRPM - IdleRPM, 1.0f),
		0.0f,
		1.0f
	);
	const float MaxBrakeTorque = FMath::IsFinite(Setup().EngineBrakeEffect)
		                             ? FMath::Max(Setup().EngineBrakeEffect, 0.0f)
		                             : 0.0f;
	const float EngineBrakeTorque = Throttle > KINDA_SMALL_NUMBER
		                                ? 0.0f
		                                : MaxBrakeTorque * BrakeScale;

	// 4) 怠速蠕行配置
	const bool bIdleCreepConfigured = IdleCreepTorqueRatio > 0.0f
		&& IdleRPM > KINDA_SMALL_NUMBER;

	// 4a) 怠速蠕行：挂挡且未踩制动/油门时，只要输入轴转速低于怠速转速就输出
	//     成比例的驱动扭矩；达到怠速转速后扭矩归零。扭矩随油门线性淡出，
	//     使驾驶员踩下油门时由动力请求平滑接管，不产生叠加突变。
	//     目标转速以引擎侧输入轴角速度表达，因此自动适配前进挡与倒挡齿比。
	const bool bIdleCreepEnabled = bDrivelineConnected && bIdleCreepConfigured && !bBrakeApplied;
	const float CreepTargetOmega = bIdleCreepEnabled
		                               ? Chaos::RPMToOmega(IdleRPM)
		                               : 0.0f;
	const float CreepSpeedScale = CreepTargetOmega > KINDA_SMALL_NUMBER
		                              ? FMath::Clamp(
			                              (CreepTargetOmega - DrivelineOmega) / CreepTargetOmega,
			                              0.0f,
			                              1.0f
		                              )
		                              : 0.0f;
	const float IdleCreepTorque = bIdleCreepEnabled
		                              ? MaxTorque * IdleCreepTorqueRatio * CreepSpeedScale * (1.0f - Throttle)
		                              : 0.0f;

	// 接合时引擎制动同样按传动系实际转向施加，反向滑行时不会助推
	const float EngineBrakeDirection = bDrivelineConnected ? LimiterDirection : 1.0f;
	const float NetTorque = ThrottleTorque + IdleCreepTorque
		- EngineBrakeDirection * EngineBrakeTorque
		- LimiterDirection * OverspeedBrakeTorque;

	// 5) 接合时传动系决定引擎转速，引擎可被离合拖到怠速以下；
	//    空挡/分离时净扭矩参与引擎自积分，使松油门后转速能够回落到怠速。
	DriveTorque = bDrivelineConnected ? 0.0f : NetTorque;
	LoadTorque = 0.0f;

	TransmitTorque(VehicleModuleSystem, NetTorque, 0.0f);
	if (bDrivelineConnected)
		LoadTorque = 0.0f;

	// 引擎制动已由负驱动力矩表达；车轮制动扭矩不应直接作用于引擎自转速，
	// 否则踩刹车时引擎转速会随制动扭矩下陷
	BrakingTorque = 0.0f;

	// 接合时引擎转速直接取传动系输入轴转速：离合/传动系的求解顺序或滑差
	// 不应让输出转速停在低于实际车速的水平；倒挡时为负值，取绝对值
	if (bDrivelineConnected)
	{
		if (FMath::IsFinite(DrivelineOmega))
			AngularVelocity = FMath::Abs(DrivelineOmega);
	}
	else if (FMath::IsFinite(AngularVelocity))
		AngularVelocity = FMath::Abs(AngularVelocity);

	const float PreIntegrationVelocity = AngularVelocity;

	const float Inertia = FMath::IsFinite(Setup().EngineInertia)
		                      ? FMath::Max(Setup().EngineInertia, EngineSimMinInertia)
		                      : EngineSimMinInertia;
	IntegrateAngularVelocity(Dt, Inertia);

	if (!bDrivelineConnected)
	{
		// 自由转速受最大变化率约束：低惯量配置下扭矩积分可能在数帧内把转速推到红区
		const float VelocityDelta = AngularVelocity - PreIntegrationVelocity;
		if (VelocityDelta > 0.0f && RevUpRateOmega > 0.0f)
		{
			AngularVelocity = PreIntegrationVelocity
				+ FMath::Min(VelocityDelta, RevUpRateOmega * Dt);
		}
		else if (VelocityDelta < 0.0f && RevDownRateOmega > 0.0f)
		{
			AngularVelocity = PreIntegrationVelocity
				+ FMath::Max(VelocityDelta, -RevDownRateOmega * Dt);
		}
	}

	// 输出数据使用净扭矩，负值表示发动机制动
	DriveTorque = NetTorque;

	if (!FMath::IsFinite(AngularVelocity))
		AngularVelocity = bDrivelineConnected ? 0.0f : EffectiveIdleSpeed;

	// 引擎不得反转；接合时允许低于怠速，分离时由怠速维持自由怠速
	AngularVelocity = FMath::Clamp(AngularVelocity, 0.0f, MaxEngineSpeed);
	if (!bDrivelineConnected && AngularVelocity < EffectiveIdleSpeed)
		AngularVelocity = EffectiveIdleSpeed;
}

bool FSingularisMorphEngineSimModule::IsDrivelineConnected(
	Chaos::FSimModuleTree& VehicleModuleSystem
) const
{
	const int32 TreeIndex = GetTreeIndex();
	if (TreeIndex < 0 || !VehicleModuleSystem.IsValidNode(TreeIndex))
		return true;

	return HasLoadedDriveline(VehicleModuleSystem, TreeIndex, 0);
}

float FSingularisMorphEngineSimModule::GetDrivelineAngularVelocity(
	const Chaos::FSimModuleTree& VehicleModuleSystem
) const
{
	const int32 TreeIndex = GetTreeIndex();
	if (TreeIndex < 0 || !VehicleModuleSystem.IsValidNode(TreeIndex))
		return 0.0f;

	// 变速箱输入轴直接反映车速对应的原动机侧转速；齿比符号使前进/倒挡
	// 在各自挡位方向上均为正值，反向滑行时为负值（蠕行扭矩应保持全额）
	auto InputShaftVelocity = 0.0f;
	if (FindDrivelineInputVelocity(VehicleModuleSystem, TreeIndex, 0, InputShaftVelocity))
		return InputShaftVelocity;

	// 无变速箱（原动机直连轮轴）时退化为直接扭矩子节点的平均角速度
	const FName SelfType = GetSimType();
	auto SumVelocity = 0.0f;
	auto NumChildren = 0;

	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(TreeIndex))
	{
		if (ChildIndex < 0 || !VehicleModuleSystem.IsValidNode(ChildIndex))
			continue;

		const ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == SelfType)
			continue;

		const auto* TorqueChild = ChildModule->Cast<FTorqueSimModule>();
		if (!TorqueChild)
			continue;

		const float ChildVelocity = TorqueChild->GetAngularVelocity();
		if (!FMath::IsFinite(ChildVelocity))
			continue;

		SumVelocity += ChildVelocity;
		++NumChildren;
	}

	return NumChildren > 0 ? SumVelocity / static_cast<float>(NumChildren) : 0.0f;
}

bool FSingularisMorphEngineSimModule::FindDrivelineInputVelocity(
	const Chaos::FSimModuleTree& VehicleModuleSystem,
	const int32 ParentIndex,
	const int32 Depth,
	float& OutVelocity
) const
{
	if (Depth >= EngineSimMaxDrivelineSearchDepth || ParentIndex < 0
		|| !VehicleModuleSystem.IsValidNode(ParentIndex))
		return false;

	const ISimulationModuleBase* ParentModule = VehicleModuleSystem.GetSimModule(ParentIndex);
	if (!ParentModule)
		return false;

	const FName ParentType = ParentModule->GetSimType();

	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(ParentIndex))
	{
		if (ChildIndex < 0 || !VehicleModuleSystem.IsValidNode(ChildIndex))
			continue;

		ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == ParentType)
			continue;

		if (!ChildModule->IsBehaviourType(Chaos::TorqueBased))
			continue;

		const FTorqueSimModule* TorqueChild = ChildModule->Cast<FTorqueSimModule>();
		if (!TorqueChild)
			continue;

		if (const auto* Clutch = ChildModule->Cast<FSingularisMorphClutchSimModule>())
		{
			if (!Clutch->IsLocked())
				continue;
		}

		if (ChildModule->IsSimType<Chaos::FTransmissionSimModule>())
		{
			// 同一路径上的空挡/换挡过渡不代表车速，继续查找其它支路
			if (auto* PluginTransmission =
				ChildModule->Cast<FSingularisMorphTransmissionSimModule>())
			{
				if (PluginTransmission->IsInNeutral())
					continue;
			}

			const float InputVelocity = TorqueChild->GetAngularVelocity();
			if (FMath::IsFinite(InputVelocity))
			{
				OutVelocity = InputVelocity;
				return true;
			}
			continue;
		}

		if (FindDrivelineInputVelocity(VehicleModuleSystem, ChildIndex, Depth + 1, OutVelocity))
			return true;
	}

	return false;
}

bool FSingularisMorphEngineSimModule::HasLoadedDriveline(
	Chaos::FSimModuleTree& VehicleModuleSystem,
	const int32 ParentIndex,
	const int32 Depth
) const
{
	if (Depth >= EngineSimMaxDrivelineSearchDepth || ParentIndex < 0
		|| !VehicleModuleSystem.IsValidNode(ParentIndex))
		return false;

	const ISimulationModuleBase* ParentModule = VehicleModuleSystem.AccessSimModule(ParentIndex);
	if (!ParentModule)
		return false;

	const FName ParentType = ParentModule->GetSimType();

	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(ParentIndex))
	{
		if (ChildIndex < 0 || !VehicleModuleSystem.IsValidNode(ChildIndex))
			continue;

		ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == ParentType)
			continue;

		// 仅扭矩链参与动力传递；悬挂等力模块不构成传动路径
		if (!ChildModule->IsBehaviourType(Chaos::TorqueBased))
			continue;

		// 车轮是动力链末端，存在即可驱动车轮
		if (ChildModule->Cast<Chaos::FWheelBaseInterface>())
			return true;

		// 离合器分离时该路径断开；非本插件离合器无法查询接合量，按接合处理
		if (const auto* Clutch = ChildModule->Cast<FSingularisMorphClutchSimModule>())
		{
			if (!Clutch->IsLocked())
				continue;
		}

		// 空挡或换挡过渡（当前齿比为零）时路径断开；非本插件变速箱无法查询挡位，按在挡处理
		if (auto* PluginTransmission =
			ChildModule->Cast<FSingularisMorphTransmissionSimModule>())
		{
			if (PluginTransmission->IsInNeutral())
				continue;
		}

		if (HasLoadedDriveline(VehicleModuleSystem, ChildIndex, Depth + 1))
			return true;
	}

	return false;
}
