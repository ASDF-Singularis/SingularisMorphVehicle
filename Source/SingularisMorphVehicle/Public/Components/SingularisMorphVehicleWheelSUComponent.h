#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisMorphVehicleWheelSUComponent.generated.h"

#pragma region 委托签名

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnWheelTouchChangeNative, int32, bool);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnWheelTouchChange, int32, Guid, bool, IsInContact);

#pragma endregion

/**
 * 引力奇点变型载具车轮仿真单元组件
 *
 * 模拟车轮的转动、摩擦、转向与制动行为。支持 ABS、牵引力控制、
 * 手刹等高级特性。通过物理线程与悬挂模块协同工作，
 * 输出数据通过 OnWheelTouchChange 事件回调至游戏线程。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具车轮仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API
	USingularisMorphVehicleWheelSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 车轮半径（厘米） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|几何",
		meta = (DisplayName = "车轮半径")
	)
	float WheelRadius = 30.0f;

	/** 车轮宽度（厘米） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|几何",
		meta = (DisplayName = "车轮宽度")
	)
	float WheelWidth = 20.0f;

	/** 轴向类型 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|几何",
		meta = (DisplayName = "轴向类型")
	)
	ESingularisMorphVehicleWheelAxisType AxisType = ESingularisMorphVehicleWheelAxisType::Y;

	/** 车轮转动惯性 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|几何",
		meta = (DisplayName = "车轮惯性")
	)
	float WheelInertia = 10.0f;

	/** 摩擦系数倍率 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|摩擦",
		meta = (DisplayName = "摩擦倍率")
	)
	float FrictionMultiplier = 2.0f;

	/** 侧偏刚度 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|摩擦",
		meta = (DisplayName = "侧偏刚度")
	)
	float CorneringStiffness = 1000.0f;

	/** 侧滑角上限 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|摩擦",
		meta = (DisplayName = "侧滑角上限")
	)
	float SlipAngleLimit = 8.0f;

	/** 是否启用 ABS */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|辅助",
		meta = (DisplayName = "启用ABS")
	)
	bool bABSEnabled = true;

	/** 是否启用牵引力控制 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|辅助",
		meta = (DisplayName = "启用牵引力控制")
	)
	bool bTractionControlEnabled = true;

	/** 最大制动扭矩 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|制动",
		meta = (DisplayName = "最大制动扭矩")
	)
	float MaxBrakeTorque = 2000.0f;

	/** 是否启用手刹 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|制动",
		meta = (DisplayName = "启用手刹")
	)
	bool bHandbrakeEnabled = false;

	/** 手刹扭矩 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|制动",
		meta = (DisplayName = "手刹扭矩", EditCondition = "bHandbrakeEnabled")
	)
	float HandbrakeTorque = 2000.0f;

	/** 是否启用转向 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|转向",
		meta = (DisplayName = "启用转向")
	)
	bool bSteeringEnabled = false;

	/** 最大转向角度 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|转向",
		meta = (DisplayName = "最大转向角度", EditCondition = "bSteeringEnabled")
	)
	float MaxSteeringAngle = 35.0f;

	/** 力作用点偏移 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|偏移",
		meta = (DisplayName = "力偏移")
	)
	FVector ForceOffset = FVector::ZeroVector;

	/** 反转旋转方向 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|偏移",
		meta = (DisplayName = "反转方向")
	)
	bool ReverseDirection = false;

	/** 链接的悬挂组件（单向引用） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|链接",
		meta = (
			DisplayName = "链接悬挂",
			UseComponentPicker,
			AllowedClasses = "/Script/SingularisMorphVehicle.SingularisMorphVehicleSuspensionComponent"
		)
	)
	FComponentReference LinkedSuspension{};

#pragma endregion

#pragma region 事件分发器

	/** 快速（低开销）原生版本的车轮触地变化委托 */
	FOnWheelTouchChangeNative OnWheelTouchChangeNativeEvent;

	/** 蓝图表单的车轮触地变化事件 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "SingularisMorphVehicle|引力奇点变型载具车轮仿真单元|事件分发器",
		meta = (DisplayName = "触地变化")
	)
	FOnWheelTouchChange OnWheelTouchChangeEvent;

#pragma endregion

#pragma region Constructors

	USingularisMorphVehicleWheelSUComponent();

#pragma endregion

#pragma region ISingularisMorphVehicleBaseInterface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Wheel;
	}

	virtual void OnOutputReady(const Chaos::FSimOutputData* OutputData) override;
	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
