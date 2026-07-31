#pragma once

#include <CoreMinimal.h>

#include "Chaos/ChaosEngineInterface.h"
#include "SimModule/SuspensionBaseInterface.h"

class FSingularisMorphSuspensionSimModule;

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
	  Chaos::TSimulationModuleTypeable<FSingularisMorphSuspensionSimModule, FSingularisMorphSuspensionSimModuleData>
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
		Ar << LastDisplacement;
	}

	virtual void Lerp(const float LerpFactor, const FModuleNetData& Min, const FModuleNetData& Max) override;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	virtual FString ToString() const override;
#endif

	float SpringDisplacement = 0.0f;
	float LastDisplacement = 0.0f;
};

/**
 * 引力奇点变型悬挂模拟输出数据。
 */
struct FSingularisMorphSuspensionOutputData
	: Chaos::FSimOutputData,
	  Chaos::TSimulationModuleTypeable<FSingularisMorphSuspensionSimModule, FSingularisMorphSuspensionOutputData>
{
	virtual FSimOutputData* MakeNewData() override { return MakeNew(); }
	static FSimOutputData* MakeNew() { return new FSingularisMorphSuspensionOutputData(); }

	virtual void FillOutputState(const Chaos::ISimulationModuleBase* SimModule) override;
	virtual void Lerp(const FSimOutputData& InCurrent, const FSimOutputData& InNext, float Alpha) override;
	virtual void GetFinalAnimDataGameThread(
		const FTransform& NewTransform,
		Chaos::FSimModuleAnimationData& AnimDataOut
	) override;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	virtual FString ToString() override;
#endif

	float SpringDisplacement = 0.0f;
	FVector SpringDisplacementVector = FVector::ZeroVector;
	float SpringSpeed = 0.0f;
	FVector ImpactNormal = FVector(0.0f, 0.0f, 1.0f);
	FVector ImpactPosition = FVector::ZeroVector;
	FVector ModuleLocalPosition = FVector::ZeroVector;
	FVector LocalSuspensionAxis = FVector::ZeroVector;
	float MaxRaise = 0.0f;
	float MaxDrop = 0.0f;
	bool bWheelHit = false;
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
		  SpringDamping(0.9f),
		  SuspensionForceEffect(100.0f) {}

	FVector SuspensionAxis;
	FVector RestOffset;
	float MaxRaise;
	float MaxDrop;
	float MaxLength;
	float SpringRate;
	float SpringPreload;
	float SpringDamping;
	float SuspensionForceEffect;
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
 * 在物理线程中运行，模拟悬挂弹簧-阻尼系统的力学行为，
 * 包括碰撞检测（射线/球体）、弹簧力计算与约束创建。
 */
class FSingularisMorphSuspensionSimModule
	: public Chaos::FSuspensionBaseInterface,
	  public Chaos::TSimModuleSettings<FSingularisMorphSuspensionSettings>,
	  public Chaos::TSimulationModuleTypeable<FSingularisMorphSuspensionSimModule>
{
	friend FSingularisMorphSuspensionSimModuleData;
	friend FSingularisMorphSuspensionOutputData;

public:
	DEFINE_CHAOSSIMTYPENAME(SingularisMorphSuspensionSim);
	FSingularisMorphSuspensionSimModule(const FSingularisMorphSuspensionSettings& Settings);

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

	virtual ~FSingularisMorphSuspensionSimModule() override;

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
	virtual void OnConstruction_External(const Chaos::FPhysicsObjectHandle& PhysicsObject) override;
	virtual void OnTermination_External() override;

	virtual void Simulate(
		float DeltaTime,
		const Chaos::FAllInputs& Inputs,
		Chaos::FSimModuleTree& VehicleModuleSystem
	) override;
	virtual void Animate() override;

	const FVector& GetRestLocation() const { return Setup().RestOffset; }
	void UpdateConstraint();

protected:
	void CreateConstraint(const Chaos::FPhysicsObjectHandle& PhysicsObject);
	void DestroyConstraint();

private:
	float SpringDisplacement = 0.0f;
	float LastDisplacement = 0.0f;
	float SpringSpeed = 0.0f;
	float CurrentTimeDilation = 1.0f;
	FPhysicsConstraintHandle ConstraintHandle;
};

/**
 * 引力奇点变型悬挂模拟工厂（自动注册）。
 */
class FSingularisMorphSuspensionSimFactory
	: public Chaos::FSimFactoryModule<FSingularisMorphSuspensionSimModuleData>,
	  public Chaos::TSimulationModuleTypeable<FSingularisMorphSuspensionSimModule,
	                                          FSingularisMorphSuspensionSimFactory>,
	  public Chaos::TSimFactoryAutoRegister<FSingularisMorphSuspensionSimFactory>
{
public:
	FSingularisMorphSuspensionSimFactory() : FSimFactoryModule(TEXT("SingularisMorphSuspensionSimFactory")) {}
};
