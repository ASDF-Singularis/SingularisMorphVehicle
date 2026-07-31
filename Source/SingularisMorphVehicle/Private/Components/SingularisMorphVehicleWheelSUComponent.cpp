#include "Components/SingularisMorphVehicleWheelSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleWheelSUComponent)

USingularisMorphVehicleWheelSUComponent::USingularisMorphVehicleWheelSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;
}

void USingularisMorphVehicleWheelSUComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	if (!OutputData) return;

	const auto WheelOutData = static_cast<const Chaos::FWheelOutputData*>(OutputData);

	for (const Chaos::FWheelTouchChangeEvent& Event : WheelOutData->WheelTouchEvents)
		OnWheelTouchChangeNativeEvent.Broadcast(WheelOutData->ModuleGuid, Event.bIsInContact);
}

Chaos::ISimulationModuleBase* USingularisMorphVehicleWheelSUComponent::CreateNewCoreModule() const
{
	Chaos::FWheelSettings Settings;

	Settings.Radius = WheelRadius;
	Settings.Width = WheelWidth;
	Settings.WheelInertia = WheelInertia;
	Settings.FrictionMultiplier = FrictionMultiplier;
	Settings.CorneringStiffness = CorneringStiffness * 10000.0f;
	Settings.SlipAngleLimit = SlipAngleLimit;
	Settings.MaxBrakeTorque = Chaos::TorqueMToCm(MaxBrakeTorque);
	Settings.HandbrakeEnabled = bHandbrakeEnabled;
	Settings.HandbrakeTorque = Chaos::TorqueMToCm(HandbrakeTorque);
	Settings.SteeringEnabled = bSteeringEnabled;
	Settings.MaxSteeringAngle = bSteeringEnabled ? MaxSteeringAngle : 0.0f;
	Settings.ABSEnabled = bABSEnabled;
	Settings.TractionControlEnabled = bTractionControlEnabled;
	Settings.Axis = static_cast<Chaos::EWheelAxis>(AxisType);
	Settings.ReverseDirection = ReverseDirection;
	Settings.ForceOffset = ForceOffset;

	Chaos::ISimulationModuleBase* Wheel = new Chaos::FWheelSimModule(Settings);
	Wheel->SetAnimationEnabled(bAnimationEnabled);

	return Wheel;
}
