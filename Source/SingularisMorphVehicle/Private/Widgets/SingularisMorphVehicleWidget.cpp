/* ====================================================================== *
 * SingularisMorphVehicleWidget.cpp                                       *
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

#include "Widgets/SingularisMorphVehicleWidget.h"

#define LOCTEXT_NAMESPACE "USingularisMorphVehicleWidget"

void USingularisMorphVehicleWidget::OnRefresh_Implementation(
	const FSingularisMorphVehicleTelemetry& InTelemetry
)
{
	Telemetry = InTelemetry;
}

void USingularisMorphVehicleWidget::OnVehicleChanged_Implementation(
	USingularisMorphVehicleSimulationComponent* OldVehicleComponent,
	USingularisMorphVehicleSimulationComponent* NewVehicleComponent
)
{
	if (OldVehicleComponent == NewVehicleComponent) return;

	// 载具切换后旧遥测立即失效，等待框架随后推送的新载具全量状态
	Telemetry = FSingularisMorphVehicleTelemetry{};
}

void USingularisMorphVehicleWidget::OnGearShifted_Implementation(int32 OldGear, int32 NewGear)
{
	// 换挡事件先于下一次周期刷新到达时，保持缓存挡位与事件一致
	Telemetry.Gear = NewGear;
	Telemetry.bReversing = NewGear < 0;
}

void USingularisMorphVehicleWidget::OnWheelSlipChanged_Implementation(
	int32 WheelModuleGuid,
	bool bSlipping
)
{
	// 打滑数量由框架推送的全量遥测携带，此处仅保留 SPI 覆写入口
}

void USingularisMorphVehicleWidget::OnSuspensionBottomedOut_Implementation(int32 SuspensionModuleGuid)
{
	// 触底为一次性事件，此处仅保留 SPI 覆写入口
}

FText USingularisMorphVehicleWidget::GetGearDisplayText() const
{
	if (Telemetry.Gear > 0)
		return FText::AsNumber(Telemetry.Gear);

	if (Telemetry.Gear < 0)
		return LOCTEXT("ReverseGear", "R");

	return LOCTEXT("NeutralGear", "N");
}

FText USingularisMorphVehicleWidget::GetSpeedKmhText() const
{
	return FText::AsNumber(FMath::RoundToInt(Telemetry.SpeedKmh));
}

FText USingularisMorphVehicleWidget::GetSpeedMphText() const
{
	return FText::AsNumber(FMath::RoundToInt(Telemetry.SpeedMph));
}

#undef LOCTEXT_NAMESPACE
