#include "Types/SingularisMorphVehicleInputProducer.h"

// Default input producer

#include <PBDRigidsSolver.h>
#include <Engine/World.h>
#include <Physics/Experimental/PhysScene_Chaos.h>

#include "Types/SingularisMorphVehicleInputUtils.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleInputProducer)

void USingularisMorphVehicleDefaultInputProducer::InitializeContainer(
	TArray<FModuleInputSetup>& SetupData,
	FInputNameMap& NameMapOut,
	EModuleInputQuantizationType InInputQuantizationType
)
{
	Super::InitializeContainer(SetupData, NameMapOut, InInputQuantizationType);

	// 容器重建会清零已捕获的输入，使拓扑变更（运行时增删模块）那一帧丢掉玩家输入；
	// 先备份旧值，重建后按名称回填。生产者持有自己的名映射副本，
	// 使回填不依赖调用方的重建顺序
	const TArray<FModuleInputValue> PreviousValues = MergedInput.AccessInputValues();

	MergedInput.Initialize(SetupData, NameMapOut);
	SingularisMorphVehicleInputUtils::CarryOverValues(MergedInput, NameMapOut, InputNameMap, PreviousValues);

	InputNameMap = NameMapOut;
}

void USingularisMorphVehicleDefaultInputProducer::BufferInput(
	const FInputNameMap& InNameMap,
	const FName InName,
	const FModuleInputValue& InValue,
	EModuleInputBufferActionType BufferAction
)
{
	switch (BufferAction)
	{
	case EModuleInputBufferActionType::Override:
		{
			// inputs are merged here rather than buffered, since they would be merged anyway before use in ProduceInput
			FInputInterface Inputs(InNameMap, MergedInput, InputQuantizationType);
			Inputs.MergeValue(InName, InValue);
			break;
		}
	case EModuleInputBufferActionType::Combine:
		{
			FInputInterface Inputs(InNameMap, MergedInput, InputQuantizationType);
			Inputs.CombineValue(InName, InValue);
			break;
		}
	case EModuleInputBufferActionType::Average:
		{
			FInputInterface Inputs(InNameMap, MergedInput, InputQuantizationType);
			Inputs.AverageValue(InName, InValue);
			break;
		}
	default:
		break;
	}
}

void USingularisMorphVehicleDefaultInputProducer::ProduceInput(
	int32 PhysicsStep,
	int32 NumSteps,
	const FInputNameMap& InNameMap,
	FModuleInputContainer& InOutContainer
)
{
	// copy state out
	InOutContainer = MergedInput;

	// reset state for next frame
	MergedInput.ZeroValues();
}


// Example Playback Input Producer

void USingularisMorphVehiclePlaybackInputProducer::InitializeContainer(
	TArray<FModuleInputSetup>& SetupData,
	FInputNameMap& NameMapOut,
	EModuleInputQuantizationType InInputQuantizationType
)
{
	Super::InitializeContainer(SetupData, NameMapOut, InInputQuantizationType);

	// 重建前先清空：运行时增删模块会再次调用本函数，追加会使缓冲单调增长，
	// 且物理线程按帧号取到的是首次填充的陈旧条目
	PlaybackBuffer.Reset();
	PlaybackBuffer.Reserve(BufferLength);

	FRandomStream Random(PlaybackSeed);

	// Initialize a single container once
	FModuleInputContainer InputsForFrame;
	InputsForFrame.Initialize(SetupData, NameMapOut);

	for (auto I = 0; I < BufferLength; I++)
	{
		// copy initialized container into array
		PlaybackBuffer.Emplace(InputsForFrame);

		// change value of some intputs
		FInputInterface Inputs(NameMapOut, PlaybackBuffer[I], InputQuantizationType);
		Inputs.SetValue("Throttle", Random.FRand());
		Inputs.SetValue("Steering", 1.0f - 2.0f * Random.FRand());
	}
}

void USingularisMorphVehiclePlaybackInputProducer::BufferInput(
	const FInputNameMap& InNameMap,
	const FName InName,
	const FModuleInputValue& InValue,
	EModuleInputBufferActionType BufferAction
)
{
	// NOP
}

void USingularisMorphVehiclePlaybackInputProducer::ProduceInput(
	int32 /*PhysicsStep*/,
	int32 /*NumSteps*/,
	const FInputNameMap& /*InNameMap*/,
	FModuleInputContainer& InOutContainer
)
{
	// 回放索引由物理线程按求解器帧号完成（FSingularisMorphVehicleSimulation::SimulateModuleTree）：
	// 物理线程会以同一份回放缓冲覆盖本容器，游戏线程按物理步号的取值没有消费方，
	// 且与物理线程的帧号时基不一致，因此此处不参与取值
	InOutContainer.ZeroValues();
}


// Example Random Input Producer

void USingularisMorphVehicleRandomInputProducer::InitializeContainer(
	TArray<FModuleInputSetup>& SetupData,
	FInputNameMap& NameMapOut,
	EModuleInputQuantizationType InInputQuantizationType
)
{
	Super::InitializeContainer(SetupData, NameMapOut, InInputQuantizationType);

	PlaybackContainer.Initialize(SetupData, NameMapOut);
}

void USingularisMorphVehicleRandomInputProducer::BufferInput(
	const FInputNameMap& InNameMap,
	const FName InName,
	const FModuleInputValue& InValue,
	EModuleInputBufferActionType BufferAction
)
{
	// NOP
}

void USingularisMorphVehicleRandomInputProducer::ProduceInput(
	int32 PhysicsStep,
	int32 NumSteps,
	const FInputNameMap& InNameMap,
	FModuleInputContainer& InOutContainer
)
{
	// 随机输入由「种子 + 求解器帧号分组」派生，而不是从实例随机流推进：
	// 后者在回放（Resimulate）时会因流位置不同而产生与首次模拟不同的输入，
	// 破坏网络预测的确定性。取不到求解器帧号时退化为按物理子步分组。
	int32 GroupIndex = PhysicsStep;
	if (UWorld* World = GetWorld())
	{
		if (FPhysScene* Scene = World->GetPhysicsScene())
		{
			if (Chaos::FPhysicsSolver* Solver = Scene->GetSolver())
				GroupIndex = Solver->GetCurrentFrame();
		}
	}

	GroupIndex /= FMath::Max(ChangeInputFrequency, 1);

	// 每 ChangeInputFrequency 帧更新一组控制量，期间沿用上一组（由帧号推导，无需额外状态）
	FRandomStream Random(Seed + GroupIndex * 7919);
	PlaybackContainer.ZeroValues();

	FInputInterface Inputs(InNameMap, PlaybackContainer, InputQuantizationType);
	Inputs.SetValue("Throttle", Random.FRand());
	Inputs.SetValue("Steering", 1.0f - 2.0f * Random.FRand());

	InOutContainer = PlaybackContainer;
}
