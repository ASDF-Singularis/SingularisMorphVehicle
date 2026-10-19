#pragma once

#include <CoreMinimal.h>

#include "VehicleUtility.h"
#include "SimModule/AxleModule.h"
#include "SimModule/ClutchModule.h"
#include "SimModule/EngineModule.h"
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
 * 不改变 GetSimType()：轮轴/电机的类型名与上游一致，故 FTorqueSimModule::TransmitTorque
 * 中按 GetSimType() 相等跳过同类子节点的判定与模块工厂注册均不受影响。需要在插件内按
 * 派生类识别离合器/变速箱时，构造时仅追加标记类型并重新写入基类类型名，
 * GetSimType() 仍返回基类名（见对应类的说明）。
 *
 * 另含离合器、引擎转速限制器与空挡输入轴跟踪模块（见下方各类）。
 */

/**
 * 引力奇点变型轮轴模拟模块。
 *
 * 轮轴是动力链中的纯扭矩中继（开式差速器节点）：TransmitTorque 将驱动力矩按子节点数
 * 均分下推，并把自身转速同步为子节点平均转速。上游在同步之后仍按 AxleInertia 自积分，
 * 但轮轴的驱动力矩已是变速箱输出（乘过齿比）的整车扭矩量级，而自惯量仅为传动轴惯量，
 * 二者比值会使轮轴转速在单步内暴涨，并经齿比放大后直接顶到引擎最大转速。
 * 此处只保留转速同步，不进行自惯量积分。
 *
 * 轮轴不保留输入扭矩：每步转发后清零 DriveTorque，避免变速箱空挡（齿比为 0）
 * 跳过传递时，上一挡的驱动力矩被持续下推给车轮。
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
		// 轮轴是纯扭矩中继：本步输入必须在转发后消费。变速箱在空挡（齿比为 0）时
		// 跳过 TransmitTorque，不会刷新轮轴的 DriveTorque；若不消费，上一挡的驱动力矩
		// 会被持续下推给车轮，造成空挡持续加速、倒挡反向持续加速。
		TransmitTorque(VehicleModuleSystem, DriveTorque, 0.0f);
		DriveTorque = 0.0f;
	}
};

/**
 * 引力奇点离合器模拟模块。
 *
 * 上游离合器无论踏板位置都会把引擎与变速箱转速按固定 0.1 系数互拉，导致离合分离或
 * 空挡时传动系仍被引擎拖动，引擎转速也会被无负载的变速箱输入轴反向拖慢。
 * 本模块以 (1 - 踏板) × ClutchStrength 作为接合量：
 * 分离或下游变速箱处于空挡时，输入轴随引擎空转并向下游写入零扭矩，清除上一物理步残留；
 * 接合时按接合量缩放扭矩与转速耦合；空挡且接合时让变速箱输入轴与引擎同步，
 * 避免重新挂挡时转速突变。
 *
 * 不改变 GetSimType()：构造时追加派生标记类型后重新写入基类类型名，
 * 网络工厂哈希、复制结构与同类型子节点跳过判定仍按 FClutchSimModule 工作。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphClutchSimModule : public Chaos::FClutchSimModule
{
public:
	DEFINE_CHAOSSIMTYPENAME(FSingularisMorphClutchSimModule);

	explicit FSingularisMorphClutchSimModule(const Chaos::FClutchSettings& Settings)
		: FClutchSimModule(Settings)
	{
		// 追加派生标记类型，供本插件按派生类识别；随后重新写入基类类型名，
		// 使 GetSimType() 保持 FClutchSimModule（网络工厂按该名称生成哈希）
		AddType(_CHAOSTYPENAMERETRIVAL_());
		AddType(FClutchSimModule::_CHAOSTYPENAMERETRIVAL_());
	}

	virtual void Simulate(
		float DeltaTime,
		const Chaos::FAllInputs& Inputs,
		Chaos::FSimModuleTree& VehicleModuleSystem
	) override;

	virtual bool GetDebugString(FString& StringOut) const override;

	/** 当前接合量 [0..1]，0 为完全分离 */
	float GetClutchValue() const { return Engagement; }

	/** 接合量是否足以让传动系约束引擎转速 */
	bool IsLocked() const { return Engagement > LockThreshold; }

private:
	/** 引擎转速可被传动系约束的最小接合量 */
	static constexpr auto LockThreshold = 0.5f;

	float Engagement = 1.0f;
};

