#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleType.generated.h"

/**
 * 引力奇点变型载具模块类型
 */
UENUM(BlueprintType)
enum class ESingularisMorphVehicleModuleType : uint8
{
	Undefined UMETA(DisplayName = "未定义"),
	Chassis UMETA(DisplayName = "底盘"),
	Thruster UMETA(DisplayName = "推进器"),
	Aerofoil UMETA(DisplayName = "翼型"),
	Wheel UMETA(DisplayName = "车轮"),
	Suspension UMETA(DisplayName = "悬挂"),
	Axle UMETA(DisplayName = "轮轴"),
	Transmission UMETA(DisplayName = "变速箱"),
	Engine UMETA(DisplayName = "引擎"),
	Motor UMETA(DisplayName = "电机"),
	Clutch UMETA(DisplayName = "离合器"),
	Wing UMETA(DisplayName = "机翼"),
	Rudder UMETA(DisplayName = "方向舵"),
	Elevator UMETA(DisplayName = "升降舵"),
	Propeller UMETA(DisplayName = "螺旋桨"),
	Balloon UMETA(DisplayName = "气球")
};

/**
 * 引力奇点变型载具翼型类型
 */
UENUM(BlueprintType)
enum class ESingularisMorphVehicleAerofoilType : uint8
{
	Fixed UMETA(DisplayName = "固定翼"),
	Wing UMETA(DisplayName = "机翼（受Roll输入影响）"),
	Rudder UMETA(DisplayName = "方向舵（受Yaw输入影响）"),
	Elevator UMETA(DisplayName = "升降舵（受Pitch输入影响）")
};

/**
 * 引力奇点变型载具车轮轴向类型
 */
UENUM(BlueprintType)
enum class ESingularisMorphVehicleWheelAxisType : uint8
{
	X UMETA(DisplayName = "X轴"),
	Y UMETA(DisplayName = "Y轴"),
	Z UMETA(DisplayName = "Z轴")
};

/**
 * 引力奇点变型载具变速箱类型
 */
UENUM(BlueprintType)
enum class ESingularisMorphVehicleTransmissionType : uint8
{
	Manual UMETA(DisplayName = "手动"),
	Automatic UMETA(DisplayName = "自动")
};
