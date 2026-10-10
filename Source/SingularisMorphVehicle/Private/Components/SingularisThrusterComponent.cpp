#include "Components/SingularisThrusterComponent.h"

#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisThrusterComponent)

USingularisThrusterComponent::USingularisThrusterComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;
}

Chaos::ISimulationModuleBase* USingularisThrusterComponent::CreateNewCoreModule() const
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
