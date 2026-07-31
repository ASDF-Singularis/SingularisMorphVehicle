#pragma once

#include "Iris/ReplicationSystem/NetTokenStructDefines.h"
#include "Iris/ReplicationSystem/StructNetTokenDataStore.h"
#include "Net/Iris/ReplicationSystem/StructNetTokenDataStoreHelper.h"
#include "SimModule/ModuleInput.h"
#include "SingularisMorphModuleInputTokenStore.generated.h"

/**
 * 引力奇点变型模块输入网络令牌数据。
 *
 * 存储模块输入值的类型与衰减标志的列表，用于 Iris 复制系统的令牌存储优化。
 */
USTRUCT()
struct FSingularisMorphModuleInputNetTokenData
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<uint8> Types;

	UPROPERTY()
	TArray<bool> DecayValues;

	UE_NET_NETTOKEN_GENERATED_BODY(SingularisMorphModuleInputNetTokenData, SINGULARISMORPHVEHICLE_API)

	uint64 GetUniqueKey() const
	{
		uint64 HashOfTypes = GetTypeHash(Types);
		uint64 HashOfDecayValues = GetTypeHash(DecayValues);
		return HashOfTypes << 32 ^ HashOfDecayValues;
	}

	void Init(const TArray<FModuleInputValue>& ModuleInputs)
	{
		Types.Reset(ModuleInputs.Num());
		DecayValues.Reset(ModuleInputs.Num());
		for (auto Idx = 0; Idx < ModuleInputs.Num(); Idx++)
		{
			Types.Add(static_cast<uint8>(ModuleInputs[Idx].GetValueType()));
			DecayValues.Add(ModuleInputs[Idx].ShouldApplyInputDecay());
		}
	}
};

UE_NET_DECLARE_NAMED_NETTOKEN_STRUCT_SERIALIZERS(SingularisMorphModuleInputNetTokenData, SINGULARISMORPHVEHICLE_API);
