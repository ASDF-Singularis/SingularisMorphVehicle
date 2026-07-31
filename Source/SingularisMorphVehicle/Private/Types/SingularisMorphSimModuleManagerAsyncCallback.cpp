#include "Types/SingularisMorphSimModuleManagerAsyncCallback.h"

#include "PBDRigidsSolver.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Core/SingularisMorphVehicleSimulationCU.h"
#include "GeometryCollection/GeometryCollectionParticlesData.h"
#include "Net/Core/NetBitArray.h"
#include "Net/Iris/ReplicationSystem/StructNetTokenDataStoreHelper.h"
#include "PhysicsProxy/ClusterUnionPhysicsProxy.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "SimModule/ModuleFactoryRegister.h"
#include "Types/SingularisMorphModuleInputTokenStore.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphSimModuleManagerAsyncCallback)

/**
 * 全局调试参数实例。
 * - EnableMultithreading：控制物理线程回调中是否启用 ParallelFor 多线程。
 *   默认关闭（false），需在运行时通过调试工具显式开启。
 * - EnableNetworkStateData：控制是否启用网络状态数据同步。
 */
FSingularisMorphSimModuleDebugParams GSingularisMorphSimModuleDebugParams;

DECLARE_CYCLE_STAT(
	TEXT("AsyncCallback:OnPreSimulate_Internal"),
	STAT_AsyncCallback_OnPreSimulate,
	STATGROUP_SingularisMorphSimModuleManager
);
DECLARE_CYCLE_STAT(
	TEXT("AsyncCallback:OnContactModification_Internal"),
	STAT_AsyncCallback_OnContactModification,
	STATGROUP_SingularisMorphSimModuleManager
);

/**
 * 模块化载具网络同步控制台变量（Console Variables）。
 * 通过控制台命令 `p.ModularVehicle.*` 在运行时动态调节网络复制行为。
 */
namespace SingularisMorphVehicleCVars
{
	/**
	 * 启用载具状态的网络令牌 + 增量序列化路径。
	 * 开启后使用 NetToken 替代完整序列化，大幅降低状态同步的带宽消耗。
	 * 默认关闭，需在确认网络兼容性后启用。
	 */
	bool bEnableStateReducedBandwidth = false;
	FAutoConsoleVariableRef EnableStateReducedBandwidth(
		TEXT("p.ModularVehicle.EnableStateReducedBandwidth"),
		bEnableStateReducedBandwidth,
		TEXT("Enable/Disable NetTokens and DeltaSerialization path for State of Modular Vehicles. Default: false")
	);
	/**
	 * 启用载具输入的网络令牌 + 增量序列化路径。
	 * 开启后使用 NetToken 替代完整序列化，降低输入同步的带宽消耗。
	 */
	bool bEnableInputReducedBandwidth = false;
	FAutoConsoleVariableRef EnableInputReducedBandwidth(
		TEXT("p.ModularVehicle.EnableInputReducedBandwidth"),
		bEnableInputReducedBandwidth,
		TEXT("Enable/Disable NetTokens and DeltaSerialization path for Input of Modular Vehicles. Default: false")
	);
	/**
	 * 启用网络序列化的调试日志输出。
	 * 仅在开发调试时开启，会产生大量日志影响性能。
	 */
	bool bEnableStateNetSerializeDebugPrinting = false;
	FAutoConsoleVariableRef EnableStateNetSerializeDebugPrinting(
		TEXT("p.ModularVehicle.EnableStateNetSerializeDebugPrinting"),
		bEnableStateNetSerializeDebugPrinting,
		TEXT("Enable/Disable debug logging during NetSerialization. Default: false")
	);
};

UE_NET_IMPLEMENT_NAMED_NETTOKEN_STRUCT_SERIALIZERS(SingularisMorphModuleInputNetTokenData)

UE_NET_IMPLEMENT_NAMED_NETTOKEN_STRUCT_SERIALIZERS(NetworkSingularisMorphVehicleStateNetTokenData)

/**
 * 返回统计 ID 名称，供 Unreal Insights 性能分析器识别此回调对象。
 */
FName FSingularisMorphSimModuleManagerAsyncCallback::GetFNameForStatId() const
{
	static constexpr FLazyName StaticName("FSingularisMorphSimModuleManagerAsyncCallback");
	return StaticName;
}

/**
 * 将网络输入数据应用到载具组件（物理线程回调）。
 *
 * 从网络包中反序列化的输入数据通过此函数写入载具的 VehicleSimulationPT，
 * 供后续 Simulate() 使用。仅在 EnableNetworkStateData 开启时执行。
 */
