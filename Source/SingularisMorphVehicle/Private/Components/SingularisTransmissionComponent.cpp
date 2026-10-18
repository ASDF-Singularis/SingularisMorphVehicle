#include "Components/SingularisTransmissionComponent.h"

#include <SimModule/SimModulesInclude.h>

#include "SingularisMorphVehicle.h"
#include "Core/SingularisMorphVehicleTransmissionSimModule.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisTransmissionComponent)

USingularisTransmissionComponent::USingularisTransmissionComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

void USingularisTransmissionComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	// 1) 有效性检查
	if (!OutputData) return;

	// 2) 将换挡事件广播至游戏线程
	const auto TransOutData = static_cast<const Chaos::FTransmissionOutputData*>(OutputData);

	for (const Chaos::FGearChangeEvent& Event : TransOutData->GearChangeEvents)
		OnGearChangeNativeEvent.Broadcast(TransOutData->ModuleGuid, Event.ChangedToGear);
}

Chaos::ISimulationModuleBase* USingularisTransmissionComponent::CreateNewCoreModule() const
{
	// 1) 配置变速箱设置
	Chaos::FTransmissionSettings Settings;

	const int32 ClampedChangeUpRPM = FMath::Max(ChangeUpRPM, 0);
	const int32 ClampedChangeDownRPM = FMath::Max(ChangeDownRPM, 0);

	Settings.ForwardRatios = ForwardRatios;
	Settings.ReverseRatios = ReverseRatios;
	Settings.FinalDriveRatio = FinalDriveRatio;
	Settings.ChangeUpRPM = static_cast<uint32>(ClampedChangeUpRPM);
	Settings.ChangeDownRPM = static_cast<uint32>(ClampedChangeDownRPM);
	Settings.GearChangeTime = FMath::Max(GearChangeTime, 0.0f);
	Settings.GearHysteresisTime = FMath::Max(GearHysteresisTime, 0.0f);
	Settings.TransmissionEfficiency = TransmissionEfficiency;
	Settings.TransmissionType = static_cast<Chaos::FTransmissionSettings::ETransType>(TransmissionType);
	Settings.AutoReverse = AutoReverse;

	// 2) 手动模式下升降挡点倒置会让自动换挡在相邻挡位间往复，提前告警
	if (!bAutoCalculateShiftPoints && ClampedChangeUpRPM <= ClampedChangeDownRPM)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT(
				"[Transmission] %s: ChangeUpRPM (%d) must be greater than ChangeDownRPM (%d) when automatic shift point calculation is disabled"
			),
			*GetName(),
			ClampedChangeUpRPM,
			ClampedChangeDownRPM
		);
	}

	// 3) 自动换挡参数；实际阈值由仿真模块在模拟线程按动力链中的原动机校准
	FSingularisMorphTransmissionShiftSettings ShiftSettings;
	ShiftSettings.bAutoCalculateShiftPoints = bAutoCalculateShiftPoints;
	ShiftSettings.ShiftUpRPMRatio = ShiftUpRPMRatio;
	ShiftSettings.ShiftDownRPMRatio = ShiftDownRPMRatio;

	// 4) 创建变速箱仿真模块
	Chaos::ISimulationModuleBase* Transmission = new FSingularisMorphTransmissionSimModule(
		Settings,
		ShiftSettings
	);

	return Transmission;
}
