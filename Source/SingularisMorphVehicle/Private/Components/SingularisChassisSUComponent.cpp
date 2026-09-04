#include "Components/SingularisChassisSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisChassisSUComponent)

USingularisChassisSUComponent::USingularisChassisSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

Chaos::ISimulationModuleBase* USingularisChassisSUComponent::CreateNewCoreModule() const
{
	// 1) 配置底盘空气动力学设置
	Chaos::FChassisSettings Settings;
	Settings.AreaMetresSquared = AreaMetresSquared;
	Settings.DragCoefficient = DragCoefficient;
	Settings.DensityOfMedium = DensityOfMedium;
	Settings.XAxisMultiplier = XAxisMultiplier;
	Settings.YAxisMultiplier = YAxisMultiplier;
	Settings.AngularDamping = AngularDamping;

	// 2) 创建底盘仿真模块
	Chaos::ISimulationModuleBase* Chassis = new Chaos::FChassisSimModule(Settings);

	return Chassis;
}
