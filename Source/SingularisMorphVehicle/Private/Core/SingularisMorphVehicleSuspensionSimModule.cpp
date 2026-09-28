#include "Core/SingularisMorphVehicleSuspensionSimModule.h"

#include "SuspensionUtility.h"
#include "SimModule/SimModuleTree.h"
#include "SimModule/TorqueSimModule.h"

#if VEHICLE_DEBUGGING_ENABLED
UE_DISABLE_OPTIMIZATION_SHIP
#endif

using namespace Chaos;

namespace
{
	/** 单个物理步的最大子步数：为轮胎刚度上限划定积分开销 */
	constexpr auto MaxTravelSubsteps = 12;

	/** 每个子步的相位推进上限（ω·Δt）：半隐式欧拉在 ω·Δt < 2 内稳定，取 0.5 保留余量 */
	constexpr auto MaxTravelPhaseStep = 0.5f;
}

void FSingularisMorphSuspensionSimModuleData::FillSimState(ISimulationModuleBase* SimModule)
{
	if (FSingularisMorphVehicleSuspensionSimModule* Sim = SimModule->Cast<FSingularisMorphVehicleSuspensionSimModule>())
	{
		Sim->SpringDisplacement = SpringDisplacement;
		Sim->SpringSpeed = SpringSpeed;

		// 状态来自上一世代或网络回滚：首帧不再以地面位置覆盖
		Sim->bTravelInitialized = true;
	}
}

void FSingularisMorphSuspensionSimModuleData::FillNetState(const ISimulationModuleBase* SimModule)
{
	if (const FSingularisMorphVehicleSuspensionSimModule* Sim = SimModule->Cast<const
		FSingularisMorphVehicleSuspensionSimModule>())
	{
		SpringDisplacement = Sim->SpringDisplacement;
		SpringSpeed = Sim->SpringSpeed;
	}
}

void FSingularisMorphSuspensionSimModuleData::Lerp(
	const float LerpFactor,
	const FModuleNetData& Min,
	const FModuleNetData& Max
)
{
	const auto& MinData = static_cast<const FSingularisMorphSuspensionSimModuleData&>(Min);
	const auto& MaxData = static_cast<const FSingularisMorphSuspensionSimModuleData&>(Max);

	SpringDisplacement = FMath::Lerp(MinData.SpringDisplacement, MaxData.SpringDisplacement, LerpFactor);
	SpringSpeed = FMath::Lerp(MinData.SpringSpeed, MaxData.SpringSpeed, LerpFactor);
}

FSingularisMorphVehicleSuspensionSimModule::FSingularisMorphVehicleSuspensionSimModule(
	const FSingularisMorphSuspensionSettings& Settings
)
	: TSimModuleSettings<FSingularisMorphSuspensionSettings>(Settings)
{
	AccessSetup().MaxLength = FMath::Abs(Settings.MaxRaise + Settings.MaxDrop);

	// 1) 阻尼系数按阻尼比与簧载质量解算（弹簧劲度的量纲为 kg/s²，质量的量纲为 kg，
	//    故阻尼系数的量纲为 kg/s）；压缩段偏软以吸收冲击，回弹段偏硬以抑制车体回弹
	CompressionDamping = FSuspensionUtility::ComputeDamping(
		Settings.SpringRate,
		Settings.SprungMass,
		Settings.CompressionDampingRatio
	);
	ReboundDamping = FSuspensionUtility::ComputeDamping(
		Settings.SpringRate,
		Settings.SprungMass,
		Settings.ReboundDampingRatio
	);

	// 2) 轮胎接触：刚度按弹簧劲度的倍率（实车轮胎径向刚度约为车轮速率的 8-12 倍），
	//    阻尼按非簧载质量的轮胎-车轮临界阻尼比例解算
	TireStiffness = Settings.SpringRate * FMath::Max(Settings.TireStiffnessRatio, 0.0f);
	TireDamping = FSuspensionUtility::ComputeDamping(
		TireStiffness,
		Settings.UnsprungMass,
		Settings.TireDampingRatio
	);
}

FSingularisMorphVehicleSuspensionSimModule::~FSingularisMorphVehicleSuspensionSimModule() {}

float FSingularisMorphVehicleSuspensionSimModule::GetSpringLength() const
{
	return -(Setup().MaxLength - SpringDisplacement);
}

void FSingularisMorphVehicleSuspensionSimModule::SetSpringLength(float InLength, float WheelRadius)
{
	// 射线给出的接触压缩量：车轮行程到达该值即轮胎触及地面。
	// 上下限均取行程范围：地面低于最大伸张时无接触，高于全压缩时按全压缩处理
	// （射线命中近垂直面时命中距离可任意小，不设上限会让轮胎力无界）
	GroundDisplacement = FMath::Clamp(Setup().MaxLength - FMath::Max(0.0f, InLength), 0.0f, Setup().MaxLength);

	// 首次收到射线结果时以地面位置起步，避免模块创建瞬间从全伸张抽向地面
	if (!bTravelInitialized)
	{
		SpringDisplacement = GroundDisplacement;
		SpringSpeed = 0.0f;
		bTravelInitialized = true;
	}
}

