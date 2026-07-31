#pragma once

#include "SimModule/ModuleInput.h"
#include "SingularisMorphVehicleInputProducer.generated.h"

/**
 * 引力奇点变型载具默认输入生产者。
 *
 * 从玩家控制器捕获实时的 EnhancedInput 输入，并量化为物理线程可消费的模块输入缓冲。
 */
UCLASS(BlueprintType, Blueprintable)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleDefaultInputProducer : public UVehicleInputProducerBase
{
	GENERATED_BODY()

public:
	virtual void InitializeContainer(
		TArray<FModuleInputSetup>& SetupData,
		FInputNameMap& NameMapOut,
		EModuleInputQuantizationType InInputQuantizationType
	) override;
	virtual void BufferInput(
		const FInputNameMap& InNameMap,
		FName InName,
		const FModuleInputValue& InValue,
		EModuleInputBufferActionType BufferAction
	) override;
	virtual void ProduceInput(
		int32 PhysicsStep,
		int32 NumSteps,
		const FInputNameMap& InNameMap,
		FModuleInputContainer& InOutContainer
	) override;

	FModuleInputContainer MergedInput;
};

/**
 * 引力奇点变型载具回放输入生产者。
 *
 * 记录输入缓冲区并在物理线程中循环回放，用于测试与确定性模拟验证。
 */
UCLASS(BlueprintType, Blueprintable)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehiclePlaybackInputProducer : public UVehicleInputProducerBase
{
	GENERATED_BODY()

public:
	virtual void InitializeContainer(
		TArray<FModuleInputSetup>& SetupData,
		FInputNameMap& NameMapOut,
		EModuleInputQuantizationType InInputQuantizationType
	) override;
	virtual void BufferInput(
		const FInputNameMap& InNameMap,
		FName InName,
		const FModuleInputValue& InValue,
		EModuleInputBufferActionType BufferAction
	) override;
	virtual void ProduceInput(
		int32 PhysicsStep,
		int32 NumSteps,
		const FInputNameMap& InNameMap,
		FModuleInputContainer& InOutContainer
	) override;

	virtual TArray<FModuleInputContainer>* GetTestInputBuffer() override { return &PlaybackBuffer; }
	virtual bool IsLoopingTestInputBuffer() override { return true; }

	TArray<FModuleInputContainer> PlaybackBuffer;
	int32 BufferLength = 150;
	int32 StartStep = 0;
};

/**
 * 引力奇点变型载具随机输入生产者。
 *
 * 在物理线程中动态生成随机输入数据，用于压力测试与随机遍历模拟。
 */
UCLASS(BlueprintType, Blueprintable)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleRandomInputProducer : public UVehicleInputProducerBase
{
	GENERATED_BODY()

public:
	virtual void InitializeContainer(
		TArray<FModuleInputSetup>& SetupData,
		FInputNameMap& NameMapOut,
		EModuleInputQuantizationType InInputQuantizationType
	) override;
	virtual void BufferInput(
		const FInputNameMap& InNameMap,
		FName InName,
		const FModuleInputValue& InValue,
		EModuleInputBufferActionType BufferAction
	) override;
	virtual void ProduceInput(
		int32 PhysicsStep,
		int32 NumSteps,
		const FInputNameMap& InNameMap,
		FModuleInputContainer& InOutContainer
	) override;

	FModuleInputContainer PlaybackContainer;
	int32 ChangeInputFrequency = 10;
};
