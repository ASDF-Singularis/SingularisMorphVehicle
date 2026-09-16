#include "Types/SingularisMorphVehicleInputProducer.h"

// Default input producer

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleInputProducer)

void USingularisMorphVehicleDefaultInputProducer::InitializeContainer(
	TArray<FModuleInputSetup>& SetupData,
	FInputNameMap& NameMapOut,
	EModuleInputQuantizationType InInputQuantizationType
)
{
	Super::InitializeContainer(SetupData, NameMapOut, InInputQuantizationType);

	MergedInput.Initialize(SetupData, NameMapOut);
}

void USingularisMorphVehicleDefaultInputProducer::BufferInput(
	const FInputNameMap& InNameMap,
	const FName InName,
	const FModuleInputValue& InValue,
	EModuleInputBufferActionType BufferAction
)
{
	//UE_LOGF(LogTemp, Warning, "BufferInput - InName %ls Val=%ls", *InName.ToString(), *InValue.ToString());

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
	//UE_LOGF(LogTemp, Warning, "ProduceInput - PhysicsStep %d, NumSteps %d", PhysicsStep, NumSteps);
	//FInputInterface Inputs(InNameMap, MergedInput);
	//UE_LOGF(LogTemp, Warning, ".. Throttle %ls", *Inputs.GetValue("Throttle").ToString());

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

	auto Seed = 123;
	FRandomStream Random(Seed);

	// Initialize a single container once
	FModuleInputContainer InputsForFrame;
	InputsForFrame.Initialize(SetupData, NameMapOut);

	PlaybackBuffer.Reserve(BufferLength);
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
	// new control settings generated every ChangeInputFrequency number of frames (every frame is too quick)
	// previous controls are held in the PlaybackContainer between the changes
	if (PhysicsStep % ChangeInputFrequency == 0)
	{
		// clear old input
		PlaybackContainer.ZeroValues();

		// generate new random input
		FInputInterface Inputs(InNameMap, PlaybackContainer, InputQuantizationType);
		Inputs.SetValue("Throttle", RandomStream.FRand());
		Inputs.SetValue("Steering", 1.0f - 2.0f * RandomStream.FRand());
	}

	InOutContainer = PlaybackContainer;
}
