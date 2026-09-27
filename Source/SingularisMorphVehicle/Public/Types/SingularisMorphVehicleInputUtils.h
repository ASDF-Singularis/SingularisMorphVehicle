#pragma once

#include <CoreMinimal.h>
#include <SimModule/ModuleInput.h>

/**
 * 引力奇点变型载具输入容器工具。
 */
namespace SingularisMorphVehicleInputUtils
{
	/**
	 * 把旧容器的已缓冲值按输入名迁移到重建后的容器。
	 *
	 * 输入配置变化（运行时增删模块）会重建容器并把全部输入清零，直接清零会丢掉
	 * 玩家已缓冲的油门/转向，表现为变形瞬间动力中断与转向回正。仅当新旧容器存在
	 * 同名且同类型的输入项时迁移；输入类型变化表示该项被重新定义，保留新容器默认值。
	 *
	 * @param Container        重建后的容器
	 * @param NameMap          重建后的输入名映射
	 * @param PreviousNameMap  重建前的输入名映射
	 * @param PreviousValues   重建前的输入值数组
	 */
	inline void CarryOverValues(
		FModuleInputContainer& Container,
		const FInputInterface::FInputNameMap& NameMap,
		const FInputInterface::FInputNameMap& PreviousNameMap,
		const TArray<FModuleInputValue>& PreviousValues
	)
	{
		const TArray<FModuleInputValue>& Values = Container.AccessInputValues();

		for (const TPair<FName, int>& Previous : PreviousNameMap)
		{
			if (!PreviousValues.IsValidIndex(Previous.Value)) continue;

			const int* NewIndex = NameMap.Find(Previous.Key);
			if (!NewIndex || !Values.IsValidIndex(*NewIndex)) continue;

			FModuleInputValue& NewValue = Container.AccessInputValues()[*NewIndex];
			const FModuleInputValue& PreviousValue = PreviousValues[Previous.Value];

			// 类型不一致表示该输入被重新定义，迁移旧值会产生类型错配
			if (NewValue.GetValueType() != PreviousValue.GetValueType()) continue;

			NewValue.Set(PreviousValue);
		}
	}
}