void FNetworkSingularisMorphVehicleInputs::ApplyData(UActorComponent* NetworkComponent) const
{
	if (GSingularisMorphSimModuleDebugParams.EnableNetworkStateData)
	{
		if (USingularisMorphVehicleSimulationComponent* ModularBaseComponent = Cast<
			USingularisMorphVehicleSimulationComponent>(
			NetworkComponent
		))
		{
			if (FSingularisMorphVehicleSimulation* VehicleSimulation = ModularBaseComponent->VehicleSimulationPT.Get())
				VehicleSimulation->VehicleInputs = VehicleInputs;
		}
	}
}

/**
 * 从载具组件构建网络输入数据（物理线程回调）。
 *
 * 从载具的 VehicleSimulationPT 中读取当前输入状态，
 * 填充到此结构体中以备网络序列化发送。
 */
void FNetworkSingularisMorphVehicleInputs::BuildData(const UActorComponent* NetworkComponent)
{
	if (GSingularisMorphSimModuleDebugParams.EnableNetworkStateData)
	{
		if (const USingularisMorphVehicleSimulationComponent* ModularBaseComponent = Cast<const
			USingularisMorphVehicleSimulationComponent>(NetworkComponent))
		{
			if (const FSingularisMorphVehicleSimulation* VehicleSimulation = ModularBaseComponent->VehicleSimulationPT.
				Get())
				VehicleInputs = VehicleSimulation->VehicleInputs;
		}
	}
}

bool FNetworkSingularisMorphVehicleInputs::NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
{
	static FNetworkSingularisMorphVehicleInputs LocalDefaultDeltaSource;
	auto InputQuantizationType = EModuleInputQuantizationType::Default_16Bits;
	if (const USingularisMorphVehicleSimulationComponent* ModularVehicleBaseComponent = Cast<
		USingularisMorphVehicleSimulationComponent>(ImplementationComponent.Get()))
		InputQuantizationType = ModularVehicleBaseComponent->InputQuantizationType;

	// If we should use delta source but the source is null, use a default source so that we at least consume the replicated bits correctly and doesn't cause a crash.
	if (bIsUsingDeltaSerialization && !DeltaSourceData)
		DeltaSourceData = &LocalDefaultDeltaSource;

	SerializeFrames(Ar);

	Ar.SerializeBits(&VehicleInputs.Reverse, 1);
	Ar.SerializeBits(&VehicleInputs.KeepAwake, 1);
	bOutSuccess = true;
	auto DeltaSource = static_cast<FNetworkSingularisMorphVehicleInputs*>(DeltaSourceData);
	if (SingularisMorphVehicleCVars::bEnableInputReducedBandwidth && bIsUsingDeltaSerialization && DeltaSource)
	{
		using namespace UE::Net;
		bOutSuccess = true;

		TArray<FModuleInputValue>& InputValues = VehicleInputs.Container.AccessInputValues();
		const TArray<FModuleInputValue>& PreviousInputValues = DeltaSource->VehicleInputs.Container.AccessInputValues();

		FSingularisMorphModuleInputNetTokenData InputStateData;
		InputStateData.Init(InputValues);
		const bool bNetTokenSuccess = TStructNetTokenDataStoreHelper<
			FSingularisMorphModuleInputNetTokenData>::NetSerializeAndExportToken(Ar, Map, InputStateData);
		if (!bNetTokenSuccess)
		{
			bOutSuccess = false;
			return bOutSuccess;
		}

		uint32 Number = InputStateData.Types.Num();
		if (Ar.IsLoading())
			InputValues.SetNum(Number);

		for (uint32 I = 0; I < Number; I++)
		{
			InputValues[I].ConvertToType(static_cast<EModuleInputValueType>(InputStateData.Types[I]));
			InputValues[I].SetApplyInputDecay(InputStateData.DecayValues[I]);
			if (PreviousInputValues.Num() == InputValues.Num())
				InputValues[I].DeltaNetSerialize(Ar, Map, bOutSuccess, PreviousInputValues[I], InputQuantizationType);
			else
			{
				bOutSuccess = false;
				//Fail case.
				InputValues[I].DeltaNetSerialize(Ar, Map, bOutSuccess, InputValues[I], InputQuantizationType);
			}
		}
	}
	else
		VehicleInputs.Container.Serialize(Ar, Map, bOutSuccess, InputQuantizationType);

	return bOutSuccess;
}


void FNetworkSingularisMorphVehicleInputs::InterpolateData(
	const FNetworkPhysicsData& MinData,
	const FNetworkPhysicsData& MaxData
)
{
	const auto& MinInput = static_cast<const FNetworkSingularisMorphVehicleInputs&>(MinData);
	const auto& MaxInput = static_cast<const FNetworkSingularisMorphVehicleInputs&>(MaxData);

	const float LerpFactor = (LocalFrame - MinInput.LocalFrame) / (MaxInput.LocalFrame - MinInput.LocalFrame);

	VehicleInputs.Reverse = MinInput.VehicleInputs.Reverse;
	VehicleInputs.KeepAwake = MinInput.VehicleInputs.KeepAwake;
	VehicleInputs.Container.Lerp(MinInput.VehicleInputs.Container, MaxInput.VehicleInputs.Container, LerpFactor);
}

