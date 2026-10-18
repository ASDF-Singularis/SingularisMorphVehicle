/* ====================================================================== *
 * SingularisMorphVehicleWidgetComponent.cpp                              *
 * ====================================================================== *
 * SPDX-License-Identifier: MIT                                           *
 * SPDX-FileCopyrightText: 2026 TrifingZW <TrifingZW@gmail.com>           *
 *                                                                        *
 * Copyright (c) 2026 TrifingZW. All Rights Reserved.                     *
 * Created: 2026/10/19 | Author: TrifingZW                                *
 * Licensed under MIT License                                             *
 *                                                                        *
 * Permission is hereby granted, free of charge, to any person obtaining  *
 * a copy of this software and associated documentation files (the        *
 * "Software"), to deal in the Software without restriction, including    *
 * without limitation the rights to use, copy, modify, merge, publish,    *
 * distribute, sublicense, and/or sell copies of the Software, and to     *
 * permit persons to whom the Software is furnished to do so, subject to  *
 * the following conditions:                                              *
 *                                                                        *
 * The above copyright notice and this permission notice shall be         *
 * included in all copies or substantial portions of the Software.        *
 *                                                                        *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        *
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     *
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. *
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   *
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   *
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      *
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 *
 * ====================================================================== */

#include "Components/SingularisMorphVehicleWidgetComponent.h"

#include <TimerManager.h>
#include <Blueprint/UserWidget.h>
#include <Engine/World.h>
#include <GameFramework/HUD.h>
#include <GameFramework/Pawn.h>
#include <GameFramework/PlayerController.h>

#include "SingularisMorphVehicle.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Interfaces/SingularisMorphVehicleControlInterface.h"

namespace
{
	/** km/h → mph 换算系数 */
	constexpr auto KmhToMph = 0.621371192f;

	/** 判断两份遥测在显示精度内是否等价 */
	bool IsTelemetryEqual(
		const FSingularisMorphVehicleTelemetry& Current,
		const FSingularisMorphVehicleTelemetry& Previous
	)
	{
		return Current.bValid == Previous.bValid
			&& FMath::IsNearlyEqual(Current.SpeedKmh, Previous.SpeedKmh, 0.01f)
			&& FMath::IsNearlyEqual(Current.SpeedMph, Previous.SpeedMph, 0.01f)
			&& Current.Gear == Previous.Gear
			&& Current.bReversing == Previous.bReversing
			&& FMath::IsNearlyEqual(Current.EngineRPM, Previous.EngineRPM, 0.5f)
			&& FMath::IsNearlyEqual(Current.EngineMaxRPM, Previous.EngineMaxRPM, 0.5f)
			&& FMath::IsNearlyEqual(Current.EngineRPMRatio, Previous.EngineRPMRatio, 0.005f)
			&& FMath::IsNearlyEqual(Current.EngineTorque, Previous.EngineTorque, 0.5f)
			&& Current.bLocallyControlled == Previous.bLocallyControlled
			&& Current.SlippingWheelCount == Previous.SlippingWheelCount
			&& Current.bWheelSlip == Previous.bWheelSlip;
	}
}

USingularisMorphVehicleWidgetComponent::USingularisMorphVehicleWidgetComponent()
{
	SetIsReplicatedByDefault(false);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

void USingularisMorphVehicleWidgetComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!ensureMsgf(
		IsValid(GetOwner()) && GetOwner()->IsA<AHUD>(),
		TEXT("[%s] BeginPlay：Owner 非 HUD"),
		*GetNameSafe(GetOwner())
	))
		return;

	OwnerHUD = Cast<AHUD>(GetOwner());

	// 1) 自动创建视图
	if (bAutoCreateView)
		CreateVehicleView();

	// 2) 观察本地玩家载具并推送一次全量遥测
	ObserveVehicle();
}

void USingularisMorphVehicleWidgetComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 1) 自动创建路径：屏幕空间控件不随组件销毁自动移除，需显式从视口移除避免残留
	if (bAutoCreateView)
	{
		if (UUserWidget* const CreatedUserWidget = Cast<UUserWidget>(VehicleView.GetObject()))
		{
			CreatedUserWidget->RemoveFromParent();

			UE_LOG(
				LogSingularisMorphVehicle,
				Display,
				TEXT("[%s] EndPlay：载具视图 %s 已从视口移除"),
				*GetNameSafe(GetOwner()),
				*GetNameSafe(CreatedUserWidget)
			);
		}
	}

	// 2) 停止遥测刷新并解绑观察事件
	StopTelemetryTimer();
	UnbindVehicleComponent(ObservedVehicleComponent.Get());
	UnbindPlayerController();

	// 3) 清空引用，事件绑定随组件销毁自动失效
	SlippingWheelGuids.Reset();
	VehicleView = nullptr;
	OwnerHUD = nullptr;

	Super::EndPlay(EndPlayReason);
}

