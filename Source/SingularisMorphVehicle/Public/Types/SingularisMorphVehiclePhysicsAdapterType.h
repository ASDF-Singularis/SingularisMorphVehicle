#pragma once

#include <CoreMinimal.h>
#include <Components/PrimitiveComponent.h>

#include "SingularisMorphVehiclePhysicsAdapterType.generated.h"

class USingularisMorphVehicleSUComponent;
class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具物理适配器上下文。
 *
 * 由 SimulationComponent 在调用适配器 SPI 方法时注入，
 * 提供适配器所需的运行时访问入口，避免适配器直接持有 SimulationComponent 引用。
 */
USTRUCT(BlueprintType)
struct SINGULARISMORPHVEHICLE_API FSingularisMorphVehiclePhysicsAdapterContext
{
	GENERATED_BODY()

	/** 宿主仿真组件 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<USingularisMorphVehicleSimulationComponent> SimulationComponent = nullptr;
};

/**
 * 引力奇点变型载具物理适配器快照实体。
 *
 * 描述集群中单个物理组件及其粒子数据，是重建物理模拟树的数据单元。
 * 适配器在集群变更后重建完整快照，
 * 由 SimulationComponent 在 PreTickGT 中通过 ConsumeSnapshot 拉取并全量重建模拟树。
 *
 * 物理模拟树结构由模块类型确定，全量重建保证父子关系正确性。
 */
USTRUCT(BlueprintType)
struct SINGULARISMORPHVEHICLE_API FSingularisMorphVehiclePhysicsAdapterSnapshotEntity
{
	GENERATED_BODY()

	/**
	 * 集群中物理组件对应的 SU 模块组件。
	 * 由适配器在构建快照时通过 Subsystem 完成映射，
	 * 消费端直接使用，无需二次查找。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<USingularisMorphVehicleSUComponent> SUComponent = nullptr;

	/**
	 * 集群内粒子索引。
	 * 用于定位 ChildParticles 数组中的 ChildToParent 变换。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 ParticleIndex = INDEX_NONE;

	/**
	 * 相对于父粒子的局部变换。
	 * 源自 ClusterUnionChildData::ChildToParent，用于初始化模块空间姿态。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FTransform ChildToParent = FTransform::Identity;
};

/**
 * 引力奇点变型载具物理适配器完整快照。
 *
 * 由 Adapter::ConsumeSnapshot() 返回，包含当前全部应存在于物理模拟树的模块实体。
 * 消费端基于此快照全量重建模拟树，而非增量增删：
 * 物理树结构由模块类型确定，增量增删无法保证重组后的父子关系。
 * 生命周期：适配器内部构建 → ConsumeSnapshot 移出 → SimComp 消费 → 析构。
 */
USTRUCT(BlueprintType)
struct SINGULARISMORPHVEHICLE_API FSingularisMorphVehiclePhysicsAdapterSnapshot
{
	GENERATED_BODY()

	/**
	 * 当前集群中所有有效模块实体的完整列表。
	 * 消费端按遍历顺序全量重建模拟树。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FSingularisMorphVehiclePhysicsAdapterSnapshotEntity> Entities;

	/** 实体列表为空时返回 true，消费端据此跳过重建 */
	bool IsEmpty() const { return Entities.IsEmpty(); }

	/** 清空所有实体并释放内存 */
	void Reset() { Entities.Reset(); }
};
