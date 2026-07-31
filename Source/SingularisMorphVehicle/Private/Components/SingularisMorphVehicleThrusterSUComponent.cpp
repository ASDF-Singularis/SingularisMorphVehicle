#include "Components/SingularisMorphVehicleThrusterSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleThrusterSUComponent)

USingularisMorphVehicleThrusterSUComponent::USingularisMorphVehicleThrusterSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;
}

Chaos::ISimulationModuleBase* USingularisMorphVehicleThrusterSUComponent::CreateNewCoreModule() const
{
	Chaos::FThrusterSettings Settings;

	Settings.MaxThrustForce = MaxThrustForce;
	Settings.ForceAxis = ForceAxis;
	Settings.ForceOffset = ForceOffset;
	Settings.SteeringEnabled = bSteeringEnabled;
	Settings.SteeringAxis = SteeringAxis;
	Settings.MaxSteeringAngle = MaxSteeringAngle;
	Settings.SteeringForceEffect = SteeringForceEffect;
	Settings.BoostMultiplier = BoostMultiplierEffect;

	Chaos::ISimulationModuleBase* Thruster = new Chaos::FThrusterSimModule(Settings);
	Thruster->SetAnimationEnabled(bAnimationEnabled);

	return Thruster;
}
