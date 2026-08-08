#include "Subsystems/SingularisMorphVehicleMappingSubsystem.h"

#include <Components/PrimitiveComponent.h>

#include "Components/SingularisMorphVehicleSUComponent.h"

void USingularisMorphVehicleMappingSubsystem::RegisterComponentMapping(
	UPrimitiveComponent* PrimComp,
	USingularisMorphVehicleSUComponent* SUComp
)
{
	if (!IsValid(PrimComp) || !IsValid(SUComp)) return;

	// 追加到该物理组件对应的 SU 列表，重复注册同一 SU 时去重
	TArray<TWeakObjectPtr<USingularisMorphVehicleSUComponent>>& SUList = ComponentMap.FindOrAdd(PrimComp);
	for (const TWeakObjectPtr<USingularisMorphVehicleSUComponent>& Weak : SUList)
	{
		if (Weak.Get() == SUComp)
			return;
	}
	SUList.Emplace(SUComp);
}

void USingularisMorphVehicleMappingSubsystem::UnregisterComponentMapping(
	UPrimitiveComponent* PrimComp,
	USingularisMorphVehicleSUComponent* SUComp
)
{
	if (!IsValid(PrimComp) || !IsValid(SUComp)) return;

	if (TArray<TWeakObjectPtr<USingularisMorphVehicleSUComponent>>* SUList = ComponentMap.Find(PrimComp))
	{
		SUList->RemoveAllSwap(
			[SUComp](const TWeakObjectPtr<USingularisMorphVehicleSUComponent>& Weak)
			{
				return Weak.Get() == SUComp;
			}
		);

		// 列表清空后移除整个键，避免留下空壳条目
		if (SUList->IsEmpty())
			ComponentMap.Remove(PrimComp);
	}
}

TArray<USingularisMorphVehicleSUComponent*> USingularisMorphVehicleMappingSubsystem::FindSUComponents(
	UPrimitiveComponent* PrimComp
) const
{
	TArray<USingularisMorphVehicleSUComponent*> Result;

	// 1) 空指针守卫
	if (!IsValid(PrimComp)) return Result;

	// 2) 哈希查找 → 过滤已过期的弱引用 → 解引用
	if (const TArray<TWeakObjectPtr<USingularisMorphVehicleSUComponent>>* SUList = ComponentMap.Find(PrimComp))
	{
		for (const TWeakObjectPtr<USingularisMorphVehicleSUComponent>& Weak : *SUList)
		{
			if (USingularisMorphVehicleSUComponent* SUComp = Weak.Get())
			{
				if (IsValid(SUComp))
					Result.Add(SUComp);
			}
		}
	}

	return Result;
}

USingularisMorphVehicleSUComponent* USingularisMorphVehicleMappingSubsystem::FindSUComponent(
	UPrimitiveComponent* PrimComp
) const
{
	// 兼容单映射场景：返回第一个有效 SU
	const TArray<USingularisMorphVehicleSUComponent*> SUs = FindSUComponents(PrimComp);
	return SUs.Num() > 0 ? SUs[0] : nullptr;
}
