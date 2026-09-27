#include "Components/SingularisEngineSUComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisEngineSUComponent)

USingularisEngineSUComponent::USingularisEngineSUComponent()
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

void USingularisEngineSUComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	// 引擎模块当前无需处理输出数据，由子类按需重写
}

Chaos::ISimulationModuleBase* USingularisEngineSUComponent::CreateNewCoreModule() const
{
	// 0) 核心结构以 uint16 存储转速，超出即静默截断，故在此钳制
	const int32 ClampedMaxRPM = FMath::Clamp(MaxRPM, 1, static_cast<int32>(MAX_uint16));

	// 1) 配置引擎设置
	Chaos::FEngineSettings Settings;

	Settings.MaxTorque = Chaos::TorqueMToCm(MaxTorque);

	// 2) 采样扭矩曲线并归一化。
	//    a) 用户曲线为空时保留设置结构内置的默认特性：归一化图求值会按点数计算步长，
	//       空图会越界读取（FNormalisedGraph::GetValue）
	//    b) 曲线非空时先清空默认特性：直接追加会使采样点排在默认点之后，
	//       归一化横轴（0..MaxRPM）与实际曲线点错位
	//    c) 采样点数固定，避免以 MaxRPM / 采样数 作步长（MaxRPM 为 0 时步长为零而永不退出）
	//    d) 曲线范围只取一次（曲线在循环内不会变化），全零曲线无法归一化，此时按原值采样
	const FRichCurve* TorqueRichCurve = TorqueCurve.GetRichCurveConst();
	if (TorqueRichCurve && !TorqueRichCurve->IsEmpty())
	{
		Settings.TorqueCurve.Empty();

		auto MinVal = 0.0f, MaxVal = 0.0f;
		TorqueRichCurve->GetValueRange(MinVal, MaxVal);
		const float NormalizeScale = FMath::IsNearlyZero(MaxVal) ? 1.0f : 1.0f / MaxVal;

		constexpr auto NumSamples = 20;
		for (auto I = 0; I <= NumSamples; ++I)
		{
			const float X = ClampedMaxRPM * static_cast<float>(I) / static_cast<float>(NumSamples);
			Settings.TorqueCurve.AddNormalized(TorqueRichCurve->Eval(X) * NormalizeScale);
		}
	}

	Settings.MaxRPM = static_cast<uint16>(ClampedMaxRPM);
	Settings.IdleRPM = static_cast<uint16>(FMath::Clamp(EngineIdleRPM, 0, ClampedMaxRPM));
	Settings.EngineBrakeEffect = EngineBrakeEffect;
	Settings.EngineInertia = EngineInertia;

	// 3) 创建引擎仿真模块
	Chaos::ISimulationModuleBase* Engine = new Chaos::FEngineSimModule(Settings);

	return Engine;
}
