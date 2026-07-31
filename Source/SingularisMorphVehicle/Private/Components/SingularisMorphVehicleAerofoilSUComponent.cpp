#include "Components/SingularisMorphVehicleAerofoilSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleAerofoilSUComponent)

USingularisMorphVehicleAerofoilSUComponent::USingularisMorphVehicleAerofoilSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;
}

Chaos::ISimulationModuleBase* USingularisMorphVehicleAerofoilSUComponent::CreateNewCoreModule() const
{
	Chaos::FAerofoilSettings Settings;

	Settings.Offset = Offset;
	Settings.ForceAxis = ForceAxis;
	Settings.ControlRotationAxis = ControlRotationAxis;
	Settings.Area = Area;
	Settings.Camber = Camber;
	Settings.MaxControlAngle = MaxControlAngle;
	Settings.StallAngle = StallAngle;
	Settings.Type = static_cast<Chaos::EAerofoil>(Type);
	Settings.LiftMultiplier = LiftMultiplier;
	Settings.DragMultiplier = DragMultiplier;
	Settings.AnimationMagnitudeMultiplier = AnimationMagnitudeMultiplier;

	Chaos::ISimulationModuleBase* Aerofoil = new Chaos::FAerofoilSimModule(Settings);
	Aerofoil->SetAnimationEnabled(bAnimationEnabled);

	return Aerofoil;
}
