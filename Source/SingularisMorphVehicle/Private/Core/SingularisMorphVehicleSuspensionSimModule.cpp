#include "Core/SingularisMorphVehicleSuspensionSimModule.h"

#include "PBDRigidsSolver.h"
#include "SingularisMorphVehicle.h"
#include "VehicleUtility.h"
#include "Chaos/PBDSuspensionConstraints.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "PhysicsEngine/PhysicsObjectExternalInterface.h"
#include "PhysicsProxy/SuspensionConstraintProxy.h"
#include "SimModule/SimModuleTree.h"
#include "SimModule/TorqueSimModule.h"


#if VEHICLE_DEBUGGING_ENABLED
UE_DISABLE_OPTIMIZATION_SHIP
#endif

using namespace Chaos;

void FSingularisMorphSuspensionSimModuleData::FillSimState(ISimulationModuleBase* SimModule)
{
	if (FSingularisMorphVehicleSuspensionSimModule* Sim = SimModule->Cast<FSingularisMorphVehicleSuspensionSimModule>())
	{
		Sim->SpringDisplacement = SpringDisplacement;
		Sim->LastDisplacement = LastDisplacement;
	}
}

void FSingularisMorphSuspensionSimModuleData::FillNetState(const ISimulationModuleBase* SimModule)
{
	if (const FSingularisMorphVehicleSuspensionSimModule* Sim = SimModule->Cast<const
		FSingularisMorphVehicleSuspensionSimModule>())
	{
		SpringDisplacement = Sim->SpringDisplacement;
		LastDisplacement = Sim->LastDisplacement;
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
	LastDisplacement = FMath::Lerp(MinData.LastDisplacement, MaxData.LastDisplacement, LerpFactor);
}

FSingularisMorphVehicleSuspensionSimModule::FSingularisMorphVehicleSuspensionSimModule(
	const FSingularisMorphSuspensionSettings& Settings
)
	: TSimModuleSettings<FSingularisMorphSuspensionSettings>(Settings),
	  SpringDisplacement(0.f),
	  LastDisplacement(0.f),
	  SpringSpeed(0.f)
{
	AccessSetup().MaxLength = FMath::Abs(Settings.MaxRaise + Settings.MaxDrop);
}

FSingularisMorphVehicleSuspensionSimModule::~FSingularisMorphVehicleSuspensionSimModule() {}

float FSingularisMorphVehicleSuspensionSimModule::GetSpringLength() const
{
	return -(Setup().MaxLength - SpringDisplacement);
}

void FSingularisMorphVehicleSuspensionSimModule::SetSpringLength(float InLength, float WheelRadius)
{
	float DisplacementInput = InLength;
	DisplacementInput = FMath::Max(0.f, DisplacementInput);
	SpringDisplacement = Setup().MaxLength - DisplacementInput;
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

void FSingularisMorphVehicleSuspensionSimModule::OnConstruction_External(const FPhysicsObjectHandle& PhysicsObject)
{
	EnsureIsInGameThreadContext();
	CreateConstraint(PhysicsObject);
}


void FSingularisMorphVehicleSuspensionSimModule::OnTermination_External()
{
	EnsureIsInGameThreadContext();
	DestroyConstraint();
}

void FSingularisMorphVehicleSuspensionSimModule::Simulate(
	float DeltaTime,
	const FAllInputs& Inputs,
	FSimModuleTree& VehicleModuleSystem
)
{
	{
		CurrentTimeDilation = FMath::Max(Inputs.CurrentTimeDilation, SMALL_NUMBER);
		auto ForceIntoSurface = 0.0f;
		if (SpringDisplacement > 0)
		{
			float Damping = Setup().SpringDamping;
			DeltaTime = FMath::Max(DeltaTime, SMALL_NUMBER);
			SpringSpeed = (LastDisplacement - SpringDisplacement) / DeltaTime;

			float StiffnessForce = SpringDisplacement * Setup().SpringRate;
			float DampingForce = SpringSpeed * Damping;
			float SuspensionForce = StiffnessForce - DampingForce;
			LastDisplacement = SpringDisplacement;

			if (SuspensionForce > 0)
			{
				ForceIntoSurface = SuspensionForce * Setup().SuspensionForceEffect;

				if (!ConstraintHandle.IsValid())
					AddLocalForce(Setup().SuspensionAxis * -SuspensionForce, true, false, true, FColor::Green);
			}
		}

		// tell wheels how much they are being pressed into the ground
		if (SimModuleTree && WheelSimTreeIndex != INVALID_IDX)
		{
			if (ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(WheelSimTreeIndex))
			{
				if (FWheelBaseInterface* Wheel = Module->Cast<FWheelBaseInterface>())
					Wheel->SetForceIntoSurface(ForceIntoSurface);
			}
		}
	}

	if (ConstraintHandle.IsValid())
		UpdateConstraint();
}

void FSingularisMorphVehicleSuspensionSimModule::Animate()
{
	FVector Movement = -Setup().SuspensionAxis * (Setup().MaxRaise + GetSpringLength());

	AnimationData.AnimFlags = EAnimationFlags::AnimatePosition;
	AnimationData.AnimationLocOffset = Movement;
}

void FSingularisMorphVehicleSuspensionSimModule::UpdateConstraint()
{
	if (auto Constraint = static_cast<FSuspensionConstraint*>(ConstraintHandle.Constraint))
	{
		if (Constraint && Constraint->IsValid())
		{
			if (FSuspensionConstraintPhysicsProxy* Proxy = Constraint->GetProxy<FSuspensionConstraintPhysicsProxy>())
			{
				FPhysicsSolver* Solver = Proxy->GetSolver<FPhysicsSolver>();
				if (!Solver) return;

				const FVector& CurrentTargetPosition = GetTargetPosition();
				const FVector& CurrentImpactNormal = GetImpactNormal();
				const IPhysicsProxyBase* GroundProxy = GetHitProxy();
				const bool bCurrentWheelInContact = IsWheelInContact();
				Solver->SetSuspensionTarget(
					Constraint,
					CurrentTargetPosition,
					CurrentImpactNormal,
					bCurrentWheelInContact,
					GroundProxy
				);
			}
		}
	}
}

void FSingularisMorphVehicleSuspensionSimModule::CreateConstraint(const FPhysicsObjectHandle& PhysicsObject)
{
	EnsureIsInGameThreadContext();

	// 幂等：已持有约束时不重建，否则重复构建会让旧约束失去句柄而永久残留
	if (ConstraintHandle.IsValid()) return;

	const FVector& LocalOffset = GetInitialParticleTransform().GetLocation();

	if (auto Scene = FPhysicsObjectExternalInterface::GetScene({&PhysicsObject, 1}))
	{
		FLockedWritePhysicsObjectExternalInterface Interface = FPhysicsObjectExternalInterface::LockWrite(Scene);
		if (Interface->GetParticle(PhysicsObject) != nullptr)
		{
			ConstraintHandle = FPhysicsInterface::CreateSuspension(PhysicsObject, LocalOffset);

			// 约束创建失败时悬挂退回纯力模拟（无硬限位、无地面法向修正），
			// 表现为车轮下坠与抓地异常，必须在日志中可见
			if (!ConstraintHandle.IsValid())
			{
				UE_LOG(
					LogSingularisMorphVehicle,
					Warning,
					TEXT(
						"Suspension[GUID=%d]: failed to create suspension constraint - falling back to force-only simulation"
					),
					GetGuid()
				);
				return;
			}

			if (auto Constraint = static_cast<FSuspensionConstraint*>(ConstraintHandle.Constraint))
			{
				Constraint->SetHardstopStiffness(1.0f);
				Constraint->SetSpringStiffness(Setup().SpringRate * 0.25f);
				Constraint->SetSpringPreload(Setup().SpringPreload);
				Constraint->SetSpringDamping(Setup().SpringDamping * 5.0f);
				Constraint->SetMinLength(-Setup().MaxRaise);
				Constraint->SetMaxLength(Setup().MaxDrop);
				Constraint->SetAxis(-Setup().SuspensionAxis);
			}
		}
	}
}

void FSingularisMorphVehicleSuspensionSimModule::DestroyConstraint()
{
	EnsureIsInGameThreadContext();

	// 幂等：约束从未创建或已释放时句柄无效，对无效句柄下命令会命中未初始化内存
	if (!ConstraintHandle.IsValid()) return;

	// 释放后立即失效句柄，避免重复终止回调对同一约束二次释放。
	// ReleaseConstraint 要求非 const 句柄，故直接作用于成员
	FPhysicsCommand::ExecuteWrite(
		ConstraintHandle,
		[this](const FPhysicsConstraintHandle&)
		{
			FPhysicsInterface::ReleaseConstraint(ConstraintHandle);
		}
	);

	ConstraintHandle = FPhysicsConstraintHandle();
}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
FString FSingularisMorphSuspensionSimModuleData::ToString() const
{
	return FString::Printf(
		TEXT("Module:%s SpringDisplacement:%f LastDisplacement:%f"),
		*DebugString,
		SpringDisplacement,
		LastDisplacement
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

		// Required to recalculate suspension length on the game thread (post physics integration) so it looks correct visually
		ImpactPosition = Sim->GetTargetPosition();
		ModuleLocalPosition = Sim->GetParentRelativeTransform().GetLocation();
		LocalSuspensionAxis = Sim->Setup().SuspensionAxis;
		MaxRaise = Sim->Setup().MaxRaise;
		MaxDrop = Sim->Setup().MaxDrop;
		bWheelHit = Sim->IsWheelInContact();
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

	ImpactPosition = Next.ImpactPosition;
	ModuleLocalPosition = FMath::Lerp(Current.ModuleLocalPosition, Next.ModuleLocalPosition, Alpha);
	LocalSuspensionAxis = FMath::Lerp(Current.LocalSuspensionAxis, Next.LocalSuspensionAxis, Alpha);
	MaxRaise = FMath::Lerp(Current.MaxRaise, Next.MaxRaise, Alpha);
	MaxDrop = FMath::Lerp(Current.MaxDrop, Next.MaxDrop, Alpha);
	bWheelHit = Next.bWheelHit;
}


void FSingularisMorphSuspensionOutputData::GetFinalAnimDataGameThread(
	const FTransform& NewTransform,
	FSimModuleAnimationData& AnimDataOut
)
{
	FSimOutputData::GetFinalAnimDataGameThread(NewTransform, AnimDataOut);

	FVector WorldLocation = NewTransform.TransformPosition(ModuleLocalPosition);
	FVector WorldDirection = NewTransform.TransformVector(LocalSuspensionAxis);
	FVector NewStart = WorldLocation - WorldDirection * MaxRaise;
	FVector NewEnd = ImpactPosition;
	float Length = FVector::DotProduct(NewStart - NewEnd, WorldDirection);
	Length = FMath::Clamp(Length, -(MaxDrop + MaxRaise), 0);
	FVector Movement = -LocalSuspensionAxis * (Length + MaxRaise);

	if (!bWheelHit)
		Movement = LocalSuspensionAxis * MaxDrop;

	AnimDataOut.AnimFlags = EAnimationFlags::AnimatePosition;
	AnimDataOut.AnimationLocOffset = Movement;
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
