#pragma once

#include <Engine/EngineTypes.h>

#include "Objects/SingularisMorphVehiclePhysicsAdapter.h"
#include "SingularisMorphVehicleStaticMeshAdapter.generated.h"

class AActor;
class UPrimitiveComponent;

/**
 * 引力奇点变型载具静态网格体物理适配器。
 *
 * 面向单刚体（Single Rigid Body）物理后端：载具由多个静态网格体组成，
 * 根部的「本体（Body）」组件承载唯一物理体，其下挂载轮胎等模块网格体。
 * 模块网格体随本体运动、不具独立物理体，车轮与动力系统为模拟内部的一维积分子系统。
 *
 * 快照实体（本体在首位，其后为各后代静态网格体组件）：
 * - 本体组件对应唯一物理粒子，作为树根模块（底盘）的驱动组件；
 * - 模块网格体组件绑定同一粒子，并携带相对本体的位姿，作为模块的静止基准。
 *
 * 与集群联合适配器的差异：
 * - 全部实体共用唯一本体粒子，模块粒子索引取无效值——单粒子施加路径不读取该索引，
 *   与参考实现的非集群路径一致；
 * - 场景组件的挂载/卸载没有可订阅的引擎事件，故以装配签名比对检测运行时增删，
 *   由 IsDirty() 暴露。
 *
 * 适配器本身不调用 SimulationComponent 任何方法——仅被动响应 SPI 查询。
 */
UCLASS(NotBlueprintable, BlueprintType)
class SINGULARISMORPHVEHICLE_API
	USingularisMorphVehicleStaticMeshAdapter : public USingularisMorphVehiclePhysicsAdapter
{
	GENERATED_BODY()

public:
#pragma region Parameter

	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "SingularisMorphVehicle|引力奇点变型载具静态网格体适配器|参数",
		meta = (DisplayName = "载具本体组件", UseComponentPicker, AllowedClasses =
			"/Script/Engine.PrimitiveComponent")
	)
	FComponentReference BodyComponentReference{};

#pragma endregion

private:
#pragma region Internal Variable

	/** 承载唯一物理体的本体组件（弱引用）；引用留空时回退到 Owner 根组件 */
	TWeakObjectPtr<UPrimitiveComponent> BodyComponent = nullptr;

	/**
	 * 装配变更脏标记（仅游戏线程访问）。
	 * Initialize 置位，装配签名比对检出变更时置位；IsDirty() 暴露，ConsumeSnapshot() 消费后清零。
	 */
	mutable bool bDirty = false;

	/** 装配签名基线：上次产出快照时存在的本体与模块组件集合（仅游戏线程访问） */
	mutable TArray<TWeakObjectPtr<UPrimitiveComponent>> CachedAssembly{};

#pragma endregion

public:
#pragma region SPI

	virtual void Initialize(const FSingularisMorphVehiclePhysicsAdapterContext& Context) override;
	virtual void Terminate() override;

	virtual bool IsReady() const override;
	virtual bool IsDirty() const override;
	virtual FString GetAdapterName() const override;
	virtual IPhysicsProxyBase* GetPhysicsProxy() const override;
	virtual FTransform GetReferenceTransform() const override;
	virtual FSingularisMorphVehiclePhysicsAdapterSnapshot ConsumeSnapshot() override;

#pragma endregion

private:
#pragma region Internal Function

	void ResolveBodyComponent(AActor* Owner);

	/** 收集本体与其后代静态网格体组件（本体在首位） */
	void GatherAssembly(TArray<TWeakObjectPtr<UPrimitiveComponent>>& OutAssembly) const;

#pragma endregion
};