void FNetworkSingularisMorphVehicleInputs::MergeData(const FNetworkPhysicsData& FromData)
{
	const auto& FromInput = static_cast<const FNetworkSingularisMorphVehicleInputs&>(FromData);
	VehicleInputs.Container.Merge(FromInput.VehicleInputs.Container);
}

void FNetworkSingularisMorphVehicleInputs::DecayData(float DecayAmount)
{
	VehicleInputs.Container.Decay(DecayAmount);
}

uint64 FNetworkSingularisMorphVehicleStateNetTokenData::GetUniqueKey() const
{
	uint64 HashOfHashes = GetTypeHash(Hashes);
	uint64 HashOfIndexes = GetTypeHash(Indexes);
	uint64 HashOfShouldSerialize = GetTypeHash(ModuleShouldSerialize);
	return HashOfHashes << 32 ^ HashOfIndexes ^ HashOfShouldSerialize;
}

void FNetworkSingularisMorphVehicleStateNetTokenData::Init(const Chaos::FModuleNetDataArray& ModuleData)
{
	Hashes.Reset(ModuleData.Num());
	Indexes.Reset(ModuleData.Num());
	ModuleShouldSerialize.Reset(ModuleData.Num());
	for (auto Idx = 0; Idx < ModuleData.Num(); Idx++)
	{
		uint32 Hash = Chaos::FModuleFactoryRegister::GetModuleHash(ModuleData[Idx]->GetSimType());
		Hashes.Add(Hash);
		Indexes.Add(ModuleData[Idx]->SimArrayIndex);
		ModuleShouldSerialize.Add(!ModuleData[Idx]->IsDefaultState());
	}
}

void FNetworkSingularisMorphVehicleStates::ApplyData(UActorComponent* NetworkComponent) const
{
	if (USingularisMorphVehicleSimulationComponent* ModularBaseComponent = Cast<
		USingularisMorphVehicleSimulationComponent>(
		NetworkComponent
	))
	{
		if (FSingularisMorphVehicleSimulation* VehicleSimulation = ModularBaseComponent->VehicleSimulationPT.Get())
			VehicleSimulation->AccessSimComponentTree()->SetSimState(ModuleData);
	}
}

void FNetworkSingularisMorphVehicleStates::BuildData(const UActorComponent* NetworkComponent)
{
	if (NetworkComponent)
	{
		if (const FSingularisMorphVehicleSimulation* VehicleSimulation = Cast<const
			USingularisMorphVehicleSimulationComponent>(NetworkComponent)->VehicleSimulationPT.Get())
			VehicleSimulation->GetSimComponentTree()->SetNetState(ModuleData);
	}
}

bool FNetworkSingularisMorphVehicleStates::NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
{
	if (SingularisMorphVehicleCVars::bEnableStateReducedBandwidth && bIsUsingDeltaSerialization)
		return DeltaNetSerialize(Ar, Map, bOutSuccess);

	using namespace UE::Net;

	SerializeFrames(Ar);

	uint32 NumNetModules = ModuleData.Num();
	Ar.SerializeIntPacked(NumNetModules);

	// Array of bits to mark which modules to serialize or not
	FNetBitArray ModulesBitArray(NumNetModules);

	if (Ar.IsLoading() && NumNetModules != ModuleData.Num())
		ModuleData.Reserve(NumNetModules);

	if (Ar.IsLoading())
	{
		Ar.SerializeBits(ModulesBitArray.GetData(), NumNetModules);

		if (NumNetModules != ModuleData.Num())
			ModuleData.SetNum(NumNetModules);

		for (uint32 I = 0; I < NumNetModules; I++)
		{
			uint32 ModuleTypeHash = 0;
			uint32 SimArrayIndexUnsigned = 0;

			Ar << ModuleTypeHash;
			Ar.SerializeIntPacked(SimArrayIndexUnsigned);

			const int32 SimArrayIndex = static_cast<int32>(SimArrayIndexUnsigned) - 1;
			// Convert back to signed and adjust

			if (TSharedPtr<Chaos::FModuleNetData> Data = Chaos::FModuleFactoryRegister::Get().GenerateNetData(
				ModuleTypeHash,
				SimArrayIndex
			))
			{
				check(ModuleTypeHash == Chaos::FModuleFactoryRegister::GetModuleHash(Data->GetSimType()));
				ModuleData[I] = Data;

				const bool bHasSerializedData = ModulesBitArray.IsBitSet(I);
				if (bHasSerializedData)
					ModuleData[I]->Serialize(Ar);
				else
					ModuleData[I]->ApplyDefaultState();
			}
		}
	}
	else
	{
		// Only mark modules for serialization if they are not in their default state
		for (uint32 I = 0; I < NumNetModules; I++)
		{
			if (ModuleData[I]->IsDefaultState() == false)
				ModulesBitArray.SetBit(I);
		}
		Ar.SerializeBits(ModulesBitArray.GetData(), NumNetModules);

		for (uint32 I = 0; I < NumNetModules; I++)
		{
			uint32 ModuleTypeHash = Chaos::FModuleFactoryRegister::GetModuleHash(ModuleData[I]->GetSimType());

			check(ModuleData[I]->SimArrayIndex + 1 >= 0);
			auto SimArrayIndexUnsigned = static_cast<uint32>(ModuleData[I]->SimArrayIndex + 1);
			// Convert to unsigned and align default -1 to 0. Done to be able to use SerializeIntPacked() for network optimization. 

			Ar << ModuleTypeHash;
			Ar.SerializeIntPacked(SimArrayIndexUnsigned);

			const bool bShouldSerializeData = ModulesBitArray.IsBitSet(I);
			if (bShouldSerializeData)
				ModuleData[I]->Serialize(Ar);
		}
	}

	bOutSuccess = true;
	return true;
}

