#include "Components/SingularisEngineComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include "Core/SingularisMorphVehicleTorqueSimModules.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisEngineComponent)

USingularisEngineComponent::USingularisEngineComponent()
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

void USingularisEngineComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	// 引擎模块当前无需处理输出数据，由子类按需重写
}

Chaos::ISimulationModuleBase* USingularisEngineComponent::CreateNewCoreModule() const
{
	// 0) 核心结构以 uint16 存储转速，超出即静默截断，故在此钳制
	const int32 ClampedMaxRPM = FMath::Clamp(MaxRPM, 1, static_cast<int32>(MAX_uint16));

	// 1) 配置引擎设置。扭矩与惯性必须处于同一单位制：ChaosVehicles 的角速度积分以
	//    kg·cm² 为转动惯量单位，扭矩则经 TorqueMToCm 由 N·m 换算为 kg·cm²/s²。
	//    若惯性仍按 kg·m² 原值传入，角加速度会被放大 10^4 倍，单步即可把转速顶到上限
	Chaos::FEngineSettings Settings;

	Settings.MaxTorque = Chaos::TorqueMToCm(FMath::Max(MaxTorque, 0.0f));

	// 2) 采样扭矩曲线并归一化。
	//    a) 用户曲线为空时保留设置结构内置的默认特性：归一化图求值会按点数计算步长，
	//       空图会越界读取（FNormalisedGraph::GetValue）
	//    b) 曲线非空时先清空默认特性：直接追加会使采样点排在默认点之后，
	//       归一化横轴（0..MaxRPM）与实际曲线点错位
	//    c) 采样点数固定，避免以 MaxRPM / 采样数 作步长（MaxRPM 为 0 时步长为零而永不退出）
	//    d) 曲线范围只取一次（曲线在循环内不会变化），全零曲线无法归一化，此时按原值采样
	//    e) 曲线横轴为归一化转速 [0..1]；FNormalisedGraph 再将均匀采样点映射到 [0..MaxRPM]，
	//       故求值必须使用归一化横轴，不能直接以 RPM 作为曲线输入
	//    外部曲线资产优先：RuntimeFloatCurve 在配置了 ExternalCurve 时以资产为准，
	//    内部曲线可能为空，读取内部曲线会静默退回默认特性
	const FRichCurve* TorqueRichCurve = TorqueCurve.ExternalCurve
		                                    ? &TorqueCurve.ExternalCurve->FloatCurve
		                                    : TorqueCurve.GetRichCurveConst();
	if (TorqueRichCurve && !TorqueRichCurve->IsEmpty())
	{
		Settings.TorqueCurve.Empty();

		auto MinVal = 0.0f, MaxVal = 0.0f;
		TorqueRichCurve->GetValueRange(MinVal, MaxVal);
		const float NormalizeScale = FMath::IsNearlyZero(MaxVal) ? 1.0f : 1.0f / MaxVal;

		constexpr auto NumSamples = 20;
		for (auto I = 0; I <= NumSamples; ++I)
		{
			const float X = static_cast<float>(I) / static_cast<float>(NumSamples);
			Settings.TorqueCurve.AddNormalized(FMath::Max(TorqueRichCurve->Eval(X) * NormalizeScale, 0.0f));
		}
	}

	Settings.MaxRPM = static_cast<uint16>(ClampedMaxRPM);
	Settings.IdleRPM = static_cast<uint16>(FMath::Clamp(EngineIdleRPM, 0, ClampedMaxRPM));
	Settings.EngineBrakeEffect = Chaos::TorqueMToCm(FMath::Max(EngineBrakeEffect, 0.0f));
	Settings.EngineInertia = Chaos::M2ToCm2(FMath::Max(EngineInertia, KINDA_SMALL_NUMBER));

	// 3) 创建引擎仿真模块（带传动系转速限制器）
	Chaos::ISimulationModuleBase* Engine = new FSingularisMorphEngineSimModule(Settings);

	return Engine;
}
