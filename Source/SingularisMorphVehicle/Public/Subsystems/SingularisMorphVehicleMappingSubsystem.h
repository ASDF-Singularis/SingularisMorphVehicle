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
	 * 物理组件 → SU 组件列表的弱引用映射表。
	 * 一个物理组件可对应多个 SU（骨骼网格体的多个物理体/骨骼可分别挂载不同模拟单元）。
	 * 双重弱引用：键和值均不阻止 GC，避免生命周期死锁。
	 */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, TArray<TWeakObjectPtr<USingularisMorphVehicleSUComponent>>>
	ComponentMap{};

#pragma endregion

public:
#pragma region API

	/**
	 * 注册物理组件到 SU 组件的映射。
	 *
	 * 由 SUComponent::BeginPlay 调用。同一物理组件可注册多个 SU，
	 * 重复注册同一 SU 会被去重。使用弱引用存储双方，不阻止 GC 回收。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具子系统|API",
		meta = (DisplayName = "注册物理组件映射")
	)
	void RegisterComponentMapping(UPrimitiveComponent* PrimComp, USingularisMorphVehicleSUComponent* SUComp);

	/**
	 * 注销指定物理组件与指定 SU 的映射。
	 *
	 * 由 SUComponent::EndPlay 调用。仅移除该 SU 的条目，
	 * 不影响共享同一物理组件的其它 SU；列表清空后移除整个键。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具子系统|API",
		meta = (DisplayName = "注销 SU 映射")
	)
	void UnregisterComponentMapping(
		UPrimitiveComponent* PrimComp,
		USingularisMorphVehicleSUComponent* SUComp
	);

	/**
	 * 通过物理组件查找对应的全部 SU 组件。
	 *
	 * O(1) 哈希查找后过滤已过期的弱引用，返回有效 SU 列表。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具子系统|API",
		meta = (DisplayName = "查找 SU 组件列表")
	)
	TArray<USingularisMorphVehicleSUComponent*> FindSUComponents(UPrimitiveComponent* PrimComp) const;

	/**
	 * 通过物理组件查找第一个有效的 SU 组件。
	 *
	 * 兼容单映射场景（如集群几何体各部件只挂一个 SU），
	 * 未找到或弱引用已过期时返回 nullptr。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "SingularisMorphVehicle|引力奇点变型载具子系统|API",
		meta = (DisplayName = "查找 SU 组件")
	)
	USingularisMorphVehicleSUComponent* FindSUComponent(UPrimitiveComponent* PrimComp) const;

#pragma endregion
};
