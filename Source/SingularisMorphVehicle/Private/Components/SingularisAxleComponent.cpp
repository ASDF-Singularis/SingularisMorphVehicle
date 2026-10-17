#include "Components/SingularisAxleComponent.h"

#include <VehicleUtility.h>

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

	// 惯性按 kg·m² → kg·cm² 换算，与扭矩保持同一单位制；
	// 轮轴为纯扭矩中继，该值不参与转速链（见 FSingularisMorphAxleSimModule）
	Settings.AxleInertia = Chaos::M2ToCm2(FMath::Max(AxleInertia, KINDA_SMALL_NUMBER));

	Chaos::ISimulationModuleBase* Axle = new FSingularisMorphAxleSimModule(Settings);

	return Axle;
}
