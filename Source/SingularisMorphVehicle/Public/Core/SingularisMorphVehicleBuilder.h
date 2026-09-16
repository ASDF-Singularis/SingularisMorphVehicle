#pragma once

#include <CoreMinimal.h>

#include "SimModule/SimModuleTree.h"

class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具构建器。
 *
 * 提供静态工具方法修复模拟树内的链接关系。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleBuilder
{
public:
	/** 修复模拟树内的父子链接关系 */
	static void FixupTreeLinks(TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree);
};
