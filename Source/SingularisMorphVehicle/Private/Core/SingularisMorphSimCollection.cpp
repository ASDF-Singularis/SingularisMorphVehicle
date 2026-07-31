#include "Core/SingularisMorphSimCollection.h"

DEFINE_LOG_CATEGORY_STATIC(FSingularisMorphSimCollectionLogging, Log, All);


FSingularisMorphSimCollection::FSingularisMorphSimCollection()
	: FGeometryCollection()
{
	Construct();
}

FSingularisMorphSimCollection* FSingularisMorphSimCollection::NewModularSimulationCollection(
	const FTransformCollection& Base
)
{
	auto Collection = new FSingularisMorphSimCollection();
	Collection->CopyMatchingAttributesFrom(Base);
	return Collection;
}


FSingularisMorphSimCollection* FSingularisMorphSimCollection::NewModularSimulationCollection()
{
	auto Collection = new FSingularisMorphSimCollection();
	Init(Collection);
	return Collection;
}


void FSingularisMorphSimCollection::Init(FSingularisMorphSimCollection* Collection)
{
	if (Collection) {}
}

// Attributes
const FName FSingularisMorphSimCollection::SimModuleIndexAttribute("SimModuleIndex");

void FSingularisMorphSimCollection::GenerateSimTree() {}

void FSingularisMorphSimCollection::Construct() {}
