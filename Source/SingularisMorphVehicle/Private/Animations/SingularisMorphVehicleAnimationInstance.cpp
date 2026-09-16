#include "Animations/SingularisMorphVehicleAnimationInstance.h"

#include "Actors/SingularisMorphVehicleClusterPawn.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleAnimationInstance)

namespace
{
	/** 模块集合签名：模块数量与骨骼名共同决定实例列表是否与最新配置一致 */
	uint32 ComputeModuleSourceSignature(const USingularisMorphVehicleSimulationComponent& Component)
	{
		const TArray<FSingularisMorphModuleAnimationSetup>& ModuleAnimationSetups =
			Component.GetModuleAnimationSetups();

		auto Signature = GetTypeHash(ModuleAnimationSetups.Num());
		for (const FSingularisMorphModuleAnimationSetup& Setup : ModuleAnimationSetups)
			Signature = HashCombine(Signature, GetTypeHash(Setup.BoneName));

		return Signature;
	}
}

void FSingularisMorphVehicleAnimationInstanceProxy::PreUpdate(
	UAnimInstance* InAnimInstance,
	const float DeltaSeconds
)
{
	Super::PreUpdate(InAnimInstance, DeltaSeconds);

	const USingularisMorphVehicleAnimationInstance* VehicleAnimInstance = Cast<
		USingularisMorphVehicleAnimationInstance>(InAnimInstance);
	if (!VehicleAnimInstance) return;

	const USingularisMorphVehicleSimulationComponent* ModularVehicleComponent = VehicleAnimInstance->
		GetModularVehicleComponent();
	if (!ModularVehicleComponent) return;

	// 1) 运行时变形会重建模块集合，集合变化时重建实例列表，
	//    保持实例下标与 ModuleAnimationSetups 一一对应
	if (NeedsRebuild(*ModularVehicleComponent))
		RebuildModuleInstances(*ModularVehicleComponent);

	// 2) 同步各模块的实时位置偏移、旋转偏移与动画标志位
	SyncModuleAnimData(*ModularVehicleComponent);
}

bool FSingularisMorphVehicleAnimationInstanceProxy::NeedsRebuild(
	const USingularisMorphVehicleSimulationComponent& Component
) const
{
	if (!bModuleInstancesValid) return true;

	return InstanceSourceSignature != ComputeModuleSourceSignature(Component);
}

void FSingularisMorphVehicleAnimationInstanceProxy::RebuildModuleInstances(
	const USingularisMorphVehicleSimulationComponent& Component
)
{
	const TArray<FSingularisMorphModuleAnimationSetup>& ModuleAnimationSetups =
		Component.GetModuleAnimationSetups();

	ModuleInstances.Empty(ModuleAnimationSetups.Num());
	if (!ModuleAnimationSetups.IsEmpty())
	{
		ModuleInstances.AddZeroed(ModuleAnimationSetups.Num());

		for (auto ModuleIndex = 0; ModuleIndex < ModuleInstances.Num(); ++ModuleIndex)
		{
			FSingularisMorphModuleAnimationData& ModuleInstance = ModuleInstances[ModuleIndex];
			const FSingularisMorphModuleAnimationSetup& ModuleSetup = ModuleAnimationSetups[ModuleIndex];

			// 实例数据归零后重新绑定骨骼名，偏移量由 Simulate 输出逐帧填充
			ModuleInstance.BoneName = ModuleSetup.BoneName;
			ModuleInstance.LocOffset = FVector::ZeroVector;
			ModuleInstance.RotOffset = FRotator::ZeroRotator;
			ModuleInstance.Flags = 0;
		}
	}

	InstanceSourceSignature = ComputeModuleSourceSignature(Component);
	bModuleInstancesValid = true;
}

void FSingularisMorphVehicleAnimationInstanceProxy::SyncModuleAnimData(
	const USingularisMorphVehicleSimulationComponent& Component
)
{
	const TArray<FSingularisMorphModuleAnimationSetup>& ModuleAnimationSetups =
		Component.GetModuleAnimationSetups();

	const int32 NumModules = FMath::Min(ModuleInstances.Num(), ModuleAnimationSetups.Num());
	for (auto ModuleIndex = 0; ModuleIndex < NumModules; ++ModuleIndex)
	{
		FSingularisMorphModuleAnimationData& ModuleInstance = ModuleInstances[ModuleIndex];
		const FSingularisMorphModuleAnimationSetup& ModuleAnimation = ModuleAnimationSetups[ModuleIndex];

		ModuleInstance.LocOffset = ModuleAnimation.LocOffset;
		ModuleInstance.RotOffset = ModuleAnimation.RotOffset;
		ModuleInstance.Flags |= ModuleAnimation.AnimFlags;
	}
}

void USingularisMorphVehicleAnimationInstance::NativeInitializeAnimation()
{
	// 1) 获取所属Actor作为组件查找的上下文
	if (const AActor* Actor = GetOwningActor())
	{
		// 2) 在Actor上查找变型载具模拟组件并设置到动画实例
		if (const USingularisMorphVehicleSimulationComponent* FoundModularVehicleComponent =
			Actor->FindComponentByClass<USingularisMorphVehicleSimulationComponent>())
			SetModularVehicleComponent(FoundModularVehicleComponent);
	}
}

FAnimInstanceProxy* USingularisMorphVehicleAnimationInstance::CreateAnimInstanceProxy()
{
	// 1) 返回预创建的成员动画实例代理指针
	return &AnimInstanceProxy;
}

void USingularisMorphVehicleAnimationInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	// 1) 动画实例代理由成员变量统一管理生命周期，无需手动销毁
}

class ASingularisMorphVehicleClusterPawn* USingularisMorphVehicleAnimationInstance::GetVehicle()
{
	// 1) 获取所属Actor并转换为变型载具集群Pawn类型
	return Cast<ASingularisMorphVehicleClusterPawn>(GetOwningActor());
}
