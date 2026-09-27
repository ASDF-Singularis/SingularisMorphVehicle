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


	if (USceneComponent* ResolvedComp = Cast<USceneComponent>(DrivenComponent.GetComponent(Owner)))
	{
		if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(ResolvedComp))
		{
			if (const UWorld* World = GetWorld())
			{
				if (USingularisMorphVehicleMappingSubsystem* Subsystem =
					World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>())
				{
					Subsystem->RegisterComponentMapping(PrimComp, this);
					MappedPrimitiveComponent = PrimComp;
				}
			}
		}
	}
}

void USingularisMorphVehicleSUComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 以 BeginPlay 记录的物理组件为准注销：重新解析 DrivenComponent 在驱动组件
	// 先于本组件销毁时必然失败，会把条目永久留在映射表中
	UPrimitiveComponent* PrimComp = MappedPrimitiveComponent.Get();
	if (!PrimComp)
	{
		if (AActor* Owner = GetOwner())
			PrimComp = Cast<UPrimitiveComponent>(DrivenComponent.GetComponent(Owner));
	}

	if (PrimComp)
	{
		if (const UWorld* World = GetWorld())
		{
			if (USingularisMorphVehicleMappingSubsystem* Subsystem =
				World->GetSubsystem<USingularisMorphVehicleMappingSubsystem>())
				// 仅注销自身条目，避免共享同一物理组件的其它 SU 被误清除
				Subsystem->UnregisterComponentMapping(PrimComp, this);
		}
	}

	MappedPrimitiveComponent = nullptr;

	Super::EndPlay(EndPlayReason);
}

void USingularisMorphVehicleSUComponent::SetTreeIndex(const int32 NewValue)
{
	TreeIndex = NewValue;
}