void USingularisMorphVehicleWidgetComponent::SetVehicleView(
	const TScriptInterface<ISingularisMorphVehicleViewInterface>& NewVehicleView
)
{
	// 1) 自动创建视图时视图由组件自身托管，拒绝外部注入
	if (bAutoCreateView) return;

	// 2) 幂等检查，视图未变更时无需重复推送
	if (VehicleView == NewVehicleView) return;

	// 3) 运行时复核接口实现，防止 C++ 侧传入未实现接口的对象
	UObject* const ViewObject = NewVehicleView.GetObject();
	if (IsValid(ViewObject) && !ViewObject->Implements<USingularisMorphVehicleViewInterface>())
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] SetVehicleView：对象 %s 未实现 SingularisMorphVehicleViewInterface"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(ViewObject)
		);
		return;
	}

	VehicleView = NewVehicleView;

	// 4) 外部注入后同步当前载具并主动拉取一次全量遥测，消除错过事件导致的空白期
	NotifyVehicleChanged(nullptr, ObservedVehicleComponent.Get());
	PushTelemetry(true);
}

void USingularisMorphVehicleWidgetComponent::RefreshTelemetry()
{
	PushTelemetry(true);
}

USingularisMorphVehicleSimulationComponent* USingularisMorphVehicleWidgetComponent::GetObservedVehicleComponent() const
{
	return ObservedVehicleComponent.Get();
}

void USingularisMorphVehicleWidgetComponent::HandlePossessedPawnChanged(
	APawn* /*OldPawn*/,
	APawn* /*NewPawn*/



)
{
	APlayerController* PlayerController = BoundPlayerController.Get();
	if (!IsValid(PlayerController))
		PlayerController = ResolveLocalPlayerController();
	if (!IsValid(PlayerController)) return;

	// Pawn 占据变化仅作为重新解析触发点：控制层提供的非 Pawn 载具优先于 Pawn 回退
	BindVehicleComponent(ResolveVehicleComponent(PlayerController));
	PushTelemetry(true);
}

void USingularisMorphVehicleWidgetComponent::HandleGearShifted(const int32 OldGear, const int32 NewGear)
{
	// 换挡事件是挡位显示的最快来源，先刷新遥测再转发事件
	PushTelemetry();

	if (!IsValid(VehicleView.GetObject())) return;

	ISingularisMorphVehicleViewInterface::Execute_OnGearShifted(
		VehicleView.GetObject(),
		OldGear,
		NewGear
	);
}

void USingularisMorphVehicleWidgetComponent::HandleWheelSlipStarted(const int32 WheelModuleGuid)
{
	// 重复的开始事件不重复通知，保持事件流幂等
	if (SlippingWheelGuids.Contains(WheelModuleGuid)) return;

	SlippingWheelGuids.Add(WheelModuleGuid);
	PushTelemetry();

	if (!IsValid(VehicleView.GetObject())) return;

	ISingularisMorphVehicleViewInterface::Execute_OnWheelSlipChanged(
		VehicleView.GetObject(),
		WheelModuleGuid,
		true
	);
}

void USingularisMorphVehicleWidgetComponent::HandleWheelSlipStopped(const int32 WheelModuleGuid)
{
	// 未记录的车轮不重复通知，兼容模块移除时的补发停止事件
	if (!SlippingWheelGuids.Remove(WheelModuleGuid)) return;

	PushTelemetry();

	if (!IsValid(VehicleView.GetObject())) return;

	ISingularisMorphVehicleViewInterface::Execute_OnWheelSlipChanged(
		VehicleView.GetObject(),
		WheelModuleGuid,
		false
	);
}

void USingularisMorphVehicleWidgetComponent::HandleSuspensionBottomedOut(const int32 SuspensionModuleGuid)
{
	if (!IsValid(VehicleView.GetObject())) return;

	ISingularisMorphVehicleViewInterface::Execute_OnSuspensionBottomedOut(
		VehicleView.GetObject(),
		SuspensionModuleGuid
	);
}

