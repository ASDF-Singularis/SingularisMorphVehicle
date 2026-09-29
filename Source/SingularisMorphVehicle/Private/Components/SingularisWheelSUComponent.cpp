#include "Components/SingularisWheelSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include "Components/SingularisUprightSUComponent.h"
#include "Core/SingularisMorphVehicleWheelSimModule.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisWheelSUComponent)

USingularisWheelSUComponent::USingularisWheelSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;
}

void USingularisWheelSUComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
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

Chaos::ISimulationModuleBase* USingularisWheelSUComponent::CreateNewCoreModule() const
{
	// 1) 轮胎设置：转向配置由立轴槽位在建树期注入（ApplySlotConfig），
	//    此处构建无转向基准；轴向/反转为本组件的回退值（自由轮胎语义）
	Chaos::FWheelSettings Settings;

	Settings.Radius = WheelRadius;
	Settings.Width = WheelWidth;
	Settings.WheelInertia = WheelInertia;
	Settings.MaxRotationVel = MaxRotationVel;
	Settings.FrictionMultiplier = FrictionMultiplier;
	Settings.LateralSlipGraphMultiplier = LateralSlipGraphMultiplier;
	for (const FVector2D& Point : LateralSlipGraph)
		Settings.LateralSlipGraph.Add(Chaos::FVec2(static_cast<float>(Point.X), static_cast<float>(Point.Y)));
	Settings.CorneringStiffness = CorneringStiffness * 10000.0f;
	Settings.SlipAngleLimit = SlipAngleLimit;
	Settings.SlipModifier = SlipModifier;
	Settings.MaxBrakeTorque = Chaos::TorqueMToCm(MaxBrakeTorque);
	Settings.HandbrakeEnabled = bHandbrakeEnabled;
	Settings.HandbrakeTorque = Chaos::TorqueMToCm(HandbrakeTorque);
	Settings.AutoHandbrakeEnabled = bAutoHandbrakeEnabled;
	Settings.AutoHandbrakeVelocityThreshold = AutoHandbrakeVelocityThreshold;
	Settings.SteeringEnabled = false;
	Settings.MaxSteeringAngle = 0.0f;
	Settings.ABSEnabled = bABSEnabled;
	Settings.TractionControlEnabled = bTractionControlEnabled;
	Settings.Axis = AxisType == ESingularisMorphVehicleWheelAxisType::Y ? Chaos::EWheelAxis::Y : Chaos::EWheelAxis::X;
	Settings.ReverseDirection = ReverseDirection;
	Settings.ForceOffset = ForceOffset;

	// 2) 转向动力学设置：默认关闭，挂载立轴槽位时由注入整体替换
	FSingularisMorphWheelSteeringSettings SteeringSettings;

	Chaos::ISimulationModuleBase* Wheel = new FSingularisMorphWheelSimModule(Settings, SteeringSettings);
	Wheel->SetAnimationEnabled(bAnimationEnabled);

	return Wheel;
}

void USingularisWheelSUComponent::ApplySlotConfig(
	Chaos::ISimulationModuleBase* CoreModule,
	const USingularisUprightSUComponent* UprightSU
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
