// Copyright Epic Games, Inc. All Rights Reserved.

#include "Metagrain.h" // This should match the header file in YourPlugin/Source/YourModule/Public/

#define LOCTEXT_NAMESPACE "FMetagrainModule"

void FMetagrainModule::StartupModule()
{
	// This code will execute after your module is loaded into memory;
	// the exact timing is specified in the .uplugin file.
	// METASOUND_REGISTER_NODE macros in your node .cpp files will typically handle registration.
	UE_LOG(LogTemp, Warning, TEXT("Metagrain module has started."));
}

void FMetagrainModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.
	// For modules that support dynamic reloading,
	// we call this function before unloading the module.
	UE_LOG(LogTemp, Warning, TEXT("Metagrain module has shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMetagrainModule, Metagrain) // This line is crucial and should only be here.
