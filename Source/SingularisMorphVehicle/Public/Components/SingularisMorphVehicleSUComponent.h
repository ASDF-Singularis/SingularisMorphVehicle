#pragma once

#include <CoreMinimal.h>
#include <Components/ActorComponent.h>
#include <SimModule/ModuleInput.h>
#include <SimModule/SimulationModuleBase.h>

#include "Interfaces/SingularisMorphVehicleSUInterface.h"
#include "SingularisMorphVehicleSUComponent.generated.h"

/**
 * 引力奇点变形载具基础仿真单元组件
 *
 * 继承自 UActorComponent，作为纯数据配置容器为载具模拟模块提供实现基类。
 * 通过显式引用字段关联视觉组件与上下游模块，不再依赖 SceneComponent 附着层级。
 */
UCLASS(Abstract, Blueprintable)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleSUComponent : public UActorComponent,
                                                                      public ISingularisMorphVehicleSUInterface
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 是否启用动画驱动 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点基础仿真单元",
		meta = (DisplayName = "启用动画")
	)
	bool bAnimationEnabled = false;

	/** 关联的骨骼名称，用于动画驱动 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点基础仿真单元",
		meta = (DisplayName = "骨骼名称", EditCondition = "bAnimationEnabled")
	)
	FName BoneName = NAME_None;

	/** 模块输入配置 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点基础仿真单元",
		meta = (DisplayName = "输入配置")
	)
	TArray<FModuleInputSetup> InputConfig{};

	/** 动画偏移量 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点基础仿真单元",
		meta = (DisplayName = "动画偏移", EditCondition = "bAnimationEnabled")
	)
	FVector AnimationOffset = FVector::ZeroVector;

	/** 叠加在代理组件变换之上的增量偏移；代理组件缺失时叠加在 Actor 根组件变换上 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点基础仿真单元",
		meta = (DisplayName = "变换偏移")
	)
	FTransform TransformOffset = FTransform::Identity;

	/** 驱动组件引用 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点基础仿真单元",
		meta = (DisplayName = "驱动组件", UseComponentPicker, AllowedClasses = "/Script/Engine.SceneComponent")
	)
	FComponentReference DrivenComponent{};

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

	/**
	 * 模块积分状态（车轮转角/转速、挡位、离合器、悬挂压缩等）。
	 *
	 * 拓扑重建会销毁并按几何重新创建模块，而积分量不随几何重算，
	 * 故重建前由 SimulationComponent 调 GenerateNetData/FillNetState 采集到此处，
	 * 新建模块后以 FillSimState 回填；不转移则每次增删部件都会让已在旋转的车轮
	 * 转角突跳（旋转抖动）、掉挡与动力中断。
	 * 每次重建被最新状态覆盖；模块暂时消失（部件离簇）时状态保留，
	 * 重新入簇后继续沿用；未提供网络数据的模块（底盘、翼型、轮轴、电机等）不参与转移。
	 * 该数据仅用于本地重建间的状态传递，不进入网络复制结构。
	 */
	TSharedPtr<Chaos::FModuleNetData> ModuleSimState{};

#pragma endregion

public:
#pragma region Constructors

	USingularisMorphVehicleSUComponent();

#pragma endregion

#pragma region ActorComponent Interface

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#pragma endregion

#pragma region Module State

	/** 获取采集的模块积分状态（尚未采集时为 nullptr） */
	const TSharedPtr<Chaos::FModuleNetData>& GetModuleSimState() const { return ModuleSimState; }

	/** 采集模块积分状态，供拓扑重建后回填到新建模块 */
	void SetModuleSimState(TSharedPtr<Chaos::FModuleNetData> InState) { ModuleSimState = MoveTemp(InState); }

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

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
