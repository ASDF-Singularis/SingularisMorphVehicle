#pragma once

#include <PhysicsInterfaceDeclaresCore.h>
#include <Modules/ModuleManager.h>

DECLARE_LOG_CATEGORY_EXTERN(LogSingularisMorphVehicle, Log, All);

class FSingularisMorphVehicleModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void PhysSceneInit(FPhysScene* PhysScene);
	void PhysSceneTerm(FPhysScene* PhysScene);

	FDelegateHandle OnPhysSceneInitHandle;
	FDelegateHandle OnPhysSceneTermHandle;
};
