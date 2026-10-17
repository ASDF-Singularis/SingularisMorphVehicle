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

	// 2) 遍历全部扭矩子节点（离合或变速箱），取其平均角速度作为传动系在引擎侧的转速。
	//    与 TransmitTorque 一致，跳过同类型子节点
	auto ChildVelocity = 0.0f;
	auto NumChildren = 0;
	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(TreeIndex))
	{
		const ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == GetSimType()) continue;

		if (const FTorqueSimModule* TorqueChild = ChildModule->Cast<FTorqueSimModule>())
		{
			ChildVelocity += TorqueChild->GetAngularVelocity();
			++NumChildren;
		}
	}

	if (NumChildren == 0) return;

	ChildVelocity /= static_cast<float>(NumChildren);

	// 3) 超速量决定反拖强度：未超速时保留官方输出，超速时按比例替换为负扭矩
	if (MaxEngineSpeed <= KINDA_SMALL_NUMBER) return;

	const float Overspeed = FMath::Abs(ChildVelocity) - MaxEngineSpeed;
	if (Overspeed <= 0.0f) return;

	const float BrakeScale = FMath::Clamp(Overspeed / (MaxEngineSpeed * RevLimiterOverspeedRange), 0.0f, 1.0f);
	const float RevLimiterTorque = -FMath::Max(Setup().MaxTorque, 0.0f) * BrakeScale / static_cast<float>(NumChildren);

	// 4) 向扭矩子节点均分负扭矩（与 TransmitTorque 的均分语义一致），经齿比放大后
	//    由车轮按附着极限转为制动力；离合处于半接合/分离时按 ClutchValue 缩放
	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(TreeIndex))
	{
		if (ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex))
		{
			if (ChildModule->GetSimType() == GetSimType()) continue;

			if (FTorqueSimModule* TorqueChild = ChildModule->Cast<FTorqueSimModule>())
				TorqueChild->SetDriveTorque(RevLimiterTorque);
		}
	}
}
