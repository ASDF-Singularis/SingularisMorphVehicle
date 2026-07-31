#include "Components/SingularisMorphVehicleSchedulerComponent.h"

#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Subsystems/SingularisMorphVehicleSchedulerSubsystem.h"

USingularisMorphVehicleSchedulerComponent::USingularisMorphVehicleSchedulerComponent()
{
	SetIsReplicatedByDefault(false);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

void USingularisMorphVehicleSchedulerComponent::BeginPlay()
{
	Super::BeginPlay();
}

void USingularisMorphVehicleSchedulerComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction
)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void USingularisMorphVehicleSchedulerComponent::Activate(const bool bReset)
{
	Super::Activate(bReset);

	// 1) VehicleSimulationComponent 守卫
	USingularisMorphVehicleSimulationComponent* VehicleComponent = Cast<USingularisMorphVehicleSimulationComponent>(
		TargetComponent.GetComponent(GetOwner())
	);
	if (!IsValid(VehicleComponent)) return;

	// 2) SchedulerSubsystem 守卫
	USingularisMorphVehicleSchedulerSubsystem* SchedulerSubsystem =
		GetWorld()->GetSubsystem<USingularisMorphVehicleSchedulerSubsystem>();
	if (!IsValid(SchedulerSubsystem)) return;

	// 3) 注册到调度器子系统
	SchedulerSubsystem->RegisterVehicleComponent(VehicleComponent);
}

void USingularisMorphVehicleSchedulerComponent::Deactivate()
{
	Super::Deactivate();

	// 1) VehicleSimulationComponent 守卫
	USingularisMorphVehicleSimulationComponent* VehicleComponent = Cast<USingularisMorphVehicleSimulationComponent>(
		TargetComponent.GetComponent(GetOwner())
	);
	if (!IsValid(VehicleComponent)) return;

	// 2) SchedulerSubsystem 守卫
	USingularisMorphVehicleSchedulerSubsystem* SchedulerSubsystem =
		GetWorld()->GetSubsystem<USingularisMorphVehicleSchedulerSubsystem>();
	if (!IsValid(SchedulerSubsystem)) return;

	// 3) 注册到调度器子系统
	SchedulerSubsystem->UnregisterVehicleComponent(VehicleComponent);
}