bool FNetworkSingularisMorphVehicleStates::DeltaNetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	static FNetworkSingularisMorphVehicleStates LocalDefaultDeltaSource;
	const bool bPrintDebugInfo = SingularisMorphVehicleCVars::bEnableStateNetSerializeDebugPrinting;
	auto BitReader = static_cast<FBitReader*>(&Ar);
	auto BitWriter = static_cast<FBitWriter*>(&Ar);
	using namespace UE::Net;

	// If we should use delta source but the source is null, use a default source so that we at least consume the replicated bits correctly and doesn't cause a crash.
	if (!DeltaSourceData)
		DeltaSourceData = &LocalDefaultDeltaSource;

	SerializeFrames(Ar);

	auto DeltaSource = static_cast<FNetworkSingularisMorphVehicleStates*>(DeltaSourceData);
	UE_CLOGF(
		bPrintDebugInfo,
		LogSingularisMorphVehicleSim,
		Warning,
		"====DeltaNetSerialize Saving: %d. ServerFrame: %d. DeltaSource_ServerFrame: %d Starting Bit: %lld",
		Ar.IsSaving(),
		ServerFrame,
		DeltaSource->ServerFrame,
		Ar.IsSaving()?BitWriter->GetNumBits():BitReader->GetPosBits()
	);

	FNetworkSingularisMorphVehicleStateNetTokenData VehicleStateData;
	VehicleStateData.Init(ModuleData);
	const bool bNetTokenSuccess = TStructNetTokenDataStoreHelper<
		FNetworkSingularisMorphVehicleStateNetTokenData>::NetSerializeAndExportToken(Ar, Map, VehicleStateData);
	if (!bNetTokenSuccess)
	{
		bOutSuccess = false;
		return bOutSuccess;
	}

	auto GetDeltaDataHelper = [&](int32 InIdx, uint32 InModuleTypeHash, int32 InSimArrayIndex)
	{
		TSharedPtr<Chaos::FModuleNetData> DeltaData = nullptr;
		if (DeltaSource->ModuleData.IsValidIndex(InIdx))
		{
			const uint32 DeltaModuleHash = Chaos::FModuleFactoryRegister::GetModuleHash(
				DeltaSource->ModuleData[InIdx]->GetSimType()
			);
			if (DeltaModuleHash == InModuleTypeHash)
				DeltaData = DeltaSource->ModuleData[InIdx];
		}
		if (DeltaData == nullptr)
		{
			UE_CLOGF(
				bPrintDebugInfo,
				LogSingularisMorphVehicleSim,
				Warning,
				"==DeltaNetSerialize Generating Default Data for DeltaData Module %u",
				InModuleTypeHash
			);
			DeltaData = Chaos::FModuleFactoryRegister::Get().GenerateNetData(InModuleTypeHash, InSimArrayIndex);
			if (DeltaData == nullptr)
			{
				UE_LOGF(
					LogSingularisMorphVehicleSim,
					Error,
					"Unable to generate net data for delta source when delta is invalid"
				);
				bOutSuccess = false;
			}
			else
				DeltaData->ApplyDefaultState();
		}
		return DeltaData;
	};

	const uint32 NumNetModules = VehicleStateData.Hashes.Num();
	bOutSuccess = true;
	TMap<FName, uint32> SerializationStash;
	SerializationStash.Add(StashServerFrameKey, ServerFrame);
	if (Ar.IsLoading())
	{
		if (NumNetModules != ModuleData.Num())
			ModuleData.SetNum(NumNetModules);
		if (bPrintDebugInfo)
		{
			FString BitString = "";
			for (uint32 NetModuleIdx = 0; NetModuleIdx < NumNetModules; ++NetModuleIdx)
				BitString += VehicleStateData.ModuleShouldSerialize[NetModuleIdx] ? "1" : "0";
			UE_CLOGF(
				bPrintDebugInfo,
				LogSingularisMorphVehicleSim,
				Warning,
				"==DeltaNetSerialize LOADING. Using ModuleShouldSerialize: %ls",
				*BitString
			);
		}
		for (uint32 I = 0; I < NumNetModules; I++)
		{
			const int32 SimArrayIndex = VehicleStateData.Indexes[I];
			const uint32 ModuleTypeHash = VehicleStateData.Hashes[I];

			if (TSharedPtr<Chaos::FModuleNetData> Data = Chaos::FModuleFactoryRegister::Get().GenerateNetData(
				ModuleTypeHash,
				SimArrayIndex
			))
			{
				int32 StartBit = Ar.IsSaving() ? BitWriter->GetNumBits() : BitReader->GetPosBits();
				UE_CLOGF(
					bPrintDebugInfo,
					LogSingularisMorphVehicleSim,
					Warning,
					"==DeltaNetSerialize LOADING. ModuleData: %d STA. Bit: %lld",
					I,
					Ar.IsSaving()?BitWriter->GetNumBits():BitReader->GetPosBits()
				);
				ModuleData[I] = Data;
				check(ModuleTypeHash == Chaos::FModuleFactoryRegister::GetModuleHash(Data->GetSimType()));
				if (VehicleStateData.ModuleShouldSerialize[I])
				{
					TSharedPtr<Chaos::FModuleNetData> DeltaData = GetDeltaDataHelper(I, ModuleTypeHash, SimArrayIndex);
					ModuleData[I]->DeltaSerializeWithStash(Ar, DeltaData.Get(), SerializationStash);
				}
				else
					ModuleData[I]->ApplyDefaultState();
				int32 EndBit = Ar.IsSaving() ? BitWriter->GetNumBits() : BitReader->GetPosBits();
				UE_CLOGF(
					bPrintDebugInfo,
					LogSingularisMorphVehicleSim,
					Warning,
					"==DeltaNetSerialize LOADING. ModuleData: %d END. Bit: %lld Total: %d Error: %d",
					I,
					Ar.IsSaving()?BitWriter->GetNumBits():BitReader->GetPosBits(),
					EndBit-StartBit,
					Ar.IsError()
				);
			}
		}
	}
	else
	{
		if (bPrintDebugInfo)
		{
			FString BitString = "";
			for (uint32 I = 0; I < NumNetModules; I++)
				BitString += VehicleStateData.ModuleShouldSerialize[I] ? "1" : "0";
			UE_CLOGF(
				bPrintDebugInfo,
				LogSingularisMorphVehicleSim,
				Warning,
				"==DeltaNetSerialize SAVING. Using ModuleShouldSerialize: %ls",
				*BitString
			);
		}
		for (uint32 I = 0; I < NumNetModules; I++)
		{
			int32 StartBit = Ar.IsSaving() ? BitWriter->GetNumBits() : BitReader->GetPosBits();
			UE_CLOGF(
				bPrintDebugInfo,
				LogSingularisMorphVehicleSim,
				Warning,
				"==DeltaNetSerialize SAVING. ModuleData: %d STA. Bit: %lld - %ls",
				I,
				Ar.IsSaving()?BitWriter->GetNumBits():BitReader->GetPosBits(),
				*ModuleData[I]->GetSimType().ToString()
			);
			if (VehicleStateData.ModuleShouldSerialize[I])
			{
				const int32 SimArrayIndex = VehicleStateData.Indexes[I];
				const uint32 ModuleTypeHash = VehicleStateData.Hashes[I];
				TSharedPtr<Chaos::FModuleNetData> DeltaData = GetDeltaDataHelper(I, ModuleTypeHash, SimArrayIndex);
				ModuleData[I]->DeltaSerializeWithStash(Ar, DeltaData.Get(), SerializationStash);
			}
			int32 EndBit = Ar.IsSaving() ? BitWriter->GetNumBits() : BitReader->GetPosBits();
			UE_CLOGF(
				bPrintDebugInfo,
				LogSingularisMorphVehicleSim,
				Warning,
				"==DeltaNetSerialize SAVING. ModuleData: %d END. Bit: %lld Size: %d - %ls",
				I,
				Ar.IsSaving()?BitWriter->GetNumBits():BitReader->GetPosBits(),
				EndBit-StartBit,
				*ModuleData[I]->GetSimType().ToString()
			);
		}
	}

	return bOutSuccess;
}

