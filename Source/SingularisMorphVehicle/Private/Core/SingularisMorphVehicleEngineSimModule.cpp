#include <SimModule/SimModuleTree.h>

#include "Core/SingularisMorphVehicleTorqueSimModules.h"
#include "Core/SingularisMorphVehicleTransmissionSimModule.h"

namespace
{
	/** 最大转速前用于线性收油的转速比例 */
	constexpr auto EngineSimRevLimiterRange = 0.05f;

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

	const float MaxRPM = FMath::Max(static_cast<float>(Setup().MaxRPM), 1.0f);
	const float IdleRPM = FMath::Clamp(static_cast<float>(Setup().IdleRPM), 0.0f, MaxRPM);
	const float EffectiveIdleSpeed = FMath::Clamp(EngineIdleSpeed, 0.0f, MaxEngineSpeed);

	// 角速度可能被异常子节点或外部状态污染，先恢复为有限值再读取转速
	if (!FMath::IsFinite(AngularVelocity))
		AngularVelocity = EffectiveIdleSpeed;

	const float CurrentRPM = GetRPM();
	const float CurveRPM = FMath::Clamp(CurrentRPM, 0.0f, MaxRPM);

	// 2) 最大转速附近线性收油，避免硬断油与引擎制动在高转速处交替
	const float LimiterScale = FMath::Clamp(
		(MaxRPM - CurveRPM) / FMath::Max(MaxRPM * EngineSimRevLimiterRange, 1.0f),
		0.0f,
		1.0f
	);
	// FEngineSimModule::GetTorqueFromRPM 虽在头文件声明为 inline，但定义位于
	// ChaosVehicles 的 cpp 且未导出，跨模块调用会链接失败。此处按相同语义直接求值：
	// 转速钳制到 [怠速, 最大转速]，最大转速处断油。
	const float MaxTorque = FMath::IsFinite(Setup().MaxTorque)
		                        ? FMath::Max(Setup().MaxTorque, 0.0f)
		                        : 0.0f;
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

	const float NetTorque = ThrottleTorque - EngineBrakeTorque;
	const bool bDrivelineConnected = IsDrivelineConnected(VehicleModuleSystem);

	// 4) 接合时传动系决定引擎转速，引擎自身不再积分油门/制动扭矩，避免转速耦合
	//    与扭矩积分叠加出虚假的转速超前；空挡/分离时净扭矩参与引擎自积分，
	//    使松油门后转速能够回落到怠速
	DriveTorque = bDrivelineConnected ? 0.0f : NetTorque;
	LoadTorque = 0.0f;

	TransmitTorque(VehicleModuleSystem, NetTorque, 0.0f);

	// 引擎制动已由负驱动力矩表达；车轮制动扭矩不应直接作用于引擎自转速，
	// 否则踩刹车时引擎转速会随制动扭矩下陷
	BrakingTorque = 0.0f;

	// 倒挡时子节点角速度为负，引擎实际仍正转；取绝对值避免怠速钳制把转速固定住
	if (FMath::IsFinite(AngularVelocity))
		AngularVelocity = FMath::Abs(AngularVelocity);

	const float PreIntegrationVelocity = AngularVelocity;

	const float Inertia = FMath::IsFinite(Setup().EngineInertia)
		                      ? FMath::Max(Setup().EngineInertia, EngineSimMinInertia)
		                      : EngineSimMinInertia;
	IntegrateAngularVelocity(Dt, Inertia);

	// 自由转速受最大变化率约束：低惯量配置下扭矩积分可能在数帧内把转速推到红区。
	// 该限制只作用于空挡/离合分离；接合时转速由传动系决定
	if (!bDrivelineConnected)
	{
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
		AngularVelocity = EffectiveIdleSpeed;

	AngularVelocity = FMath::Clamp(AngularVelocity, -MaxEngineSpeed, MaxEngineSpeed);
	if (AngularVelocity < EffectiveIdleSpeed)
		AngularVelocity = EffectiveIdleSpeed;

	// 5) 传动系超速时向扭矩子节点施加与其实际转向相反的反拖制动。
	//    比例控制避免在最大转速附近硬通断造成抖动，同时超速越多制动越强
	if (MaxTorque <= 0.0f)
		return;

	const int32 TreeIndex = GetTreeIndex();
	if (TreeIndex < 0 || !VehicleModuleSystem.IsValidNode(TreeIndex))
		return;

	auto NumTorqueChildren = 0;
	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(TreeIndex))
	{
		if (ChildIndex < 0 || !VehicleModuleSystem.IsValidNode(ChildIndex))
			continue;

		const ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == GetSimType())
			continue;

		if (ChildModule->Cast<FTorqueSimModule>())
			++NumTorqueChildren;
	}

	if (NumTorqueChildren == 0)
		return;

	const float TorquePerChild = MaxTorque / static_cast<float>(NumTorqueChildren);

	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(TreeIndex))
	{
		if (ChildIndex < 0 || !VehicleModuleSystem.IsValidNode(ChildIndex))
			continue;

		ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == GetSimType())
			continue;

		FTorqueSimModule* TorqueChild = ChildModule->Cast<FTorqueSimModule>();
		if (!TorqueChild)
			continue;

		const float ChildVelocity = TorqueChild->GetAngularVelocity();
		if (!FMath::IsFinite(ChildVelocity))
			continue;

		const float Overspeed = FMath::Abs(ChildVelocity) - MaxEngineSpeed;
		if (Overspeed <= 0.0f)
			continue;

		const float LimiterBrakeScale = FMath::Clamp(
			Overspeed / FMath::Max(MaxEngineSpeed * EngineSimRevLimiterRange, 1.0f),
			0.0f,
			1.0f
		);

		// Overspeed > 0 保证角速度非零，Sign 不会退化
		TorqueChild->SetDriveTorque(
			-FMath::Sign(ChildVelocity) * TorquePerChild * LimiterBrakeScale
		);
	}
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

		// 空挡时该路径断开；非本插件变速箱无法查询挡位，按在挡处理
		if (auto* Transmission = ChildModule->Cast<FSingularisMorphTransmissionSimModule>())
		{
			if (Transmission->IsInNeutral())
				continue;
		}

		if (HasLoadedDriveline(VehicleModuleSystem, ChildIndex, Depth + 1))
			return true;
	}

	return false;
}
