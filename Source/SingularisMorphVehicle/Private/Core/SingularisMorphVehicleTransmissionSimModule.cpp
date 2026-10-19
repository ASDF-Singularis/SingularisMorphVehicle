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
	  ShiftSettings(InShiftSettings)
{
	// 追加派生标记类型，供本插件按派生类识别；随后重新写入基类类型名，
	// 使 GetSimType() 保持 FTransmissionSimModule（网络工厂按该名称生成哈希）
	AddType(_CHAOSTYPENAMERETRIVAL_());
	AddType(FTransmissionSimModule::_CHAOSTYPENAMERETRIVAL_());

	// 非法齿比会让扭矩与转速耦合产生 NaN；非有限值按 0（空挡）处理
	Chaos::FTransmissionSettings& SettingsRef = AccessSetup();
	for (float& Ratio : SettingsRef.ForwardRatios)
	{
		if (!FMath::IsFinite(Ratio))
			Ratio = 0.0f;
	}
	auto bHasReverseGear = false;
	for (float& Ratio : SettingsRef.ReverseRatios)
	{
		if (!FMath::IsFinite(Ratio))
			Ratio = 0.0f;

		if (FMath::Abs(Ratio) > SMALL_NUMBER)
			bHasReverseGear = true;
	}
	if (!FMath::IsFinite(SettingsRef.FinalDriveRatio))
		SettingsRef.FinalDriveRatio = 0.0f;

	// AutoReverse 由官方状态机在 Simulate 开头直接改写 TargetGear，且不经过 SetGear 的
	// 空挡过渡：手动变速箱按住油门降挡到空挡时会被改回倒挡，倒挡松键时又会被直接拉回
	// 前进挡，形成换挡冲击与倒车失控。手动变速箱的方向只由 ChangeUp/ChangeDown 脉冲
	// 逐挡决定，故仅在「自动变速箱且存在有效倒挡」时保留 AutoReverse；
	// 无倒挡时 AutoReverse 会把目标挡位夹成 0，随后自动逻辑又升回前进挡。
	if (SettingsRef.TransmissionType != Chaos::FTransmissionSettings::AutomaticType || !bHasReverseGear)
		SettingsRef.AutoReverse = false;

	// 上游构造函数无条件把初始挡位设为 1；前进挡为空时 GetGearRatio(1) 会越界访问
	if (SettingsRef.ForwardRatios.IsEmpty())
		SetGear(0, true);
}

void FSingularisMorphTransmissionSimModule::Simulate(
	const float DeltaTime,
	const Chaos::FAllInputs& Inputs,
	Chaos::FSimModuleTree& VehicleModuleSystem
)
{
	// 0) 兜底：运行时改写设置或直接构造模块时，手动变速箱仍不得接受 AutoReverse 的
	//    目标挡位覆盖（官方实现绕过 SetGear，没有空挡过渡）
	if (Setup().TransmissionType != Chaos::FTransmissionSettings::AutomaticType
		&& Setup().AutoReverse)
		AccessSetup().AutoReverse = false;

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

	// 输入扭矩由上游每个物理步重新写入；本步消费后清零，避免原动机被移除或停止写入时，
	// 旧扭矩在挡位内被持续下推（倒挡会表现为持续反向加速）
	DriveTorque = 0.0f;

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

		if (FMath::Abs(ChildDriveTorque) > SMALL_NUMBER)
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

	// 5) 空挡时输入轴随原动机空转。无离合器直连拓扑中，原动机的 TransmitTorque
	//    会把自身转速同步为直接子节点（本模块）的转速；本模块若在空挡保持旧转速，
	//    会把原动机转速钳回旧值，使空挡油门无法提升转速。
	//    上游为离合器时由其负责输入轴跟踪，此处不重复处理。
	if (IsInNeutral())
	{
		const int32 SelfIndex = GetTreeIndex();
		if (SelfIndex >= 0 && VehicleModuleSystem.IsValidNode(SelfIndex))
		{
			const int32 ParentIndex = VehicleModuleSystem.GetParent(SelfIndex);
			if (ParentIndex >= 0 && VehicleModuleSystem.IsValidNode(ParentIndex))
			{
				ISimulationModuleBase* ParentModule = VehicleModuleSystem.AccessSimModule(ParentIndex);
				if (ParentModule
					&& (ParentModule->IsSimType<Chaos::FEngineSimModule>()
						|| ParentModule->IsSimType<Chaos::FMotorSimModule>()))
				{
					if (const FTorqueSimModule* PrimeMover = ParentModule->Cast<FTorqueSimModule>())
					{
						const float PrimeMoverVelocity = PrimeMover->GetAngularVelocity();
						if (FMath::IsFinite(PrimeMoverVelocity))
							SetAngularVelocity(PrimeMoverVelocity);
					}
				}
			}
		}
	}
}

bool FSingularisMorphTransmissionSimModule::GetDebugString(FString& StringOut) const
{
	const bool bResult = FTransmissionSimModule::GetDebugString(StringOut);

	// 手动变速箱没有 RPM 阈值，只报告方向由换挡脉冲驱动，避免把官方默认值误读为配置
	if (Setup().TransmissionType != Chaos::FTransmissionSettings::AutomaticType)
	{
		StringOut += TEXT(", manual (pulse-driven)");
		return bResult;
	}

	StringOut += FString::Printf(
		TEXT(", AutoShiftUp %d RPM, AutoShiftDown %d RPM (%s)"),
		static_cast<int32>(Setup().ChangeUpRPM),
		static_cast<int32>(Setup().ChangeDownRPM),
		bShiftPointsResolved ? TEXT("resolved") : TEXT("unresolved")
	);

	return bResult;
}

void FSingularisMorphTransmissionSimModule::UpdateShiftPoints(
	const Chaos::FSimModuleTree& VehicleModuleSystem
)
{
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
			IdleRPM
		);

	// 3) 原动机转速区间无效时保留当前设置；转速区间变化后重新计算
	if (!Points.bAutoCalculated)
	{
		bShiftPointsResolved = false;
		return;
	}

	Chaos::FTransmissionSettings& Settings = AccessSetup();
	Settings.ChangeUpRPM = ToRPMValue(Points.ChangeUpRPM);
	Settings.ChangeDownRPM = ToRPMValue(Points.ChangeDownRPM);
	bShiftPointsResolved = true;

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
