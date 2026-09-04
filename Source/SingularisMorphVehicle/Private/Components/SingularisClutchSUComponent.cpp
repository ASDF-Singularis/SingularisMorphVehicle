#include "Components/SingularisClutchSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisClutchSUComponent)

USingularisClutchSUComponent::USingularisClutchSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

Chaos::ISimulationModuleBase* USingularisClutchSUComponent::CreateNewCoreModule() const
{
	Chaos::FClutchSettings Settings;
	Settings.ClutchStrength = ClutchStrength;

	Chaos::ISimulationModuleBase* Clutch = new Chaos::FClutchSimModule(Settings);

	return Clutch;
}
