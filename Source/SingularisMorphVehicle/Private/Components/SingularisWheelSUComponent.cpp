#include "Components/SingularisWheelSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include "Core/SingularisMorphVehicleWheelSimModule.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisWheelSUComponent)

namespace
{
	/** 车速敏感曲线的等距采样段数（采样点数为段数 + 1） */
	constexpr auto SpeedSteeringCurveSamples = 20;

	/**
	 * 将编辑器曲线烘培为物理线程可用的等距采样图。
	 *
	 * 曲线按峰值归一化（Y 峰值映射为 1），横轴取曲线最后一个关键帧的时间；
	 * 空曲线不产生采样点，模拟侧据此跳过车速衰减。
	 */
	void BakeSpeedSteeringCurve(const FRuntimeFloatCurve& Curve, Chaos::FGraph& OutGraph)
	{
		const FRichCurve* RichCurve = Curve.GetRichCurveConst();
		if (!RichCurve || RichCurve->IsEmpty()) return;

		auto MinValue = 0.0f;
		auto MaxValue = 1.0f;
		RichCurve->GetValueRange(MinValue, MaxValue);
		const float NormalizeScale = FMath::IsNearlyZero(MaxValue) ? 1.0f : 1.0f / MaxValue;

		const float MaxX = RichCurve->GetLastKey().Time;
		for (auto I = 0; I <= SpeedSteeringCurveSamples; ++I)
		{
			const float X = MaxX * static_cast<float>(I) / static_cast<float>(SpeedSteeringCurveSamples);
			OutGraph.Add(Chaos::FVec2(X, RichCurve->Eval(X) * NormalizeScale));
		}
	}
}

USingularisWheelSUComponent::USingularisWheelSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
	bAnimationEnabled = true;

	// 速度敏感转向曲线的默认值取自经典载具插件：车速越高可用转角越小
	FRichCurve* SteeringCurve = SteeringSetup.SpeedSteeringCurve.GetRichCurve();
	SteeringCurve->AddKey(0.0f, 1.0f);
	SteeringCurve->AddKey(32.0f, 0.8f);
	SteeringCurve->AddKey(97.0f, 0.4f);
	SteeringCurve->AddKey(193.0f, 0.3f);
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
	// 0) 转向配置的先决量：负的最大转角（蓝图/C++ 可写，编辑器 ClampMin 仅约束 UI）会
	//    让轮胎设置得到反向转向角，故统一钳制到非负
	const float EffectiveMaxSteeringAngle = bSteeringEnabled ? FMath::Max(0.0f, MaxSteeringAngle) : 0.0f;

	// 1) 轮胎设置
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
	Settings.SteeringEnabled = bSteeringEnabled;
	Settings.MaxSteeringAngle = EffectiveMaxSteeringAngle;
	Settings.ABSEnabled = bABSEnabled;
	Settings.TractionControlEnabled = bTractionControlEnabled;
	Settings.Axis = AxisType == ESingularisMorphVehicleWheelAxisType::Y ? Chaos::EWheelAxis::Y : Chaos::EWheelAxis::X;
	Settings.ReverseDirection = ReverseDirection;
	Settings.ForceOffset = ForceOffset;

	// 2) 转向动力学设置：最大转角与轮胎设置共用同一来源，保证归一化写回与
	//    基类的还原换算严格互逆
	FSingularisMorphWheelSteeringSettings SteeringSettings;
	SteeringSettings.bEnabled = bSteeringEnabled && EffectiveMaxSteeringAngle > KINDA_SMALL_NUMBER;
	SteeringSettings.SteeringType = SteeringSetup.SteeringType;
	SteeringSettings.MaxSteeringAngle = EffectiveMaxSteeringAngle;
	SteeringSettings.SteeringRiseRate = SteeringSetup.SteeringRiseRate;
	SteeringSettings.SteeringFallRate = SteeringSetup.SteeringFallRate;
	SteeringSettings.AngleRatio = SteeringSetup.AngleRatio;
	SteeringSettings.WheelBase = SteeringSetup.WheelBase;
	SteeringSettings.TrackWidth = SteeringSetup.TrackWidth;
	SteeringSettings.SelfAligningTorqueGain = SteeringSetup.SelfAligningTorqueGain;
	SteeringSettings.InitialSteeringAngle = CachedSteeringAngleDegrees;
	BakeSpeedSteeringCurve(SteeringSetup.SpeedSteeringCurve, SteeringSettings.SpeedSteeringCurve);

	Chaos::ISimulationModuleBase* Wheel = new FSingularisMorphWheelSimModule(Settings, SteeringSettings);
	Wheel->SetAnimationEnabled(bAnimationEnabled);

	return Wheel;
}
