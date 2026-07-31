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
	FName BoneName;

	/** 旋转偏移量 */
	FRotator RotOffset;

	/** 位置偏移量 */
	FVector LocOffset;

	/** 动画标志位 */
	uint16 Flags;
};

/**
 * 引力奇点变型载具动画实例代理。
 *
 * 在 PreUpdate 阶段从变型载具基础组件获取最新的模块动画数据，
 * 供 AnimNode 在 AnyThread 上下文中消费。
 */
USTRUCT()
struct SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleAnimationInstanceProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FSingularisMorphVehicleAnimationInstanceProxy() : FAnimInstanceProxy() {}
	FSingularisMorphVehicleAnimationInstanceProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

	void SetModularVehicleComponent(const USingularisMorphVehicleSimulationComponent* InWheeledVehicleComponent);

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;

	const TArray<FSingularisMorphModuleAnimationData>& GetModuleAnimData() const { return ModuleInstances; }

private:
	TArray<FSingularisMorphModuleAnimationData> ModuleInstances;
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

	/** 模块动画数据数组，用于存储各模块的骨骼变换信息 */
	TArray<TArray<FSingularisMorphModuleAnimationData>> ModuleData;

	FSingularisMorphVehicleAnimationInstanceProxy AnimInstanceProxy;

	UPROPERTY(transient)
	TObjectPtr<const USingularisMorphVehicleSimulationComponent> ModularVehicleComponent;

#pragma endregion

public:
#pragma region Constructors

	USingularisMorphVehicleAnimationInstance();

#pragma endregion

#pragma region UAnimInstance Interface

	virtual void NativeInitializeAnimation() override;
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

#pragma endregion

#pragma region API

	/** 获取关联的变型载具集群 Pawn */
	UFUNCTION(BlueprintCallable, Category = "SingularisMorphVehicle|变型载具动画实例|API")
	class ASingularisMorphVehicleClusterPawn* GetVehicle();

	void SetModularVehicleComponent(const USingularisMorphVehicleSimulationComponent* InWheeledVehicleComponent)
	{
		ModularVehicleComponent = InWheeledVehicleComponent;
		AnimInstanceProxy.SetModularVehicleComponent(InWheeledVehicleComponent);
	}

	const USingularisMorphVehicleSimulationComponent* GetModularVehicleComponent() const
	{
		return ModularVehicleComponent;
	}

#pragma endregion
};
