#pragma once

#include <CoreMinimal.h>

#include "SimModule/SimModuleTree.h"

class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具构建器。
 *
 * 提供静态工具方法用于从 USingularisMorphVehicleSimulationComponent 的组件层级中
 * 生成模块模拟树，并修复树内链接关系。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleBuilder
{
public:
	/** 从变型载具组件生成模拟树 */
	static void GenerateSimTree(USingularisMorphVehicleSimulationComponent* ModularVehicle);

	/** 修复模拟树内的父子链接关系 */
	static void FixupTreeLinks(TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree);
};
