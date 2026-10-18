#include "Core/SingularisMorphVehicleTorqueSimModules.h"
#include "SimModule/SimModuleTree.h"

namespace
{
	/** 超速达到最大转速的该比例时施加满额反拖扭矩；越小限速越硬 */
	constexpr auto RevLimiterOverspeedRange = 0.05f;
}

void FSingularisMorphEngineSimModule::Simulate(
	float DeltaTime,
	const Chaos::FAllInputs& Inputs,
	Chaos::FSimModuleTree& VehicleModuleSystem
)
{
	// 1) 官方引擎行为：油门扭矩、传动链下推、角速度积分与 MaxRPM 钳制
	FEngineSimModule::Simulate(DeltaTime, Inputs, VehicleModuleSystem);

	if (!EngineStarted) return;

	const int32 TreeIndex = GetTreeIndex();
	if (!VehicleModuleSystem.IsValidNode(TreeIndex)) return;
	if (MaxEngineSpeed <= KINDA_SMALL_NUMBER) return;

	// 2) 统计扭矩子节点（离合或变速箱）：与 TransmitTorque 一致，跳过同类型子节点。
	//    制动力矩按子节点数均分，避免多子节点拓扑下总制动力随子节点数放大
	auto NumTorqueChildren = 0;
	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(TreeIndex))
	{
		const ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == GetSimType()) continue;

		if (ChildModule->Cast<FTorqueSimModule>())
			++NumTorqueChildren;
	}

	if (NumTorqueChildren == 0) return;

	// 3) 逐个扭矩子节点计算超速量，并按子节点实际转向施加反拖扭矩。
	//    a) 子节点转向与引擎正转方向相反（倒挡行驶、前进挡溜车或传动系被反向拖动）时，
	//       固定负扭矩会顺着子节点转向继续加速传动系，形成正反馈并把车速推到失控，
	//       因此力矩方向必须取子节点角速度的反向
	//    b) 未超速的子节点保留官方的油门/引擎制动输出
	//    c) 传动系超速时经齿比放大后由车轮按附着极限转为制动力；
	//       离合处于半接合/分离时按 ClutchValue 缩放
	const float MaxTorque = FMath::IsFinite(Setup().MaxTorque)
		                        ? FMath::Max(Setup().MaxTorque, 0.0f)
		                        : 0.0f;
	if (MaxTorque <= 0.0f) return;

	const float TorquePerChild = MaxTorque / static_cast<float>(NumTorqueChildren);

	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(TreeIndex))
	{
		ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == GetSimType()) continue;

		FTorqueSimModule* TorqueChild = ChildModule->Cast<FTorqueSimModule>();
		if (!TorqueChild) continue;

		const float ChildVelocity = TorqueChild->GetAngularVelocity();
		if (!FMath::IsFinite(ChildVelocity)) continue;

		const float Overspeed = FMath::Abs(ChildVelocity) - MaxEngineSpeed;
		if (Overspeed <= 0.0f) continue;

		const float BrakeScale = FMath::Clamp(
			Overspeed / (MaxEngineSpeed * RevLimiterOverspeedRange),
			0.0f,
			1.0f
		);

		// Overspeed > 0 保证角速度非零，Sign 不会退化
		TorqueChild->SetDriveTorque(-FMath::Sign(ChildVelocity) * TorquePerChild * BrakeScale);
	}
}
