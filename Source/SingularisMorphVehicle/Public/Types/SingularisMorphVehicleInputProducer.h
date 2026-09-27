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

	/** 已捕获的输入缓冲（BufferInput 合并写入，ProduceInput 拷出并清零） */
	FModuleInputContainer MergedInput;

	/**
	 * 生产者的输入名映射副本。
	 *
	 * 容器重建时需按名称回填已捕获的输入，而调用方传入的名映射在重建前后不同，
	 * 故生产者自持一份，使回填不依赖调用方的调用顺序。
	 */
	FInputNameMap InputNameMap;
};

/**
 * 引力奇点变型载具回放输入生产者。
 *
 * 记录输入缓冲区并在物理线程中循环回放，用于测试与确定性模拟验证。
 * 回放索引由物理线程按求解器帧号推进，游戏线程不参与取值。
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

	/** 回放缓冲的随机生成种子（固定值以保证回放可重现） */
	int32 PlaybackSeed = 123;
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

	/** 输入变化周期（帧）：每经过该帧数换一组随机控制量 */
	int32 ChangeInputFrequency = 10;

	/** 随机种子（与求解器帧号共同决定输入，保证回放可重现） */
	int32 Seed = 123;
};
