#pragma once

#include <CoreMinimal.h>
#include <SimModule/ModuleInput.h>
#include <UObject/Interface.h>

#include "Types/SingularisMorphVehicleType.h"
#include "SingularisMorphVehicleBaseInterface.generated.h"

namespace Chaos
{
	class ISimulationModuleBase;
	struct FSimOutputData;
}

UINTERFACE(Blueprintable, BlueprintType)
class USingularisMorphVehicleBaseInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * 引力奇点变形载具基础接口
 */
class SINGULARISMORPHVEHICLE_API ISingularisMorphVehicleBaseInterface
{
	GENERATED_BODY()

public:
	virtual ESingularisMorphVehicleModuleType GetModuleType() const
	{
		return ESingularisMorphVehicleModuleType::Undefined;
	}

	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const { return nullptr; }

	virtual FName GetBoneName() const { return NAME_None; }

	virtual bool GetAnimationEnabled() const { return false; }
	virtual void SetAnimationEnabled(bool AnimationEnabledIn) {}
	virtual int32 GetAnimationSetupIndex() const { return INDEX_NONE; }
	virtual const FVector& GetAnimationOffset() const { return FVector::ZeroVector; }

	virtual TArray<FModuleInputSetup> GetInputConfig() const { return TArray<FModuleInputSetup>(); }

	virtual int32 GetTreeIndex() const { return INDEX_NONE; }
	virtual void SetTreeIndex(const int32 NewValue) {}

	virtual int32 GetModuleGuid() const { return INDEX_NONE; }
	virtual void SetModuleGuid(const int32 NewValue) {}

	virtual void OnAdded() {}
	virtual void OnRemoved() {}

	virtual void OnOutputReady(const Chaos::FSimOutputData* OutputData) {}
};
