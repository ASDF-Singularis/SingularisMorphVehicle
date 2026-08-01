#pragma once

#include <CoreMinimal.h>
#include <Subsystems/WorldSubsystem.h>

#include "SingularisMorphVehicleMappingSubsystem.generated.h"

class USingularisMorphVehicleSUComponent;
class UPrimitiveComponent;

/**
 * 引力奇点变型载具子系统。
 *
 * 作为 UWorldSubsystem 中介注册表，维护从集群物理组件到
 * 仿真模块单元组件的弱引用映射，供物理适配器 Delta 消费时
 * O(1) 反向查找。组件在 BeginPlay/EndPlay 中自行注册/注销。
 */
UCLASS(NotBlueprintable, BlueprintType)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleMappingSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

#pragma region Internal Variable

	/**
	 * 物理组件 → SU 组件的弱引用映射表。
	 * 双重弱引用：键和值均不阻止 GC，避免生命周期死锁。
	 */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, TWeakObjectPtr<USingularisMorphVehicleSUComponent>> ComponentMap{};

#pragma endregion

public:
#pragma region API

	/**
	 * 注册物理组件到 SU 组件的映射。
	 *
	 * 由 SUComponent::BeginPlay 调用。同一物理组件多次注册时覆盖前次映射。
	 * 使用弱引用存储双方，不阻止 GC 回收。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具子系统|API",
		meta = (DisplayName = "注册物理组件映射")
	)
	void RegisterComponentMapping(UPrimitiveComponent* PrimComp, USingularisMorphVehicleSUComponent* SUComp);

	/**
	 * 注销指定物理组件的映射。
	 *
	 * 由 SUComponent::EndPlay 调用。已不存在的键会被忽略。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具子系统|API",
		meta = (DisplayName = "注销物理组件映射")
	)
	void UnregisterComponentMapping(UPrimitiveComponent* PrimComp);

	/**
	 * 通过物理组件查找对应的 SU 组件。
	 *
	 * O(1) 哈希查找，未找到或弱引用已过期时返回 nullptr。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具子系统|API",
		meta = (DisplayName = "查找 SU 组件")
	)
	USingularisMorphVehicleSUComponent* FindSUComponent(UPrimitiveComponent* PrimComp) const;

#pragma endregion
};
