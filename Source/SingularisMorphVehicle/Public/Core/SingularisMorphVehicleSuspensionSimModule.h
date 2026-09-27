#pragma once

#include <CoreMinimal.h>

#include "Chaos/ChaosEngineInterface.h"
#include "SimModule/SuspensionBaseInterface.h"

class FSingularisMorphVehicleSuspensionSimModule;

namespace Chaos
{
	struct FAllInputs;
	class FSimModuleTree;
	class FClusterUnionPhysicsProxy;
}

/**
 * 引力奇点变型悬挂网络复制数据。
 */
struct FSingularisMorphSuspensionSimModuleData
	: Chaos::FModuleNetData,
	  Chaos::TSimulationModuleTypeable<FSingularisMorphVehicleSuspensionSimModule,
	                                   FSingularisMorphSuspensionSimModuleData>
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	FSingularisMorphSuspensionSimModuleData(int NodeArrayIndex, const FString& InDebugString) : FModuleNetData(
		NodeArrayIndex,
		InDebugString
	) {}
#else
	FSingularisMorphSuspensionSimModuleData(int NodeArrayIndex) : FModuleNetData(NodeArrayIndex) {}
#endif

	virtual void FillSimState(Chaos::ISimulationModuleBase* SimModule) override;
	virtual void FillNetState(const Chaos::ISimulationModuleBase* SimModule) override;

	virtual void Serialize(FArchive& Ar) override
	{
		Ar << SpringDisplacement;
		Ar << SpringSpeed;
	}

	virtual void Lerp(const float LerpFactor, const FModuleNetData& Min, const FModuleNetData& Max) override;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	virtual FString ToString() const override;
#endif

	/** 悬挂压缩量（cm）：0 = 全伸张，MaxLength = 全压缩 */
	float SpringDisplacement = 0.0f;

	/** 压缩速度（cm/s，沿压缩方向为正） */
	float SpringSpeed = 0.0f;
};

/**
 * 引力奇点变型悬挂模拟输出数据。
 */
struct FSingularisMorphSuspensionOutputData
	: Chaos::FSimOutputData,
	  Chaos::TSimulationModuleTypeable<FSingularisMorphVehicleSuspensionSimModule, FSingularisMorphSuspensionOutputData>
{
	virtual FSimOutputData* MakeNewData() override { return MakeNew(); }
	static FSimOutputData* MakeNew() { return new FSingularisMorphSuspensionOutputData(); }

	virtual void FillOutputState(const Chaos::ISimulationModuleBase* SimModule) override;
	virtual void Lerp(const FSimOutputData& InCurrent, const FSimOutputData& InNext, float Alpha) override;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	virtual FString ToString() override;
#endif

	/** 悬挂压缩量（cm）：0 = 全伸张，MaxLength = 全压缩 */
	float SpringDisplacement = 0.0f;

	/** 压缩方向的车轮位移（已含动画偏移，供可视化与调试） */
	FVector SpringDisplacementVector = FVector::ZeroVector;

	/** 压缩速度（cm/s，沿压缩方向为正） */
	float SpringSpeed = 0.0f;

	/** 命中面法向 */
	FVector ImpactNormal = FVector(0.0f, 0.0f, 1.0f);
};

/**
 * 引力奇点变型悬挂配置参数。
 */
struct SINGULARISMORPHVEHICLE_API FSingularisMorphSuspensionSettings
{
	FSingularisMorphSuspensionSettings()
		: SuspensionAxis(FVector(0.0f, 0.0f, -1.0f)),
		  RestOffset(FVector::ZeroVector),
		  MaxRaise(5.0f),
		  MaxDrop(5.0f),
		  MaxLength(0.0f),
		  SpringRate(1.0f),
		  SpringPreload(0.5f),
		  SuspensionForceEffect(100.0f) {}

	FVector SuspensionAxis;
	FVector RestOffset;
	float MaxRaise;
	float MaxDrop;
	float MaxLength;

	/** 弹簧劲度（kg/s²，与 N/m 同值） */
	float SpringRate;

	/** 弹簧预载力（kg·cm/s²） */
	float SpringPreload;

	/** 轮胎载荷（下发车轮的 ForceIntoSurface）缩放系数；沿用上游默认 100 */
	float SuspensionForceEffect;

	/** 非簧载质量（kg）：车轮沿悬挂轴的等效质量，决定车轮对地面的跟随快慢 */
	float UnsprungMass = 40.0f;

	/** 簧载质量（kg，单车轮份额）：仅用于按阻尼比推算阻尼系数 */
	float SprungMass = 375.0f;

	/** 轮胎径向刚度相对弹簧劲度的倍率；实际轮胎刚度 = SpringRate × 该值 */
	float TireStiffnessRatio = 10.0f;

	/** 轮胎径向阻尼比（相对轮胎-车轮临界阻尼）：抑制车轮触地弹跳 */
	float TireDampingRatio = 0.1f;

	/** 回弹阻尼比（0-1，相对临界阻尼）：抑制车体回弹，通常大于压缩阻尼比 */
	float ReboundDampingRatio = 0.3f;

	/** 压缩阻尼比（0-1，相对临界阻尼）：抑制冲击传入车体，通常小于回弹阻尼比 */
	float CompressionDampingRatio = 0.15f;

	/** 重力加速度（cm/s²，沿世界下方向）：由组件在创建模块时从世界设置写入 */
	float Gravity = 980.0f;
};

/**
 * 引力奇点变型悬挂工厂。
 */
class FSingularisMorphSuspensionFactory : public Chaos::IFactoryModule
{
public:
	virtual TSharedPtr<Chaos::FModuleNetData> GenerateNetData(const int32 SimArrayIndex) const override
	{
		return MakeShared<FSingularisMorphSuspensionSimModuleData>(
			SimArrayIndex
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			,
			TEXT("SingularisMorphSuspensionSim")
#endif
		);
	}
};

