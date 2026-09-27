#include "Core/SingularisMorphVehicleWheelSimModule.h"

#include "SimModule/SimModuleTree.h"

#if VEHICLE_DEBUGGING_ENABLED
UE_DISABLE_OPTIMIZATION_SHIP
#endif

FSingularisMorphWheelSimModule::FSingularisMorphWheelSimModule(
	const Chaos::FWheelSettings& InSettings,
	const FSingularisMorphWheelSteeringSettings& InSteeringSettings
)
	: FWheelSimModule(InSettings),
	  SteeringSettings(InSteeringSettings),
	  CurrentSteeringAngleDegrees(InSteeringSettings.InitialSteeringAngle) {}

void FSingularisMorphWheelSimModule::Simulate(
	float DeltaTime,
	const Chaos::FAllInputs& Inputs,
	Chaos::FSimModuleTree& VehicleModuleSystem
)
{
	// 1) 未启用转向（或未配置有效最大转角）时完全保持官方车轮行为
	const float BaseMaxSteeringAngle = Setup().MaxSteeringAngle;
	if (!SteeringSettings.bEnabled || BaseMaxSteeringAngle <= KINDA_SMALL_NUMBER)
	{
		FWheelSimModule::Simulate(DeltaTime, Inputs, VehicleModuleSystem);
		return;
	}

	UpdateSteeringAngle(DeltaTime, Inputs);

	// 2) 基类以「输入 × 本模块最大转角」得出转向角，故此处写入归一化后的实际转向角。
	//    Inputs 为 const 引用而 ControlInputs 为非 const 指针，可直接写入共享容器；
	//    物理步末尾还原原值，使同帧内其余车轮与模块仍读到原始控制输入。
	//    输入配置缺少 Steering 项或该项不是一维轴（如被配成二维轴，取模长会丢符号）时
	//    无处写入（基类同样得不到正确输入，转角保持为 0），此时直接交给基类
	FInputInterface& Controls = Inputs.GetControls();
	const bool bCanInject = Controls.NameMap.Contains(Chaos::SteeringControlName) &&
		Controls.GetValueType(Chaos::SteeringControlName) == EModuleInputValueType::MAxis1D;

	FModuleInputValue OriginalSteering{};
	if (bCanInject)
	{
		OriginalSteering = Controls.GetValue(Chaos::SteeringControlName);
		Controls.SetValue(
			Chaos::SteeringControlName,
			FModuleInputValue(CurrentSteeringAngleDegrees / BaseMaxSteeringAngle),
			false
		);
	}

	FWheelSimModule::Simulate(DeltaTime, Inputs, VehicleModuleSystem);

	if (bCanInject)
		Controls.SetValue(Chaos::SteeringControlName, OriginalSteering, false);

	// 3) 记录本步侧向力：下一物理步按回正力矩削减转向角
	const FVector FrictionForce = GetForceFromFriction();
	LastLateralForce = Setup().Axis == Chaos::EWheelAxis::X ? FrictionForce.Y : FrictionForce.X;
}

float FSingularisMorphWheelSimModule::ComputeTargetSteeringAngle(const Chaos::FAllInputs& Inputs) const
{
	const float InNormSteering = FMath::Clamp(
		Inputs.GetControls().GetMagnitude(Chaos::SteeringControlName),
		-1.0f,
		1.0f
	);

	// 1) 车速敏感：按 |车速| 查表求得转向倍率，降低高速下的转向灵敏度
	auto SpeedScale = 1.0f;
	if (!SteeringSettings.SpeedSteeringCurve.IsEmpty())
	{
		const float SpeedKmh = Chaos::CmSToKmH(FMath::Abs(GetLongitudinalSpeed()));
		SpeedScale = FMath::Clamp(SteeringSettings.SpeedSteeringCurve.EvaluateY(SpeedKmh), 0.0f, 1.0f);
	}

	// 2) 参考角：外侧轮的目标角
	const float ReferenceAngle = InNormSteering * SteeringSettings.MaxSteeringAngle * SpeedScale;
	if (FMath::Abs(ReferenceAngle) <= KINDA_SMALL_NUMBER)
		return 0.0f;

	const float LateralSign = GetLateralSideSign();
	if (FMath::IsNearlyZero(LateralSign))
		return ReferenceAngle;

	// 3) 内侧轮转角大于外侧轮：转向侧的轮为内侧轮（正转向角使车头偏向 +Y）
	const float SteerSign = FMath::Sign(ReferenceAngle);
	const bool bInnerWheel = SteerSign * LateralSign > 0.0f;
	if (!bInnerWheel)
		return ReferenceAngle;

	switch (SteeringSettings.SteeringType)
	{
	case ESingularisMorphVehicleSteeringType::AngleRatio:
		{
			const float Ratio = FMath::Clamp(SteeringSettings.AngleRatio, KINDA_SMALL_NUMBER, 1.0f);
			return ReferenceAngle / Ratio;
		}

	case ESingularisMorphVehicleSteeringType::Ackermann:
		{
			// 理想阿克曼几何：cot(外侧) − cot(内侧) = 轮距 / 轴距。
			// 参考角即外侧轮转角，故外侧轮轨迹半径 R = 轴距 / tan(参考角)，
			// 内侧轮轨迹半径 = R − 轮距（两轮轨迹半径之差就是整条轮距）。
			// 低速满舵时内侧半径可能退化为零或负（超出机械极限），此时直接取最大转角
			const float TurnRadius = SteeringSettings.WheelBase / FMath::Tan(
				FMath::DegreesToRadians(FMath::Abs(ReferenceAngle))
			);
			const float InnerRadius = TurnRadius - SteeringSettings.TrackWidth;
			if (InnerRadius <= KINDA_SMALL_NUMBER)
				return SteerSign * SteeringSettings.MaxSteeringAngle;

			const float InnerAngle = FMath::RadiansToDegrees(
				FMath::Atan(SteeringSettings.WheelBase / InnerRadius)
			);
			return SteerSign * InnerAngle;
		}

	default:
	case ESingularisMorphVehicleSteeringType::SingleAngle:
		return ReferenceAngle;
	}
}

