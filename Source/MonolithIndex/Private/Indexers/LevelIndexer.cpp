#include "Indexers/LevelIndexer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "MonolithMemoryHelper.h"
#include "MonolithSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Components/ActorComponent.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WorldPartition/WorldPartition.h"
#include "Subsystems/WorldSubsystem.h"

bool FLevelIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Find all World assets under indexed paths
	TArray<FAssetData> WorldAssets;
	FARFilter Filter;
	if (IndexedPaths.Num() > 0)
	{
		for (const FName& Path : IndexedPaths)
		{
			Filter.PackagePaths.Add(Path);
		}
	}
	else
	{
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
	}
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, WorldAssets);

	// Get settings for batching
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const int32 BatchSize = FMath::Max(1, FMonolithMemoryHelper::GetResolvedPostPassBatchSize());
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
	const bool bLogMemory = Settings->bLogMemoryStats;

	UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: Found %d World assets to index (batch size: %d)"),
		WorldAssets.Num(), BatchSize);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer start"));
	}

	int32 ActorsInserted = 0;
	int32 LevelsProcessed = 0;
	int32 BatchNumber = 0;

	for (int32 i = 0; i < WorldAssets.Num(); i += BatchSize)
	{
		// Compiler-idle gate is enforced by FMonolithCompilerSafeDispatch at the call site (see issue #19).

		// Memory budget check before each batch
		if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: Memory budget exceeded, forcing GC..."));
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FMonolithMemoryHelper::YieldToEditor();

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer after throttle GC"));
			}
		}

		int32 BatchEnd = FMath::Min(i + BatchSize, WorldAssets.Num());

		// Process batch
		for (int32 j = i; j < BatchEnd; ++j)
		{
			const FAssetData& WorldData = WorldAssets[j];

			int64 LevelAssetId = DB.GetAssetId(WorldData.PackageName.ToString());
			if (LevelAssetId < 0) continue;

			// Capture residency BEFORE LoadPackage (issue #81): a world already resident
			// (e.g. an open level) must keep RF_Standalone; only strip what this pass loads.
			const bool bWasLoaded = FindPackage(nullptr, *WorldData.PackageName.ToString()) != nullptr;

			// Load the package to access level data without initializing gameplay.
			// IMPORTANT: LoadPackage() for a UWorld asset broadcasts FCoreUObjectDelegates::OnAssetLoaded,
			// which UEditorEngine::OnAssetLoaded (EditorEngine.cpp:6400) handles for World assets by
			// calling InitializeNewlyCreatedInactiveWorld() -> full UWorld::UpdateWorldComponents() /
			// ULevel::UpdateLevelComponents() component registration - including registering Skeletal
			// Mesh components, which initializes and ticks their anim instances. Observed crash: a level
			// containing an actor whose anim instance faults in UAnimInstance::UpdateAnimation()
			// (AnimInstance.cpp:828) during this editor-side auto-init, reached from right here via
			// LoadPackage -> LoadPackageInternal -> FlushAsyncLoading -> OnAssetLoaded. LOAD_NoWarn |
			// LOAD_Quiet do NOT suppress this broadcast, so it fires regardless and directly contradicts
			// the "without initializing gameplay" intent above. Temporarily clearing the delegate for the
			// duration of this one load restores that intent: metadata-only load, no component
			// registration, no anim ticking, no crash risk from whatever a given level's actors do when
			// fully registered outside of PIE/gameplay context.
			auto SavedOnAssetLoaded = FCoreUObjectDelegates::OnAssetLoaded;
			FCoreUObjectDelegates::OnAssetLoaded.Clear();
			UPackage* Package = LoadPackage(nullptr, *WorldData.PackageName.ToString(), LOAD_NoWarn | LOAD_Quiet);
			FCoreUObjectDelegates::OnAssetLoaded = SavedOnAssetLoaded;
			if (!Package) continue;

			UWorld* World = FindObject<UWorld>(Package, *WorldData.AssetName.ToString());
			if (!World)
			{
				// Try the common naming convention
				World = FindObject<UWorld>(Package, TEXT("World"));
			}
			if (!World || !World->PersistentLevel)
			{
				// Mark package for unload even if we couldn't find the world
				FMonolithMemoryHelper::TryUnloadPackage(Package, bWasLoaded);
				continue;
			}

			// Only index the persistent level - skip streaming sub-levels for performance
			ULevel* Level = World->PersistentLevel;
			ActorsInserted += IndexActorsInLevel(Level, DB, LevelAssetId);

			// Detect whether this world has a landscape subsystem. LoadPackage never calls UWorld::InitWorld, but a
			// landscape actor's PostRegisterAllComponents lazily creates + Initialize()s a ULandscapeSubsystem on
			// this never-InitWorld'd world. If we then tear the world down (TryUnloadPackage + GC), the GC destroys
			// the world while ULandscapeSubsystem is still bInitialized -> handled-but-noisy ensure at
			// WorldSubsystem.cpp:158 (issue #67). The obvious "fix" (UWorld::CleanupWorld) is FATAL here: its
			// SubsystemCollection.Deinitialize() drives ULandscapeSubsystem::Deinitialize() which frees
			// Nanite/grass/streaming builders that assume a fully InitWorld'd render scene -> access violation
			// (the rejected approach, see issue #67 plan). So for landscape worlds we instead SKIP teardown and
			// leave the world resident (RF_Standalone intact) so it is never GC'd during this index run - no
			// Deinitialize crash, no GC-time ensure. Cost: this world stays in memory for the session.
			//
			// We detect the ULandscapeSubsystem itself rather than scanning for an ALandscapeProxy actor, because the
			// subsystem is the thing that ensures at GC, and in World Partition / streaming worlds the landscape actor
			// can live in a streaming sublevel or as an external WP actor that is NOT present in PersistentLevel->Actors
			// (issue #67 refinement: LVL_NiagaraDestructionDriver_Demo_Cube still tripped the ensure with the actor
			// scan because its landscape lives outside the persistent level, but its ULandscapeSubsystem was
			// initialized). Resolve the subsystem class by script path so we avoid a Landscape Build.cs dependency
			// (which would force a full rebuild + hard link); if the class can't be resolved (Landscape module not
			// loaded) there can be no landscape subsystem, so we safely fall back to the original teardown.
			// GetSubsystemBase(TSubclassOf<UWorldSubsystem>) performs the lookup against the world's subsystem
			// collection without a compile-time type dependency and returns non-null iff the subsystem instance exists.
			bool bContainsLandscape = false;
			{
				static UClass* LandscapeSubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/Landscape.LandscapeSubsystem"));
				if (LandscapeSubsystemClass)
				{
					if (World->GetSubsystemBase(LandscapeSubsystemClass) != nullptr)
					{
						bContainsLandscape = true;
					}
				}
			}

			// Skip teardown if this is the world currently open in the editor - uninit would stop viewport WP cell streaming and unload would close the level
			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World != EditorWorld)
			{
				if (bContainsLandscape)
				{
					// Keep the landscape world resident for the rest of the session - NEVER tear it down here, whether
					// or not this pass freshly loaded it: no proxy UnregisterAllComponents, no CleanupWorld, no
					// WorldPartition uninit, no TryUnloadPackage. (Upstream's issue #67 optimization unregisters the
					// landscape proxies then drives CleanupWorld on freshly loaded landscape worlds; on Macabre content
					// that teardown crashed in a world subsystem's Deinitialize, the residual risk upstream's own comment
					// documents, so this fork keeps the earlier resident-skip behavior.) Leaving RF_Standalone intact
					// keeps the world referenced so GC never destroys it while ULandscapeSubsystem is bInitialized (no
					// ensure) and we never deinitialize the half-initialized landscape subsystem (no fatal crash). Cost:
					// this world stays in memory for the session. The number of landscape levels is finite, so this is
					// a bounded, acceptable cost. Issue #81 (never strip RF_Standalone from / deinitialize a world that
					// was already resident) is satisfied trivially: this branch never touches a resident world at all,
					// so bWasLoaded needs no check here (it still gates TryUnloadPackage in the other branches).
					//
					// Note (rebased onto v0.23.0, 2026-10-14): with the OnAssetLoaded suppression around LoadPackage above, a
					// freshly loaded world is never InitWorld'd and its components are never registered, so it normally has no
					// ULandscapeSubsystem and takes the else branch (TryUnloadPackage + GC) like any other level. In practice
					// this branch is reached by worlds that were already resident and initialized elsewhere, and it remains
					// the safety net should that suppression ever be removed. The memory cost above applies only then.
					UE_LOG(LogMonolithIndex, Verbose,
						TEXT("LevelIndexer: '%s' has a landscape subsystem - keeping the world resident (skipping teardown) to avoid the ULandscapeSubsystem teardown ensure/crash (issue #67)."),
						*WorldData.PackageName.ToString());
				}
				else
				{
					// Uninitialize WorldPartition before unload - LoadPackage skips the editor teardown path, so GC would otherwise assert in UWorldPartitionSubsystem::Deinitialize
					if (UWorldPartition* WP = World->GetWorldPartition())
					{
						if (WP->IsInitialized())
						{
							WP->Uninitialize();
						}
					}

					// Mark world/package for unloading after indexing
					FMonolithMemoryHelper::TryUnloadPackage(World, bWasLoaded);
				}
			}

			LevelsProcessed++;
		}

		BatchNumber++;

		// GC after each batch to prevent memory accumulation
		FMonolithMemoryHelper::ForceGarbageCollection(false);
		FMonolithMemoryHelper::YieldToEditor();

		// Log progress periodically
		if (BatchNumber % 5 == 0 || BatchEnd == WorldAssets.Num())
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: processed %d / %d levels"),
				LevelsProcessed, WorldAssets.Num());

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("LevelIndexer batch %d"), BatchNumber));
			}
		}
	}

	// Final GC
	FMonolithMemoryHelper::ForceGarbageCollection(true);

	UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: indexed %d levels, %d actors total"),
		LevelsProcessed, ActorsInserted);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer complete"));
	}

	return true;
}