void FNetworkSingularisMorphVehicleStates::InterpolateData(
	const FNetworkPhysicsData& MinData,
	const FNetworkPhysicsData& MaxData
)
{
	const auto& MinState = static_cast<const FNetworkSingularisMorphVehicleStates&>(MinData);
	const auto& MaxState = static_cast<const FNetworkSingularisMorphVehicleStates&>(MaxData);

	const float LerpFactor = (LocalFrame - MinState.LocalFrame) / (MaxState.LocalFrame - MinState.LocalFrame);

	if (ModuleData.Num() != MinState.ModuleData.Num() || ModuleData.Num() != MaxState.ModuleData.Num())
	{
		UE_LOGF(
			LogSingularisMorphVehicleSim,
			Error,
			"Mismatch in module data num when interpolating between min and max states!"
		);
		return;
	}
	for (auto I = 0; I < ModuleData.Num(); I++)
	{
		// if these don't match then something has gone terribly wrong
		check(ModuleData[I]->GetSimType() == MinState.ModuleData[I]->GetSimType());
		check(ModuleData[I]->GetSimType() == MaxState.ModuleData[I]->GetSimType());

		ModuleData[I]->Lerp(LerpFactor, *MinState.ModuleData[I].Get(), *MaxState.ModuleData[I].Get());
	}
}

