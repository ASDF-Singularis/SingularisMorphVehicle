#pragma once

#include <CoreMinimal.h>
#include <UObject/Object.h>

#include "Types/SingularisMorphVehiclePhysicsAdapterType.h"
#include "SingularisMorphVehiclePhysicsAdapter.generated.h"

class USceneComponent;
class IPhysicsProxyBase;

/**
 * 引力奇点变型载具物理适配器（抽象基类）。
 *
 * 作为 USingularisMorphVehicleSimulationComponent 的 UObject 子对象挂载。
 * 遵循被动查询模式（Pull Model），适配器不持有、不引用 SimulationComponent，
 * 也不主动调用其任何方法。SimComp 按帧调用 SPI 接口拉取数据后自行消费。
 *
 * SPI（Service Provider Interface）设计原则：
 * - 被动查询：适配器仅返回数据，不执行副作用操作。
 * - 单向依赖：适配器对 SimComp 零感知，由 SimComp 通过 FSingularisMorphVehiclePhysicsAdapterContext 注入上下文。
 * - 可替换：不同物理后端（如 ClusterUnion、自定义物理代理）可通过继承本类实现对应适配器。
 * - 职责正交：IsDirty() 暴露变更状态，ConsumeSnapshot() 仅构建快照，两者解耦、互不依赖。
 *
 * 每帧轮询流程（SimComp::PreTickGT）：
 *   IsReady()? → IsDirty()? → ConsumeSnapshot() → RebuildFromSnapshot()
 *
 * 生命周期：
 *   构造 → Initialize(Context) → [IsReady / IsDirty / ConsumeSnapshot 每帧轮询] → Terminate() → 析构
 */
UCLASS(Abstract, NotBlueprintable, BlueprintType, EditInlineNew, CollapseCategories)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehiclePhysicsAdapter : public UObject
{
	GENERATED_BODY()

public:
#pragma region UObject Interface

	/**
	 * 重写 UObject::GetWorld，沿 Outer 链向上查找所属世界。
	 * CDO（类默认对象）上返回 nullptr，避免在无世界上下文中返回无效指针。
	 */
	virtual UWorld* GetWorld() const override;

#pragma endregion

#pragma region SPI

	/**
	 * 初始化适配器。
	 *
	 * 由 SimulationComponent 在适配器创建后调用，传入运行时上下文。
	 * 子类在此方法中完成物理后端的初始化（如创建 ClusterUnion、注册物理代理等）。
	 * 默认实现为空，具体适配器按需覆写。
	 */
	virtual void Initialize(const FSingularisMorphVehiclePhysicsAdapterContext& Context);

	/**
	 * 终止适配器。
	 *
	 * 由 SimulationComponent 在适配器销毁前调用。
	 * 子类在此方法中释放物理后端资源（如销毁 ClusterUnion、注销物理代理等）。
	 * 默认实现为空，具体适配器按需覆写。
	 */
	virtual void Terminate();

	/**
	 * 查询适配器是否就绪。
	 *
	 * SimComp 在 PreTickGT 中每帧优先检查此标志，
	 * 未就绪时跳过后续所有 SPI 查询与模拟树更新。
	 * 默认返回 false，子类在物理后端初始化完成后覆写返回 true。
	 */
	virtual bool IsReady() const;

	/**
	 * 查询适配器是否存在待处理的集群拓扑变更。
	 *
	 * 生命周期函数，由 SimulationComponent 在确认 IsReady() 后每帧调用。
	 * 仅在返回 true 时 SimComp 才进一步调用 ConsumeSnapshot 拉取快照并重建模拟树。
	 * 适配器通过内部事件（如集群组件增删回调）标记脏状态，
	 * 由本函数作为 SPI 对外暴露该状态供消费端查询。
	 * 默认返回 false，具体适配器按需覆写。
	 */
	virtual bool IsDirty() const;

	/**
	 * 获取适配器名称。
	 *
	 * 用于日志输出、调试信息与编辑器面板显示。
	 * 默认返回 "PhysicsAdapter"，子类覆写以提供具体适配器标识（如 "ClusterUnionAdapter"）。
	 */
	virtual FString GetAdapterName() const;

	/**
	 * 获取底层物理代理接口。
	 *
	 * 返回适配器所管理的物理模拟代理对象（如 FClusterUnionPhysicsProxy），
	 * 供 SimComp 在物理场景中注册或访问底层物理数据。
	 * 默认返回 nullptr，具体适配器按需覆写。
	 */
	virtual IPhysicsProxyBase* GetPhysicsProxy() const;

	/**
	 * 获取参考变换。
	 *
	 * 返回适配器对应物理集群的世界空间参考姿态，
	 * 通常为集群根粒子或质心的当前变换。
	 * 默认返回 Identity 变换，具体适配器覆写返回实际参考变换。
	 */
	virtual FTransform GetReferenceTransform() const;

	/**
	 * 构建并返回当前帧的物理集群完整快照。
	 *
	 * 被动拉取接口（Pull Model），由 SimulationComponent 在 PreTickGT 中
	 * 确认 IsDirty() 返回 true 后调用，用于全量重建物理模拟树。
	 * 适配器内部仅负责构建快照，不对脏标记做任何判断——
	 * 脏标记的查询由 IsDirty() 生命周期函数独立承担，两者职责正交。
	 *
	 * 快照语义：
	 * - 调用后快照所有权转移至调用方，适配器内部状态重置。
	 * - 返回的快照用于全量重建物理模拟树，不做增量增删。
	 *
	 * 默认实现返回空快照，具体适配器（如 ClusterUnionAdapter）按需覆写。
	 */
	virtual FSingularisMorphVehiclePhysicsAdapterSnapshot ConsumeSnapshot();

#pragma endregion
};
