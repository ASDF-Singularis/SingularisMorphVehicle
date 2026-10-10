#include "Components/SingularisClutchComponent.h"

#include <SimModule/SimModulesInclude.h>

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
	Settings.ClutchStrength = ClutchStrength;

	Chaos::ISimulationModuleBase* Clutch = new Chaos::FClutchSimModule(Settings);

	return Clutch;
}
