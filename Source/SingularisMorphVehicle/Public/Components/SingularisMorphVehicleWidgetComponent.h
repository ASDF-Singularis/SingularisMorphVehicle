/* ====================================================================== *
 * SingularisMorphVehicleWidgetComponent.h                                *
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

#pragma once

#include <CoreMinimal.h>
#include <Components/ActorComponent.h>
#include <Engine/EngineTypes.h>
#include <UObject/ScriptInterface.h>

#include "Interfaces/SingularisMorphVehicleViewInterface.h"
#include "SingularisMorphVehicleWidgetComponent.generated.h"

class AActor;
class AHUD;
class APawn;
class APlayerController;
class UUserWidget;
class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具控件组件。
 *
 * HUD 作用域观察者组件：挂载于玩家 HUD，实例化载具控件并添加至视口，
 * 观察本地玩家控制层提供的受控载具（载具无需是 Pawn），解析其载具仿真组件，
 * 周期采集车速、挡位与原动机遥测，并转发换挡、车轮打滑与悬挂触底事件至
 * ISingularisMorphVehicleViewInterface（SPI）。
 * 自动创建路径由本地玩家检查过滤归属；可关闭自动创建后由外部注入视图；
 * 仅依赖接口而非具体控件或控制组件类型，任意实现视图接口的 UObject 均可接入。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具控件组件")
)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleWidgetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 自动创建视图 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具控件组件",
		meta = (DisplayName = "自动创建视图")
	)
	bool bAutoCreateView = true;

	/** 载具控件类 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具控件组件",
		meta = (
			DisplayName = "载具控件类",
			MustImplement = "/Script/SingularisMorphVehicle.SingularisMorphVehicleViewInterface",
			EditCondition = "bAutoCreateView"
		)
	)
	TSubclassOf<UUserWidget> VehicleWidgetClass = nullptr;

	/** 视图层级 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具控件组件",
		meta = (DisplayName = "视图层级")
	)
	int32 ViewZOrder = 0;

	/** 遥测刷新间隔（秒），周期采集车速、挡位与原动机状态 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "引力奇点变型载具控件组件",
		meta = (DisplayName = "遥测刷新间隔(s)", ClampMin = "0.01", UIMin = "0.01")
	)
	float TelemetryUpdateInterval = 0.05f;

#pragma endregion

private:
#pragma region State

	/** 拥有本组件的 HUD */
	TWeakObjectPtr<AHUD> OwnerHUD = nullptr;

	/** 已绑定 Pawn 变更事件的本地玩家控制器 */
	TWeakObjectPtr<APlayerController> BoundPlayerController = nullptr;

	/** 当前观察的载具仿真组件 */
	TWeakObjectPtr<USingularisMorphVehicleSimulationComponent> ObservedVehicleComponent = nullptr;

	/** 运行时实例化的载具视图缓存 */
	UPROPERTY(Transient)
	TScriptInterface<ISingularisMorphVehicleViewInterface> VehicleView{};

	/** 当前处于打滑状态的车轮模块 GUID（从绑定时刻开始累积） */
	TSet<int32> SlippingWheelGuids;

	/** 最近一次成功推送的遥测，用于抑制无变化更新 */
	FSingularisMorphVehicleTelemetry LastPushedTelemetry{};

	/** 是否已成功推送过遥测 */
	bool bHasPushedTelemetry = false;

	/** 是否已绑定玩家控制器 Pawn 变更事件 */
	bool bPossessionDelegateBound = false;

	/** 遥测刷新定时器 */
	FTimerHandle TelemetryTimerHandle;

#pragma endregion

public:
#pragma region Constructors

	USingularisMorphVehicleWidgetComponent();

#pragma endregion

#pragma region ActorComponent Interface

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#pragma endregion

