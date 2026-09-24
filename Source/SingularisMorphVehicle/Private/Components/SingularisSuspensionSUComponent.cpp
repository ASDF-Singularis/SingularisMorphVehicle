#include "Components/SingularisSuspensionSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include "Core/SingularisMorphVehicleSuspensionSimModule.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisSuspensionSUComponent)

USingularisSuspensionSUComponent::USingularisSuspensionSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;
}

Chaos::ISimulationModuleBase* USingularisSuspensionSUComponent::CreateNewCoreModule() const
{
	// 1) 配置悬挂设置
	FSingularisMorphSuspensionSettings Settings;

	Settings.SuspensionAxis = SuspensionAxis;
	Settings.MaxRaise = SuspensionMaxRaise;
	Settings.MaxDrop = SuspensionMaxDrop;
	Settings.SpringRate = Chaos::MToCm(SpringRate);
	Settings.SpringPreload = Chaos::MToCm(SpringPreload);
	Settings.SpringDamping = SpringDamping;
	Settings.VirtualWheelMassKg = VirtualWheelMassKg;
	Settings.SuspensionForceEffect = SuspensionForceEffect;

	// 2) 创建悬挂仿真模块并启用动画
	Chaos::ISimulationModuleBase* Suspension = new FSingularisMorphVehicleSuspensionSimModule(Settings);
	Suspension->SetAnimationEnabled(bAnimationEnabled);

	return Suspension;
}