void USingularisMorphVehicleWidgetComponent::CreateVehicleView()
{
	// 1) 本地玩家检查
	APlayerController* const PlayerController = ResolveLocalPlayerController();
	if (!IsValid(PlayerController))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Verbose,
			TEXT("[%s] CreateVehicleView：当前不是本地玩家，跳过自动创建"),
			*GetNameSafe(GetOwner())
		);
		return;
	}

	// 2) 控件类校验
	if (!IsValid(VehicleWidgetClass.Get()))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] CreateVehicleView：未配置载具控件类，跳过自动创建"),
			*GetNameSafe(GetOwner())
		);
		return;
	}

	// 3) 创建载具控件
	UUserWidget* const CreatedWidget = CreateWidget<UUserWidget>(PlayerController, VehicleWidgetClass);
	if (!IsValid(CreatedWidget))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Warning,
			TEXT("[%s] CreateVehicleView：控件类 %s 创建控件失败"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(VehicleWidgetClass.Get())
		);
		return;
	}

	// MustImplement 仅约束编辑器选择器，C++ 与蓝图图赋值可绕过，创建后运行时复核接口实现
	if (!ensureMsgf(
		CreatedWidget->Implements<USingularisMorphVehicleViewInterface>(),
		TEXT("[%s] CreateVehicleView：控件类 %s 未实现 SingularisMorphVehicleViewInterface"),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(VehicleWidgetClass.Get())
	))
		return;

	// 4) 缓存视图并添加到视口
	VehicleView = CreatedWidget;
	CreatedWidget->AddToViewport(ViewZOrder);

	UE_LOG(
		LogSingularisMorphVehicle,
		Display,
		TEXT("[%s] CreateVehicleView：载具视图 %s 创建成功"),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(CreatedWidget)
	);
}

void USingularisMorphVehicleWidgetComponent::ObserveVehicle()
{
	// 1) 本地玩家检查
	APlayerController* const PlayerController = ResolveLocalPlayerController();
	if (!IsValid(PlayerController))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Verbose,
			TEXT("[%s] ObserveVehicle：当前不是本地玩家，跳过载具观察"),
			*GetNameSafe(GetOwner())
		);
		return;
	}

	// 2) 绑定玩家控制器以跟踪载具变更
	BindPlayerController(PlayerController);

	// 3) 解析并绑定当前载具仿真组件
	USingularisMorphVehicleSimulationComponent* const VehicleComponent =
		ResolveVehicleComponent(PlayerController);
	BindVehicleComponent(VehicleComponent);

	if (!IsValid(VehicleComponent))
	{
		UE_LOG(
			LogSingularisMorphVehicle,
			Verbose,
			TEXT("[%s] ObserveVehicle：未找到控制层提供的受控载具或载具仿真组件，将在载具就绪后自动绑定"),
			*GetNameSafe(GetOwner())
		);
	}

	// 4) 启动周期遥测并推送一次全量状态
	StartTelemetryTimer();
	PushTelemetry(true);
}

APlayerController* USingularisMorphVehicleWidgetComponent::ResolveLocalPlayerController()
{
	AHUD* HUD = OwnerHUD.Get();
	if (!IsValid(HUD))
	{
		HUD = Cast<AHUD>(GetOwner());
		OwnerHUD = HUD;
	}

	if (!IsValid(HUD)) return nullptr;

	APlayerController* const PlayerController = HUD->GetOwningPlayerController();
	if (!IsValid(PlayerController) || !PlayerController->IsLocalController()) return nullptr;

	return PlayerController;
}

AActor* USingularisMorphVehicleWidgetComponent::ResolveControlledVehicle(
	APlayerController* PlayerController
) const
{
	if (!IsValid(PlayerController)) return nullptr;

	// 控制层组件实现 ISingularisMorphVehicleControlInterface，受控载具无需是 Pawn
	for (UActorComponent* Component : PlayerController->GetComponents())
	{
		if (!IsValid(Component) || !Component->Implements<USingularisMorphVehicleControlInterface>())
			continue;

		if (AActor* const ControlledVehicle =
				ISingularisMorphVehicleControlInterface::Execute_GetControlledVehicle(Component);
			IsValid(ControlledVehicle))
			return ControlledVehicle;
	}

	return nullptr;
}

USingularisMorphVehicleSimulationComponent* USingularisMorphVehicleWidgetComponent::ResolveVehicleComponent(
	APlayerController* PlayerController
) const
{
	if (!IsValid(PlayerController)) return nullptr;

	// 1) 优先使用控制层提供的受控载具，兼容非 Pawn 载具
	if (AActor* const ControlledVehicle = ResolveControlledVehicle(PlayerController);
		IsValid(ControlledVehicle))
		return ControlledVehicle->FindComponentByClass<USingularisMorphVehicleSimulationComponent>();

	// 2) Pawn 回退：兼容直接以 Pawn 作为载具的控制方式
	if (APawn* const Pawn = PlayerController->GetPawn(); IsValid(Pawn))
		return Pawn->FindComponentByClass<USingularisMorphVehicleSimulationComponent>();

	return nullptr;
}

