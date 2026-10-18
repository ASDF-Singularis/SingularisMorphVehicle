#include "Core/SingularisMorphVehicleTransmissionSimModule.h"

#include <SimModule/EngineModule.h>
#include <SimModule/MotorModule.h>
#include <SimModule/SimModuleTree.h>

#include "SingularisMorphVehicle.h"

namespace
{
	/** 父链遍历深度上限，避免损坏的树索引造成死循环 */
	constexpr auto MaxPrimeMoverSearchDepth = 32;

	/** 下游驱动力矩清理的递归深度上限，避免异常树结构造成无限递归 */
	constexpr auto MaxDownstreamClearDepth = 32;

	/** 将浮点 RPM 写入 uint32 设置前的安全转换 */
	uint32 ToRPMValue(const float RPM)
	{
		if (!FMath::IsFinite(RPM))
			return 0;

		const float ClampedRPM = FMath::Clamp(RPM, 0.0f, static_cast<float>(TNumericLimits<int32>::Max()));
		return static_cast<uint32>(FMath::RoundToInt(ClampedRPM));
	}
}

FSingularisMorphTransmissionSimModule::FSingularisMorphTransmissionSimModule(
	const Chaos::FTransmissionSettings& Settings,
	const FSingularisMorphTransmissionShiftSettings& InShiftSettings
)
	: FTransmissionSimModule(Settings),
	  ShiftSettings(InShiftSettings),
	  ManualChangeUpRPM(static_cast<float>(Settings.ChangeUpRPM)),
	  ManualChangeDownRPM(static_cast<float>(Settings.ChangeDownRPM)) {}

void FSingularisMorphTransmissionSimModule::Simulate(
	const float DeltaTime,
	const Chaos::FAllInputs& Inputs,
	Chaos::FSimModuleTree& VehicleModuleSystem
)
{
	// 1) 换挡点随原动机配置动态校准，保证换装不同引擎后仍按同一换挡逻辑工作
	UpdateShiftPoints(VehicleModuleSystem);

	// 2) 清除直接扭矩子节点上一物理步的驱动力矩。
	//    官方模块在空挡（齿比为 0）时跳过 TransmitTorque，子节点会保留上一挡的
	//    DriveTorque；轮轴随后会把它继续下推给车轮，造成空挡持续加速，
	//    倒挡下则表现为反向持续加速。
	TArray<FTorqueSimModule*, TInlineAllocator<4>> TorqueChildren;
	const auto TreeIndex = GetTreeIndex();
	if (TreeIndex >= 0 && VehicleModuleSystem.IsValidNode(TreeIndex))
	{
		for (const auto ChildIndex : VehicleModuleSystem.GetChildren(TreeIndex))
		{
			ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
			if (!ChildModule || ChildModule->GetSimType() == GetSimType())
				continue;

			if (FTorqueSimModule* TorqueChild = ChildModule->Cast<FTorqueSimModule>())
				TorqueChildren.Add(TorqueChild);
		}
	}

	for (FTorqueSimModule* TorqueChild : TorqueChildren)
		TorqueChild->SetDriveTorque(0.0f);

	// 3) 官方自动/手动换挡状态机与扭矩传递
	FTransmissionSimModule::Simulate(DeltaTime, Inputs, VehicleModuleSystem);

	// 4) 本步未向下游写入有效驱动力矩（空挡、零油门或非法值）时，递归清除更深层的
	//    残留力矩，避免轮轴下一步把上一挡扭矩继续推给车轮
	auto bTransmittedTorque = false;
	for (FTorqueSimModule* TorqueChild : TorqueChildren)
	{
		const auto ChildDriveTorque = TorqueChild->GetDriveTorque();
		if (!FMath::IsFinite(ChildDriveTorque))
		{
			TorqueChild->SetDriveTorque(0.0f);
			continue;
		}

		if (!FMath::IsNearlyZero(ChildDriveTorque))
			bTransmittedTorque = true;
	}

	if (!bTransmittedTorque)
	{
		for (const FTorqueSimModule* TorqueChild : TorqueChildren)
		{
			const auto ChildTreeIndex = TorqueChild->GetTreeIndex();
			if (ChildTreeIndex >= 0)
				ClearDownstreamDriveTorque(VehicleModuleSystem, ChildTreeIndex, 0);
		}
	}
}

bool FSingularisMorphTransmissionSimModule::GetDebugString(FString& StringOut) const
{
	const bool bResult = FTransmissionSimModule::GetDebugString(StringOut);
	const TCHAR* ShiftMode = bShiftPointsResolved ? TEXT("auto") : TEXT("manual");

	StringOut += FString::Printf(
		TEXT(", ShiftUp %d RPM, ShiftDown %d RPM (%s)"),
		static_cast<int32>(Setup().ChangeUpRPM),
		static_cast<int32>(Setup().ChangeDownRPM),
		ShiftMode
	);

	return bResult;
}

