#include "Core/SingularisMorphVehicleSimCollection.h"

DEFINE_LOG_CATEGORY_STATIC(FSingularisMorphSimCollectionLogging, Log, All);


FSingularisMorphVehicleSimCollection::FSingularisMorphVehicleSimCollection()
	: FGeometryCollection()
{
	Construct();
}

FSingularisMorphVehicleSimCollection* FSingularisMorphVehicleSimCollection::NewModularSimulationCollection(
	const FTransformCollection& Base
)
{
	auto Collection = new FSingularisMorphVehicleSimCollection();
	Collection->CopyMatchingAttributesFrom(Base);
	return Collection;
}


FSingularisMorphVehicleSimCollection* FSingularisMorphVehicleSimCollection::NewModularSimulationCollection()
{
	auto Collection = new FSingularisMorphVehicleSimCollection();
	Init(Collection);
	return Collection;
}


void FSingularisMorphVehicleSimCollection::Init(FSingularisMorphVehicleSimCollection* Collection)
{
	if (Collection) {}
}

// Attributes
const FName FSingularisMorphVehicleSimCollection::SimModuleIndexAttribute("SimModuleIndex");

void FSingularisMorphVehicleSimCollection::GenerateSimTree() {}

void FSingularisMorphVehicleSimCollection::Construct() {}
