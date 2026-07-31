#pragma once

#include <CoreMinimal.h>

#include "Core/SingularisMorphSimCollection.h"
#include "SingularisMorphVehicleAsset.generated.h"

/**
 * 引力奇点变型载具资产编辑作用域。
 * 在构造时锁定资产以进行结构化编辑，析构时自动序列化回动态集合。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphVehicleAssetEdit
{
public:
	explicit FSingularisMorphVehicleAssetEdit(USingularisMorphVehicleAsset* InAsset);
	~FSingularisMorphVehicleAssetEdit();

	USingularisMorphVehicleAsset* GetAsset();

private:
	USingularisMorphVehicleAsset* Asset;
};

/**
 * 引力奇点变型载具资产。
 *
 * FSingularisMorphSimCollection 的 UObject 包装器，
 * 提供序列化与缩略图信息支持，用于将模块化模拟数据保存为独立资产。
 */
UCLASS(customconstructor)
class SINGULARISMORPHVEHICLE_API USingularisMorphVehicleAsset : public UObject
{
	GENERATED_UCLASS_BODY()
	friend class FSingularisMorphVehicleAssetEdit;

	USingularisMorphVehicleAsset(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 获取可编辑的资产作用域 */
	FSingularisMorphVehicleAssetEdit EditRestCollection() { return FSingularisMorphVehicleAssetEdit(this); }

	virtual void Serialize(FArchive& Ar) override;

#if WITH_EDITORONLY_DATA
	/** 缩略图渲染信息 */
	UPROPERTY(VisibleAnywhere, Instanced, AdvancedDisplay, Category = "SingularisMorphVehicle|变型载具资产")
	TObjectPtr<class UThumbnailInfo> ThumbnailInfo;
#endif

private:
	TSharedPtr<FSingularisMorphSimCollection, ESPMode::ThreadSafe> ModularSimCollection;
};