int32 FLevelIndexer::IndexActorsInLevel(ULevel* Level, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!Level) return 0;

	int32 Count = 0;
	for (AActor* Actor : Level->Actors)
	{
		if (!Actor) continue;

		// Skip the world settings and default brush - they're internal
		if (Actor->IsA(AWorldSettings::StaticClass())) continue;

		FIndexedActor IndexedActor;
		IndexedActor.AssetId = AssetId;
		IndexedActor.ActorName = Actor->GetName();
		IndexedActor.ActorClass = Actor->GetClass()->GetName();
		IndexedActor.ActorLabel = Actor->GetActorLabel();
		IndexedActor.Transform = SerializeTransform(Actor->GetActorTransform());
		IndexedActor.Components = SerializeComponents(Actor);

		DB.InsertActor(IndexedActor);
		Count++;
	}
	return Count;
}

FString FLevelIndexer::SerializeTransform(const FTransform& Transform)
{
	auto Obj = MakeShared<FJsonObject>();

	const FVector& Loc = Transform.GetLocation();
	auto LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Obj->SetObjectField(TEXT("location"), LocObj);

	const FRotator Rot = Transform.GetRotation().Rotator();
	auto RotObj = MakeShared<FJsonObject>();
	RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
	Obj->SetObjectField(TEXT("rotation"), RotObj);

	const FVector& Scale = Transform.GetScale3D();
	auto ScaleObj = MakeShared<FJsonObject>();
	ScaleObj->SetNumberField(TEXT("x"), Scale.X);
	ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
	ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
	Obj->SetObjectField(TEXT("scale"), ScaleObj);

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(Obj, *Writer, true);
	return Result;
}

FString FLevelIndexer::SerializeComponents(const AActor* Actor)
{
	TArray<TSharedPtr<FJsonValue>> CompArray;

	TInlineComponentArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	for (const UActorComponent* Comp : Components)
	{
		if (!Comp) continue;

		auto CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Comp->GetName());
		CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

		CompArray.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(CompArray, *Writer);
	return Result;
}
