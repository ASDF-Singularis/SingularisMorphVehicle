#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisSuspensionSUComponent.generated.h"

/**
 * 引力奇点悬挂仿真单元组件
 *
 * 模拟弹簧-阻尼悬挂系统：悬挂行程为积分状态（非簧载质量模型），车轮在弹簧-阻尼与
 * 轮胎接触力作用下逐步跟随地面，簧上反力与轮胎载荷均由动态行程解出，
 * 并下发给配对的车轮模块决定抓地力。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点悬挂仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API USingularisSuspensionSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 悬挂运动轴（本地空间） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "悬挂轴")
	)
	FVector SuspensionAxis = FVector(0, 0, -1);

	/** 悬挂最大抬起距离（厘米） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "悬挂最大上升")
	)
	float SuspensionMaxRaise = 10.0f;

	/** 悬挂最大下压距离（厘米） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "悬挂最大下降")
	)
	float SuspensionMaxDrop = 30.0f;

	/** 弹簧劲度系数 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "弹簧劲度")
	)
	float SpringRate = 200.0f;

	/** 弹簧预载力 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "弹簧预载")
	)
	float SpringPreload = 50.0f;

	/** 非簧载质量（千克）：车轮沿悬挂轴的等效质量，决定车轮对地面的跟随快慢 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "非簧载质量", ClampMin = "1.0", UIMin = "1.0")
	)
	float UnsprungMass = 40.0f;

	/** 簧载质量（千克，单车轮份额）：取整车质量除以悬挂数，仅用于推算阻尼系数 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "簧载质量", ClampMin = "1.0", UIMin = "1.0")
	)
	float SprungMass = 375.0f;

	/** 轮胎径向刚度相对弹簧劲度的倍率；实车轮胎刚度约为车轮速率的 8-12 倍 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "轮胎刚度倍率", ClampMin = "0.0", UIMin = "0.0")
	)
	float TireStiffnessRatio = 10.0f;

	/** 轮胎径向阻尼比（0-1，相对轮胎-车轮临界阻尼）：抑制车轮触地弹跳 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "轮胎阻尼比", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0")
	)
	float TireDampingRatio = 0.1f;

	/** 回弹阻尼比（0-1，相对临界阻尼）：抑制车体回弹，通常大于压缩阻尼比 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "回弹阻尼比", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0")
	)
	float ReboundDampingRatio = 0.3f;

	/** 压缩阻尼比（0-1，相对临界阻尼）：抑制冲击传入车体，通常小于回弹阻尼比 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "压缩阻尼比", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0")
	)
	float CompressionDampingRatio = 0.15f;

	/** 悬挂力效应（将车轮压向地面的力） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点悬挂仿真单元",
		meta = (DisplayName = "悬挂力效应")
	)
	float SuspensionForceEffect = 100.0f;

#pragma endregion

#pragma region Constructors

	USingularisSuspensionSUComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Suspension;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
