#pragma once

#include <CoreMinimal.h>

#include "SingularisMorphVehicleSocket.generated.h"

class USingularisMorphVehicleClusterUnionComponent;

/**
 * 引力奇点变型载具 Socket。
 *
 * 定义模块化载具上命名连接点的位置、旋转与变换查询接口。
 * 用于其他模块化组件（如武器挂点、附件等）的相对挂载。
 */
USTRUCT(BlueprintType)
struct SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleSocket
{
	GENERATED_USTRUCT_BODY()

	FSingularisMorphVehicleSocket();

	/** Socket 唯一名称 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|Socket参数",
		meta = (DisplayName = "Socket名称")
	)
	FName SocketName;

	/** 相对位置 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|Socket参数",
		meta = (DisplayName = "相对位置")
	)
	FVector RelativeLocation = FVector::ZeroVector;

	/** 相对旋转 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|Socket参数",
		meta = (DisplayName = "相对旋转")
	)
	FRotator RelativeRotation = FRotator::ZeroRotator;

	/** 获取世界空间下的 Socket 位置 */
	FVector GetLocation(const USingularisMorphVehicleClusterUnionComponent* Component) const;

	/** 获取 Socket 本地变换 */
	FTransform GetLocalTransform() const;

	/** 获取 Socket 当前世界变换 */
	FTransform GetTransform(const USingularisMorphVehicleClusterUnionComponent* Component) const;
};