/**
 * 引力奇点变型悬挂模拟模块。
 *
 * 在物理线程中运行，负责悬挂射线几何（起止点）、行程动力学积分、
 * 簧上反力与轮胎载荷的产出。
 *
 * 悬挂行程是被积分的状态量（非簧载质量模型）：车轮在弹簧-阻尼与轮胎接触力作用下
 * 逐步跟随地面，簧上反力与轮胎载荷均由该动态行程解出（ChaosVehicles 的两代插件
 * 均把射线距离直接映射成弹簧长度，没有行程过程）。行程到达全伸张或全压缩限位时
 * 清零继续压向限位的速度，车轮不会越过机械行程。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleSuspensionSimModule
	: public Chaos::FSuspensionBaseInterface,
	  public Chaos::TSimModuleSettings<FSingularisMorphSuspensionSettings>,
	  public Chaos::TSimulationModuleTypeable<FSingularisMorphVehicleSuspensionSimModule>
{
	friend FSingularisMorphSuspensionSimModuleData;
	friend FSingularisMorphSuspensionOutputData;

public:
	DEFINE_CHAOSSIMTYPENAME(SingularisMorphSuspensionSim);
	FSingularisMorphVehicleSuspensionSimModule(const FSingularisMorphSuspensionSettings& Settings);

	virtual TSharedPtr<Chaos::FModuleNetData> GenerateNetData(const int32 SimArrayIndex) const override
	{
		return MakeShared<FSingularisMorphSuspensionSimModuleData>(
			SimArrayIndex
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			,
			GetDebugName()
#endif
		);
	}

	virtual ~FSingularisMorphVehicleSuspensionSimModule() override;

	virtual Chaos::FSimOutputData* GenerateOutputData() const override
	{
		return FSingularisMorphSuspensionOutputData::MakeNew();
	}

	virtual const FString GetDebugName() const override { return TEXT("SingularisMorphSuspension"); }

	virtual float GetMaxSpringLength() const override { return Setup().MaxLength; }
	virtual float GetSpringLength() const override;
	virtual void SetSpringLength(float InLength, float WheelRadius) override;
	virtual void GetWorldTraceEndpoints(
		float DeltaSeconds,
		const FTransform& BodyTransform,
		const FVector& Velocity,
		float WheelRadius,
		Chaos::FSpringTrace& OutTrace
	) const override;

	virtual void Simulate(
		float DeltaTime,
		const Chaos::FAllInputs& Inputs,
		Chaos::FSimModuleTree& VehicleModuleSystem
	) override;
	virtual void Animate() override;

	const FVector& GetRestLocation() const { return Setup().RestOffset; }

private:
	/**
	 * 积分悬挂行程动力学（非簧载质量模型）。
	 *
	 * 车轮沿悬挂轴的自由度由弹簧、压缩/回弹阻尼与轮胎接触力驱动，子步长按
	 * 轮胎-车轮固有频率限制（显式积分在轮胎刚度远高于弹簧时必然发散）；
	 * 产出簧上反力（弹簧 + 阻尼）与地面法向反力（轮胎载荷）。
	 *
	 * @param DeltaTime 物理步长（秒）
	 * @param OutSuspensionForce 簧上反力（沿悬挂轴向上）
	 * @param OutForceIntoSurface 地面法向反力（未经 SuspensionForceEffect 缩放）
	 */
	void IntegrateTravel(float DeltaTime, float& OutSuspensionForce, float& OutForceIntoSurface);

	/** 按轮胎-车轮固有频率解算本帧子步参数 */
	void ComputeSubstepLayout(float DeltaTime, int32& OutNumSubsteps, float& OutSubDeltaTime) const;

	/** 悬挂压缩量（cm）：0 = 全伸张，MaxLength = 全压缩；行程动力学的积分状态 */
	float SpringDisplacement = 0.0f;

	/** 压缩速度（cm/s，沿压缩方向为正）：行程动力学的积分状态 */
	float SpringSpeed = 0.0f;

	/** 本帧射线给出的接触压缩量（cm）：车轮行程到达该值即轮胎触及地面 */
	float GroundDisplacement = 0.0f;

	/** 是否已收到过射线结果：首次以地面位置起步，避免模块创建瞬间从全伸张抽向地面 */
	bool bTravelInitialized = false;

	float CurrentTimeDilation = 1.0f;

	/** 压缩阻尼系数（kg/s），由压缩阻尼比与簧载质量解算 */
	float CompressionDamping = 0.0f;

	/** 回弹阻尼系数（kg/s），由回弹阻尼比与簧载质量解算 */
	float ReboundDamping = 0.0f;

	/** 轮胎径向刚度（kg/s²），由弹簧劲度与倍率解算 */
	float TireStiffness = 0.0f;

	/** 轮胎径向阻尼（kg/s），由轮胎刚度与非簧载质量按阻尼比解算 */
	float TireDamping = 0.0f;
};

/**
 * 引力奇点变型悬挂模拟工厂（自动注册）。
 */
class FSingularisMorphSuspensionSimFactory
	: public Chaos::FSimFactoryModule<FSingularisMorphSuspensionSimModuleData>,
	  public Chaos::TSimulationModuleTypeable<FSingularisMorphVehicleSuspensionSimModule,
	                                          FSingularisMorphSuspensionSimFactory>,
	  public Chaos::TSimFactoryAutoRegister<FSingularisMorphSuspensionSimFactory>
{
public:
	FSingularisMorphSuspensionSimFactory() : FSimFactoryModule(TEXT("SingularisMorphSuspensionSimFactory")) {}
};
