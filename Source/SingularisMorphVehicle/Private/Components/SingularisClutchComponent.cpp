#include "Components/SingularisClutchComponent.h"

#include <SimModule/SimModulesInclude.h>

#include "Core/SingularisMorphVehicleTorqueSimModules.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisClutchComponent)

USingularisClutchComponent::USingularisClutchComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

Chaos::ISimulationModuleBase* USingularisClutchComponent::CreateNewCoreModule() const
{
	Chaos::FClutchSettings Settings;

	// 接合强度同时用于扭矩缩放与转速耦合，取值超出 [0..1] 会使耦合过冲或反向
	Settings.ClutchStrength = FMath::Clamp(ClutchStrength, 0.0f, 1.0f);

	Chaos::ISimulationModuleBase* Clutch = new FSingularisMorphClutchSimModule(Settings);

	return Clutch;
}
