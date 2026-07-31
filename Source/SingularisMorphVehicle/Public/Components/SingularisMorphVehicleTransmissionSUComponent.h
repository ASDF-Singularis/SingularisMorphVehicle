#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSUComponent.h"
#include "SingularisMorphVehicleTransmissionSUComponent.generated.h"

#pragma region 委托签名

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnGearChangeNative, int32, int32);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGearChange, int32, Guid, int32, CurrentGear);

#pragma endregion

/**
 * 引力奇点变型载具变速箱仿真单元组件
 *
 * 管理前进/倒车齿轮比率与自动/手动换挡逻辑。
 * 通过齿轮比倍率将引擎扭矩传递至车轮驱动系统。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具变速箱仿真单元组件")
)
class SINGULARISMORPHVEHICLE_API
	USingularisMorphVehicleTransmissionSUComponent : public USingularisMorphVehicleSUComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 前进挡齿轮比列表 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|齿轮比",
		meta = (DisplayName = "前进挡比")
	)
	TArray<float> ForwardRatios = {2.85f, 2.02f, 1.35f, 1.0f};

	/** 倒车挡齿轮比列表 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|齿轮比",
		meta = (DisplayName = "倒挡比")
	)
	TArray<float> ReverseRatios = {2.86f};

	/** 主减速比 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|齿轮比",
		meta = (DisplayName = "主减速比")
	)
	float FinalDriveRatio = 3.08f;

	/** 升挡转速（RPM） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|换挡",
		meta = (DisplayName = "升挡RPM")
	)
	int32 ChangeUpRPM = 4500;

	/** 降挡转速（RPM） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|换挡",
		meta = (DisplayName = "降挡RPM")
	)
	int32 ChangeDownRPM = 1600;

	/** 换挡时间（秒） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|换挡",
		meta = (DisplayName = "换挡时间")
	)
	float GearChangeTime = 0.5f;

	/** 挡位滞后时间（秒） */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|换挡",
		meta = (DisplayName = "滞后时间")
	)
	float GearHysteresisTime = 2.0f;

	/** 变速箱传动效率 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|传动",
		meta = (DisplayName = "传动效率")
	)
	float TransmissionEfficiency = 0.9f;

	/** 变速箱类型 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|传动",
		meta = (DisplayName = "变速箱类型")
	)
	ESingularisMorphVehicleTransmissionType TransmissionType = ESingularisMorphVehicleTransmissionType::Automatic;

	/** 是否自动倒挡 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|传动",
		meta = (DisplayName = "自动倒挡")
	)
	bool AutoReverse = true;

#pragma endregion

#pragma region 事件分发器

	/** 快速（低开销）原生版本的换挡委托 */
	FOnGearChangeNative OnGearChangeNativeEvent;

	/** 蓝图表单的换挡事件 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "SingularisMorphVehicle|引力奇点变型载具变速箱仿真单元|事件分发器",
		meta = (DisplayName = "挡位变化")
	)
	FOnGearChange OnGearChangeEvent{};

#pragma endregion

#pragma region Constructors

	USingularisMorphVehicleTransmissionSUComponent();

#pragma endregion

#pragma region ISingularisMorphVehicleBaseInterface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Transmission;
	}

	virtual void OnOutputReady(const Chaos::FSimOutputData* OutputData) override;
	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
