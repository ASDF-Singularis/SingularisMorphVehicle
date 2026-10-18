using UnrealBuildTool;

public class SingularisMorphVehicle : ModuleRules
{
	public SingularisMorphVehicle(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			[
				"Core",
				"CoreUObject",
				"Engine",
				"NetCore",

				"UMG",
				"Slate",
				"SlateCore",

				"RenderCore",
				"Renderer",
				"RHI",

				"Chaos",
				"ChaosVehiclesCore",
				"ChaosVehiclesEngine",
				"ChaosSolverEngine",

				"GeometryCollectionEngine",

				"AnimGraphRuntime",

				"AudioExtensions",
				"MetasoundEngine",

				"InputCore",
				"EnhancedInput",

				"EngineSettings",
				"DeveloperSettings"
			]
		);

		SetupIrisSupport(Target);
		SetupModulePhysicsSupport(Target);
		PrivateDefinitions.Add("CHAOS_INCLUDE_LEVEL_1=1");
	}
}