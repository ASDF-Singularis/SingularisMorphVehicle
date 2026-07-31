#include "Components/SingularisMorphVehicleClutchSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleClutchSUComponent)

USingularisMorphVehicleClutchSUComponent::USingularisMorphVehicleClutchSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

Chaos::ISimulationModuleBase* USingularisMorphVehicleClutchSUComponent::CreateNewCoreModule() const
{
	Chaos::FClutchSettings Settings;
	Settings.ClutchStrength = ClutchStrength;

	Chaos::ISimulationModuleBase* Clutch = new Chaos::FClutchSimModule(Settings);

	return Clutch;
}
