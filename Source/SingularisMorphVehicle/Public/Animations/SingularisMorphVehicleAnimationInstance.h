#pragma once

#include <CoreMinimal.h>

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "SingularisMorphVehicleAnimationInstance.generated.h"

class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型模块动画数据。
 * 包含单个模块的骨骼名称、偏移量与标志位。
 */
struct FSingularisMorphModuleAnimationData
{
	/** 关联的骨骼名称 */
	FName BoneName = NAME_None;

	/** 旋转偏移量 */
	FRotator RotOffset = FRotator::ZeroRotator;

	/** 位置偏移量 */
	FVector LocOffset = FVector::ZeroVector;

	/** 动画标志位 */
	uint16 Flags = 0;
};

/**
 * 引力奇点变型载具动画实例代理。
 *
 * 在 PreUpdate（动画线程）阶段按载具组件的模块集合重建实例列表并同步实时动画数据，
 * 供 AnimNode 在同一线程上下文中消费。
 * 游戏线程只持有载具组件引用，不写入本代理的数据，避免与动画线程竞态。
 */
USTRUCT()
struct SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleAnimationInstanceProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FSingularisMorphVehicleAnimationInstanceProxy() : FAnimInstanceProxy() {}
	FSingularisMorphVehicleAnimationInstanceProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;

	const TArray<FSingularisMorphModuleAnimationData>& GetModuleAnimData() const { return ModuleInstances; }

private:
	/** 模块集合是否与当前实例列表不一致（运行时变形会重建模块集合） */
	bool NeedsRebuild(const USingularisMorphVehicleSimulationComponent& Component) const;

	/** 按载具组件的模块动画配置重建实例列表 */
	void RebuildModuleInstances(const USingularisMorphVehicleSimulationComponent& Component);

	/** 同步各模块的实时位置/旋转偏移与动画标志位 */
	void SyncModuleAnimData(const USingularisMorphVehicleSimulationComponent& Component);

	TArray<FSingularisMorphModuleAnimationData> ModuleInstances;

	/** 上次重建实例列表时的模块集合签名（仅动画线程读写） */
	uint32 InstanceSourceSignature = 0;

	/** 实例列表是否已按载具组件的模块集合构建 */
	bool bModuleInstancesValid = false;
};

/**
 * 引力奇点变型载具动画实例。
 *
 * 挂载于变型载具骨骼 Pawn 的骨骼网格体上，
 * 将模块模拟输出数据桥接至动画蓝图。
 */
UCLASS(transient)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleAnimationInstance : public UAnimInstance
{
	GENERATED_BODY()

#pragma region Internal Variable

	FSingularisMorphVehicleAnimationInstanceProxy AnimInstanceProxy;

	UPROPERTY(transient)
	TObjectPtr<const USingularisMorphVehicleSimulationComponent> ModularVehicleComponent;

#pragma endregion

public:
#pragma region UAnimInstance Interface

	virtual void NativeInitializeAnimation() override;
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

#pragma endregion

#pragma region API

	/** 获取关联的变型载具集群 Pawn */
	UFUNCTION(BlueprintCallable, Category = "SingularisMorphVehicle|变型载具动画实例|API")
	class ASingularisMorphVehicleClusterPawn* GetVehicle();

	/** 设置模块动画的数据来源组件（实例列表由动画线程按最新模块集合构建） */
	void SetModularVehicleComponent(const USingularisMorphVehicleSimulationComponent* InWheeledVehicleComponent)
	{
		ModularVehicleComponent = InWheeledVehicleComponent;
	}

	const USingularisMorphVehicleSimulationComponent* GetModularVehicleComponent() const
	{
		return ModularVehicleComponent;
	}

#pragma endregion
};
