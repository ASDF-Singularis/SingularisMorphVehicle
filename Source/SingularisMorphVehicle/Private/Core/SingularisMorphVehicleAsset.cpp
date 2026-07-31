#include "Core/SingularisMorphVehicleAsset.h"

#include "Core/SingularisMorphSimCollection.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleAsset)

DEFINE_LOG_CATEGORY_STATIC(LogSingularisMorphVehicleAssetInternal, Log, All);

FSingularisMorphVehicleAssetEdit::FSingularisMorphVehicleAssetEdit(USingularisMorphVehicleAsset* InAsset)
	: Asset(InAsset) {}

FSingularisMorphVehicleAssetEdit::~FSingularisMorphVehicleAssetEdit() {}

USingularisMorphVehicleAsset* FSingularisMorphVehicleAssetEdit::GetAsset()
{
	if (Asset)
		return Asset;
	return nullptr;
}

USingularisMorphVehicleAsset::USingularisMorphVehicleAsset(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer), ModularSimCollection(new FSingularisMorphSimCollection()) {}


/** Serialize */
void USingularisMorphVehicleAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	auto bCreateSimulationData = false;
	Chaos::FChaosArchive ChaosAr(Ar);
	ModularSimCollection->Serialize(ChaosAr);
}