#pragma region API

	/**
	 * 设置载具视图。
	 *
	 * 仅在关闭自动创建视图时用于外部注入，设置后立即同步当前载具并推送一次全量遥测。
	 *
	 * @param NewVehicleView 载具视图对象。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具控件组件|API",
		meta = (DisplayName = "设置载具视图")
	)
	void SetVehicleView(const TScriptInterface<ISingularisMorphVehicleViewInterface>& NewVehicleView);

	/**
	 * 获取运行时实例化的载具视图。
	 *
	 * @return 载具视图对象；尚未设置时返回 nullptr。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具控件组件|API",
		meta = (DisplayName = "获取载具视图")
	)
	UObject* GetVehicleView() const { return VehicleView.GetObject(); }

	/**
	 * 立即采集并推送一次遥测。
	 *
	 * 跳过差异抑制，适用于外部修改载具状态后强制刷新视图。
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "引力奇点变型载具控件组件|API",
		meta = (DisplayName = "刷新载具遥测")
	)
	void RefreshTelemetry();

	/**
	 * 获取当前观察的载具仿真组件。
	 *
	 * @return 载具仿真组件；尚未绑定时返回 nullptr。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具控件组件|API",
		meta = (DisplayName = "获取观察载具组件")
	)
	USingularisMorphVehicleSimulationComponent* GetObservedVehicleComponent() const;

#pragma endregion

private:
#pragma region Callback

	/** 玩家控制器 Pawn 变更回调：重新解析并绑定载具 */
	UFUNCTION()
	void HandlePossessedPawnChanged(APawn* OldPawn, APawn* NewPawn);

	/** 换挡回调：刷新遥测并转发至视图 */
	UFUNCTION()
	void HandleGearShifted(int32 OldGear, int32 NewGear);

	/** 车轮开始打滑回调 */
	UFUNCTION()
	void HandleWheelSlipStarted(int32 WheelModuleGuid);

	/** 车轮停止打滑回调 */
	UFUNCTION()
	void HandleWheelSlipStopped(int32 WheelModuleGuid);

	/** 悬挂触底回调 */
	UFUNCTION()
	void HandleSuspensionBottomedOut(int32 SuspensionModuleGuid);

#pragma endregion

#pragma region Internal Function

	/** 在本地玩家侧实例化载具视图并添加到视口 */
	void CreateVehicleView();

	/** 绑定本地玩家控制器与载具仿真组件，并启动遥测刷新 */
	void ObserveVehicle();

	/** 解析本地玩家控制器 */
	APlayerController* ResolveLocalPlayerController();

	/** 从本地玩家控制器上的控制层组件解析受控载具 */
	AActor* ResolveControlledVehicle(APlayerController* PlayerController) const;

	/** 从受控载具解析载具仿真组件，控制层未提供受控载具时回退到本地玩家 Pawn */
	USingularisMorphVehicleSimulationComponent* ResolveVehicleComponent(
		APlayerController* PlayerController
	) const;

	/** 绑定玩家控制器 Pawn 变更事件 */
	void BindPlayerController(APlayerController* PlayerController);

	/** 解绑玩家控制器 Pawn 变更事件 */
	void UnbindPlayerController();

	/** 绑定载具仿真组件事件 */
	void BindVehicleComponent(USingularisMorphVehicleSimulationComponent* NewVehicleComponent);

	/** 解绑载具仿真组件事件 */
	void UnbindVehicleComponent(USingularisMorphVehicleSimulationComponent* OldVehicleComponent);

	/** 通知视图载具已变更 */
	void NotifyVehicleChanged(
		USingularisMorphVehicleSimulationComponent* OldVehicleComponent,
		USingularisMorphVehicleSimulationComponent* NewVehicleComponent
	) const;

	/** 构建当前遥测快照 */
	FSingularisMorphVehicleTelemetry BuildTelemetry() const;

	/** 推送遥测至视图；bForce 为 true 时跳过差异抑制 */
	void PushTelemetry(bool bForce = false);

	/** 启动遥测刷新定时器 */
	void StartTelemetryTimer();

	/** 停止遥测刷新定时器 */
	void StopTelemetryTimer();

	/** 定时器回调：解析载具并推送遥测 */
	void UpdateTelemetry();

#pragma endregion
};
