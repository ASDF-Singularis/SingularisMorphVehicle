#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleBaseComponent.h"
#include "SingularisTransmissionComponent.generated.h"

#pragma region 委托签名

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnGearChangeNative, int32, int32);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGearChange, int32, Guid, int32, CurrentGear);

#pragma endregion

/**
 * 引力奇点变速箱组件
 *
 * 管理前进/倒车齿轮比率与自动/手动换挡逻辑。
 * 通过齿轮比倍率将引擎扭矩传递至车轮驱动系统。
 * 自动换挡点始终按原动机可用转速区间与相邻挡位齿比步长动态计算，
 * 换装不同引擎后无需重新标定固定 RPM；仅保留升降挡比例作为手感调节入口。
 * 载具创建时默认空挡，自动变速箱只有在油门超过起步阈值或请求倒挡时才挂入方向挡。
 * 自动倒挡不再是组件配置：自动变速箱在存在有效倒挡与主减速比时固定启用，
 * 手动变速箱方向由 ChangeUp/ChangeDown 脉冲逐挡决定。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("SingularisMorphVehicle"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变速箱组件")
)
class SINGULARISMORPHVEHICLE_API USingularisTransmissionComponent : public USingularisMorphVehicleBaseComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 前进挡齿轮比列表 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|齿轮比",
		meta = (DisplayName = "前进挡比")
	)
	TArray<float> ForwardRatios = {2.85f, 2.02f, 1.35f, 1.0f};

	/** 倒车挡齿轮比列表 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|齿轮比",
		meta = (DisplayName = "倒挡比")
	)
	TArray<float> ReverseRatios = {2.86f};

	/** 主减速比 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|齿轮比",
		meta = (DisplayName = "主减速比")
	)
	float FinalDriveRatio = 3.08f;

	/** 升挡转速占原动机可用转速带（怠速 → 最大转速）的比例；仅自动变速箱类型生效 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|换挡",
		meta = (
			DisplayName = "升挡转速比例",
			ClampMin = "0.1",
			ClampMax = "1.0",
			UIMin = "0.1",
			UIMax = "1.0"
		)
	)
	float ShiftUpRPMRatio = 0.9f;

	/**
	 * 降挡转速占升挡转速的比例上限
	 *
	 * 实际降挡点还会按相邻挡位齿比步长压低，确保降挡后的转速仍低于升挡点，
	 * 避免降挡后立即触发升挡而在相邻挡位间往复摆动。
	 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|换挡",
		meta = (
			DisplayName = "降挡转速比例",
			ClampMin = "0.0",
			ClampMax = "1.0",
			UIMin = "0.0",
			UIMax = "1.0"
		)
	)
	float ShiftDownRPMRatio = 0.55f;

	/** 换挡时间（秒） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|换挡",
		meta = (DisplayName = "换挡时间")
	)
	float GearChangeTime = 0.5f;

	/** 挡位滞后时间（秒） */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|换挡",
		meta = (DisplayName = "滞后时间")
	)
	float GearHysteresisTime = 2.0f;

	/** 变速箱传动效率 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|传动",
		meta = (DisplayName = "传动效率")
	)
	float TransmissionEfficiency = 0.9f;

	/** 变速箱类型 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变速箱组件|传动",
		meta = (DisplayName = "变速箱类型")
	)
	ESingularisMorphVehicleTransmissionType TransmissionType = ESingularisMorphVehicleTransmissionType::Automatic;

#pragma endregion

#pragma region 事件分发器

	/** 快速（低开销）原生版本的换挡委托 */
	FOnGearChangeNative OnGearChangeNativeEvent{};

	/** 蓝图表单的换挡事件 */
	UPROPERTY(
		BlueprintAssignable,
		Category = "引力奇点变速箱组件|事件分发器",
		meta = (DisplayName = "挡位变化")
	)
	FOnGearChange OnGearChangeEvent{};

#pragma endregion

#pragma region Constructors

	USingularisTransmissionComponent();

#pragma endregion

#pragma region SingularisMorphVehicleSU Interface

	virtual ESingularisMorphVehicleModuleType GetModuleType() const override
	{
		return ESingularisMorphVehicleModuleType::Transmission;
	}

	virtual void OnOutputReady(const Chaos::FSimOutputData* OutputData) override;

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;

#pragma endregion
};
