#include "Components/SingularisMotorComponent.h"

#include <VehicleUtility.h>

#include "Core/SingularisMorphVehicleTorqueSimModules.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMotorComponent)

USingularisMotorComponent::USingularisMotorComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

Chaos::ISimulationModuleBase* USingularisMotorComponent::CreateNewCoreModule() const
{
	Chaos::FMotorSettings Settings;

	Settings.MaxTorque = Chaos::TorqueMToCm(MaxTorque);
	Settings.MaxRPM = MaxRPM;
	Settings.EngineInertia = EngineInertia;

	Chaos::ISimulationModuleBase* Motor = new FSingularisMorphMotorSimModule(Settings);

	return Motor;
}
