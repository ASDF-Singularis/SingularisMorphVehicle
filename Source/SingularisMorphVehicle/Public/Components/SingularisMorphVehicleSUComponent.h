#pragma once

#include <CoreMinimal.h>
#include <Components/ActorComponent.h>
#include <SimModule/ModuleInput.h>

#include "Interfaces/SingularisMorphVehicleBaseInterface.h"
#include "SingularisMorphVehicleSUComponent.generated.h"

/**
 * 引力奇点变型基础模拟单元组件
 *
 * 继承自 UActorComponent，作为纯数据配置容器为载具模拟模块提供实现基类。
 * 通过显式引用字段关联视觉组件与上下游模块，不再依赖 SceneComponent 附着层级。
 */
UCLASS(Abstract, Blueprintable)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleSUComponent : public UActorComponent,
                                                                      public ISingularisMorphVehicleBaseInterface
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 是否启用动画驱动 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型基础模拟单元|动画",
		meta = (DisplayName = "启用动画")
	)
	bool bAnimationEnabled = false;

	/** 关联的骨骼名称，用于动画驱动 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型基础模拟单元|动画",
		meta = (DisplayName = "骨骼名称", EditCondition = "bAnimationEnabled")
	)
	FName BoneName = NAME_None;

	/** 动画偏移量 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型基础模拟单元|动画",
		meta = (DisplayName = "动画偏移", EditCondition = "bAnimationEnabled")
	)
	FVector AnimationOffset = FVector::ZeroVector;

	/** 模块输入配置 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型基础模拟单元|输入",
		meta = (DisplayName = "输入配置")
	)
	TArray<FModuleInputSetup> InputConfig{};

	/** 代理组件引用：承载此模块动画输出的场景组件，同时作为空间变换的数据源 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型基础模拟单元|代理",
		meta = (DisplayName = "代理组件", UseComponentPicker, AllowedClasses = "/Script/Engine.SceneComponent")
	)
	FComponentReference ProxyComponent{};

	/** 叠加在代理组件变换之上的额外偏移量（有代理组件时为增量偏移，无代理组件时作为绝对变换） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型基础模拟单元|偏移",
		meta = (DisplayName = "变换偏移")
	)
	FTransform TransformOffset = FTransform::Identity;

#pragma endregion

private:
#pragma region Internal Variable

	int32 AnimationSetupIndex = INDEX_NONE;
	int32 TreeIndex = INDEX_NONE;

	/**
	 * 仿真模块 GUID。
	 * 由 SimulationComponent::AddModuleToTree 创建模块后回写，
	 * 用于 RemoveSimulationModule 的精确移除定位。
	 * 移除操作依赖此值查找对应模块，误改将导致模块泄露。
	 */
	int32 ModuleGuid = INDEX_NONE;

#pragma endregion

public:
#pragma region Constructors

	USingularisMorphVehicleSUComponent();

#pragma endregion

#pragma region ActorComponent Interface

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#pragma endregion

#pragma region ISingularisMorphVehicleBaseInterface

	virtual FName GetBoneName() const override { return BoneName; }

	virtual bool GetAnimationEnabled() const override { return bAnimationEnabled; }
	virtual void SetAnimationEnabled(const bool AnimationEnabledIn) override { bAnimationEnabled = AnimationEnabledIn; }
	virtual int32 GetAnimationSetupIndex() const override { return AnimationSetupIndex; }
	virtual const FVector& GetAnimationOffset() const override { return AnimationOffset; }

	virtual TArray<FModuleInputSetup> GetInputConfig() const override { return InputConfig; }

	virtual int32 GetTreeIndex() const override { return TreeIndex; }
	virtual void SetTreeIndex(const int32 NewValue) override;

	virtual int32 GetModuleGuid() const override { return ModuleGuid; }
	virtual void SetModuleGuid(const int32 NewValue) override { ModuleGuid = NewValue; }

#pragma endregion
};