void USingularisMorphVehicleWidgetComponent::BindPlayerController(APlayerController* PlayerController)
{
	if (!IsValid(PlayerController))
	{
		UnbindPlayerController();
		return;
	}

	if (bPossessionDelegateBound && BoundPlayerController.Get() == PlayerController) return;

	UnbindPlayerController();

	BoundPlayerController = PlayerController;
	PlayerController->OnPossessedPawnChanged.AddDynamic(
		this,
		&USingularisMorphVehicleWidgetComponent::HandlePossessedPawnChanged
	);
	bPossessionDelegateBound = true;

	UE_LOG(
		LogSingularisMorphVehicle,
		Display,
		TEXT("[%s] BindPlayerController：已绑定玩家控制器 %s"),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(PlayerController)
	);
}

void USingularisMorphVehicleWidgetComponent::UnbindPlayerController()
{
	if (bPossessionDelegateBound)
	{
		if (APlayerController* const PlayerController = BoundPlayerController.Get(); IsValid(PlayerController))
		{
			PlayerController->OnPossessedPawnChanged.RemoveDynamic(
				this,
				&USingularisMorphVehicleWidgetComponent::HandlePossessedPawnChanged
			);
		}
	}

	bPossessionDelegateBound = false;
	BoundPlayerController = nullptr;
}

