using UnrealBuildTool;

public class AssetQuickOpenEditor : ModuleRules
{
	public AssetQuickOpenEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
			"UnrealEd",
			"ToolMenus",
			"ContentBrowser",
			"ContentBrowserData",
			"AssetRegistry",
			"EditorSubsystem"
		});
	}
}
