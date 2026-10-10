#include "Subsystems/SingularisMorphVehicleMappingSubsystem.h"

#include <Components/PrimitiveComponent.h>

#include "Components/SingularisMorphVehicleBaseComponent.h"

void USingularisMorphVehicleMappingSubsystem::RegisterComponentMapping(
	UPrimitiveComponent* PrimComp,
	USingularisMorphVehicleBaseComponent* SUComp
)
{
	if (!IsValid(PrimComp) || !IsValid(SUComp)) return;

	// 追加到该物理组件对应的 SU 列表，重复注册同一 SU 时去重
	TArray<TWeakObjectPtr<USingularisMorphVehicleBaseComponent>>& SUList = ComponentMap.FindOrAdd(PrimComp);
	for (const TWeakObjectPtr<USingularisMorphVehicleBaseComponent>& Weak : SUList)
	{
		if (Weak.Get() == SUComp)
			return;
	}
	SUList.Emplace(SUComp);
}

void USingularisMorphVehicleMappingSubsystem::UnregisterComponentMapping(
	UPrimitiveComponent* PrimComp,
	USingularisMorphVehicleBaseComponent* SUComp
)
{
	if (!IsValid(PrimComp) || !IsValid(SUComp)) return;

	if (TArray<TWeakObjectPtr<USingularisMorphVehicleBaseComponent>>* SUList = ComponentMap.Find(PrimComp))
	{
		SUList->RemoveAllSwap(
			[SUComp](const TWeakObjectPtr<USingularisMorphVehicleBaseComponent>& Weak)
			{
				return Weak.Get() == SUComp;
			}
		);

		// 列表清空后移除整个键，避免留下空壳条目
		if (SUList->IsEmpty())
			ComponentMap.Remove(PrimComp);
	}
}

TArray<USingularisMorphVehicleBaseComponent*> USingularisMorphVehicleMappingSubsystem::FindSUComponents(
	UPrimitiveComponent* PrimComp
) const
{
	TArray<USingularisMorphVehicleBaseComponent*> Result;

	// 1) 空指针守卫
	if (!IsValid(PrimComp)) return Result;

	// 2) 哈希查找 → 过滤已过期的弱引用 → 解引用
	if (const TArray<TWeakObjectPtr<USingularisMorphVehicleBaseComponent>>* SUList = ComponentMap.Find(PrimComp))
	{
		for (const TWeakObjectPtr<USingularisMorphVehicleBaseComponent>& Weak : *SUList)
		{
			if (USingularisMorphVehicleBaseComponent* SUComp = Weak.Get())
			{
				if (IsValid(SUComp))
					Result.Add(SUComp);
			}
		}
	}

	return Result;
}

USingularisMorphVehicleBaseComponent* USingularisMorphVehicleMappingSubsystem::FindSUComponent(
	UPrimitiveComponent* PrimComp
) const
{
	// 兼容单映射场景：返回第一个有效 SU
	const TArray<USingularisMorphVehicleBaseComponent*> SUs = FindSUComponents(PrimComp);
	return SUs.Num() > 0 ? SUs[0] : nullptr;
}
