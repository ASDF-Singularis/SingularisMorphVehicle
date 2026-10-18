/* ====================================================================== *
 * SingularisMorphVehicleControlInterface.h                               *
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

#include "SingularisMorphVehicleControlInterface.generated.h"

class AActor;

/**
 * 引力奇点变型载具控制接口。
 *
 * 控制层（例如挂在本地玩家控制器上的控制组件）实现本接口，
 * 向 HUD、相机等观察者暴露当前受控载具。载具无需是 Pawn；
 * 观察者只依赖本接口，不依赖具体控制组件类型。
 */
UINTERFACE(Blueprintable, BlueprintType)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleControlInterface : public UInterface
{
	GENERATED_BODY()
};

/** 载具控制提供者实现接口：暴露当前受控载具。 */
class SINGULARISMORPHVEHICLE_API ISingularisMorphVehicleControlInterface
{
	GENERATED_BODY()

public:
	/**
	 * 获取当前受控载具。
	 *
	 * @return 当前受控载具 Actor；未控制时返回 nullptr。
	 */
	UFUNCTION(
		BlueprintNativeEvent,
		BlueprintCallable,
		Category = "引力奇点变型载具控制接口",
		meta = (DisplayName = "获取受控载具")
	)
	AActor* GetControlledVehicle() const;
};
