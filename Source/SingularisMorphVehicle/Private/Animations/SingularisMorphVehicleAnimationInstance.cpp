#include "Animations/SingularisMorphVehicleAnimationInstance.h"

#include "AnimationRuntime.h"
#include "Actors/SingularisMorphVehicleClusterPawn.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleAnimationInstance)

USingularisMorphVehicleAnimationInstance::USingularisMorphVehicleAnimationInstance()
{
	// 1) 调用父类默认构造函数完成动画实例初始化
}

class ASingularisMorphVehicleClusterPawn* USingularisMorphVehicleAnimationInstance::GetVehicle()
{
	// 1) 获取所属Actor并转换为变型载具集群Pawn类型
	return Cast<ASingularisMorphVehicleClusterPawn>(GetOwningActor());
}

void USingularisMorphVehicleAnimationInstance::NativeInitializeAnimation()
{
	// 1) 获取所属Actor作为组件查找的上下文
	if (AActor* Actor = GetOwningActor())
	{
		// 2) 在Actor上查找变型载具模拟组件
		if (USingularisMorphVehicleSimulationComponent* FoundModularVehicleComponent = Actor->FindComponentByClass<
			USingularisMorphVehicleSimulationComponent>())
			// 3) 将找到的组件设置到动画实例及代理
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

void FSingularisMorphVehicleAnimationInstanceProxy::SetModularVehicleComponent(
	const USingularisMorphVehicleSimulationComponent* InWheeledVehicleComponent
)
{
	const USingularisMorphVehicleSimulationComponent* ModularVehicleComponent = InWheeledVehicleComponent;

	// 1) 获取模块动画配置数组
	const TArray<FSingularisMorphModuleAnimationSetup>& ModuleAnimationSetups = ModularVehicleComponent->
		GetModuleAnimationSetups();

	const int32 NumOfModules = ModuleAnimationSetups.Num();
	ModuleInstances.Empty(NumOfModules);
	if (NumOfModules > 0)
	{
		ModuleInstances.AddZeroed(NumOfModules);

		// 2) 遍历初始化每个模块的骨骼名称与默认偏移量
		for (int32 ModuleIndex = 0; ModuleIndex < ModuleInstances.Num(); ++ModuleIndex)
		{
			FSingularisMorphModuleAnimationData& ModuleInstance = ModuleInstances[ModuleIndex];
			const FSingularisMorphModuleAnimationSetup& ModuleSetup = ModuleAnimationSetups[ModuleIndex];

			// 3) 设置骨骼名称并将位置与旋转偏移归零
			ModuleInstance.BoneName = ModuleSetup.BoneName;
			ModuleInstance.LocOffset = FVector::ZeroVector;
			ModuleInstance.RotOffset = FRotator::ZeroRotator;
		}
	}
}

void FSingularisMorphVehicleAnimationInstanceProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	Super::PreUpdate(InAnimInstance, DeltaSeconds);

	if (const USingularisMorphVehicleAnimationInstance* VehicleAnimInstance = Cast<
		USingularisMorphVehicleAnimationInstance>(InAnimInstance))
	{
		// 1) 检测并处理运行时动态新增的模块实例
		if (const USingularisMorphVehicleSimulationComponent* ModularVehicleComponent = VehicleAnimInstance->
			GetModularVehicleComponent())
		{
			if (ModuleInstances.Num() < ModularVehicleComponent->GetModuleAnimationSetups().Num())
			{
				int32 NumNew = ModularVehicleComponent->GetModuleAnimationSetups().Num() - ModuleInstances.Num();
				int32 StartIdx = ModuleInstances.Num();
				for (int32 I = 0; I < NumNew; I++)
				{
					FSingularisMorphModuleAnimationData ModuleInstance;
					ModuleInstance.BoneName = ModularVehicleComponent->GetModuleAnimationSetups()[StartIdx + I].
						BoneName;
					ModuleInstance.LocOffset = FVector::ZeroVector;
					ModuleInstance.RotOffset = FRotator::ZeroRotator;

					ModuleInstances.Add(ModuleInstance);
				}
			}
		}

		// 2) 同步每个模块的实时位置偏移、旋转偏移与动画标志位
		if (const USingularisMorphVehicleSimulationComponent* ModularVehicleComponent = VehicleAnimInstance->
			GetModularVehicleComponent())
		{
			for (int32 ModuleIndex = 0; ModuleIndex < ModuleInstances.Num(); ++ModuleIndex)
			{
				FSingularisMorphModuleAnimationData& ModuleInstance = ModuleInstances[ModuleIndex];
				if (ModularVehicleComponent->GetModuleAnimationSetups().IsValidIndex(ModuleIndex))
				{
					const FSingularisMorphModuleAnimationSetup& ModuleAnim = ModularVehicleComponent->
						GetModuleAnimationSetups()[ModuleIndex];
					{
						ModuleInstance.LocOffset = ModuleAnim.LocOffset;
						ModuleInstance.RotOffset = ModuleAnim.RotOffset;
						ModuleInstance.Flags |= ModuleAnim.AnimFlags;
					}
				}
			}
		}
	}
}