/**
 * 载具模拟入口（物理线程）。
 *
 * 每个载具在每个物理子步中由 OnPreSimulate_Internal 通过 ParallelFor 并行调用。
 * 执行流程：
 * 1) 创建空的 FSingularisMorphVehicleAsyncOutput 作为输出容器。
 * 2) 若 Proxy 为空（载具尚未完全初始化），返回空输出。
 *    —— 这允许"宽填充"策略：预先为所有槽位分配输出，未就绪的载具产生空结果。
 * 3) 通过 VehicleSimulationPT->Simulate() 执行实际的模块模拟计算。
 * 4) 若非重模拟（Resimming）场景，调用 FillOutputState() 填充输出状态数据。
 *    —— 重模拟时跳过状态填充，避免覆盖回放的确定性数据。
 * 5) 标记输出有效（bValid = true），返回所有权给调用方。
 *
 * @param World       物理场景所属的 UWorld（仅用于场景查询，如射线检测）。
 * @param DeltaSeconds 本物理子步的时间步长。
 * @param TotalSeconds 模拟累计时间。
 * @param bWakeOut     输出参数：是否唤醒休眠的物理粒子。
 * @return 包含模拟结果的异步输出对象。
 */
TUniquePtr<FSingularisMorphVehicleAsyncOutput> FSingularisMorphVehicleAsyncInput::Simulate(
	UWorld* World,
	const float DeltaSeconds,
	const float TotalSeconds,
	bool& bWakeOut
) const
{
	QUICK_SCOPE_CYCLE_COUNTER(Stat_FSingularisMorphVehicleAsyncInput_Simulate);
	auto Output = MakeUnique<FSingularisMorphVehicleAsyncOutput>();

	//support nullptr because it allows us to go wide on filling the async inputs
	if (Proxy == nullptr)
		return Output;

	if (Vehicle)
	{
		if (FSingularisMorphVehicleSimulation* Sim = Vehicle->VehicleSimulationPT.Get())
		{
			// FILL OUTPUT DATA HERE THAT WILL GET PASSED BACK TO THE GAME THREAD
			Sim->Simulate(World, DeltaSeconds, *this, *Output.Get(), Proxy);

			Chaos::FPhysicsSolver* Solver = Proxy->GetSolver<Chaos::FPhysicsSolver>();
			if (Solver && Solver->GetEvolution())
			{
				if (!Solver->GetEvolution()->IsResimming())
				{
					FSingularisMorphVehicleAsyncOutput& OutputData = *Output.Get();
					Sim->FillOutputState(OutputData);
				}
			}
			else
			{
				FSingularisMorphVehicleAsyncOutput& OutputData = *Output.Get();
				Sim->FillOutputState(OutputData);
			}
		}
	}


	Output->bValid = true;

	return MoveTemp(Output);
}

/**
 * 碰撞修正回调（物理线程，每个子步）。
 *
 * 将碰撞修正委托给载具的 VehicleSimulationPT，允许模块化载具
 * 在碰撞解算阶段修改碰撞响应（如实现自定义悬挂碰撞行为）。
 */
void FSingularisMorphVehicleAsyncInput::OnContactModification(Chaos::FCollisionContactModifier& Modifications) const
{
	if (Vehicle && Vehicle->VehicleSimulationPT)
		Vehicle->VehicleSimulationPT->OnContactModification(Modifications, Proxy);
}

