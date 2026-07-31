#pragma once

#include <Engine/EngineTypes.h>

#include "Objects/SingularisMorphVehiclePhysicsAdapter.h"
#include "SingularisMorphVehicleClusterUnionAdapter.generated.h"

struct FClusterUnionBoneData;
class UPrimitiveComponent;
class USingularisMorphVehicleClusterUnionComponent;

/**
 * 引力奇点变型载具集群联合物理适配器。
 *
 * 桥接集群联合组件与仿真组件。
 * 集群增删事件触发脏标记，ConsumeSnapshot 返回当前集群的完整组件快照，
 * 由 SimComp 通过 ConsumeSnapshot 拉取并全量重建物理模拟树。
 *
 * 适配器本身不调用 SimComp 任何方法——仅被动接收集群事件、
 * 等待 SimComp 拉取，完全符合 SPI 设计约束。
 */
UCLASS(NotBlueprintable, BlueprintType)
class SINGULARISMORPHVEHICLE_API
	USingularisMorphVehicleClusterUnionAdapter : public USingularisMorphVehiclePhysicsAdapter
{
	GENERATED_BODY()

public:
#pragma region Parameter

	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具集群联合适配器|参数",
		meta = (DisplayName = "集群联合组件", UseComponentPicker, AllowedClasses = "/Script/Engine.ClusterUnionComponent")
	)
	FComponentReference ClusterUnionComponentReference{};

#pragma endregion

private:
#pragma region Internal Variable

	TWeakObjectPtr<USingularisMorphVehicleClusterUnionComponent> ClusterUnionComponent = nullptr;

	/** 集群变更脏标记，下次 ConsumeSnapshot 时重建完整快照并清零 */
	bool bDirty = false;

	/** 防止 Initialize 重复绑定事件 */
	bool bEventsBound = false;

#pragma endregion

public:
#pragma region SPI

	virtual void Initialize(const FSingularisMorphVehiclePhysicsAdapterContext& Context) override;
	virtual void Terminate() override;

	virtual bool IsReady() const override;
	virtual FString GetAdapterName() const override;
	virtual IPhysicsProxyBase* GetPhysicsProxy() const override;
	virtual FTransform GetReferenceTransform() const override;
	virtual FSingularisMorphVehiclePhysicsAdapterSnapshot ConsumeSnapshot() override;

#pragma endregion

private:
#pragma region Internal Function

	void ResolveClusterUnionComponent(AActor* Owner);

#pragma endregion

#pragma region Callback

	UFUNCTION()
	void OnClusterComponentAdded(
		UPrimitiveComponent* Component,
		const TArray<FClusterUnionBoneData>& BonesData,
		const TArray<FClusterUnionBoneData>& RemovedBoneIDs,
		bool bIsNew
	);

	UFUNCTION()
	void OnClusterComponentRemoved(
		UPrimitiveComponent* Component,
		const TArray<FClusterUnionBoneData>& RemovedBonesData
	);

#pragma endregion
};
