#include "Components/SingularisTransmissionSUComponent.h"

#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisTransmissionSUComponent)

USingularisTransmissionSUComponent::USingularisTransmissionSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

void USingularisTransmissionSUComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	// 1) 有效性检查
	if (!OutputData) return;

	// 2) 将换挡事件广播至游戏线程
	const auto TransOutData = static_cast<const Chaos::FTransmissionOutputData*>(OutputData);

	for (const Chaos::FGearChangeEvent& Event : TransOutData->GearChangeEvents)
		OnGearChangeNativeEvent.Broadcast(TransOutData->ModuleGuid, Event.ChangedToGear);
}

Chaos::ISimulationModuleBase* USingularisTransmissionSUComponent::CreateNewCoreModule() const
{
	// 1) 配置变速箱设置
	Chaos::FTransmissionSettings Settings;

	Settings.ForwardRatios = ForwardRatios;
	Settings.ReverseRatios = ReverseRatios;
	Settings.FinalDriveRatio = FinalDriveRatio;
	Settings.ChangeUpRPM = ChangeUpRPM;
	Settings.ChangeDownRPM = ChangeDownRPM;
	Settings.GearChangeTime = GearChangeTime;
	Settings.GearHysteresisTime = GearHysteresisTime;
	Settings.TransmissionEfficiency = TransmissionEfficiency;
	Settings.TransmissionType = static_cast<Chaos::FTransmissionSettings::ETransType>(TransmissionType);
	Settings.AutoReverse = AutoReverse;

	// 2) 创建变速箱仿真模块
	Chaos::ISimulationModuleBase* Transmission = new Chaos::FTransmissionSimModule(Settings);

	return Transmission;
}
