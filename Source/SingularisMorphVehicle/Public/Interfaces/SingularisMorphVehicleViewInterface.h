/* ====================================================================== *
 * SingularisMorphVehicleViewInterface.h                                  *
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
#include <UObject/Interface.h>

#include "Types/SingularisMorphVehicleTelemetryType.h"
#include "SingularisMorphVehicleViewInterface.generated.h"

class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具视图接口。
 *
 * 载具视图契约：实现者接收载具仿真组件的全量遥测、载具变更、换挡、车轮打滑与悬挂触底事件，
 * 由载具控件组件经 Execute_ 调用。实现者不限于控件——任意 UObject 实现本接口即可接入驱动。
 */
UINTERFACE(Blueprintable, BlueprintType)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleViewInterface : public UInterface
{
	GENERATED_BODY()
};

/** 载具视图实现接口：接收载具遥测的全量刷新与本地离散事件。 */
class SINGULARISMORPHVEHICLE_API ISingularisMorphVehicleViewInterface
{
	GENERATED_BODY()

public:
	/**
	 * 载具状态全量刷新。
	 *
	 * 由载具控件组件在绑定完成、载具变更或周期采集后调用；
	 * bValid 为 false 时表示当前没有可观察的载具。
	 *
	 * @param Telemetry 当前载具遥测快照。
	 */
	UFUNCTION(
		BlueprintNativeEvent,
		BlueprintCallable,
		Category = "引力奇点变型载具视图接口",
		meta = (DisplayName = "载具刷新")
	)
	void OnRefresh(const FSingularisMorphVehicleTelemetry& Telemetry);

	/**
	 * 当前观察的载具变更。
	 *
	 * 载具变更后框架会立即推送一次全量遥测。
	 *
	 * @param OldVehicleComponent 变更前的载具仿真组件。
	 * @param NewVehicleComponent 变更后的载具仿真组件；nullptr 表示当前没有载具。
	 */
	UFUNCTION(
		BlueprintNativeEvent,
		BlueprintCallable,
		Category = "引力奇点变型载具视图接口",
		meta = (DisplayName = "载具变更")
	)
	void OnVehicleChanged(
		USingularisMorphVehicleSimulationComponent* OldVehicleComponent,
		USingularisMorphVehicleSimulationComponent* NewVehicleComponent
	);

	/**
	 * 载具换挡。
	 *
	 * @param OldGear 换挡前挡位。
	 * @param NewGear 换挡后挡位：0 = 空挡，负数 = 倒挡，正数 = 前进挡。
	 */
	UFUNCTION(
		BlueprintNativeEvent,
		BlueprintCallable,
		Category = "引力奇点变型载具视图接口",
		meta = (DisplayName = "换挡")
	)
	void OnGearShifted(int32 OldGear, int32 NewGear);

	/**
	 * 车轮打滑状态变更。
	 *
	 * @param WheelModuleGuid 车轮模块 GUID。
	 * @param bSlipping true 表示开始打滑，false 表示停止打滑。
	 */
	UFUNCTION(
		BlueprintNativeEvent,
		BlueprintCallable,
		Category = "引力奇点变型载具视图接口",
		meta = (DisplayName = "车轮打滑变更")
	)
	void OnWheelSlipChanged(int32 WheelModuleGuid, bool bSlipping);

	/**
	 * 悬挂触底。
	 *
	 * @param SuspensionModuleGuid 触底的悬挂模块 GUID。
	 */
	UFUNCTION(
		BlueprintNativeEvent,
		BlueprintCallable,
		Category = "引力奇点变型载具视图接口",
		meta = (DisplayName = "悬挂触底")
	)
	void OnSuspensionBottomedOut(int32 SuspensionModuleGuid);
};
