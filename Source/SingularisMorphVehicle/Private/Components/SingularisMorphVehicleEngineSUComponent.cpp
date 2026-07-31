#include "Components/SingularisMorphVehicleEngineSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleEngineSUComponent)

USingularisMorphVehicleEngineSUComponent::USingularisMorphVehicleEngineSUComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;

	// 初始化默认扭矩曲线关键帧
	TorqueCurve.GetRichCurve()->AddKey(0, 0.5f);
	TorqueCurve.GetRichCurve()->AddKey(0.5, 1.0f);
	TorqueCurve.GetRichCurve()->AddKey(1.0f, 0.75f);
}

void USingularisMorphVehicleEngineSUComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	// 引擎模块当前无需处理输出数据，由子类按需重写
}

Chaos::ISimulationModuleBase* USingularisMorphVehicleEngineSUComponent::CreateNewCoreModule() const
{
	// 1) 配置引擎设置
	Chaos::FEngineSettings Settings;

	Settings.MaxTorque = Chaos::TorqueMToCm(MaxTorque);

	// 2) 采样扭矩曲线并归一化
	constexpr float NumSamples = 20;
	for (float X = 0.0; X <= MaxRPM; X += MaxRPM / NumSamples)
	{
		float MinVal = 0.0f, MaxVal = 0.0f;
		TorqueCurve.GetRichCurveConst()->GetValueRange(MinVal, MaxVal);
		const float Y = this->TorqueCurve.GetRichCurveConst()->Eval(X) / MaxVal;
		Settings.TorqueCurve.AddNormalized(Y);
	}

	Settings.MaxRPM = MaxRPM;
	Settings.IdleRPM = EngineIdleRPM;
	Settings.EngineBrakeEffect = EngineBrakeEffect;
	Settings.EngineInertia = EngineInertia;

	// 3) 创建引擎仿真模块
	Chaos::ISimulationModuleBase* Engine = new Chaos::FEngineSimModule(Settings);

	return Engine;
}
