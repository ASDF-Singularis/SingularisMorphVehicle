#pragma once

#include "CollisionQueryParams.h"
#include "SingularisMorphSimModuleManagerAsyncCallback.h"

class USingularisMorphVehicleSimulationComponent;

/**
 * 引力奇点变型载具默认异步输入。
 *
 * 扩展 FSingularisMorphVehicleAsyncInput，提供碰撞追踪参数与默认的模拟/延迟力实现。
 */
struct FSingularisMorphVehicleDefaultAsyncInput : FSingularisMorphVehicleAsyncInput
{
	mutable FCollisionQueryParams TraceParams;
	mutable FCollisionResponseContainer TraceCollisionResponse;

	FSingularisMorphVehicleDefaultAsyncInput();

	virtual TUniquePtr<FSingularisMorphVehicleAsyncOutput> Simulate(
		UWorld* World,
		float DeltaSeconds,
		float TotalSeconds,
		bool& bWakeOut
	) const override;
	virtual void ApplyDeferredForces() const override;

	void SetVehicle(USingularisMorphVehicleSimulationComponent* VehicleIn) { GCVehicle = VehicleIn; }
	USingularisMorphVehicleSimulationComponent* GetVehicle() { return GCVehicle; }

private:
	USingularisMorphVehicleSimulationComponent* GCVehicle = nullptr;
};