void FSingularisMorphVehicleSuspensionSimModule::GetWorldTraceEndpoints(
	float DeltaSeconds,
	const FTransform& BodyTransform,
	const FVector& Velocity,
	float WheelRadius,
	FSpringTrace& OutTrace
) const
{
	FVector LocalDirection = Setup().SuspensionAxis;
	FVector Local = GetParentRelativeTransform().GetLocation(); // change to just a vector and GetLocalLocation
	FVector WorldLocation = BodyTransform.TransformPosition(Local);
	FVector WorldDirection = BodyTransform.TransformVector(LocalDirection);

	FVector MovementExpansion = FVector::ZeroVector;
	// time dilation only an issue when time is slowed down
	if (CurrentTimeDilation < 1.0f)
	{
		float Amount = FMath::Abs(WorldDirection.Dot(Velocity));
		MovementExpansion += WorldDirection * (Amount * DeltaSeconds / CurrentTimeDilation);
	}

	OutTrace.Start = WorldLocation - WorldDirection * Setup().MaxRaise;
	OutTrace.End = WorldLocation + WorldDirection * (Setup().MaxDrop + WheelRadius) + MovementExpansion;
}

void FSingularisMorphVehicleSuspensionSimModule::Simulate(
	float DeltaTime,
	const FAllInputs& Inputs,
	FSimModuleTree& VehicleModuleSystem
)
{
	CurrentTimeDilation = FMath::Max(Inputs.CurrentTimeDilation, SMALL_NUMBER);

	// 1) 行程动力学积分：产出簧上反力与轮胎载荷
	auto SuspensionForce = 0.0f;
	auto ForceIntoSurface = 0.0f;
	IntegrateTravel(FMath::Max(DeltaTime, SMALL_NUMBER), SuspensionForce, ForceIntoSurface);

	// 2) 簧上反力沿悬挂轴作用（弹簧无法下拉车体，负值截断由 IntegrateTravel 完成）
	if (SuspensionForce > 0.0f)
		AddLocalForce(Setup().SuspensionAxis * -SuspensionForce, true, false, true, FColor::Green);

	// 3) 把轮胎载荷交给配对车轮，决定可用抓地力
	if (SimModuleTree && WheelSimTreeIndex != INVALID_IDX)
	{
		if (ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(WheelSimTreeIndex))
		{
			if (FWheelBaseInterface* Wheel = Module->Cast<FWheelBaseInterface>())
				Wheel->SetForceIntoSurface(ForceIntoSurface * Setup().SuspensionForceEffect);
		}
	}
}

void FSingularisMorphVehicleSuspensionSimModule::Animate()
{
	FVector Movement = -Setup().SuspensionAxis * (Setup().MaxRaise + GetSpringLength());

	AnimationData.AnimFlags = EAnimationFlags::AnimatePosition;
	AnimationData.AnimationLocOffset = Movement;
}

