#pragma once

#include <CoreMinimal.h>
#include <GeometryCollection/GeometryCollection.h>

namespace Chaos
{
	class FChaosArchive;
}

/**
 * 引力奇点变型模拟集合。
 *
 * FGeometryCollection 的子类，为变型载具的模块化模拟提供变换节点的索引映射。
 * SimModuleIndex 属性将每个变换节点关联至特定的模拟模块。
 */
class SINGULARISMORPHVEHICLE_API FSingularisMorphSimCollection : public FGeometryCollection
{
public:
	using Super = FGeometryCollection;

	FSingularisMorphSimCollection();
	FSingularisMorphSimCollection(FSingularisMorphSimCollection&) = delete;
	FSingularisMorphSimCollection& operator=(const FSingularisMorphSimCollection&) = delete;
	FSingularisMorphSimCollection(FSingularisMorphSimCollection&&) = default;
	FSingularisMorphSimCollection& operator=(FSingularisMorphSimCollection&&) = default;

	static FSingularisMorphSimCollection* NewModularSimulationCollection(const FTransformCollection& Base);
	static FSingularisMorphSimCollection* NewModularSimulationCollection();
	static void Init(FSingularisMorphSimCollection* Collection);

	/** 模拟模块索引属性名（TransformGroup） */
	static const FName SimModuleIndexAttribute;

	/** 每个变换节点关联的模拟模块索引数组 */
	TManagedArray<int32> SimModuleIndex;

	/** 生成模拟模块树 */
	void GenerateSimTree();

protected:
	void Construct();
};

inline Chaos::FChaosArchive& operator<<(Chaos::FChaosArchive& Ar, FSingularisMorphSimCollection& Value)
{
	Value.Serialize(Ar);
	return Ar;
}
