#include "Types/SingularisMorphVehicleSocket.h"

#include "Components/SingularisMorphVehicleClusterUnionComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SingularisMorphVehicleSocket)

FSingularisMorphVehicleSocket::FSingularisMorphVehicleSocket()
{
	// 1) 初始化位置与旋转偏移为零
	RelativeLocation = FVector::ZeroVector;
	RelativeRotation = FRotator::ZeroRotator;
}

FVector FSingularisMorphVehicleSocket::GetLocation(
	const class USingularisMorphVehicleClusterUnionComponent* Component
) const
{
	// 1) 前置条件校验
	if (!ensure(Component)) return FVector(0.f);

	// 2) 计算世界空间下的位置
	const FTransform& LocalT = GetLocalTransform();
	FTransform ComponentT = Component->GetComponentTransform();
	return (LocalT * ComponentT).GetLocation();
}

FTransform FSingularisMorphVehicleSocket::GetLocalTransform() const
{
	// 1) 构建本地空间下的相对变换
	return FTransform(RelativeRotation, RelativeLocation);
}

FTransform FSingularisMorphVehicleSocket::GetTransform(
	const class USingularisMorphVehicleClusterUnionComponent* Component
) const
{
	// 1) 有效性检查
	if (!Component) return FTransform::Identity;

	// 2) 计算世界空间下的完整变换
	const FTransform& LocalT = GetLocalTransform();
	FTransform ComponentT = Component->GetComponentTransform();
	return LocalT * ComponentT;
}
