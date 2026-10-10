#include "Components/SingularisAxleComponent.h"

#include "Core/SingularisMorphVehicleTorqueSimModules.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisAxleComponent)

USingularisAxleComponent::USingularisAxleComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

Chaos::ISimulationModuleBase* USingularisAxleComponent::CreateNewCoreModule() const
{
	Chaos::FAxleSettings Settings;

	Settings.AxleInertia = AxleInertia;

	Chaos::ISimulationModuleBase* Axle = new FSingularisMorphAxleSimModule(Settings);

	return Axle;
}
