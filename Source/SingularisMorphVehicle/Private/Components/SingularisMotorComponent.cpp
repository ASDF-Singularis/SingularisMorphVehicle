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

	// 扭矩与惯性必须处于同一单位制（N·m → kg·cm²/s²，kg·m² → kg·cm²），
	// 否则角加速度被放大 10^4 倍
	Settings.MaxTorque = Chaos::TorqueMToCm(FMath::Max(MaxTorque, 0.0f));
	Settings.MaxRPM = FMath::Max(MaxRPM, 0.0f);
	Settings.EngineInertia = Chaos::M2ToCm2(FMath::Max(EngineInertia, KINDA_SMALL_NUMBER));

	Chaos::ISimulationModuleBase* Motor = new FSingularisMorphMotorSimModule(Settings);

	return Motor;
}