void USingularisMorphVehicleWidgetComponent::BindVehicleComponent(
	USingularisMorphVehicleSimulationComponent* NewVehicleComponent
)
{
	USingularisMorphVehicleSimulationComponent* const OldVehicleComponent =
		ObservedVehicleComponent.Get();
	if (OldVehicleComponent == NewVehicleComponent) return;

	UnbindVehicleComponent(OldVehicleComponent);

	ObservedVehicleComponent = IsValid(NewVehicleComponent) ? NewVehicleComponent : nullptr;
	SlippingWheelGuids.Reset();

	if (USingularisMorphVehicleSimulationComponent* const VehicleComponent = ObservedVehicleComponent.Get())
	{
		VehicleComponent->OnGearShiftedEvent.AddDynamic(
			this,
			&USingularisMorphVehicleWidgetComponent::HandleGearShifted
		);
		VehicleComponent->OnWheelSlipStartedEvent.AddDynamic(
			this,
			&USingularisMorphVehicleWidgetComponent::HandleWheelSlipStarted
		);
		VehicleComponent->OnWheelSlipStoppedEvent.AddDynamic(
			this,
			&USingularisMorphVehicleWidgetComponent::HandleWheelSlipStopped
		);
		VehicleComponent->OnSuspensionBottomedOutEvent.AddDynamic(
			this,
			&USingularisMorphVehicleWidgetComponent::HandleSuspensionBottomedOut
		);

		UE_LOG(
			LogSingularisMorphVehicle,
			Display,
			TEXT("[%s] BindVehicleComponent：已绑定载具仿真组件 %s"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(VehicleComponent)
		);
	}

	NotifyVehicleChanged(OldVehicleComponent, ObservedVehicleComponent.Get());
}

void USingularisMorphVehicleWidgetComponent::UnbindVehicleComponent(
	USingularisMorphVehicleSimulationComponent* OldVehicleComponent
)
{
	if (IsValid(OldVehicleComponent))
	{
		OldVehicleComponent->OnGearShiftedEvent.RemoveDynamic(
			this,
			&USingularisMorphVehicleWidgetComponent::HandleGearShifted
		);
		OldVehicleComponent->OnWheelSlipStartedEvent.RemoveDynamic(
			this,
			&USingularisMorphVehicleWidgetComponent::HandleWheelSlipStarted
		);
		OldVehicleComponent->OnWheelSlipStoppedEvent.RemoveDynamic(
			this,
			&USingularisMorphVehicleWidgetComponent::HandleWheelSlipStopped
		);
		OldVehicleComponent->OnSuspensionBottomedOutEvent.RemoveDynamic(
			this,
			&USingularisMorphVehicleWidgetComponent::HandleSuspensionBottomedOut
		);

		UE_LOG(
			LogSingularisMorphVehicle,
			Display,
			TEXT("[%s] UnbindVehicleComponent：已解除载具仿真组件 %s 的事件绑定"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(OldVehicleComponent)
		);
	}

	if (ObservedVehicleComponent.Get() == OldVehicleComponent)
		ObservedVehicleComponent = nullptr;
}

void USingularisMorphVehicleWidgetComponent::NotifyVehicleChanged(
	USingularisMorphVehicleSimulationComponent* OldVehicleComponent,
	USingularisMorphVehicleSimulationComponent* NewVehicleComponent
) const
{
	if (OldVehicleComponent == NewVehicleComponent) return;
	if (!IsValid(VehicleView.GetObject())) return;

	ISingularisMorphVehicleViewInterface::Execute_OnVehicleChanged(
		VehicleView.GetObject(),
		OldVehicleComponent,
		NewVehicleComponent
	);
}

FSingularisMorphVehicleTelemetry USingularisMorphVehicleWidgetComponent::BuildTelemetry() const
{
	FSingularisMorphVehicleTelemetry Telemetry{};

	const USingularisMorphVehicleSimulationComponent* const VehicleComponent =
		ObservedVehicleComponent.Get();
	if (!IsValid(VehicleComponent)) return Telemetry;

	Telemetry.bValid = true;
	Telemetry.SpeedKmh = VehicleComponent->GetVehicleSpeed();
	Telemetry.SpeedMph = Telemetry.SpeedKmh * KmhToMph;
	Telemetry.Gear = VehicleComponent->GetCurrentGear();
	Telemetry.bReversing = VehicleComponent->IsReversing();
	Telemetry.EngineRPM = VehicleComponent->GetEngineRPM();
	Telemetry.EngineMaxRPM = VehicleComponent->GetEngineMaxRPM();

	if (Telemetry.EngineMaxRPM > KINDA_SMALL_NUMBER)
	{
		Telemetry.EngineRPMRatio = FMath::Clamp(
			Telemetry.EngineRPM / Telemetry.EngineMaxRPM,
			0.0f,
			1.0f
		);
	}

	Telemetry.EngineTorque = VehicleComponent->GetEngineTorque();
	Telemetry.bLocallyControlled = VehicleComponent->IsLocallyControlled();
	Telemetry.SlippingWheelCount = SlippingWheelGuids.Num();
	Telemetry.bWheelSlip = Telemetry.SlippingWheelCount > 0;

	return Telemetry;
}

void USingularisMorphVehicleWidgetComponent::PushTelemetry(const bool bForce)
{
	if (!IsValid(VehicleView.GetObject())) return;

	const FSingularisMorphVehicleTelemetry Telemetry = BuildTelemetry();
	if (!bForce && bHasPushedTelemetry && IsTelemetryEqual(Telemetry, LastPushedTelemetry)) return;

	LastPushedTelemetry = Telemetry;
	bHasPushedTelemetry = true;

	ISingularisMorphVehicleViewInterface::Execute_OnRefresh(VehicleView.GetObject(), Telemetry);
}

void USingularisMorphVehicleWidgetComponent::StartTelemetryTimer()
{
	UWorld* const World = GetWorld();
	if (!IsValid(World)) return;

	World->GetTimerManager().ClearTimer(TelemetryTimerHandle);

	const float Interval = FMath::Max(TelemetryUpdateInterval, 0.01f);
	World->GetTimerManager().SetTimer(
		TelemetryTimerHandle,
		this,
		&USingularisMorphVehicleWidgetComponent::UpdateTelemetry,
		Interval,
		true
	);
}

void USingularisMorphVehicleWidgetComponent::StopTelemetryTimer()
{
	if (UWorld* const World = GetWorld(); IsValid(World))
		World->GetTimerManager().ClearTimer(TelemetryTimerHandle);

	TelemetryTimerHandle.Invalidate();
}

void USingularisMorphVehicleWidgetComponent::UpdateTelemetry()
{
	APlayerController* const PlayerController = ResolveLocalPlayerController();
	if (!IsValid(PlayerController))
	{
		if (ObservedVehicleComponent.IsValid())
		{
			BindVehicleComponent(nullptr);
			PushTelemetry(true);
		}

		return;
	}

	BindPlayerController(PlayerController);

	USingularisMorphVehicleSimulationComponent* const ResolvedVehicleComponent =
		ResolveVehicleComponent(PlayerController);

	const bool bVehicleChanged = ResolvedVehicleComponent != ObservedVehicleComponent.Get();
	if (bVehicleChanged)
		BindVehicleComponent(ResolvedVehicleComponent);

	PushTelemetry(bVehicleChanged);
}
