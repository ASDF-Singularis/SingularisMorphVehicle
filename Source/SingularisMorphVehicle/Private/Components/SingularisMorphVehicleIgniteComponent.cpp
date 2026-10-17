#include "Components/SingularisMorphVehicleIgniteComponent.h"

#include <TimerManager.h>
#include <Engine/World.h>
#include <GameFramework/Actor.h>
#include <Net/UnrealNetwork.h>
#include <SimModule/SimulationModuleBase.h>

#include "SingularisMorphVehicle.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"

USingularisMorphVehicleIgniteComponent::USingularisMorphVehicleIgniteComponent()
{
	SetIsReplicatedByDefault(true);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

void USingularisMorphVehicleIgniteComponent::BeginPlay()
{
	Super::BeginPlay();

	// 1) 解析仿真组件引用
	ResolveSimulationComponent();

	// 2) 初始熄火：直接禁用油门，等待点火成功后再放行
	ApplyThrottleGate();
}

void USingularisMorphVehicleIgniteComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 1) 清理点火计时
	if (const UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(IgnitionTimer);

	// 2) 释放缓存
	CachedSimulationComponent = nullptr;

	Super::EndPlay(EndPlayReason);
}

void USingularisMorphVehicleIgniteComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps
) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(USingularisMorphVehicleIgniteComponent, IgnitionState);
}

void USingularisMorphVehicleIgniteComponent::StartIgnition()
{
	// 1) 服务器权威
	if (!IsValid(GetOwner()) || !GetOwner()->HasAuthority()) return;

	// 2) 仅熄火状态可开始点火
	if (IgnitionState != ESingularisMorphVehicleIgnitionState::Off) return;

	// 3) 没有仿真组件就无法接管油门，避免进入无法生效的点火状态
	if (!CachedSimulationComponent.IsValid())
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] 点火组件：仿真组件引用无效，无法开始点火"),
			*GetNameSafe(GetOwner())
		);
		return;
	}

	UWorld* World = GetWorld();
	if (!World) return;

	// 4) 进入点火中：禁用油门并广播开始
	SetIgnitionState(ESingularisMorphVehicleIgnitionState::Igniting);

	// 5) 点火时长归零时立即完成
	if (IgnitionDuration <= 0.0f)
	{
		FinishIgnition();
		return;
	}

	// 6) 服务器计时，结束后进入运行中；客户端由状态复制驱动
	World->GetTimerManager().ClearTimer(IgnitionTimer);
	World->GetTimerManager().SetTimer(
		IgnitionTimer,
		this,
		&USingularisMorphVehicleIgniteComponent::FinishIgnition,
		IgnitionDuration,
		false
	);
}

void USingularisMorphVehicleIgniteComponent::CancelIgnition()
{
	// 1) 服务器权威
	if (!IsValid(GetOwner()) || !GetOwner()->HasAuthority()) return;

	// 2) 仅点火中可取消，运行中请使用熄火
	if (IgnitionState != ESingularisMorphVehicleIgnitionState::Igniting) return;

	// 3) 停止计时并回到熄火
	if (const UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(IgnitionTimer);

	SetIgnitionState(ESingularisMorphVehicleIgnitionState::Off);
}

void USingularisMorphVehicleIgniteComponent::Shutdown()
{
	// 1) 服务器权威
	if (!IsValid(GetOwner()) || !GetOwner()->HasAuthority()) return;

	// 2) 点火中调用等价于取消点火
	if (IgnitionState == ESingularisMorphVehicleIgnitionState::Igniting)
	{
		CancelIgnition();
		return;
	}

	// 3) 仅运行中可熄火
	if (IgnitionState != ESingularisMorphVehicleIgnitionState::Running) return;

	if (const UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(IgnitionTimer);

	SetIgnitionState(ESingularisMorphVehicleIgnitionState::Off);
}

void USingularisMorphVehicleIgniteComponent::OnRep_IgnitionState(
	const ESingularisMorphVehicleIgnitionState OldState
) const
{
	ApplyState(OldState);
}

void USingularisMorphVehicleIgniteComponent::FinishIgnition()
{
	if (IgnitionState != ESingularisMorphVehicleIgnitionState::Igniting) return;

	SetIgnitionState(ESingularisMorphVehicleIgnitionState::Running);
}

void USingularisMorphVehicleIgniteComponent::ResolveSimulationComponent()
{
	USingularisMorphVehicleSimulationComponent* Resolved =
		Cast<USingularisMorphVehicleSimulationComponent>(
			SimulationComponentReference.GetComponent(GetOwner())
		);
	if (!IsValid(Resolved))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] 点火组件：仿真组件引用无效，油门闸门不可用"),
			*GetNameSafe(GetOwner())
		);
		return;
	}

	CachedSimulationComponent = Resolved;
}

void USingularisMorphVehicleIgniteComponent::SetIgnitionState(
	const ESingularisMorphVehicleIgnitionState NewState
)
{
	// 1) 服务器权威
	if (!IsValid(GetOwner()) || !GetOwner()->HasAuthority()) return;

	// 2) 幂等性检查，若状态未变更则直接返回
	if (IgnitionState == NewState) return;

	// 3) 捕获旧状态后写入新状态
	const ESingularisMorphVehicleIgnitionState OldState = IgnitionState;
	IgnitionState = NewState;

	// 4) 响应式编程：服务器应用副作用
	ApplyState(OldState);
}

void USingularisMorphVehicleIgniteComponent::ApplyState(
	const ESingularisMorphVehicleIgnitionState OldState
) const
{
	// 1) 油门闸门只认运行中
	ApplyThrottleGate();

	// 2) 按状态迁移广播事件
	if (OldState == IgnitionState) return;

	switch (IgnitionState)
	{
	case ESingularisMorphVehicleIgnitionState::Off:
		if (OldState == ESingularisMorphVehicleIgnitionState::Igniting)
			OnIgnitionCancelled.Broadcast();
		else if (OldState == ESingularisMorphVehicleIgnitionState::Running)
			OnIgnitionShutdown.Broadcast();
		break;
	case ESingularisMorphVehicleIgnitionState::Igniting:
		if (OldState == ESingularisMorphVehicleIgnitionState::Off)
			OnIgnitionStarted.Broadcast();
		break;
	case ESingularisMorphVehicleIgnitionState::Running:
		if (OldState == ESingularisMorphVehicleIgnitionState::Igniting)
			OnIgnitionSucceeded.Broadcast();
		break;
	default:
		break;
	}
}

void USingularisMorphVehicleIgniteComponent::ApplyThrottleGate() const
{
	if (!CachedSimulationComponent.IsValid()) return;

	CachedSimulationComponent->SetControlEnabled(
		Chaos::ThrottleControlName,
		IgnitionState == ESingularisMorphVehicleIgnitionState::Running
	);
}
