#include "Components/SingularisMorphVehicleSUComponent.h"

#include <Components/PrimitiveComponent.h>
#include <Engine/World.h>

#include "Subsystems/SingularisMorphVehicleMappingSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleSUComponent)

USingularisMorphVehicleSUComponent::USingularisMorphVehicleSUComponent()
{
	SetIsReplicatedByDefault(false);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

void USingularisMorphVehicleSUComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	if (!IsValid(Owner)) return;


	if (USceneComponent* ResolvedComp = Cast<USceneComponent>(ProxyComponent.GetComponent(Owner)))
	{
		if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(ResolvedComp))
		{
			if (const UWorld* World = GetWorld())
			{
				if (USingularisMorphVehicleMappingSubsystem* Subsystem =
					World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>())
					Subsystem->RegisterComponentMapping(PrimComp, this);
			}
		}
	}
}

void USingularisMorphVehicleSUComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	AActor* Owner = GetOwner();
	if (IsValid(Owner))
	{
		if (USceneComponent* ResolvedComp = Cast<USceneComponent>(ProxyComponent.GetComponent(Owner)))
		{
			if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(ResolvedComp))
			{
				if (const UWorld* World = GetWorld())
				{
					if (USingularisMorphVehicleMappingSubsystem* Subsystem =
						World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>())
						Subsystem->UnregisterComponentMapping(PrimComp);
				}
			}
		}
	}

	Super::EndPlay(EndPlayReason);
}

void USingularisMorphVehicleSUComponent::SetTreeIndex(const int32 NewValue)
{
	TreeIndex = NewValue;
}
