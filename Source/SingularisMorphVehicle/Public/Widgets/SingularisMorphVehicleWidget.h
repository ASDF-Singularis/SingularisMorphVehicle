/* ====================================================================== *
 * SingularisMorphVehicleWidget.h                                         *
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
#include <Blueprint/UserWidget.h>

#include "Interfaces/SingularisMorphVehicleViewInterface.h"
#include "SingularisMorphVehicleWidget.generated.h"

class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具控件。
 *
 * 默认载具视图：实现 ISingularisMorphVehicleViewInterface，缓存框架推送的遥测快照，
 * 并提供车速与挡位的显示辅助函数。
 * 具体控件表现由蓝图或 C++ 子类覆写 SPI 完成；建议以本类为父类以获得遥测缓存与显示辅助。
 * 未覆写 SPI 事件时，本类会缓存最新遥测供属性绑定读取；蓝图覆写时可直接使用事件参数。
 */
UCLASS(Blueprintable)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleWidget : public UUserWidget,
                                                                 public ISingularisMorphVehicleViewInterface
{
	GENERATED_BODY()

public:
#pragma region State

	/** 最近一次遥测快照 */
	UPROPERTY(
		BlueprintReadOnly,
		Transient,
		Category = "引力奇点变型载具控件|State",
		meta = (DisplayName = "遥测")
	)
	FSingularisMorphVehicleTelemetry Telemetry{};

#pragma endregion

#pragma region ISingularisMorphVehicleViewInterface

	/** 载具状态全量刷新。 */
	virtual void OnRefresh_Implementation(const FSingularisMorphVehicleTelemetry& InTelemetry) override;

	/** 当前观察的载具变更。 */
	virtual void OnVehicleChanged_Implementation(
		USingularisMorphVehicleSimulationComponent* OldVehicleComponent,
		USingularisMorphVehicleSimulationComponent* NewVehicleComponent
	) override;

	/** 载具换挡。 */
	virtual void OnGearShifted_Implementation(int32 OldGear, int32 NewGear) override;

	/** 车轮打滑状态变更。 */
	virtual void OnWheelSlipChanged_Implementation(int32 WheelModuleGuid, bool bSlipping) override;

	/** 悬挂触底。 */
	virtual void OnSuspensionBottomedOut_Implementation(int32 SuspensionModuleGuid) override;

#pragma endregion

#pragma region API

	/**
	 * 获取最近一次遥测快照。
	 *
	 * @return 当前遥测数据副本。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具控件|API",
		meta = (DisplayName = "获取遥测")
	)
	FSingularisMorphVehicleTelemetry GetTelemetry() const { return Telemetry; }

	/**
	 * 获取挡位显示文本。
	 *
	 * @return 正数挡位为数字，负数挡位为 R，空挡为 N。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具控件|API",
		meta = (DisplayName = "获取挡位显示文本")
	)
	FText GetGearDisplayText() const;

	/**
	 * 获取车速显示文本（km/h）。
	 *
	 * @return 四舍五入后的整数车速，不含单位后缀。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具控件|API",
		meta = (DisplayName = "获取车速文本(km/h)")
	)
	FText GetSpeedKmhText() const;

	/**
	 * 获取车速显示文本（mph）。
	 *
	 * @return 四舍五入后的整数车速，不含单位后缀。
	 */
	UFUNCTION(
		BlueprintPure,
		Category = "引力奇点变型载具控件|API",
		meta = (DisplayName = "获取车速文本(mph)")
	)
	FText GetSpeedMphText() const;

#pragma endregion
};