/**
 * 延迟力的施加（物理线程，在 Simulate 完成后串行调用）。
 *
 * 力的施加不能多线程执行（会破坏 Chaos 物理求解器的内部状态），
 * 因此在 OnPreSimulate_Internal 中先并行执行 Simulate（纯计算），
 * 再串行遍历调用此函数施加力。
 */
void FSingularisMorphVehicleAsyncInput::ApplyDeferredForces() const
{
	if (Vehicle && Proxy && Vehicle->VehicleSimulationPT)
		Vehicle->VehicleSimulationPT->ApplyDeferredForces(Proxy);
}

/**
 * 处理输入方向（物理线程，每个子步在 Simulate 之前调用）。
 *
 * 根据控制来源决定输入数据的流向：
 * - 本地控制 + 非重模拟：玩家本地输入 → VehicleSim（本地预测）。
 *   VehicleSim->VehicleInputs 被赋值为来自本地玩家的输入。
 * - 远程控制或重模拟：VehicleSim → NetworkInputs（网络同步源）。
 *   PhysicsInputs.NetworkInputs.VehicleInputs 被赋值为模拟的当前输入状态，
 *   供后续网络序列化发送给远程端。
 */
void FSingularisMorphVehicleAsyncInput::ProcessInputs()
{
	if (!GetVehicle())
		return;

	if (!GetVehicle()->VehicleSimulationPT)
		return;

	FSingularisMorphVehicleSimulation* VehicleSim = GetVehicle()->VehicleSimulationPT.Get();

	if (VehicleSim == nullptr || !GetVehicle()->bUsingNetworkPhysicsPrediction || GetVehicle()->GetWorld() == nullptr)
		return;
	auto bIsResimming = false;
	if (FPhysScene* PhysScene = GetVehicle()->GetWorld()->GetPhysicsScene())
	{
		if (Chaos::FPhysicsSolver* LocalSolver = PhysScene->GetSolver())
			bIsResimming = LocalSolver->GetEvolution()->IsResimming();
	}

	if (bIsLocallyControlled && !bIsResimming)
		VehicleSim->VehicleInputs = PhysicsInputs.NetworkInputs.VehicleInputs;
	else
		PhysicsInputs.NetworkInputs.VehicleInputs = VehicleSim->VehicleInputs;
}

/**
 * 输入预处理回调入口（物理线程，每个子步）。
 *
 * 由 TSimCallbackObject 框架在每个物理子步开始时自动调用。
 * 遍历所有载具的异步输入并调用 ProcessInputs() 处理输入方向。
 *
 * @param PhysicsStep 当前子步索引（在此回调中未使用）。
 */
void FSingularisMorphSimModuleManagerAsyncCallback::ProcessInputs_Internal(int32 PhysicsStep)
{
	const FSingularisMorphChaosSimModuleManagerAsyncInput* AsyncInput = GetConsumerInput_Internal();
	if (AsyncInput == nullptr)
		return;

	for (const TUniquePtr<FSingularisMorphVehicleAsyncInput>& VehicleInput : AsyncInput->VehicleInputs)
		VehicleInput->ProcessInputs();
}

/**
 * 预模拟回调（物理线程，每个子步调用一次）。
 *
 * 这是物理线程侧载具模拟的核心入口，执行流程分为三个阶段：
 *
 * 1) 前置校验：
 *    - 获取并校验消费端输入（GetConsumerInput_Internal）。
 *    - 校验 World 有效性与载具数量。
 *    - 校验 PhysicsSolver 存在。
 *
 * 2) 并行模拟（ParallelFor）：
 *    - 预分配输出槽位（AddDefaulted），复用时间戳。
 *    - 通过 PhysicsParallelFor 并行调用每个载具的 Simulate()。
 *    - 每个 Lambda 捕获 World/DeltaTime/SimTime 及输入输出批次引用。
 *    - 线程安全注意事项：代码必须保持无数据竞争——
 *      不同载具的 Simulate 操作各自独立的 Chaos 粒子，互不干扰。
 *
 * 3) 延迟力施加（串行循环）：
 *    - 力的施加（ApplyDeferredForces）不能多线程执行——
 *      Chaos 物理求解器内部的力缓冲不是线程安全的。
 *    - 在并行模拟全部完成后，串行遍历施加积累的力。
 *
 * 多线程控制：
 * - PhysicsParallelFor 的 ForceSingleThread 参数由 GSingularisMorphSimModuleDebugParams.EnableMultithreading 控制。
 * - 默认单线程执行，需通过调试工具显式开启多线程。
 */
