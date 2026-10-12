#include "Components/SingularisMorphVehicleSoundComponent.h"

#include <MetasoundSource.h>
#include <Components/SceneComponent.h>
#include <GameFramework/Actor.h>

#include "SingularisMorphVehicle.h"
#include "Components/AudioComponent.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"

namespace
{
	/** 参数变更检测的最小差异（归一化值分辨率） */
	constexpr auto ParameterEpsilon = 1e-4f;
}

USingularisMorphVehicleSoundComponent::USingularisMorphVehicleSoundComponent()
{
	SetIsReplicatedByDefault(false);

	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.bCanEverTick = true;

	bAutoActivate = true;
}

void USingularisMorphVehicleSoundComponent::BeginPlay()
{
	Super::BeginPlay();

	// 1) 装配音频组件（播放载体）
	ProxyAudioComponent();

	// 2) 订阅仿真组件事件（推路线）
	ObserveSimulationComponent();

	// 3) 装配载具根组件碰撞回调（仿真输出之外的物理真相）
	ProxyCollisionEvents();

	// 4) 自动播放
	if (bAutoPlay)
		Play();
}

void USingularisMorphVehicleSoundComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction
)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 拉路线：轮询连续状态并推送音频参数
	PushContinuousParameters();
}

void USingularisMorphVehicleSoundComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 1) 解绑全部事件订阅（仿真事件 + 根组件碰撞事件）
	UnbindSimulationEvents();
	if (const AActor* Owner = GetOwner())
	{
		if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
			RootPrimitive->OnComponentHit.RemoveAll(this);
	}

	// 2) 停止发声：音频组件为外部显式装配，本组件不持有所有权，仅停止播放
	if (AudioComponent)
		AudioComponent->Stop();

	AudioComponent = nullptr;
	SimulationComponent = nullptr;
	ResetPushCaches();

	Super::EndPlay(EndPlayReason);
}

void USingularisMorphVehicleSoundComponent::Play()
{
	if (!AudioComponent || !MetaSoundSource) return;

	// 幂等：播放中不重启，避免声音从头起播
	if (AudioComponent->IsPlaying()) return;

	AudioComponent->SetSound(MetaSoundSource);
	AudioComponent->Play();

	// 声音重启后音频侧参数复位为默认值，必须强制重推全部参数
	ResetPushCaches();
}

void USingularisMorphVehicleSoundComponent::Stop() const
{
	if (!AudioComponent) return;
	if (!AudioComponent->IsPlaying()) return;

	AudioComponent->Stop();
}

void USingularisMorphVehicleSoundComponent::HandleGearShifted(const int32 /*OldGear*/, const int32 /*NewGear*/) const
{
	if (!AudioComponent) return;

	AudioComponent->SetTriggerParameter(GearShiftedTriggerName);
}

void USingularisMorphVehicleSoundComponent::HandleWheelSlipStarted(const int32 /*WheelModuleGuid*/) const
{
	if (!AudioComponent) return;

	AudioComponent->SetTriggerParameter(WheelSlipStartedTriggerName);
}

void USingularisMorphVehicleSoundComponent::HandleWheelSlipStopped(const int32 /*WheelModuleGuid*/) const
{
	if (!AudioComponent) return;

	AudioComponent->SetTriggerParameter(WheelSlipStoppedTriggerName);
}

void USingularisMorphVehicleSoundComponent::HandleSuspensionBottomedOut(const int32 /*SuspensionModuleGuid*/) const
{
	if (!AudioComponent) return;

	AudioComponent->SetTriggerParameter(SuspensionBottomedOutTriggerName);
}

void USingularisMorphVehicleSoundComponent::HandleCollisionHit(
	UPrimitiveComponent* /*HitComponent*/,
	AActor* /*OtherActor*/,
	UPrimitiveComponent* /*OtherComp*/,
	FVector NormalImpulse,
	const FHitResult& /*Hit*/



) const
{
	if (!AudioComponent) return;

	// 微小接触（刮蹭/沉降）不构成碰撞事件，避免触发参数刷屏
	if (NormalImpulse.SizeSquared() < FMath::Square(CollisionImpulseThreshold)) return;

	// 强度先行、触发随后：MetaSound 图在同一帧内可读到本次冲量
	AudioComponent->SetFloatParameter(ImpactStrengthParameterName, NormalImpulse.Size());
	AudioComponent->SetTriggerParameter(CollisionImpactTriggerName);
}

void USingularisMorphVehicleSoundComponent::ProxyAudioComponent()
{
	// 1) 解析音频组件引用（显式装配，引用无效时本组件保持静默）
	UAudioComponent* Resolved = Cast<UAudioComponent>(AudioComponentReference.GetComponent(GetOwner()));
	if (!IsValid(Resolved))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] ProxyAudioComponent：音频组件引用无效，本组件保持静默"),
			*GetNameSafe(GetOwner())
		);
		return;
	}

	AudioComponent = Resolved;

	// 2) 指派 MetaSound 音源（已配置时；播放路径幂等重指派）
	if (MetaSoundSource)
		AudioComponent->SetSound(MetaSoundSource);

	UE_LOG(
		LogSingularisMorphVehicle,
		Display,
		TEXT("[%s] ProxyAudioComponent：音频组件 %s 装配完成"),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(Resolved)
	);
}

