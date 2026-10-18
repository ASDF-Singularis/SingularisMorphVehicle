/* ====================================================================== *
 * SingularisMorphVehicleTelemetryType.h                                  *
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

#include "SingularisMorphVehicleTelemetryType.generated.h"

/**
 * 引力奇点变型载具遥测快照。
 *
 * 由载具控件组件在每次状态刷新时构建，承载仪表所需的车速、挡位与原动机状态。
 * bValid 为 false 表示当前没有可观察的载具，其余字段均为零值。
 */
USTRUCT(BlueprintType)
struct SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleTelemetry
{
	GENERATED_BODY()

	/** 是否存在有效载具 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "有效"))
	bool bValid = false;

	/** 车速（km/h） */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "车速(km/h)"))
	float SpeedKmh = 0.0f;

	/** 车速（mph） */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "车速(mph)"))
	float SpeedMph = 0.0f;

	/** 当前挡位：0 = 空挡，负数 = 倒挡，正数 = 前进挡 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "挡位"))
	int32 Gear = 0;

	/** 是否倒车 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "倒车"))
	bool bReversing = false;

	/** 引擎转速（RPM） */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "引擎转速(RPM)"))
	float EngineRPM = 0.0f;

	/** 引擎最大转速（RPM），无法解析时为 0 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "引擎最大转速(RPM)"))
	float EngineMaxRPM = 0.0f;

	/** 引擎转速归一化值，范围 [0, 1]，最大转速不可用时为 0 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "引擎转速比"))
	float EngineRPMRatio = 0.0f;

	/** 引擎扭矩 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "引擎扭矩"))
	float EngineTorque = 0.0f;

	/** 是否由本地玩家控制 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "本地控制"))
	bool bLocallyControlled = false;

	/** 当前打滑车轮数量 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "打滑车轮数"))
	int32 SlippingWheelCount = 0;

	/** 是否存在车轮打滑 */
	UPROPERTY(BlueprintReadOnly, Category = "引力奇点变型载具遥测", meta = (DisplayName = "车轮打滑"))
	bool bWheelSlip = false;
};