void FSingularisMorphWheelSimModule::UpdateSteeringAngle(
	float DeltaTime,
	const Chaos::FAllInputs& Inputs
)
{
	const float TargetAngle = ComputeTargetSteeringAngle(Inputs);
	const float MaxAngle = FMath::Abs(SteeringSettings.MaxSteeringAngle);

	// 1) 角速率限幅：向目标角逼近（上升）与离开目标角（回落）使用不同速率上限，
	//    构成转向的执行过程——输入阶跃不再等价于转角阶跃
	const float DeltaAngle = TargetAngle - CurrentSteeringAngleDegrees;
	const bool bBuildingAngle = FMath::Abs(TargetAngle) > FMath::Abs(CurrentSteeringAngleDegrees);
	const float Rate = FMath::Max(
		bBuildingAngle ? SteeringSettings.SteeringRiseRate : SteeringSettings.SteeringFallRate,
		0.0f
	);
	CurrentSteeringAngleDegrees += FMath::Clamp(DeltaAngle, -Rate * DeltaTime, Rate * DeltaTime);

	// 2) 轮胎回正效应：以侧向力大小为比例持续削减转向角幅值，使稳态转角小于目标角
	//    （模拟驾驶员必须持续施加转向力矩以维持转角），并在附着突变时产生转向反馈。
	//    取绝对值而非带符号力矩：回正力矩总是指向减小转角幅值的方向
	if (SteeringSettings.SelfAligningTorqueGain > 0.0f && FMath::Abs(CurrentSteeringAngleDegrees) > KINDA_SMALL_NUMBER)
	{
		const float AligningStep = FMath::Abs(LastLateralForce) * SteeringSettings.SelfAligningTorqueGain * DeltaTime;
		const float Sign = FMath::Sign(CurrentSteeringAngleDegrees);
		CurrentSteeringAngleDegrees = Sign > 0.0f
			                              ? FMath::Max(CurrentSteeringAngleDegrees - AligningStep, 0.0f)
			                              : FMath::Min(CurrentSteeringAngleDegrees + AligningStep, 0.0f);
	}

	// 3) 机械限位
	CurrentSteeringAngleDegrees = FMath::Clamp(CurrentSteeringAngleDegrees, -MaxAngle, MaxAngle);
}

float FSingularisMorphWheelSimModule::GetLongitudinalSpeed() const
{
	const FVector& LocalVelocity = GetLocalLinearVelocity();
	const float Longitudinal = Setup().Axis == Chaos::EWheelAxis::X ? LocalVelocity.X : LocalVelocity.Y;
	return Setup().ReverseDirection ? -Longitudinal : Longitudinal;
}

float FSingularisMorphWheelSimModule::GetLateralSideSign() const
{
	// 以模块的组件变换（相对车辆参考变换）的横向分量取符号：
	// 该变换在模块构造时写入且不随后续运动变化，恒为「轮位相对车辆中线」的横向偏移；
	// 而 GetParentRelativeTransform 是相对父粒子（簇质心）的位姿，质心横向偏移时两侧会判为同一侧
	const auto LateralPosition = static_cast<float>(GetComponentTransform().GetLocation().Y);
	return FMath::Abs(LateralPosition) > KINDA_SMALL_NUMBER ? FMath::Sign(LateralPosition) : 0.0f;
}

#if VEHICLE_DEBUGGING_ENABLED
UE_ENABLE_OPTIMIZATION_SHIP
#endif
