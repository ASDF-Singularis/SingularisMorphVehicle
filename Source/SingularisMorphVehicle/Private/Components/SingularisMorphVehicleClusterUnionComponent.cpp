#include "Components/SingularisMorphVehicleClusterUnionComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleClusterUnionComponent)

USingularisMorphVehicleClusterUnionComponent::USingularisMorphVehicleClusterUnionComponent(
	const FObjectInitializer& ObjectInitializer
)
	: Super(ObjectInitializer)
{
	SetIsReplicatedByDefault(false);

	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	bAutoActivate = true;
}

bool USingularisMorphVehicleClusterUnionComponent::HasAnySockets() const
{
	if (!Sockets.IsEmpty())
		return true;

	//for (const TPair<TObjectKey<UPrimitiveComponent>, FClusteredComponentData>& Kvp : PerComponentData)
	//{
	//	if (UPrimitiveComponent* Component = Kvp.Key.ResolveObjectPtr())
	//	{
	//		if (Component->HasAnySockets())
	//		{
	//			return true;
	//		}
	//	}
	//}

	return Super::HasAnySockets();
}

bool USingularisMorphVehicleClusterUnionComponent::DoesSocketExist(const FName InSocketName) const
{
	for (const FSingularisMorphVehicleSocket& Socket : Sockets)
	{
		if (Socket.SocketName == InSocketName)
			return true;
	}

	//for (const TPair<TObjectKey<UPrimitiveComponent>, FClusteredComponentData>& Kvp : PerComponentData)
	//{
	//	if (UPrimitiveComponent* Component = Kvp.Key.ResolveObjectPtr())
	//	{
	//		if (Component->DoesSocketExist(InSocketName))
	//		{
	//			return true;
	//		}
	//	}
	//}

	return Super::DoesSocketExist(InSocketName);
}

FTransform USingularisMorphVehicleClusterUnionComponent::GetSocketTransform(
	const FName InSocketName,
	const ERelativeTransformSpace TransformSpace
) const
{
	for (const FSingularisMorphVehicleSocket& Socket : Sockets)
	{
		if (Socket.SocketName == InSocketName)
		{
			FTransform SocketComponentSpaceTransform = Socket.GetLocalTransform();

			switch (TransformSpace)
			{
			case RTS_World:
				return SocketComponentSpaceTransform * GetComponentTransform();

			case RTS_Actor:
				{
					if (const AActor* Actor = GetOwner())
					{
						const FTransform SocketWorldSpaceTransform = SocketComponentSpaceTransform *
							GetComponentTransform();
						return SocketWorldSpaceTransform.GetRelativeTransform(Actor->GetTransform());
					}
					break;
				}

			case RTS_Component:
				{
					return SocketComponentSpaceTransform;
				}

			case RTS_ParentBoneSpace:
			default:
				{
					check(false);
				}
			}
		}
	}

	//for (const TPair<TObjectKey<UPrimitiveComponent>, FClusteredComponentData>& Kvp : PerComponentData)
	//{
	//	if (UPrimitiveComponent* Component = Kvp.Key.ResolveObjectPtr())
	//	{
	//		if (Component->DoesSocketExist(InSocketName))
	//		{
	//			return Component->GetSocketTransform(InSocketName, TransformSpace);
	//		}
	//	}
	//}

	return Super::GetSocketTransform(InSocketName, TransformSpace);
}

void USingularisMorphVehicleClusterUnionComponent::QuerySupportedSockets(
	TArray<FComponentSocketDescription>& OutSockets
) const
{
	for (const FSingularisMorphVehicleSocket& Socket : Sockets)
	{
		FComponentSocketDescription& Desc = OutSockets.AddZeroed_GetRef();
		Desc.Name = Socket.SocketName;
		Desc.Type = EComponentSocketType::Type::Socket;
	}


	//for (const TPair<TObjectKey<UPrimitiveComponent>, FClusteredComponentData>& Kvp : PerComponentData)
	//{
	//	if (UPrimitiveComponent* Component = Kvp.Key.ResolveObjectPtr())
	//	{
	//		Component->QuerySupportedSockets(OutSockets);
	//	}
	//}
}
