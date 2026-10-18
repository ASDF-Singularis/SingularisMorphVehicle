#include "Components/SingularisMotorComponent.h"

#include <VehicleUtility.h>

#include "Core/SingularisMorphVehicleTorqueSimModules.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMotorComponent)

namespace
{
	/** 角速度积分的最小转动惯量（kg·cm²） */
	constexpr auto MotorComponentMinInertia = 1.0f;

	/** 非法配置时的回退转动惯量（kg·cm²） */
	constexpr auto MotorComponentFallbackInertia = 100.0f;
}

USingularisMotorComponent::USingularisMotorComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

Chaos::ISimulationModuleBase* USingularisMotorComponent::CreateNewCoreModule() const
{
	Chaos::FMotorSettings Settings;

	// 扭矩与惯性均使用 Chaos 的 CGS 单位：扭矩 N·m → kg·cm²/s²，
	// 转动惯量属性本身即 kg·cm²。若把惯性再按 kg·m² 换算，角加速度会被缩小 10^4 倍
	const float SanitizedMaxTorque = FMath::IsFinite(MaxTorque)
		                                 ? FMath::Max(MaxTorque, 0.0f)
		                                 : 0.0f;
	Settings.MaxTorque = Chaos::TorqueMToCm(SanitizedMaxTorque);
	Settings.MaxRPM = FMath::IsFinite(MaxRPM) ? FMath::Max(MaxRPM, 0.0f) : 0.0f;
	Settings.EngineInertia = FMath::IsFinite(EngineInertia)
		                         ? FMath::Max(EngineInertia, MotorComponentMinInertia)
		                         : MotorComponentFallbackInertia;

	Chaos::ISimulationModuleBase* Motor = new FSingularisMorphMotorSimModule(Settings);

	return Motor;
}
