// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Metagrain : ModuleRules
{
	public Metagrain(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "MetasoundEngine",
                "MetasoundGraphCore",
                "MetasoundGenerator",
                "MetasoundEngineTest",
                "MetasoundEditor",
                "MetasoundStandardNodes",
                "MetasoundFrontend",
                "MetasoundGenerator",
                "MetasoundEngineTest",
                "MetasoundEditor",
                "WaveTable",
                "AudioExtensions",
                "SignalProcessing",
                "MetasoundGraphCore"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "AudioExtensions",
                "MetasoundEditor",
                "MetasoundEngineTest",
                "MetasoundEngine",
                "MetasoundFrontend",
                "MetasoundGenerator",
                "MetasoundGraphCore",
                "MetasoundStandardNodes",
                "WaveTable",
                "SignalProcessing",
                "AudioExtensions"
            }
        );
    }
}
