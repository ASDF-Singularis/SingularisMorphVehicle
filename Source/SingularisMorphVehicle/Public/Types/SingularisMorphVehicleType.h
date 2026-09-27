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
 *
 * 仅包含 ChaosVehicles 车轮模块支持的两种纵轴约定，
 * 不暴露模块无法处理的取值以避免静默错配。
 */
UENUM(BlueprintType)
enum class ESingularisMorphVehicleWheelAxisType : uint8
{
	X UMETA(DisplayName = "X轴"),
	Y UMETA(DisplayName = "Y轴")
};

/**
 * 引力奇点变型载具转向几何类型
 *
 * 决定同一转向轴上内外侧车轮的目标转角分配。转角本身始终经角速度限幅
 * 逐步逼近目标值，故任何取值都保留转向的执行过程。
 */
UENUM(BlueprintType)
enum class ESingularisMorphVehicleSteeringType : uint8
{
	SingleAngle UMETA(DisplayName = "单一角度（内外轮同角）"),
	AngleRatio UMETA(DisplayName = "角度比例（内侧轮放大）"),
	Ackermann UMETA(DisplayName = "阿克曼几何（按轴距与轮距计算）")
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
