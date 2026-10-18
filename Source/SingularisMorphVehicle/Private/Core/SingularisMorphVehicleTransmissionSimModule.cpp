#include "Core/SingularisMorphVehicleTransmissionSimModule.h"

#include <SimModule/EngineModule.h>
#include <SimModule/MotorModule.h>
#include <SimModule/SimModuleTree.h>

#include "SingularisMorphVehicle.h"

namespace
{
	/** 父链遍历深度上限，避免损坏的树索引造成死循环 */
	constexpr auto MaxPrimeMoverSearchDepth = 32;

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

	// 2) 官方自动/手动换挡状态机与扭矩传递
	FTransmissionSimModule::Simulate(DeltaTime, Inputs, VehicleModuleSystem);
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