void FSingularisMorphVehicleSuspensionSimModule::IntegrateTravel(
	float DeltaTime,
	float& OutSuspensionForce,
	float& OutForceIntoSurface
)
{
	const FSingularisMorphSuspensionSettings& Settings = Setup();
	const float MaxLength = Settings.MaxLength;
	const float SpringRate = FMath::Max(Settings.SpringRate, 0.0f);
	const float Preload = FMath::Max(Settings.SpringPreload, 0.0f);
	const float Mass = FMath::Max(Settings.UnsprungMass, 1.0f);
	const float Gravity = FMath::Max(Settings.Gravity, 0.0f);

	auto NumSubsteps = 1;
	float SubDeltaTime = DeltaTime;
	ComputeSubstepLayout(DeltaTime, NumSubsteps, SubDeltaTime);

	// 子步预算内的刚度上限：超出部分按上限折算，避免显式积分发散（子步数已封顶）
	const float MaxOmega = static_cast<float>(MaxTravelSubsteps) * MaxTravelPhaseStep / FMath::Max(
		DeltaTime,
		SMALL_NUMBER
	);
	const float EffectiveTireStiffness = FMath::Min(TireStiffness, MaxOmega * MaxOmega * Mass);

	// 阻尼力上限：全压缩行程处的弹簧力（真实减振器在高速段的泄压），
	// 避免异常大的相对速度（如射线命中近垂直面）把车体弹飞
	const float MaxDampingForce = SpringRate * MaxLength + Preload;

	auto SpringForce = 0.0f;
	auto DampingForce = 0.0f;
	auto GroundForce = 0.0f;

	// 车轮受力（沿压缩方向为正）：弹簧与阻尼向下，地面接触力向上，重力向下
	auto EvaluateForces = [&]
	{
		const float DampingRate = SpringSpeed > 0.0f ? CompressionDamping : ReboundDamping;
		const float Penetration = GroundDisplacement - SpringDisplacement;

		SpringForce = SpringRate * SpringDisplacement + Preload;
		DampingForce = FMath::Clamp(DampingRate * SpringSpeed, -MaxDampingForce, MaxDampingForce);
		GroundForce = Penetration > 0.0f
			              ? FMath::Max(0.0f, EffectiveTireStiffness * Penetration + TireDamping * SpringSpeed)
			              : 0.0f;
	};

	for (auto Step = 0; Step < NumSubsteps; ++Step)
	{
		EvaluateForces();

		// 1) 半隐式欧拉积分：先更新速度再更新位置
		const float Acceleration = (GroundForce - SpringForce - DampingForce - Mass * Gravity) / Mass;
		SpringSpeed += Acceleration * SubDeltaTime;
		SpringDisplacement += SpringSpeed * SubDeltaTime;

		// 2) 行程限位：到达限位时清零继续压向限位方向的速度
		if (SpringDisplacement < 0.0f)
		{
			SpringDisplacement = 0.0f;
			SpringSpeed = FMath::Max(SpringSpeed, 0.0f);
		}
		else if (SpringDisplacement > MaxLength)
		{
			SpringDisplacement = MaxLength;
			SpringSpeed = FMath::Min(SpringSpeed, 0.0f);
		}
	}

	// 3) 输出按最终状态求值：簧上反力为弹簧与阻尼之和，轮胎载荷为地面法向反力
	EvaluateForces();

	OutSuspensionForce = FMath::Max(0.0f, SpringForce + DampingForce);
	OutForceIntoSurface = GroundForce;
}

void FSingularisMorphVehicleSuspensionSimModule::ComputeSubstepLayout(
	float DeltaTime,
	int32& OutNumSubsteps,
	float& OutSubDeltaTime
) const
{
	const float Mass = FMath::Max(Setup().UnsprungMass, 1.0f);
	const float PeakStiffness = FMath::Max(TireStiffness, Setup().SpringRate);
	const float Omega = FMath::Sqrt(FMath::Max(PeakStiffness, 0.0f) / Mass);

	OutNumSubsteps = FMath::Clamp(
		FMath::CeilToInt32(Omega * DeltaTime / MaxTravelPhaseStep),
		1,
		MaxTravelSubsteps
	);
	OutSubDeltaTime = DeltaTime / static_cast<float>(OutNumSubsteps);
}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
FString FSingularisMorphSuspensionSimModuleData::ToString() const
{
	return FString::Printf(
		TEXT("Module:%s SpringDisplacement:%f SpringSpeed:%f"),
		*DebugString,
		SpringDisplacement,
		SpringSpeed
	);
}
#endif

void FSingularisMorphSuspensionOutputData::FillOutputState(const ISimulationModuleBase* SimModule)
{
	FSimOutputData::FillOutputState(SimModule);

	if (const FSingularisMorphVehicleSuspensionSimModule* Sim = SimModule->Cast<const
		FSingularisMorphVehicleSuspensionSimModule>())
	{
		SpringDisplacement = Sim->SpringDisplacement;
		SpringDisplacementVector = -Sim->Setup().SuspensionAxis * Sim->SpringDisplacement + Sim->GetAnimationOffset();
		SpringSpeed = Sim->SpringSpeed;
		ImpactNormal = Sim->GetImpactNormal();
	}
}

void FSingularisMorphSuspensionOutputData::Lerp(
	const FSimOutputData& InCurrent,
	const FSimOutputData& InNext,
	float Alpha
)
{
	FSimOutputData::Lerp(InCurrent, InNext, Alpha);

	const auto& Current = static_cast<const FSingularisMorphSuspensionOutputData&>(InCurrent);
	const auto& Next = static_cast<const FSingularisMorphSuspensionOutputData&>(InNext);

	SpringDisplacement = FMath::Lerp(Current.SpringDisplacement, Next.SpringDisplacement, Alpha);
	SpringSpeed = FMath::Lerp(Current.SpringSpeed, Next.SpringSpeed, Alpha);
	ImpactNormal = FMath::Lerp(Current.ImpactNormal, Next.ImpactNormal, Alpha);
	SpringDisplacementVector = FMath::Lerp(Current.SpringDisplacementVector, Next.SpringDisplacementVector, Alpha);
}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
FString FSingularisMorphSuspensionOutputData::ToString()
{
	return FString::Printf(
		TEXT("%s, SpringDisplacement=%3.3f, SpringSpeed=%3.3f"),
		*DebugString,
		SpringDisplacement,
		SpringSpeed
	);
}
#endif

#if VEHICLE_DEBUGGING_ENABLED
UE_ENABLE_OPTIMIZATION_SHIP
#endif
