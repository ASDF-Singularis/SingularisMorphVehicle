#pragma once

#include <PhysicsEngine/ClusterUnionComponent.h>
#include <PhysicsProxy/ClusterUnionPhysicsProxy.h>

#include "Types/SingularisMorphVehicleSocket.h"
#include "SingularisMorphVehicleClusterUnionComponent.generated.h"

/**
 * 引力奇点变型载具集群联合组件
 *
 * 扩展引擎的 UClusterUnionComponent，为变型载具提供 Socket 支持与受保护的 API 公共访问入口，
 * 使得模块化模拟系统可查询各模块在集群内的相对位置。
 */
UCLASS(
	Blueprintable,
	BlueprintType,
	ClassGroup = ("Singularis"),
	meta = (BlueprintSpawnableComponent, DisplayName = "引力奇点变型载具集群联合组件")
)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleClusterUnionComponent : public UClusterUnionComponent
{
	GENERATED_BODY()

public:
#pragma region Parameter

	/** 变型载具 Socket 配置列表，定义模块化连接点位置与方向 */
	UPROPERTY(
		EditDefaultsOnly,
		BlueprintReadOnly,
		Category = "SingularisMorphVehicle|引力奇点变型载具集群联合组件|参数",
		meta = (DisplayName = "Sockets")
	)
	TArray<FSingularisMorphVehicleSocket> Sockets{};

#pragma endregion

#pragma region Constructors

	USingularisMorphVehicleClusterUnionComponent(const FObjectInitializer& ObjectInitializer);

#pragma endregion

#pragma region SceneComponent Interface

	virtual bool HasAnySockets() const override;
	virtual bool DoesSocketExist(FName InSocketName) const override;
	virtual FTransform GetSocketTransform(
		FName InSocketName,
		ERelativeTransformSpace TransformSpace = RTS_World
	) const override;
	virtual void QuerySupportedSockets(TArray<FComponentSocketDescription>& OutSockets) const override;

#pragma endregion

#pragma region API

	/**
	 * 获取物理代理的公共访问入口。
	 * UE 5.8 将 GetPhysicsProxy 改为 protected，此处提供公共桥梁。
	 */
	Chaos::FClusterUnionPhysicsProxy* GetPhysicsProxyPublic() { return GetPhysicsProxy(); }
	const Chaos::FClusterUnionPhysicsProxy* GetPhysicsProxyPublic() const { return GetPhysicsProxy(); }

	/**
	 * 获取 IsAuthority 的公共访问入口。
	 * UE 5.8 将 IsAuthority 改为 protected，此处提供公共桥梁。
	 */
	bool IsAuthorityPublic() const { return IsAuthority(); }

#pragma endregion
};
