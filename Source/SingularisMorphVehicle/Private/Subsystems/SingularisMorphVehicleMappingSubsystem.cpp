#include "Subsystems/SingularisMorphVehicleMappingSubsystem.h"

#include <Components/PrimitiveComponent.h>

#include "Components/SingularisMorphVehicleSUComponent.h"

void USingularisMorphVehicleMappingSubsystem::RegisterComponentMapping(
	UPrimitiveComponent* PrimComp,
	USingularisMorphVehicleSUComponent* SUComp
)
{
	if (!IsValid(PrimComp) || !IsValid(SUComp)) return;
	ComponentMap.Add(PrimComp, SUComp);
}

void USingularisMorphVehicleMappingSubsystem::UnregisterComponentMapping(UPrimitiveComponent* PrimComp)
{
	if (!IsValid(PrimComp)) return;
	ComponentMap.Remove(PrimComp);
}

USingularisMorphVehicleSUComponent* USingularisMorphVehicleMappingSubsystem::FindSUComponent(
	UPrimitiveComponent* PrimComp
) const
{
	// 1) 空指针守卫
	if (!IsValid(PrimComp)) return nullptr;

	// 2) 哈希查找 → 弱引用有效性检查 → 解引用
	if (const TWeakObjectPtr<USingularisMorphVehicleSUComponent>* Found = ComponentMap.Find(PrimComp))
	{
		if (Found->IsValid()) return Found->Get();
	}

	return nullptr;
}