/**
 * 引力奇点引擎模拟模块。
 *
 * 在官方引擎模块之上补齐以下行为：
 * 最大转速前按比例线性收油，避免官方模块在最大转速处断油与制动交替造成的转速抖动；
 * 超速制动并入净扭矩连续施加，不再以子节点扭矩硬覆盖，避免极速附近出现顿挫；
 * 松油门时把随转速缩放的负扭矩作为驱动力矩下推，使挂挡滑行经传动系产生引擎制动，
 * 空挡时则只降低引擎自身转速；
 * 挂挡且未踩制动/油门时输出怠速蠕行扭矩：输入轴转速低于怠速对应转速时按差值成比例
 * 推动传动系，达到怠速转速后扭矩归零，使载具在挡位中能够自行加速到怠速行驶速度；
 * 制动踏板或手刹有效时蠕行扭矩归零，避免主动驱动与制动对抗；
 * 倒挡传动系角速度为负，而引擎实际仍正转，转速耦合后取绝对值。
 *
 * 离合接合时引擎转速直接取传动系输入轴转速，允许低于怠速；载具在蠕行扭矩下
 * 加速时转速随车速逐步回升至怠速。只有空挡或离合分离时，自由转速才钳制在怠速以上。
 *
 * 空挡与离合分离时的自由转速由离合器/变速箱的输入轴跟踪保证：无负载时输入轴转速
 * 会同步为原动机转速，因此引擎的转速耦合不会把自身积分结果钳回旧值。
 *
 * 自由升速/降速受最大转速变化率限制，避免低转动惯量配置下转速在数帧内冲到红区；
 * 直连接合时转速仍由传动系决定。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphEngineSimModule : public Chaos::FEngineSimModule
{
public:
	explicit FSingularisMorphEngineSimModule(
		const Chaos::FEngineSettings& Settings,
		float InRevUpRateRPM = 5000.0f,
		float InRevDownRateRPM = 4000.0f,
		float InIdleCreepTorqueRatio = 0.0f
	)
		: FEngineSimModule(Settings),
		  RevUpRateOmega(
			  FMath::IsFinite(InRevUpRateRPM)
				  ? Chaos::RPMToOmega(FMath::Max(InRevUpRateRPM, 0.0f))
				  : 0.0f
		  ),
		  RevDownRateOmega(
			  FMath::IsFinite(InRevDownRateRPM)
				  ? Chaos::RPMToOmega(FMath::Max(InRevDownRateRPM, 0.0f))
				  : 0.0f
		  ),
		  IdleCreepTorqueRatio(
			  FMath::IsFinite(InIdleCreepTorqueRatio)
				  ? FMath::Clamp(InIdleCreepTorqueRatio, 0.0f, 1.0f)
				  : 0.0f
		  )
	{
		// 引擎创建即处于怠速：避免首个物理步从 0 被怠速钳制瞬间拉到怠速
		AngularVelocity = EngineIdleSpeed;
	}

	virtual void Simulate(
		float DeltaTime,
		const Chaos::FAllInputs& Inputs,
		Chaos::FSimModuleTree& VehicleModuleSystem
	) override;

private:
	/** 空挡自由升速的最大角速度变化率（rad/s²）；0 表示不限制 */
	float RevUpRateOmega = 0.0f;

	/** 空挡自由降速的最大角速度变化率（rad/s²）；0 表示不限制 */
	float RevDownRateOmega = 0.0f;

	/** 怠速蠕行扭矩占最大扭矩的比例；0 表示关闭 */
	float IdleCreepTorqueRatio = 0.0f;

	/** 动力链是否挂有可驱动车轮（离合器接合且变速箱不在空挡） */
	bool IsDrivelineConnected(Chaos::FSimModuleTree& VehicleModuleSystem) const;

	/**
	 * 传动系输入轴角速度（优先取变速箱输入轴，无变速箱时取直接子节点平均）。
	 * 齿比符号使前进/倒挡在各自挡位方向上为正，反向滑行为负。
	 */
	float GetDrivelineAngularVelocity(const Chaos::FSimModuleTree& VehicleModuleSystem) const;

	/** 沿扭矩链查找变速箱并读取其输入轴角速度 */
	bool FindDrivelineInputVelocity(
		const Chaos::FSimModuleTree& VehicleModuleSystem,
		int32 ParentIndex,
		int32 Depth,
		float& OutVelocity
	) const;

	/** 从 ParentIndex 沿扭矩子节点向下查找车轮 */
	bool HasLoadedDriveline(
		Chaos::FSimModuleTree& VehicleModuleSystem,
		int32 ParentIndex,
		int32 Depth
	) const;
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
