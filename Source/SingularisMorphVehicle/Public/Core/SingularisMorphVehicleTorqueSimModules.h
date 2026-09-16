#pragma once

#include <CoreMinimal.h>

#include "SimModule/AxleModule.h"
#include "SimModule/MotorModule.h"

/**
 * 引力奇点变型载具扭矩模拟模块补全。
 *
 * ChaosVehicles 的 FAxleSimModule 与 FMotorSimModule 未覆写 ISimulationModuleBase 的纯虚函数
 * GenerateNetData，因此自身即为抽象类，上游从未实例化。二者的 Simulate() 实现完整（轮轴扭矩
 * 传递、电机转速平方扭矩曲线），仅缺该纯虚函数，此处按 FAerofoilSimModule 的既有做法补为返回
 * 空网络数据（FSimModuleTree::GenerateReplicationStructure 声明 "nullptr is a valid response"），
 * 即这两个模块不参与网络状态复制。
 *
 * 不引入新的模拟类型名：GetSimType() 与 IsSimType<FAxleSimModule>() 仍与上游一致，故 FSingularisMorphVehicleBuilder
 * 中的轮轴识别（FixupTreeLinks）、FTorqueSimModule::TransmitTorque 中按 GetSimType() 相等跳过同类子
 * 节点的判定与模块工厂注册均不受影响。
 */

/** 引力奇点变型轮轴模拟模块。 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphAxleSimModule : public Chaos::FAxleSimModule
{
public:
	explicit FSingularisMorphAxleSimModule(const Chaos::FAxleSettings& Settings)
		: FAxleSimModule(Settings) {}

	virtual TSharedPtr<Chaos::FModuleNetData> GenerateNetData(const int32 NodeArrayIndex) const override
	{
		return nullptr;
	}
};

/** 引力奇点变型电机模拟模块。 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphMotorSimModule : public Chaos::FMotorSimModule
{
public:
	explicit FSingularisMorphMotorSimModule(const Chaos::FMotorSettings& Settings)
		: FMotorSimModule(Settings) {}

	virtual TSharedPtr<Chaos::FModuleNetData> GenerateNetData(const int32 NodeArrayIndex) const override
	{
		return nullptr;
	}
};