void FSingularisMorphTransmissionSimModule::UpdateShiftPoints(
	const Chaos::FSimModuleTree& VehicleModuleSystem
)
{
	if (!ShiftSettings.bAutoCalculateShiftPoints)
		return;

	// 手动变速箱不使用 RPM 阈值，跳过无效计算
	if (Setup().TransmissionType != Chaos::FTransmissionSettings::AutomaticType)
		return;

	// 1) 解析原动机转速区间；尚未挂入动力链时保留当前阈值，下一帧重试
	const ISimulationModuleBase* PrimeMover = FindPrimeMover(VehicleModuleSystem);
	if (!PrimeMover)
		return;

	auto MaxRPM = 0.0f;
	auto IdleRPM = 0.0f;

	if (const Chaos::FEngineSimModule* Engine = PrimeMover->Cast<Chaos::FEngineSimModule>())
	{
		MaxRPM = Engine->Setup().MaxRPM;
		IdleRPM = Engine->Setup().IdleRPM;
	}
	else if (const Chaos::FMotorSimModule* Motor = PrimeMover->Cast<Chaos::FMotorSimModule>())
		MaxRPM = Motor->Setup().MaxRPM;
	else
		return;

	// 转速区间不可信时保留当前阈值，避免每步重复回退
	if (!FMath::IsFinite(MaxRPM) || !FMath::IsFinite(IdleRPM))
		return;

	// 2) 转速区间未变化时复用缓存，避免每步重复计算齿比步长
	if (FMath::IsNearlyEqual(MaxRPM, CachedMaxRPM) && FMath::IsNearlyEqual(IdleRPM, CachedIdleRPM))
		return;

	CachedMaxRPM = MaxRPM;
	CachedIdleRPM = IdleRPM;

	const FSingularisMorphTransmissionShiftPoints Points =
		FSingularisMorphTransmissionShiftCalculator::Calculate(
			ShiftSettings,
			Setup().ForwardRatios,
			Setup().ReverseRatios,
			MaxRPM,
			IdleRPM,
			ManualChangeUpRPM,
			ManualChangeDownRPM
		);

	// 3) 写回官方设置；无法自动计算时 Calculate 会返回原手动阈值
	Chaos::FTransmissionSettings& Settings = AccessSetup();
	Settings.ChangeUpRPM = ToRPMValue(Points.ChangeUpRPM);
	Settings.ChangeDownRPM = ToRPMValue(Points.ChangeDownRPM);
	bShiftPointsResolved = Points.bAutoCalculated;

	if (Points.bAutoCalculated)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Verbose,
			TEXT(
				"[Transmission] Auto shift points updated: up=%.0f RPM, down=%.0f RPM (prime mover max=%.0f RPM, idle=%.0f RPM)"
			),
			Points.ChangeUpRPM,
			Points.ChangeDownRPM,
			MaxRPM,
			IdleRPM
		);
	}
	else
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Verbose,
			TEXT(
				"[Transmission] Auto shift points unavailable; manual fallback: up=%.0f RPM, down=%.0f RPM (prime mover max=%.0f RPM, idle=%.0f RPM)"
			),
			Points.ChangeUpRPM,
			Points.ChangeDownRPM,
			MaxRPM,
			IdleRPM
		);
	}
}

const Chaos::ISimulationModuleBase* FSingularisMorphTransmissionSimModule::FindPrimeMover(
	const Chaos::FSimModuleTree& VehicleModuleSystem
) const
{
	auto Index = GetTreeIndex();
	if (Index < 0 || !VehicleModuleSystem.IsValidNode(Index))
		return nullptr;

	for (auto Depth = 0; Depth < MaxPrimeMoverSearchDepth; ++Depth)
	{
		if (Index < 0 || !VehicleModuleSystem.IsValidNode(Index))
			return nullptr;

		const ISimulationModuleBase* Module = VehicleModuleSystem.AccessSimModule(Index);
		if (!Module)
			return nullptr;

		if (Module->IsSimType<Chaos::FEngineSimModule>() || Module->IsSimType<Chaos::FMotorSimModule>())
			return Module;

		const auto ParentIndex = VehicleModuleSystem.GetParent(Index);
		if (ParentIndex < 0)
			return nullptr;

		Index = ParentIndex;
	}

	return nullptr;
}

void FSingularisMorphTransmissionSimModule::ClearDownstreamDriveTorque(
	const Chaos::FSimModuleTree& VehicleModuleSystem,
	const int32 ParentIndex,
	const int32 Depth
)
{
	if (Depth >= MaxDownstreamClearDepth || ParentIndex < 0 || !VehicleModuleSystem.IsValidNode(ParentIndex))
		return;

	const ISimulationModuleBase* ParentModule = VehicleModuleSystem.AccessSimModule(ParentIndex);
	if (!ParentModule)
		return;

	// 与 TransmitTorque 的语义一致：父模块不驱动同类型子节点，其扭矩来自其它来源，
	// 清理时必须整棵跳过，避免误清独立动力支路
	const FName ParentSimType = ParentModule->GetSimType();

	for (const auto ChildIndex : VehicleModuleSystem.GetChildren(ParentIndex))
	{
		ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIndex);
		if (!ChildModule || ChildModule->GetSimType() == ParentSimType)
			continue;

		if (FTorqueSimModule* TorqueChild = ChildModule->Cast<FTorqueSimModule>())
			TorqueChild->SetDriveTorque(0.0f);

		ClearDownstreamDriveTorque(VehicleModuleSystem, ChildIndex, Depth + 1);
	}
}
