#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisSuspensionSUComponent.generated.h"

/**
 * 引力奇点悬挂仿真单元组件
 *
 * 模拟弹簧-阻尼悬挂系统，通过射线/球体碰撞检测确定悬挂压缩量，
 * 将车重传递至地面并产生抓地力。与车轮模块协同工作。
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
		Category = "SingularisMorphVehicle|引力奇点悬挂仿真单元|行程",
		meta = (DisplayName = "悬挂轴")
	)
	FVector SuspensionAxis = FVector(0, 0, -1);

	/** 悬挂最大抬起距离（厘米） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点悬挂仿真单元|行程",
		meta = (DisplayName = "悬挂最大上升")
	)
	float SuspensionMaxRaise = 10.0f;

	/** 悬挂最大下压距离（厘米） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点悬挂仿真单元|行程",
		meta = (DisplayName = "悬挂最大下降")
	)
	float SuspensionMaxDrop = 30.0f;

	/** 弹簧劲度系数 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点悬挂仿真单元|弹簧力学",
		meta = (DisplayName = "弹簧劲度")
	)
	float SpringRate = 200.0f;

	/** 弹簧预载力 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点悬挂仿真单元|弹簧力学",
		meta = (DisplayName = "弹簧预载")
	)
	float SpringPreload = 50.0f;

	/** 弹簧阻尼系数 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点悬挂仿真单元|弹簧力学",
		meta = (DisplayName = "弹簧阻尼")
	)
	float SpringDamping = 0.9f;

	/** 悬挂力效应（将车轮压向地面的力） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点悬挂仿真单元|弹簧力学",
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
