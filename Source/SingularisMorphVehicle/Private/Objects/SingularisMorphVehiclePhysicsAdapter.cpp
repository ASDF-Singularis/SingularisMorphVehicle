#include "Objects/SingularisMorphVehiclePhysicsAdapter.h"

UWorld* USingularisMorphVehiclePhysicsAdapter::GetWorld() const
{
	if (HasAnyFlags(RF_ClassDefaultObject)) return nullptr;
	if (const UObject* Outer = GetOuter()) return Outer->GetWorld();
	return Super::GetWorld();
}

void USingularisMorphVehiclePhysicsAdapter::Initialize(const FSingularisMorphVehiclePhysicsAdapterContext& Context) {}

void USingularisMorphVehiclePhysicsAdapter::Terminate() {}

bool USingularisMorphVehiclePhysicsAdapter::IsReady() const
{
	return false;
}

bool USingularisMorphVehiclePhysicsAdapter::IsDirty() const
{
	return false;
}

FString USingularisMorphVehiclePhysicsAdapter::GetAdapterName() const
{
	return TEXT("PhysicsAdapter");
}

IPhysicsProxyBase* USingularisMorphVehiclePhysicsAdapter::GetPhysicsProxy() const
{
	return nullptr;
}

FTransform USingularisMorphVehiclePhysicsAdapter::GetReferenceTransform() const
{
	return FTransform::Identity;
}

FSingularisMorphVehiclePhysicsAdapterSnapshot USingularisMorphVehiclePhysicsAdapter::ConsumeSnapshot()
{
	return {};
}
