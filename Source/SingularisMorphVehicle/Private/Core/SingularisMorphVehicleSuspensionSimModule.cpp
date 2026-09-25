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
		Sim->DisplacementVelocity = DisplacementVelocity;
	}
}

void FSingularisMorphSuspensionSimModuleData::FillNetState(const ISimulationModuleBase* SimModule)
{
	if (const FSingularisMorphVehicleSuspensionSimModule* Sim = SimModule->Cast<const
		FSingularisMorphVehicleSuspensionSimModule>())
	{
		SpringDisplacement = Sim->SpringDisplacement;
		LastDisplacement = Sim->LastDisplacement;
		DisplacementVelocity = Sim->DisplacementVelocity;
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
	DisplacementVelocity = FMath::Lerp(MinData.DisplacementVelocity, MaxData.DisplacementVelocity, LerpFactor);
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

	// 初始状态 = 安放点（位移 MaxDrop 处，轮心与组件位置重合）：
	// 出生与动态加装时轮胎直接出现在安放点，无瞬移；落地首帧几何值与静态一致，无跳变
	SpringDisplacement = FMath::Clamp(Settings.MaxDrop, 0.f, AccessSetup().MaxLength);
	LastDisplacement = SpringDisplacement;
}

FSingularisMorphVehicleSuspensionSimModule::~FSingularisMorphVehicleSuspensionSimModule() {}

float FSingularisMorphVehicleSuspensionSimModule::GetSpringLength() const
{
	return -(Setup().MaxLength - SpringDisplacement);
}

void FSingularisMorphVehicleSuspensionSimModule::SetSpringLength(float InLength, float WheelRadius)
{
	// 仅记录几何目标位移：接触时由 Simulate 采纳（地面钳制车轮），
	// 空中时不再直接覆盖，由动力学积分接管
	const float DisplacementInput = FMath::Max(0.f, InLength);
	GeometricDisplacement = FMath::Clamp(Setup().MaxLength - DisplacementInput, 0.f, Setup().MaxLength);
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
	CurrentTimeDilation = FMath::Max(Inputs.CurrentTimeDilation, SMALL_NUMBER);
	const float ClampedDT = FMath::Max(DeltaTime, SMALL_NUMBER);

	// 真实触地判据：射线命中且地面已达安放态轮底（几何位移 ≥ MaxDrop）。
	// 命中但间隙未消除（地面在安放态轮底之下）按空中处理：直接采纳几何值会让
	// 车轮瞬移下探远端地面——接近地面时瞬移落地、离地时先下探再猛弹回的根源。
	// 首触时几何位移 == MaxDrop，与安放点重合，连续无跳变
	const bool bTouching = IsWheelInContact() && GeometricDisplacement >= Setup().MaxDrop;

	if (bTouching)
	{
		// 触地：地面钳制车轮，位移由几何决定；
		// 速度取几何变化率，作为离地瞬间动力学积分的初速度
		SpringDisplacement = GeometricDisplacement;
		DisplacementVelocity = FMath::Clamp(
			(SpringDisplacement - LastDisplacement) / ClampedDT, -1e4f, 1e4f);
	}
	else
	{
		// 空中：虚拟轮质量 + 弹簧 + 阻尼一维积分，舒展有过程而非瞬时跳变。
		// 视觉舒展位 = 安放点（位移 MaxDrop 处，轮心与组件位置重合）：
		// 弹簧把轮心拉回安放点，沿压缩方向（位移增大为正），AboveRest 为距安放点的距离
		const float Mass = FMath::Max(Setup().VirtualWheelMassKg, 1.0f);
		const float AboveRest = SpringDisplacement - Setup().MaxDrop;
		const float ExtensionForce = Setup().SpringRate * AboveRest + Setup().SpringPreload;
		const float Accel = -(ExtensionForce + Setup().SpringDamping * DisplacementVelocity) / Mass;
		DisplacementVelocity += Accel * ClampedDT;
		SpringDisplacement += DisplacementVelocity * ClampedDT;

		// 视觉行程限位：下限 = 安放点（MaxDrop），上限 = 全压缩（MaxLength）
		if (SpringDisplacement <= Setup().MaxDrop)
		{
			SpringDisplacement = Setup().MaxDrop;
			DisplacementVelocity = FMath::Max(DisplacementVelocity, 0.f);
		}
		else if (SpringDisplacement >= Setup().MaxLength)
		{
			SpringDisplacement = Setup().MaxLength;
			DisplacementVelocity = FMath::Min(DisplacementVelocity, 0.f);
		}
	}

	// SpringSpeed 沿用旧约定：(LastDisplacement - 当前位移) / dt，即 -压缩速率
	SpringSpeed = -DisplacementVelocity;
	LastDisplacement = SpringDisplacement;

	// 载荷沿用原版语义：跟随几何位移、射线命中即输出。
	// 间隙阶段载荷随几何值渐进（与原版一致，落地渐进、停放抓地连续无死区），
	// 空中不输出；解析力仅在约束创建失败时兜底车体
	float ForceIntoSurface = 0.0f;
	if (IsWheelInContact() && GeometricDisplacement > 0.f)
	{
		const float StiffnessForce = GeometricDisplacement * Setup().SpringRate;
		const float DampingForce = SpringSpeed * Setup().SpringDamping;
		const float SuspensionForce = StiffnessForce - DampingForce;
		if (SuspensionForce > 0.f)
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

	if (ConstraintHandle.IsValid())
		UpdateConstraint();
}

void FSingularisMorphVehicleSuspensionSimModule::Animate()
{
	// 零偏移点 = 安放点（位移 MaxDrop，轮心与组件位置重合）：
	// 等价于 up×(SpringDisplacement − MaxDrop)，接触时即地面真值，悬空时收敛到安放点
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

	// 按值捕获句柄：释放命令可能被延迟到物理线程执行（本插件对集群根的其它写入
	// 均走 Solver->EnqueueCommandImmediate，同理），届时模块对象可能已被删除；
	// 若 lambda 读取成员，既可能拿到下方已复位的空句柄（真实约束泄漏为幻影弹簧，
	// 持续按陈旧目标对车体施力，表现为加装越多车越高、随机大幅抖动），
	// 也可能悬空访问已删除模块的成员
	FPhysicsConstraintHandle HandleToRelease = ConstraintHandle;

	// 先复位成员再下发命令：命令只携带句柄副本，重复终止回调被幂等挡下
	ConstraintHandle = FPhysicsConstraintHandle();

	FPhysicsCommand::ExecuteWrite(
		HandleToRelease,
		[HandleToRelease](const FPhysicsConstraintHandle&)
		{
			FPhysicsInterface::ReleaseConstraint(HandleToRelease);
		}
	);
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

	// 真实触地（命中且 Length ≥ −MaxRaise，地面已达安放态轮底）才按几何重算，
	// 与物理线程 Animate 的触地映射逐点一致；命中但间隙未消除、或无命中时，
	// 使用动力学积分位移——车轮保持在安放点，不瞬移下探远端地面
	const bool bTrueTouch = bWheelHit && Length >= -MaxRaise;
	if (!bTrueTouch)
		Movement = -LocalSuspensionAxis * (SpringDisplacement - MaxDrop);

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