void FSingularisMorphSimModuleManagerAsyncCallback::OnPreSimulate_Internal()
{
	using namespace Chaos;

	SCOPE_CYCLE_COUNTER(STAT_AsyncCallback_OnPreSimulate);

	float DeltaTime = GetDeltaTime_Internal();
	float SimTime = GetSimTime_Internal();

	const FSingularisMorphChaosSimModuleManagerAsyncInput* Input = GetConsumerInput_Internal();
	if (Input == nullptr)
		return;

	const int32 NumVehicles = Input->VehicleInputs.Num();

	UWorld* World = Input->World.Get(); //only safe to access for scene queries
	if (World == nullptr || NumVehicles == 0)
	{
		//world is gone so don't bother, or nothing to simulate.
		return;
	}

	auto PhysicsSolver = static_cast<FPhysicsSolver*>(GetSolver());
	if (PhysicsSolver == nullptr)
		return;

	FSingularisMorphChaosSimModuleManagerAsyncOutput& Output = GetProducerOutputData_Internal();
	Output.VehicleOutputs.AddDefaulted(NumVehicles);
	Output.Timestamp = Input->Timestamp;

	const TArray<TUniquePtr<FSingularisMorphVehicleAsyncInput>>& InputVehiclesBatch = Input->VehicleInputs;
	TArray<TUniquePtr<FSingularisMorphVehicleAsyncOutput>>& OutputVehiclesBatch = Output.VehicleOutputs;

	// beware running the vehicle simulation in parallel, code must remain threadsafe
	auto LambdaParallelUpdate = [World, DeltaTime, SimTime, &InputVehiclesBatch, &OutputVehiclesBatch](int32 Idx)
	{
		QUICK_SCOPE_CYCLE_COUNTER(Stat_LambdaParallelUpdate);
		const FSingularisMorphVehicleAsyncInput& VehicleInput = *InputVehiclesBatch[Idx];

		if (VehicleInput.Proxy == nullptr)
			return;

		auto bWake = false;
		OutputVehiclesBatch[Idx] = VehicleInput.Simulate(World, DeltaTime, SimTime, bWake);
		OutputVehiclesBatch[Idx]->Vehicle = VehicleInput.GetVehicle();
	};

	bool ForceSingleThread = !GSingularisMorphSimModuleDebugParams.EnableMultithreading;
	PhysicsParallelFor(OutputVehiclesBatch.Num(), LambdaParallelUpdate, ForceSingleThread);

	// Delayed application of forces - This is separate from Simulate because forces cannot be executed multi-threaded
	for (const TUniquePtr<FSingularisMorphVehicleAsyncInput>& VehicleInput : InputVehiclesBatch)
	{
		if (VehicleInput.IsValid())
			VehicleInput->ApplyDeferredForces();
	}
}

/**
 * 碰撞修正回调（物理线程，每个子步调用一次）。
 *
 * 在 Chaos 碰撞解算阶段被调用，允许载具修改碰撞响应。
 * 通过 PhysicsParallelFor 并行调用每个载具的 OnContactModification()。
 *
 * 前置校验逻辑与 OnPreSimulate_Internal 一致：
 * 校验输入、World、载具数量与 PhysicsSolver。
 *
 * 注意：此回调当前未在载具模拟中使用（OnContactModification 为空实现），
 * 保留作为扩展点供未来实现自定义碰撞行为。
 */
void FSingularisMorphSimModuleManagerAsyncCallback::OnContactModification_Internal(
	Chaos::FCollisionContactModifier& Modifications
)
{
	using namespace Chaos;

	SCOPE_CYCLE_COUNTER(STAT_AsyncCallback_OnContactModification);

	float DeltaTime = GetDeltaTime_Internal();
	float SimTime = GetSimTime_Internal();

	const FSingularisMorphChaosSimModuleManagerAsyncInput* Input = GetConsumerInput_Internal();
	if (Input == nullptr)
		return;

	const int32 NumVehicles = Input->VehicleInputs.Num();

	UWorld* World = Input->World.Get(); //only safe to access for scene queries
	if (World == nullptr || NumVehicles == 0)
	{
		//world is gone so don't bother.
		return;
	}

	auto PhysicsSolver = static_cast<FPhysicsSolver*>(GetSolver());
	if (PhysicsSolver == nullptr)
		return;

	const TArray<TUniquePtr<FSingularisMorphVehicleAsyncInput>>& InputVehiclesBatch = Input->VehicleInputs;

	// beware running the vehicle simulation in parallel, code must remain threadsafe
	auto LambdaParallelUpdate = [&Modifications, &InputVehiclesBatch](int32 Idx)
	{
		const FSingularisMorphVehicleAsyncInput& VehicleInput = *InputVehiclesBatch[Idx];

		if (VehicleInput.Proxy == nullptr)
			return;

		auto bWake = false;
		VehicleInput.OnContactModification(Modifications);
	};

	bool ForceSingleThread = !GSingularisMorphSimModuleDebugParams.EnableMultithreading;
	PhysicsParallelFor(InputVehiclesBatch.Num(), LambdaParallelUpdate, ForceSingleThread);
}
