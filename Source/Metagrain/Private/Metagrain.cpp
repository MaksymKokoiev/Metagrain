// Copyright Epic Games, Inc. All Rights Reserved.

#include "Metagrain.h"

#define LOCTEXT_NAMESPACE "FMetagrainModule"

void FMetagrainModule::StartupModule()
{

	UE_LOG(LogTemp, Warning, TEXT("Metagrain module has started."));
}

void FMetagrainModule::ShutdownModule()
{

	UE_LOG(LogTemp, Warning, TEXT("Metagrain module has shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMetagrainModule, Metagrain) 
