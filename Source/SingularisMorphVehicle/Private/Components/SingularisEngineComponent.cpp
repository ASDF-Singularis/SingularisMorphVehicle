#include "Components/SingularisEngineComponent.h"

#include <VehicleUtility.h>
#include <SimModule/SimModulesInclude.h>

#include "SingularisMorphVehicle.h"
#include "Core/SingularisMorphVehicleTorqueSimModules.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisEngineComponent)

namespace
{
	/** 转速以 uint16 存储，超过该值会被静默截断 */
	constexpr auto EngineComponentMaxStoredRPM = static_cast<int32>(MAX_uint16);

	/** 角速度积分的最小转动惯量（kg·cm²） */
	constexpr auto EngineComponentMinInertia = 1.0f;

	/** 非法配置时的回退转动惯量（kg·cm²） */
	constexpr auto EngineComponentFallbackInertia = 1000.0f;
}

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
	// 0) 转速以 uint16 存储，超出即静默截断；怠速必须低于最大转速，
	//    否则引擎会一直被怠速钳制在最大转速，失去可用转速区间
	const int32 ClampedMaxRPM = FMath::Clamp(MaxRPM, 1, EngineComponentMaxStoredRPM);
	const int32 ClampedIdleRPM = FMath::Clamp(
		EngineIdleRPM,
		0,
		FMath::Max(ClampedMaxRPM - 1, 0)
	);

	if (ClampedIdleRPM != EngineIdleRPM)
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] EngineIdleRPM (%d) must be lower than MaxRPM (%d); clamped to %d"),
			*GetName(),
			EngineIdleRPM,
			ClampedMaxRPM,
			ClampedIdleRPM
		);
	}

	// 1) 配置引擎设置。Chaos 的角速度积分使用 CGS 单位：扭矩 kg·cm²/s²、
	//    转动惯量 kg·cm²。上游模块化引擎组件同样直接使用 kg·cm² 值
	//    （默认 1000 即 0.1 kg·m²）。本属性也以 kg·cm² 存储，不能再按
	//    kg·m² 换算为 cm²，否则角加速度会被缩小 10^4 倍，空挡油门几乎无法提升转速。
	Chaos::FEngineSettings Settings;

	const float SanitizedMaxTorque = FMath::IsFinite(MaxTorque)
		                                 ? FMath::Max(MaxTorque, 0.0f)
		                                 : 0.0f;
	Settings.MaxTorque = Chaos::TorqueMToCm(SanitizedMaxTorque);

	// 2) 采样扭矩曲线并归一化。
	//    a) 用户曲线为空时保留设置结构内置的默认特性：归一化图求值会按点数计算步长，
	//       空图会越界读取（FNormalisedGraph::GetValue）
	//    b) 曲线非空时先清空默认特性：直接追加会使采样点排在默认点之后，
	//       归一化横轴（0..MaxRPM）与实际曲线点错位
	//    c) 采样点数固定，避免以 MaxRPM / 采样数 作步长（MaxRPM 为 0 时步长为零而永不退出）
	//    d) 归一化分母取曲线绝对峰值，避免全负曲线被负分母翻转为正扭矩；
	//       全零或非有限峰值时按原值采样
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

		const float PeakMagnitude = FMath::Max(FMath::Abs(MinVal), FMath::Abs(MaxVal));
		const float NormalizeScale = FMath::IsFinite(PeakMagnitude)
		                             && PeakMagnitude > KINDA_SMALL_NUMBER
			                             ? 1.0f / PeakMagnitude
			                             : 1.0f;

		constexpr auto NumSamples = 20;
		for (auto I = 0; I <= NumSamples; ++I)
		{
			const float X = static_cast<float>(I) / static_cast<float>(NumSamples);
			const float Value = TorqueRichCurve->Eval(X) * NormalizeScale;
			Settings.TorqueCurve.AddNormalized(
				FMath::IsFinite(Value) ? FMath::Max(Value, 0.0f) : 0.0f
			);
		}
	}

	Settings.MaxRPM = static_cast<uint16>(ClampedMaxRPM);
	Settings.IdleRPM = static_cast<uint16>(ClampedIdleRPM);

	const float SanitizedEngineBrakeEffect = FMath::IsFinite(EngineBrakeEffect)
		                                         ? FMath::Max(EngineBrakeEffect, 0.0f)
		                                         : 0.0f;
	Settings.EngineBrakeEffect = Chaos::TorqueMToCm(SanitizedEngineBrakeEffect);

	const float SanitizedEngineInertia = FMath::IsFinite(EngineInertia)
		                                     ? FMath::Max(EngineInertia, EngineComponentMinInertia)
		                                     : EngineComponentFallbackInertia;
	Settings.EngineInertia = SanitizedEngineInertia;

	const float SanitizedRevUpRate = FMath::IsFinite(EngineRevUpRate)
		                                 ? FMath::Max(EngineRevUpRate, 0.0f)
		                                 : 0.0f;
	const float SanitizedRevDownRate = FMath::IsFinite(EngineRevDownRate)
		                                   ? FMath::Max(EngineRevDownRate, 0.0f)
		                                   : 0.0f;

	const float SanitizedCreepTorqueRatio = FMath::IsFinite(IdleCreepTorqueRatio)
		                                        ? FMath::Clamp(IdleCreepTorqueRatio, 0.0f, 1.0f)
		                                        : 0.0f;

	// 3) 创建引擎仿真模块（软转速限制器、引擎制动、怠速蠕行、空挡自由转速与升降速速率限制）
	Chaos::ISimulationModuleBase* Engine = new FSingularisMorphEngineSimModule(
		Settings,
		SanitizedRevUpRate,
		SanitizedRevDownRate,
		SanitizedCreepTorqueRatio
	);

	return Engine;
}
