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

	// 非有限齿比会在扭矩与转速耦合中传播为 NaN，按空挡（齿比 0）处理
	Settings.ForwardRatios = ForwardRatios;
	for (float& Ratio : Settings.ForwardRatios)
	{
		if (!FMath::IsFinite(Ratio))
			Ratio = 0.0f;
	}

	Settings.ReverseRatios = ReverseRatios;
	for (float& Ratio : Settings.ReverseRatios)
	{
		if (!FMath::IsFinite(Ratio))
			Ratio = 0.0f;
	}

	Settings.FinalDriveRatio = FMath::IsFinite(FinalDriveRatio) ? FinalDriveRatio : 0.0f;

	// ChangeUpRPM/ChangeDownRPM 由仿真模块在首次模拟时按原动机转速区间自动写入；
	// 此处保留官方默认值，仅作为原动机尚未挂入动力链时的临时阈值
	Settings.GearChangeTime = FMath::IsFinite(GearChangeTime) ? FMath::Max(GearChangeTime, 0.0f) : 0.0f;
	Settings.GearHysteresisTime = FMath::IsFinite(GearHysteresisTime)
		                              ? FMath::Max(GearHysteresisTime, 0.0f)
		                              : 0.0f;
	Settings.TransmissionEfficiency = TransmissionEfficiency;
	Settings.TransmissionType = static_cast<Chaos::FTransmissionSettings::ETransType>(TransmissionType);
	Settings.AutoReverse = AutoReverse;

	// 前进挡为空或主减速比为 0 时所有挡位都没有有效齿比；提前告警，
	// 避免误判为空挡油门失效
	if (Settings.ForwardRatios.IsEmpty())
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[Transmission] %s: ForwardRatios is empty - the transmission will stay in neutral"),
			*GetName()
		);
	}
	else if (FMath::Abs(Settings.FinalDriveRatio) <= SMALL_NUMBER)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[Transmission] %s: FinalDriveRatio is zero - all gears behave as neutral"),
			*GetName()
		);
	}

	// 2) 自动换挡参数；实际阈值由仿真模块在每个物理步按动力链中的原动机校准
	FSingularisMorphTransmissionShiftSettings ShiftSettings;
	ShiftSettings.ShiftUpRPMRatio = ShiftUpRPMRatio;
	ShiftSettings.ShiftDownRPMRatio = ShiftDownRPMRatio;

	// 3) 创建变速箱仿真模块
	Chaos::ISimulationModuleBase* Transmission = new FSingularisMorphTransmissionSimModule(
		Settings,
		ShiftSettings
	);

	return Transmission;
}