void USingularisMorphVehicleSoundComponent::ObserveSimulationComponent()
{
	// 1) 解析仿真组件引用（显式装配，引用无效时无事件可订阅）
	USingularisMorphVehicleSimulationComponent* Resolved = Cast<USingularisMorphVehicleSimulationComponent>(
		SimulationComponentReference.GetComponent(GetOwner())
	);
	if (!IsValid(Resolved))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] ObserveSimulationComponent：仿真组件引用无效，无法订阅事件"),
			*GetNameSafe(GetOwner())
		);
		return;
	}

	SimulationComponent = Resolved;

	// 2) 订阅仿真组件的离散事件（推路线）
	Resolved->OnGearShiftedEvent.AddDynamic(this, &USingularisMorphVehicleSoundComponent::HandleGearShifted);
	Resolved->OnWheelSlipStartedEvent.AddDynamic(
		this,
		&USingularisMorphVehicleSoundComponent::HandleWheelSlipStarted
	);
	Resolved->OnWheelSlipStoppedEvent.AddDynamic(
		this,
		&USingularisMorphVehicleSoundComponent::HandleWheelSlipStopped
	);
	Resolved->OnSuspensionBottomedOutEvent.AddDynamic(
		this,
		&USingularisMorphVehicleSoundComponent::HandleSuspensionBottomedOut
	);

	UE_LOG(
		LogSingularisMorphVehicle,
		Display,
		TEXT("[%s] ObserveSimulationComponent：已绑定仿真组件 %s"),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(Resolved)
	);
}

void USingularisMorphVehicleSoundComponent::ProxyCollisionEvents()
{
	// 载具根组件为物理接触的来源：非图元根（无碰撞）时跳过装配
	UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(
		GetOwner() ? GetOwner()->GetRootComponent() : nullptr
	);
	if (!IsValid(RootPrimitive))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] ProxyCollisionEvents：载具根组件不是图元组件，无法订阅碰撞事件"),
			*GetNameSafe(GetOwner())
		);
		return;
	}

	RootPrimitive->OnComponentHit.AddDynamic(
		this,
		&USingularisMorphVehicleSoundComponent::HandleCollisionHit
	);
}

void USingularisMorphVehicleSoundComponent::UnbindSimulationEvents() const
{
	// RemoveAll 为非 const 委托调用，须以可变指针访问目标组件
	USingularisMorphVehicleSimulationComponent* Simulation = SimulationComponent.Get();
	if (!Simulation) return;

	Simulation->OnGearShiftedEvent.RemoveAll(this);
	Simulation->OnWheelSlipStartedEvent.RemoveAll(this);
	Simulation->OnWheelSlipStoppedEvent.RemoveAll(this);
	Simulation->OnSuspensionBottomedOutEvent.RemoveAll(this);
}

void USingularisMorphVehicleSoundComponent::PushContinuousParameters()
{
	if (!AudioComponent) return;

	const USingularisMorphVehicleSimulationComponent* Simulation = SimulationComponent.Get();
	if (!Simulation) return;

	// 1) 引擎转速归一化：分母取仿真组件聚合的引擎最大转速，取不到（手动注册路径/无原动机）
	//    时回退到 FallbackMaxEngineRPM；瞬时过速钳制到 [0..1]
	const float MaxRPM = Simulation->GetEngineMaxRPM();
	const float RPMDenominator = MaxRPM > 0.0f ? MaxRPM : FMath::Max(FallbackMaxEngineRPM, 1.0f);
	const float NormalizedRPM = FMath::Clamp(Simulation->GetEngineRPM() / RPMDenominator, 0.0f, 1.0f);

	// 2) 车速归一化（公里/小时）
	const float NormalizedSpeed = FMath::Clamp(
		Simulation->GetVehicleSpeed() / FMath::Max(MaxSpeedKmh, 1.0f),
		0.0f,
		1.0f
	);

	// 3) 挡位与引擎扭矩原始值直传：正负号与量纲承载物理语义，映射由 MetaSound 图决定
	const int32 CurrentGear = Simulation->GetCurrentGear();
	const float EngineTorque = Simulation->GetEngineTorque();

	// 4) 变更检测后写入：音频参数语义为持续保持，重复写同值只产生冗余音频线程流量
	if (!FMath::IsNearlyEqual(NormalizedRPM, LastPushedRPM, ParameterEpsilon))
	{
		AudioComponent->SetFloatParameter(RPMParameterName, NormalizedRPM);
		LastPushedRPM = NormalizedRPM;
	}

	if (!FMath::IsNearlyEqual(NormalizedSpeed, LastPushedSpeed, ParameterEpsilon))
	{
		AudioComponent->SetFloatParameter(SpeedParameterName, NormalizedSpeed);
		LastPushedSpeed = NormalizedSpeed;
	}

	if (!FMath::IsNearlyEqual(EngineTorque, LastPushedTorque, ParameterEpsilon))
	{
		AudioComponent->SetFloatParameter(EngineTorqueParameterName, EngineTorque);
		LastPushedTorque = EngineTorque;
	}

	if (CurrentGear != LastPushedGear)
	{
		AudioComponent->SetIntParameter(GearParameterName, CurrentGear);
		LastPushedGear = CurrentGear;
	}
}

void USingularisMorphVehicleSoundComponent::ResetPushCaches()
{
	LastPushedRPM = -1.0f;
	LastPushedSpeed = -1.0f;
	LastPushedTorque = -1.0f;
	LastPushedGear = INDEX_NONE;
}
