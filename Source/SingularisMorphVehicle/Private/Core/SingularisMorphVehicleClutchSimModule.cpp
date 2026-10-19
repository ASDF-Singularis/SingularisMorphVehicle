#include <SimModule/SimModuleTree.h>

#include "Core/SingularisMorphVehicleTorqueSimModules.h"
#include "Core/SingularisMorphVehicleTransmissionSimModule.h"

void FSingularisMorphClutchSimModule::Simulate(
	const float /*DeltaTime*/,
	const Chaos::FAllInputs& Inputs,
	Chaos::FSimModuleTree& VehicleModuleSystem
)
{
	// 1) 接合量：踏板 0 为完全接合，1 为完全分离
	const float RawPedal = Inputs.GetControls().GetMagnitude(Chaos::ClutchControlName);
	const float Pedal = FMath::IsFinite(RawPedal) ? FMath::Clamp(RawPedal, 0.0f, 1.0f) : 0.0f;
	const float Strength = FMath::IsFinite(Setup().ClutchStrength)
		                       ? FMath::Clamp(Setup().ClutchStrength, 0.0f, 1.0f)
		                       : 1.0f;
	Engagement = FMath::Clamp((1.0f - Pedal) * Strength, 0.0f, 1.0f);

	const int32 SelfIndex = GetTreeIndex();
	if (SelfIndex < 0 || !VehicleModuleSystem.IsValidNode(SelfIndex))
	{
		Engagement = 0.0f;
		DriveTorque = 0.0f;
		return;
	}

	// 2) 上游为原动机侧，下游为变速箱/轮轴/车轮
	FTorqueSimModule* Parent = nullptr;
	const int32 ParentIndex = VehicleModuleSystem.GetParent(SelfIndex);
	if (ParentIndex >= 0 && VehicleModuleSystem.IsValidNode(ParentIndex))
	{
		if (ISimulationModuleBase* ParentModule = VehicleModuleSystem.AccessSimModule(ParentIndex))
			Parent = ParentModule->Cast<FTorqueSimModule>();
	}

	TArray<FTorqueSimModule*, TInlineAllocator<4>> Children;
	auto bHasKnownTransmission = false;
	auto bAnyTransmissionInGear = false;

	for (const int32 ChildIndex : VehicleModuleSystem.GetChildren(SelfIndex))
	{
		if (ChildIndex < 0 || !VehicleModuleSystem.IsValidNode(ChildIndex))
			continue;

		ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == GetSimType())
			continue;

		FTorqueSimModule* TorqueChild = ChildModule->Cast<FTorqueSimModule>();
		if (!TorqueChild)
			continue;

		Children.Add(TorqueChild);

		// 下游为空挡时输入轴无负载，需要随引擎空转；非本插件变速箱按在挡处理
		if (auto* Transmission = ChildModule->Cast<FSingularisMorphTransmissionSimModule>())
		{
			bHasKnownTransmission = true;
			if (!Transmission->IsInNeutral())
				bAnyTransmissionInGear = true;
		}
	}

	const bool bDownstreamNeutral = bHasKnownTransmission && !bAnyTransmissionInGear;

	// 3) 分离或空挡：输入轴随引擎空转，并向传动系写入零扭矩清除上一物理步残留。
	//    接合且空挡时同时同步变速箱输入轴，避免重新挂挡时转速突变
	if (Engagement <= KINDA_SMALL_NUMBER || bDownstreamNeutral)
	{
		if (Parent)
		{
			const float ParentVelocity = Parent->GetAngularVelocity();
			if (FMath::IsFinite(ParentVelocity))
				SetAngularVelocity(ParentVelocity);
		}

		TransmitTorque(VehicleModuleSystem, 0.0f, 0.0f, 1.0f, 0.0f);
		DriveTorque = 0.0f;

		if (bDownstreamNeutral && Engagement > KINDA_SMALL_NUMBER)
		{
			const float InputShaftVelocity = GetAngularVelocity();
			if (FMath::IsFinite(InputShaftVelocity))
			{
				for (FTorqueSimModule* Child : Children)
					Child->SetAngularVelocity(InputShaftVelocity);
			}
		}

		return;
	}

	// 4) 接合：扭矩与转速耦合均按接合量缩放。输入扭矩由原动机每个物理步重新写入，
	//    本步消费后清零，避免原动机被移除或停止写入时旧扭矩被持续下推
	TransmitTorque(VehicleModuleSystem, DriveTorque, 0.0f, 1.0f, Engagement);
	DriveTorque = 0.0f;
}

bool FSingularisMorphClutchSimModule::GetDebugString(FString& StringOut) const
{
	const bool bResult = ISimulationModuleBase::GetDebugString(StringOut);
	StringOut += FString::Printf(TEXT(", Engagement %.3f"), Engagement);
	return bResult;
}
