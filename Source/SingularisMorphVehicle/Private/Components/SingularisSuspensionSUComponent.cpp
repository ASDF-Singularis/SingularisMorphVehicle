#include "Components/SingularisSuspensionSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include "Core/SingularisMorphVehicleSuspensionSimModule.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisSuspensionSUComponent)

USingularisSuspensionSUComponent::USingularisSuspensionSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;
}

Chaos::ISimulationModuleBase* USingularisSuspensionSUComponent::CreateNewCoreModule() const
{
	// 1) 配置悬挂设置
	FSingularisMorphSuspensionSettings Settings;

	Settings.SuspensionAxis = SuspensionAxis;
	Settings.MaxRaise = SuspensionMaxRaise;
	Settings.MaxDrop = SuspensionMaxDrop;
	Settings.SpringRate = Chaos::MToCm(SpringRate);
	Settings.SpringPreload = Chaos::MToCm(SpringPreload);
	Settings.SuspensionForceEffect = SuspensionForceEffect;

	// 2) 行程动力学参数：阻尼按阻尼比在模块构造时解算，此处只传原始参数
	Settings.UnsprungMass = UnsprungMass;
	Settings.SprungMass = SprungMass;
	Settings.TireStiffnessRatio = TireStiffnessRatio;
	Settings.TireDampingRatio = TireDampingRatio;
	Settings.ReboundDampingRatio = ReboundDampingRatio;
	Settings.CompressionDampingRatio = CompressionDampingRatio;

	// 3) 重力：模块在物理线程无法读取世界设置，创建时从世界取（世界重力为负值，取模长）
	if (const UWorld* World = GetWorld())
		Settings.Gravity = FMath::Abs(World->GetGravityZ());

	// 4) 创建悬挂仿真模块并启用动画
	Chaos::ISimulationModuleBase* Suspension = new FSingularisMorphVehicleSuspensionSimModule(Settings);
	Suspension->SetAnimationEnabled(bAnimationEnabled);

	return Suspension;
}
