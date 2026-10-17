#include "Components/SingularisWheelComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include "Components/SingularisUprightComponent.h"
#include "Core/SingularisMorphVehicleWheelSimModule.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisWheelComponent)

USingularisWheelComponent::USingularisWheelComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;
}

void USingularisWheelComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	if (!OutputData) return;

	const auto WheelOutData = static_cast<const Chaos::FWheelOutputData*>(OutputData);

	// 缓存实际转向角：拓扑重建后新模块以其为初值，避免转向角瞬间归零
	CachedSteeringAngleDegrees = WheelOutData->SteeringAngleDegrees;

	for (const Chaos::FWheelTouchChangeEvent& Event : WheelOutData->WheelTouchEvents)
	{
		OnWheelTouchChangeNativeEvent.Broadcast(WheelOutData->ModuleGuid, Event.bIsInContact);
		OnWheelTouchChangeEvent.Broadcast(WheelOutData->ModuleGuid, Event.bIsInContact);
	}
}

Chaos::ISimulationModuleBase* USingularisWheelComponent::CreateNewCoreModule() const
{
	// 1) 轮胎设置：转向/轴向/反转均为轮位（立轴）配置，由槽位在建树期注入
	//    （ApplySlotConfig）；此处仅构建自由轮语义的中性基准：
	//    无转向、轴向 X、不反转，未挂槽位的轮胎按该默认结算地面交互
	Chaos::FWheelSettings Settings;

	// 几何与惯性：半径与惯性是角速度积分的分母，零值会退化为除零；
	// 惯性按 kg·m² → kg·cm² 换算，与经 TorqueMToCm 换算的扭矩保持同一单位制
	Settings.Radius = FMath::Max(WheelRadius, 1.0f);
	Settings.Width = FMath::Max(WheelWidth, 0.0f);
	Settings.WheelInertia = Chaos::M2ToCm2(FMath::Max(WheelInertia, KINDA_SMALL_NUMBER));
	Settings.MaxRotationVel = FMath::Max(FMath::Max(MaxRotationVel, 0.0f), EffectiveMaxRotationVel);
	Settings.FrictionMultiplier = FMath::Max(FrictionMultiplier, 0.0f);
	Settings.LateralSlipGraphMultiplier = LateralSlipGraphMultiplier;
	for (const FVector2D& Point : LateralSlipGraph)
		Settings.LateralSlipGraph.Add(Chaos::FVec2(static_cast<float>(Point.X), static_cast<float>(Point.Y)));
	Settings.CorneringStiffness = FMath::Max(CorneringStiffness, 0.0f) * 10000.0f;
	Settings.SlipAngleLimit = FMath::Max(SlipAngleLimit, 0.0f);
	Settings.SlipModifier = SlipModifier;
	Settings.MaxBrakeTorque = Chaos::TorqueMToCm(FMath::Max(MaxBrakeTorque, 0.0f));
	Settings.HandbrakeEnabled = bHandbrakeEnabled;
	Settings.HandbrakeTorque = Chaos::TorqueMToCm(FMath::Max(HandbrakeTorque, 0.0f));
	Settings.AutoHandbrakeEnabled = bAutoHandbrakeEnabled;
	Settings.AutoHandbrakeVelocityThreshold = FMath::Max(AutoHandbrakeVelocityThreshold, 0.0f);
	Settings.SteeringEnabled = false;
	Settings.MaxSteeringAngle = 0.0f;
	Settings.ABSEnabled = bABSEnabled;
	Settings.TractionControlEnabled = bTractionControlEnabled;
	Settings.Axis = Chaos::EWheelAxis::X;
	Settings.ReverseDirection = false;
	Settings.ForceOffset = ForceOffset;

	// 2) 转向动力学设置：默认关闭，挂载立轴槽位时由注入整体替换
	FSingularisMorphWheelSteeringSettings SteeringSettings;

	Chaos::ISimulationModuleBase* Wheel = new FSingularisMorphWheelSimModule(Settings, SteeringSettings);
	Wheel->SetAnimationEnabled(bAnimationEnabled);

	return Wheel;
}

void USingularisWheelComponent::ApplySlotConfig(
	Chaos::ISimulationModuleBase* CoreModule,
	const USingularisUprightComponent* UprightSU
)
{
	if (!CoreModule || !UprightSU) return;

	// 本组件创建的模块恒为 FSingularisMorphWheelSimModule（见 CreateNewCoreModule）；
	// 轴向/反转由核心物理路径从模块设置读取，注入必须在入树前一次性完成，
	// 不能按帧从父模块查询（否则需复制核心 Simulate 的物理路径）
	static_cast<FSingularisMorphWheelSimModule*>(CoreModule)->ApplyUprightSlotConfig(
		UprightSU->BuildSteeringSettings(CachedSteeringAngleDegrees),
		UprightSU->AxisType == ESingularisMorphVehicleWheelAxisType::Y ? Chaos::EWheelAxis::Y : Chaos::EWheelAxis::X,
		UprightSU->ReverseDirection
	);
}
