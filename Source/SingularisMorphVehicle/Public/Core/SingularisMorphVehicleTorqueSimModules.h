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
 * 不引入新的模拟类型名：GetSimType() 与 IsSimType<FAxleSimModule>() 仍与上游一致，故
 * FTorqueSimModule::TransmitTorque 中按 GetSimType() 相等跳过同类子节点的判定与模块
 * 工厂注册均不受影响。
 */

/**
 * 引力奇点变型轮轴模拟模块。
 *
 * 轮轴是动力链中的纯扭矩中继（开式差速器节点）：TransmitTorque 将驱动力矩按子节点数
 * 均分下推，并把自身转速同步为子节点平均转速。上游在同步之后仍按 AxleInertia 自积分，
 * 但轮轴的驱动力矩已是变速箱输出（乘过齿比）的整车扭矩量级，而自惯量仅为传动轴惯量，
 * 二者比值会使轮轴转速在单步内暴涨，并经齿比放大后直接顶到引擎最大转速。
 * 此处只保留转速同步，不进行自惯量积分。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphAxleSimModule : public Chaos::FAxleSimModule
{
public:
	explicit FSingularisMorphAxleSimModule(const Chaos::FAxleSettings& Settings)
		: FAxleSimModule(Settings) {}

	virtual TSharedPtr<Chaos::FModuleNetData> GenerateNetData(const int32 NodeArrayIndex) const override
	{
		return nullptr;
	}

	virtual void Simulate(
		float /*DeltaTime*/,
		const Chaos::FAllInputs& /*Inputs*/,
		Chaos::FSimModuleTree& VehicleModuleSystem
	) override
	{
		TransmitTorque(VehicleModuleSystem, DriveTorque, 0.0f);
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
