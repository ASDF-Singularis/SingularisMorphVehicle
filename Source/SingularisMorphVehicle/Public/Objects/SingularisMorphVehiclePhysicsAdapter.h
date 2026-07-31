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
 * 作为 SimulationComponent 的 UObject 子对象。
 * 不持有、不引用 SimulationComponent，不主动调用任何 SimComp 方法。
 * 由 SimComp 调用 SPI 方法获取数据后自行消费。
 *
 * SPI（Service Provider Interface）：
 *   被动查询接口，适配器仅返回数据，不执行副作用操作。
 */
UCLASS(Abstract, NotBlueprintable, EditInlineNew, CollapseCategories)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehiclePhysicsAdapter : public UObject
{
	GENERATED_BODY()

public:
#pragma region UObject Interface

	virtual UWorld* GetWorld() const override;

#pragma endregion

#pragma region SPI

	virtual void Initialize(const FSingularisMorphVehiclePhysicsAdapterContext& Context);

	virtual void Terminate();

	virtual bool IsReady() const;

	virtual FString GetAdapterName() const;

	virtual IPhysicsProxyBase* GetPhysicsProxy() const;

	virtual FTransform GetReferenceTransform() const;

	/**
	 * 拉取完整快照用于全量重建物理模拟树。
	 *
	 * 被动拉取接口（Pull Model）：SimComp 每帧在 PreTickGT 中调用。
	 * 适配器仅在集群发生变更时（脏标记为真）查询集群当前状态并构建完整快照；
	 * 无变更时返回空快照，避免不必要的重建开销。
	 * 默认实现返回空快照，具体适配器（如 ClusterUnionAdapter）重写。
	 */
	virtual FSingularisMorphVehiclePhysicsAdapterSnapshot ConsumeSnapshot();

#pragma endregion
};
