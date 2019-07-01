// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
CyLandEdit.cpp: CyLand editing
=============================================================================*/

#include "CyLandEdit.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/FeedbackContext.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "CyLand.h"
#include "CyLandStreamingProxy.h"
#include "CyLandInfo.h"
#include "CyLandComponent.h"
#include "CyLandLayerInfoObject.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Landscape/Classes/Materials/MaterialExpressionLandscapeVisibilityMask.h"
#include "Landscape/Classes/Materials/MaterialExpressionLandscapeLayerWeight.h"
#include "Landscape/Classes/Materials/MaterialExpressionLandscapeLayerSample.h"
#include "Landscape/Classes/Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Landscape/Classes/Materials/MaterialExpressionLandscapeLayerSwitch.h"
#include "CyLandDataAccess.h"
#include "CyLandRender.h"
#include "CyLandRenderMobile.h"
#include "Materials/MaterialInstanceConstant.h"
#include "CyLandMaterialInstanceConstant.h"
#include "CyLandHeightfieldCollisionComponent.h"
#include "CyLandMeshCollisionComponent.h"
#include "CyLandGizmoActiveActor.h"
#include "InstancedFoliageActor.h"
#include "LevelUtils.h"
#include "Logging/TokenizedMessage.h"
#include "Logging/MessageLog.h"
#include "Misc/MapErrors.h"
#include "CyLandSplinesComponent.h"
#include "Serialization/MemoryWriter.h"
#if WITH_EDITOR
#include "MeshDescription.h"
#include "MeshAttributes.h"
#include "MeshAttributeArray.h"
#include "MeshUtilitiesCommon.h"

#include "EngineUtils.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "CyLandEditorModule.h"
#include "CyLandFileFormatInterface.h"
#include "ComponentRecreateRenderStateContext.h"
#include "Interfaces/ITargetPlatform.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Settings/EditorExperimentalSettings.h"
#endif
#include "Algo/Count.h"
#include "Serialization/MemoryWriter.h"
#include "Engine/Canvas.h"

DEFINE_LOG_CATEGORY(LogCyLand);
DEFINE_LOG_CATEGORY(LogCyLandBP);

#define LOCTEXT_NAMESPACE "CyLand"

int32 GMobileCompressCyLandWeightMaps = 0;
FAutoConsoleVariableRef CVarMobileCompressLanscapeWeightMaps(
    TEXT("r.Mobile.CompressCyLandWeightMaps"),
    GMobileCompressCyLandWeightMaps,
    TEXT("Whether to compress the terrain weight maps for mobile."),
    ECVF_ReadOnly
);

#if WITH_EDITOR

// Channel remapping
extern const size_t ChannelOffsets[4];

UCyLandLayerInfoObject* ACyLandProxy::VisibilityLayer = nullptr;

void UCyLandComponent::Init(int32 InBaseX, int32 InBaseY, int32 InComponentSizeQuads, int32 InNumSubsections, int32 InSubsectionSizeQuads)
{
	SetSectionBase(FIntPoint(InBaseX, InBaseY));
	SetRelativeLocation(FVector(GetSectionBase() - GetCyLandProxy()->CyLandSectionOffset));
	ComponentSizeQuads = InComponentSizeQuads;
	NumSubsections = InNumSubsections;
	SubsectionSizeQuads = InSubsectionSizeQuads;
	check(NumSubsections * SubsectionSizeQuads == ComponentSizeQuads);
	UCyLandInfo* Info = GetCyLandInfo();
}

void UCyLandComponent::UpdateCachedBounds()
{
	FCyLandComponentDataInterface CDI(this);

	// Update local-space bounding box
	CachedLocalBox.Init();
	for (int32 y = 0; y < ComponentSizeQuads + 1; y++)
	{
		for (int32 x = 0; x < ComponentSizeQuads + 1; x++)
		{
			CachedLocalBox += CDI.GetLocalVertex(x, y);
		}
	}
	if (CachedLocalBox.GetExtent().Z == 0)
	{
		// expand bounds to avoid flickering issues with zero-size bounds
		CachedLocalBox.ExpandBy(FVector(0, 0, 1));
	}

	// Update collision component bounds
	UCyLandHeightfieldCollisionComponent* HFCollisionComponent = CollisionComponent.Get();
	if (HFCollisionComponent)
	{
		HFCollisionComponent->Modify();
		HFCollisionComponent->CachedLocalBox = CachedLocalBox;
		HFCollisionComponent->UpdateComponentToWorld();
	}
}

void UCyLandComponent::UpdateNavigationRelevance()
{
	ACyLandProxy* Proxy = GetCyLandProxy();
	if (CollisionComponent && Proxy)
	{
		CollisionComponent->SetCanEverAffectNavigation(Proxy->bUsedForNavigation);
		// UNavigationSystem::UpdateNavOctree(CollisionComponent.Get());
	}
}

UCyLandMaterialInstanceConstant* ACyLandProxy::GetLayerThumbnailMIC(UMaterialInterface* CyLandMaterial, FName LayerName, UTexture2D* ThumbnailWeightmap, UTexture2D* ThumbnailHeightmap, ACyLandProxy* Proxy)
{
	if (!CyLandMaterial)
	{
		CyLandMaterial = Proxy ? Proxy->GetCyLandMaterial() : UMaterial::GetDefaultMaterial(MD_Surface);
	}

	FlushRenderingCommands();

	UCyLandMaterialInstanceConstant* MaterialInstance = NewObject<UCyLandMaterialInstanceConstant>(GetTransientPackage());
	MaterialInstance->bIsLayerThumbnail = true;
	MaterialInstance->bMobile = false;
	MaterialInstance->SetParentEditorOnly(CyLandMaterial);

	FStaticParameterSet StaticParameters;
	MaterialInstance->GetStaticParameterValues(StaticParameters);

	for (int32 LayerParameterIdx = 0; LayerParameterIdx < StaticParameters.TerrainLayerWeightParameters.Num(); ++LayerParameterIdx)
	{
		FStaticTerrainLayerWeightParameter& LayerParameter = StaticParameters.TerrainLayerWeightParameters[LayerParameterIdx];
		if (LayerParameter.ParameterInfo.Name == LayerName)
		{
			LayerParameter.WeightmapIndex = 0;
			LayerParameter.bOverride = true;
		}
		else
		{
			LayerParameter.WeightmapIndex = INDEX_NONE;
		}
	}
	MaterialInstance->UpdateStaticPermutation(StaticParameters);

	FLinearColor Mask(1.0f, 0.0f, 0.0f, 0.0f);
	MaterialInstance->SetVectorParameterValueEditorOnly(FName(*FString::Printf(TEXT("LayerMask_%s"), *LayerName.ToString())), Mask);
	MaterialInstance->SetTextureParameterValueEditorOnly(FName(TEXT("Weightmap0")), ThumbnailWeightmap);
	MaterialInstance->SetTextureParameterValueEditorOnly(FName(TEXT("Heightmap")), ThumbnailHeightmap);

	MaterialInstance->PostEditChange();

	return MaterialInstance;
}

/**
* Generate a key for this component's layer allocations to use with MaterialInstanceConstantMap.
*/
FString UCyLandComponent::GetLayerAllocationKey(const TArray<FCyWeightmapLayerAllocationInfo>& Allocations, UMaterialInterface* CyLandMaterial, bool bMobile /*= false*/)
{
	if (!CyLandMaterial)
	{
		return FString();
	}

	FString Result = CyLandMaterial->GetPathName();

	// Generate a string to describe each allocation
	TArray<FString> LayerStrings;
	for (int32 LayerIdx = 0; LayerIdx < Allocations.Num(); LayerIdx++)
	{
		const bool bNoWeightBlend = Allocations[LayerIdx].LayerInfo && Allocations[LayerIdx].LayerInfo->bNoWeightBlend;
		LayerStrings.Add(FString::Printf(TEXT("_%s_%s%d"), *Allocations[LayerIdx].GetLayerName().ToString(), bNoWeightBlend ? TEXT("n") : TEXT("w"), Allocations[LayerIdx].WeightmapTextureIndex));
	}
	// Sort them alphabetically so we can share across components even if the order is different
	LayerStrings.Sort(TGreater<FString>());

	for (int32 LayerIdx = 0; LayerIdx < LayerStrings.Num(); LayerIdx++)
	{
		Result += LayerStrings[LayerIdx];
	}

	if (bMobile)
	{
		Result += TEXT("M");
	}

	return Result;
}

UMaterialInstanceConstant* UCyLandComponent::GetCombinationMaterial(FMaterialUpdateContext* InMaterialUpdateContext, const TArray<FCyWeightmapLayerAllocationInfo>& Allocations, int8 InLODIndex, bool bMobile /*= false*/) const
{
	check(GIsEditor);

	const bool bComponentHasHoles = ComponentHasVisibilityPainted();
	UMaterialInterface* const CyLandMaterial = GetCyLandMaterial(InLODIndex);
	UMaterialInterface* const HoleMaterial = bComponentHasHoles ? GetCyLandHoleMaterial() : nullptr;
	UMaterialInterface* const MaterialToUse = bComponentHasHoles && HoleMaterial ? HoleMaterial : CyLandMaterial;
	bool bOverrideBlendMode = bComponentHasHoles && !HoleMaterial && CyLandMaterial->GetBlendMode() == BLEND_Opaque;

	if (bOverrideBlendMode)
	{
		UMaterial* Material = CyLandMaterial->GetMaterial();
		if (Material && Material->bUsedAsSpecialEngineMaterial)
		{
			bOverrideBlendMode = false;
#if WITH_EDITOR
			static TWeakPtr<SNotificationItem> ExistingNotification;
			if (!ExistingNotification.IsValid())
			{
				// let the user know why they are not seeing holes
				FNotificationInfo Info(LOCTEXT("AssignCyLandMaterial", "You must assign a regular, non-engine material to your landscape in order to see holes created with the visibility tool."));
				Info.ExpireDuration = 5.0f;
				Info.bUseSuccessFailIcons = true;
				ExistingNotification = TWeakPtr<SNotificationItem>(FSlateNotificationManager::Get().AddNotification(Info));
			}
#endif
			return nullptr;
		}
	}

	if (ensure(MaterialToUse != nullptr))
	{
		ACyLandProxy* Proxy = GetCyLandProxy();
		FString LayerKey = GetLayerAllocationKey(Allocations, MaterialToUse, bMobile);

		// Find or set a matching MIC in the CyLand's map.
		UMaterialInstanceConstant* CombinationMaterialInstance = Proxy->MaterialInstanceConstantMap.FindRef(*LayerKey);
		if (CombinationMaterialInstance == nullptr || CombinationMaterialInstance->Parent != MaterialToUse || GetOutermost() != CombinationMaterialInstance->GetOutermost())
		{
			FlushRenderingCommands();

			UCyLandMaterialInstanceConstant* CyLandCombinationMaterialInstance = NewObject<UCyLandMaterialInstanceConstant>(GetOutermost());
			CyLandCombinationMaterialInstance->bMobile = bMobile;
			CombinationMaterialInstance = CyLandCombinationMaterialInstance;
			UE_LOG(LogCyLand, Log, TEXT("Looking for key %s, making new combination %s"), *LayerKey, *CombinationMaterialInstance->GetName());
			Proxy->MaterialInstanceConstantMap.Add(*LayerKey, CombinationMaterialInstance);
			CombinationMaterialInstance->SetParentEditorOnly(MaterialToUse);

			CombinationMaterialInstance->BasePropertyOverrides.bOverride_BlendMode = bOverrideBlendMode;
			if (bOverrideBlendMode)
			{
				CombinationMaterialInstance->BasePropertyOverrides.BlendMode = bComponentHasHoles ? BLEND_Masked : BLEND_Opaque;
			}

			FStaticParameterSet StaticParameters;
			for (const FCyWeightmapLayerAllocationInfo& Allocation : Allocations)
			{
				if (Allocation.LayerInfo)
				{
					const FName LayerParameter = (Allocation.LayerInfo == ACyLandProxy::VisibilityLayer) ? UMaterialExpressionLandscapeVisibilityMask::ParameterName : Allocation.LayerInfo->LayerName;
					StaticParameters.TerrainLayerWeightParameters.Add(FStaticTerrainLayerWeightParameter(LayerParameter, Allocation.WeightmapTextureIndex, true, FGuid(), !Allocation.LayerInfo->bNoWeightBlend));
				}
			}
			CombinationMaterialInstance->UpdateStaticPermutation(StaticParameters, InMaterialUpdateContext);

			CombinationMaterialInstance->PostEditChange();
		}

		return CombinationMaterialInstance;
	}
	return nullptr;
}

void UCyLandComponent::UpdateMaterialInstances_Internal(FMaterialUpdateContext& Context)
{
	check(GIsEditor);

	int32 MaxLOD = FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1;
	TMap<UMaterialInterface*, int8> NewMaterialPerLOD;
	LODIndexToMaterialIndex.SetNumUninitialized(MaxLOD+1);
	int8 LastLODIndex = INDEX_NONE;

	UMaterialInterface* BaseMaterial = GetCyLandMaterial();
	UMaterialInterface* LOD0Material = GetCyLandMaterial(0);

	for (int32 LODIndex = 0; LODIndex <= MaxLOD; ++LODIndex)
	{
		UMaterialInterface* CurrentMaterial = GetCyLandMaterial(LODIndex);

		// if we have a LOD0 override, do not let the base material override it, it should override everything!
		if (CurrentMaterial == BaseMaterial && BaseMaterial != LOD0Material)
		{
			CurrentMaterial = LOD0Material;
		}

		const int8* MaterialLOD = NewMaterialPerLOD.Find(CurrentMaterial);

		if (MaterialLOD != nullptr)
		{
			LODIndexToMaterialIndex[LODIndex] = *MaterialLOD > LastLODIndex ? *MaterialLOD : LastLODIndex;
		}
		else
		{
			int32 AddedIndex = NewMaterialPerLOD.Num();
			NewMaterialPerLOD.Add(CurrentMaterial, LODIndex);
			LODIndexToMaterialIndex[LODIndex] = AddedIndex;
			LastLODIndex = AddedIndex;
		}
	}

	MaterialPerLOD = NewMaterialPerLOD;

	MaterialInstances.SetNumZeroed(MaterialPerLOD.Num() * 2); // over allocate in case we are using tessellation
	MaterialIndexToDisabledTessellationMaterial.Init(INDEX_NONE, MaxLOD + 1);
	int8 TessellatedMaterialCount = 0;
	int8 MaterialIndex = 0;

	for (auto It = MaterialPerLOD.CreateConstIterator(); It; ++It)
	{
		const int8 MaterialLOD = It.Value();

		// Find or set a matching MIC in the CyLand's map.
		UMaterialInstanceConstant* CombinationMaterialInstance = GetCombinationMaterial(&Context, WeightmapLayerAllocations, MaterialLOD, false);

		if (CombinationMaterialInstance != nullptr)
		{
			// Create the instance for this component, that will use the layer combination instance.
			UMaterialInstanceConstant* MaterialInstance = NewObject<UCyLandMaterialInstanceConstant>(GetOutermost());
			MaterialInstances[MaterialIndex] = MaterialInstance;

			// Material Instances don't support Undo/Redo (the shader map goes out of sync and crashes happen)
			// so we call UpdateMaterialInstances() from UCyLandComponent::PostEditUndo instead
			//MaterialInstance->SetFlags(RF_Transactional);
			//MaterialInstance->Modify();

			MaterialInstance->SetParentEditorOnly(CombinationMaterialInstance);
			MaterialInstance->ClearParameterValuesEditorOnly();
			Context.AddMaterialInstance(MaterialInstance); // must be done after SetParent

			FLinearColor Masks[4] = { FLinearColor(1.0f, 0.0f, 0.0f, 0.0f), FLinearColor(0.0f, 1.0f, 0.0f, 0.0f), FLinearColor(0.0f, 0.0f, 1.0f, 0.0f), FLinearColor(0.0f, 0.0f, 0.0f, 1.0f) };

			// Set the layer mask
			for (int32 AllocIdx = 0; AllocIdx < WeightmapLayerAllocations.Num(); AllocIdx++)
			{
				FCyWeightmapLayerAllocationInfo& Allocation = WeightmapLayerAllocations[AllocIdx];

				FName LayerName = Allocation.LayerInfo == ACyLandProxy::VisibilityLayer ? UMaterialExpressionLandscapeVisibilityMask::ParameterName : Allocation.LayerInfo ? Allocation.LayerInfo->LayerName : NAME_None;
				MaterialInstance->SetVectorParameterValueEditorOnly(FName(*FString::Printf(TEXT("LayerMask_%s"), *LayerName.ToString())), Masks[Allocation.WeightmapTextureChannel]);
			}

			// Set the weightmaps
			for (int32 i = 0; i < WeightmapTextures.Num(); i++)
			{
				MaterialInstance->SetTextureParameterValueEditorOnly(FName(*FString::Printf(TEXT("Weightmap%d"), i)), WeightmapTextures[i]);
			}

			// Set the heightmap, if needed.
			if (HeightmapTexture)
			{
				MaterialInstance->SetTextureParameterValueEditorOnly(FName(TEXT("Heightmap")), HeightmapTexture);
			}
			MaterialInstance->PostEditChange();

			// Setup material instance with disabled tessellation
			if (CombinationMaterialInstance->GetMaterial()->D3D11TessellationMode != EMaterialTessellationMode::MTM_NoTessellation)
			{
				UCyLandMaterialInstanceConstant* TessellationMaterialInstance = NewObject<UCyLandMaterialInstanceConstant>(GetOutermost());
				int32 TessellatedMaterialIndex = MaterialPerLOD.Num() + TessellatedMaterialCount++;
				MaterialInstances[TessellatedMaterialIndex] = TessellationMaterialInstance;
				MaterialIndexToDisabledTessellationMaterial[MaterialIndex] = TessellatedMaterialIndex;

				TessellationMaterialInstance->SetParentEditorOnly(MaterialInstance);
				Context.AddMaterialInstance(TessellationMaterialInstance); // must be done after SetParent
				TessellationMaterialInstance->bDisableTessellation = true;
				TessellationMaterialInstance->PostEditChange();
			}
		}

		++MaterialIndex;
	}

	MaterialInstances.Remove(nullptr);
	MaterialInstances.Shrink();

	if (MaterialPerLOD.Num() == 0)
	{
		MaterialInstances.Empty(1);
		MaterialInstances.Add(nullptr);
		LODIndexToMaterialIndex.Empty(1);
		LODIndexToMaterialIndex.Add(0);
	}

	// Update mobile combination material
	{
		GenerateMobileWeightmapLayerAllocations();

		MobileCombinationMaterialInstances.SetNumZeroed(MaterialPerLOD.Num());
		int8 MobileMaterialIndex = 0;

		for (auto It = MaterialPerLOD.CreateConstIterator(); It; ++It)
		{
			const int8 MaterialLOD = It.Value();

			UMaterialInstanceConstant* MobileCombinationMaterialInstance = GetCombinationMaterial(&Context, MobileWeightmapLayerAllocations, MaterialLOD, true);
			MobileCombinationMaterialInstances[MobileMaterialIndex] = MobileCombinationMaterialInstance;
			Context.AddMaterialInstance(MobileCombinationMaterialInstance);
						
			++MobileMaterialIndex;
		}
	}
}

void UCyLandComponent::UpdateMaterialInstances()
{
	// we're not having the material update context recreate the render state because we will manually do it for only this component
	TOptional<FComponentRecreateRenderStateContext> RecreateRenderStateContext;
	RecreateRenderStateContext.Emplace(this);
	TOptional<FMaterialUpdateContext> MaterialUpdateContext;
	MaterialUpdateContext.Emplace(FMaterialUpdateContext::EOptions::Default & ~FMaterialUpdateContext::EOptions::RecreateRenderStates);

	UpdateMaterialInstances_Internal(MaterialUpdateContext.GetValue());

	// End material update
	MaterialUpdateContext.Reset();

	// Recreate the render state for this component, needed to update the static drawlist which has cached the MaterialRenderProxies
	// Must be after the FMaterialUpdateContext is destroyed
	RecreateRenderStateContext.Reset();
}

void ACyLandProxy::UpdateAllComponentMaterialInstances()
{
	// we're not having the material update context recreate render states because we will manually do it for only our components
	TArray<FComponentRecreateRenderStateContext> RecreateRenderStateContexts;
	for (UCyLandComponent* Component : CyLandComponents)
	{
		RecreateRenderStateContexts.Emplace(Component);
	}
	TOptional<FMaterialUpdateContext> MaterialUpdateContext;
	MaterialUpdateContext.Emplace(FMaterialUpdateContext::EOptions::Default & ~FMaterialUpdateContext::EOptions::RecreateRenderStates);

	for (UCyLandComponent* Component : CyLandComponents)
	{
		Component->UpdateMaterialInstances_Internal(MaterialUpdateContext.GetValue());
	}

	// End material update
	MaterialUpdateContext.Reset();

	// Recreate the render state for our components, needed to update the static drawlist which has cached the MaterialRenderProxies
	// Must be after the FMaterialUpdateContext is destroyed
	RecreateRenderStateContexts.Empty();
}

int32 UCyLandComponent::GetNumMaterials() const
{
	return 1;
}

class UMaterialInterface* UCyLandComponent::GetMaterial(int32 ElementIndex) const
{
	if (ensure(ElementIndex == 0))
	{
		return GetCyLandMaterial(ElementIndex);
	}

	return nullptr;
}

void UCyLandComponent::SetMaterial(int32 ElementIndex, class UMaterialInterface* Material)
{
	if (ensure(ElementIndex == 0))
	{
		GetCyLandProxy()->CyLandMaterial = Material;
	}
}

bool UCyLandComponent::ComponentIsTouchingSelectionBox(const FBox& InSelBBox, const FEngineShowFlags& ShowFlags, const bool bConsiderOnlyBSP, const bool bMustEncompassEntireComponent) const
{
	if (ShowFlags.Landscape)
	{
		return Super::ComponentIsTouchingSelectionBox(InSelBBox, ShowFlags, bConsiderOnlyBSP, bMustEncompassEntireComponent);
	}

	return false;
}

bool UCyLandComponent::ComponentIsTouchingSelectionFrustum(const FConvexVolume& InFrustum, const FEngineShowFlags& ShowFlags, const bool bConsiderOnlyBSP, const bool bMustEncompassEntireComponent) const
{
	if (ShowFlags.Landscape)
	{
		return Super::ComponentIsTouchingSelectionFrustum(InFrustum, ShowFlags, bConsiderOnlyBSP, bMustEncompassEntireComponent);
	}

	return false;
}

void UCyLandComponent::PreFeatureLevelChange(ERHIFeatureLevel::Type PendingFeatureLevel)
{
	Super::PreFeatureLevelChange(PendingFeatureLevel);

	if (PendingFeatureLevel <= ERHIFeatureLevel::ES3_1)
	{
		// See if we need to cook platform data for ES2 preview in editor
		CheckGenerateCyLandPlatformData(false, nullptr);
	}
}

void UCyLandComponent::PostEditUndo()
{
	if (!IsPendingKill())
	{
		UpdateMaterialInstances();
	}

	Super::PostEditUndo();

	if (!IsPendingKill())
	{
		EditToolRenderData.UpdateDebugColorMaterial(this);

		EditToolRenderData.UpdateSelectionMaterial(EditToolRenderData.SelectedType, this);
		UpdateEditToolRenderData();
	}

	TSet<UCyLandComponent*> Components;
	Components.Add(this);
	GetCyLandProxy()->FlushGrassComponents(&Components);
}

void ACyLandProxy::FixupWeightmaps()
{
	WeightmapUsageMap.Empty();
	for (UCyLandComponent* Component : CyLandComponents)
	{
		Component->FixupWeightmaps();
	}
}

void UCyLandComponent::FixupWeightmaps()
{
	if (GIsEditor && !HasAnyFlags(RF_ClassDefaultObject))
	{
		UCyLandInfo* Info = GetCyLandInfo();
		ACyLandProxy* Proxy = GetCyLandProxy();

		if (Info)
		{
			TArray<UCyLandLayerInfoObject*> LayersToDelete;
			bool bFixedLayerDeletion = false;

			// make sure the weightmap textures are fully loaded or deleting layers from them will crash! :)
			for (UTexture* WeightmapTexture : WeightmapTextures)
			{
				WeightmapTexture->ConditionalPostLoad();
			}

			// LayerInfo Validation check...
			for (const auto& Allocation : WeightmapLayerAllocations)
			{
				if (!Allocation.LayerInfo
					|| (Allocation.LayerInfo != ACyLandProxy::VisibilityLayer && Info->GetLayerInfoIndex(Allocation.LayerInfo) == INDEX_NONE))
				{
					if (!bFixedLayerDeletion)
					{
						FFormatNamedArguments Arguments;
						Arguments.Add(TEXT("CyLandName"), FText::FromString(GetName()));
						FMessageLog("MapCheck").Warning()
							->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_FixedUpDeletedLayerWeightmap", "{CyLandName} : Fixed up deleted layer weightmap"), Arguments)))
							->AddToken(FMapErrorToken::Create(FMapErrors::FixedUpDeletedLayerWeightmap));
					}

					bFixedLayerDeletion = true;
					LayersToDelete.Add(Allocation.LayerInfo);
				}
			}

			if (bFixedLayerDeletion)
			{
				FCyLandEditDataInterface CyLandEdit(Info);
				for (int32 Idx = 0; Idx < LayersToDelete.Num(); ++Idx)
				{
					DeleteLayer(LayersToDelete[Idx], CyLandEdit);
				}
			}

			bool bFixedWeightmapTextureIndex = false;

			// Store the weightmap allocations in WeightmapUsageMap
			for (int32 LayerIdx = 0; LayerIdx < WeightmapLayerAllocations.Num(); LayerIdx++)
			{
				FCyWeightmapLayerAllocationInfo& Allocation = WeightmapLayerAllocations[LayerIdx];

				// Fix up any problems caused by the layer deletion bug.
				if (Allocation.WeightmapTextureIndex >= WeightmapTextures.Num())
				{
					Allocation.WeightmapTextureIndex = WeightmapTextures.Num() - 1;
					if (!bFixedWeightmapTextureIndex)
					{
						FFormatNamedArguments Arguments;
						Arguments.Add(TEXT("CyLandName"), FText::FromString(GetName()));
						FMessageLog("MapCheck").Warning()
							->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_FixedUpIncorrectLayerWeightmap", "{CyLandName} : Fixed up incorrect layer weightmap texture index"), Arguments)))
							->AddToken(FMapErrorToken::Create(FMapErrors::FixedUpIncorrectLayerWeightmap));
					}
					bFixedWeightmapTextureIndex = true;
				}

				UTexture2D* WeightmapTexture = WeightmapTextures[Allocation.WeightmapTextureIndex];
				FCyLandWeightmapUsage& Usage = Proxy->WeightmapUsageMap.FindOrAdd(WeightmapTexture);

				// Detect a shared layer allocation, caused by a previous undo or layer deletion bugs
				if (Usage.ChannelUsage[Allocation.WeightmapTextureChannel] != nullptr &&
					Usage.ChannelUsage[Allocation.WeightmapTextureChannel] != this)
				{
					FFormatNamedArguments Arguments;
					Arguments.Add(TEXT("LayerName"), FText::FromString(Allocation.GetLayerName().ToString()));
					Arguments.Add(TEXT("CyLandName"), FText::FromString(GetName()));
					Arguments.Add(TEXT("ChannelName"), FText::FromString(Usage.ChannelUsage[Allocation.WeightmapTextureChannel]->GetName()));
					FMessageLog("MapCheck").Warning()
						->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_FixedUpSharedLayerWeightmap", "Fixed up shared weightmap texture for layer {LayerName} in component '{CyLandName}' (shares with '{ChannelName}')"), Arguments)))
						->AddToken(FMapErrorToken::Create(FMapErrors::FixedUpSharedLayerWeightmap));
					WeightmapLayerAllocations.RemoveAt(LayerIdx);
					LayerIdx--;
					continue;
				}
				else
				{
					Usage.ChannelUsage[Allocation.WeightmapTextureChannel] = this;
				}
			}

			RemoveInvalidWeightmaps();

			// Store the layer combination in the MaterialInstanceConstantMap
			if (GetMaterialInstance(0, false) != nullptr)
			{
				UMaterialInstanceConstant* CombinationMaterialInstance = Cast<UMaterialInstanceConstant>(GetMaterialInstance(0, false)->Parent);
				if (CombinationMaterialInstance)
				{
					Proxy->MaterialInstanceConstantMap.Add(*GetLayerAllocationKey(WeightmapLayerAllocations, CombinationMaterialInstance->Parent), CombinationMaterialInstance);
				}
			}
		}
	}
}

void UCyLandComponent::UpdateLayerWhitelistFromPaintedLayers()
{
	for (const auto& Allocation : WeightmapLayerAllocations)
	{
		LayerWhitelist.AddUnique(Allocation.LayerInfo);
	}
}

//
// CyLandComponentAlphaInfo
//
struct FCyLandComponentAlphaInfo
{
	int32 LayerIndex;
	TArray<uint8> AlphaValues;

	// tor
	FCyLandComponentAlphaInfo(UCyLandComponent* InOwner, int32 InLayerIndex)
		: LayerIndex(InLayerIndex)
	{
		AlphaValues.Empty(FMath::Square(InOwner->ComponentSizeQuads + 1));
		AlphaValues.AddZeroed(FMath::Square(InOwner->ComponentSizeQuads + 1));
	}

	bool IsLayerAllZero() const
	{
		for (int32 Index = 0; Index < AlphaValues.Num(); Index++)
		{
			if (AlphaValues[Index] != 0)
			{
				return false;
			}
		}
		return true;
	}
};


void UCyLandComponent::UpdateCollisionHeightData(const FColor* const HeightmapTextureMipData, const FColor* const SimpleCollisionHeightmapTextureData, int32 ComponentX1/*=0*/, int32 ComponentY1/*=0*/, int32 ComponentX2/*=MAX_int32*/, int32 ComponentY2/*=MAX_int32*/, bool bUpdateBounds/*=false*/, const FColor* XYOffsetTextureMipData/*=nullptr*/)
{
	UCyLandInfo* Info = GetCyLandInfo();
	ACyLandProxy* Proxy = GetCyLandProxy();
	FIntPoint ComponentKey = GetSectionBase() / ComponentSizeQuads;
	UCyLandHeightfieldCollisionComponent* CollisionComp = CollisionComponent.Get();
	UCyLandMeshCollisionComponent* MeshCollisionComponent = Cast<UCyLandMeshCollisionComponent>(CollisionComp);
	UCyLandHeightfieldCollisionComponent* OldCollisionComponent = CollisionComp;

	// Simple collision is not currently supported with mesh collision components
	const bool bUsingSimpleCollision = (SimpleCollisionMipLevel > CollisionMipLevel && SimpleCollisionHeightmapTextureData && !XYOffsetmapTexture);

	const int32 CollisionSubsectionSizeVerts = ((SubsectionSizeQuads + 1) >> CollisionMipLevel);
	const int32 CollisionSubsectionSizeQuads = CollisionSubsectionSizeVerts - 1;
	const int32 CollisionSizeVerts = NumSubsections * CollisionSubsectionSizeQuads + 1;
	const int32 SimpleCollisionSubsectionSizeVerts = bUsingSimpleCollision ? ((SubsectionSizeQuads + 1) >> SimpleCollisionMipLevel) : 0;
	const int32 SimpleCollisionSubsectionSizeQuads = bUsingSimpleCollision ? SimpleCollisionSubsectionSizeVerts - 1 : 0;
	const int32 SimpleCollisionSizeVerts = bUsingSimpleCollision ? NumSubsections * SimpleCollisionSubsectionSizeQuads + 1 : 0;

	const int32 TotalCollisionSize = FMath::Square(CollisionSizeVerts) + FMath::Square(SimpleCollisionSizeVerts);

	uint16* CollisionHeightData = nullptr;
	uint16* CollisionXYOffsetData = nullptr;
	bool CreatedNew = false;
	bool ChangeType = false;

	if (CollisionComp)
	{
		CollisionComp->Modify();
	}

	// Existing collision component is same type with collision
	if (CollisionComp && ((XYOffsetmapTexture == nullptr) == (MeshCollisionComponent == nullptr)))
	{
		ComponentX1 = FMath::Min(ComponentX1, ComponentSizeQuads);
		ComponentY1 = FMath::Min(ComponentY1, ComponentSizeQuads);
		ComponentX2 = FMath::Max(ComponentX2, 0);
		ComponentY2 = FMath::Max(ComponentY2, 0);

		if (ComponentX2 < ComponentX1 || ComponentY2 < ComponentY1)
		{
			// nothing to do
			return;
		}

		if (bUpdateBounds)
		{
			CollisionComp->CachedLocalBox = CachedLocalBox;
			CollisionComp->UpdateComponentToWorld();
		}

		CollisionHeightData = (uint16*)CollisionComp->CollisionHeightData.Lock(LOCK_READ_WRITE);

		if (XYOffsetmapTexture && MeshCollisionComponent)
		{
			CollisionXYOffsetData = (uint16*)MeshCollisionComponent->CollisionXYOffsetData.Lock(LOCK_READ_WRITE);
		}
	}
	else
	{
		ComponentX1 = 0;
		ComponentY1 = 0;
		ComponentX2 = ComponentSizeQuads;
		ComponentY2 = ComponentSizeQuads;

		TArray<uint8> DominantLayerData;
		TArray<UCyLandLayerInfoObject*> LayerInfos;
		if (CollisionComp) // remove old component before changing to other type collision...
		{
			ChangeType = true;

			if (CollisionComp->DominantLayerData.GetElementCount())
			{
				check(CollisionComp->DominantLayerData.GetElementCount() >= TotalCollisionSize);
				DominantLayerData.AddUninitialized(TotalCollisionSize);

				const uint8* SrcDominantLayerData = (uint8*)CollisionComp->DominantLayerData.Lock(LOCK_READ_ONLY);
				FMemory::Memcpy(DominantLayerData.GetData(), SrcDominantLayerData, TotalCollisionSize * sizeof(uint8));
				CollisionComp->DominantLayerData.Unlock();
			}

			if (CollisionComp->ComponentLayerInfos.Num())
			{
				LayerInfos = CollisionComp->ComponentLayerInfos;
			}

			if (Info)
			{
				Info->Modify();
			}
			Proxy->Modify();
			CollisionComp->DestroyComponent();
			CollisionComp = nullptr;
		}

		if (XYOffsetmapTexture)
		{
			MeshCollisionComponent = NewObject<UCyLandMeshCollisionComponent>(Proxy, NAME_None, RF_Transactional);
			CollisionComp = MeshCollisionComponent;
		}
		else
		{
			MeshCollisionComponent = nullptr;
			CollisionComp = NewObject<UCyLandHeightfieldCollisionComponent>(Proxy, NAME_None, RF_Transactional);
		}

		CollisionComp->SetRelativeLocation(RelativeLocation);
		CollisionComp->SetupAttachment(Proxy->GetRootComponent(), NAME_None);
		Proxy->CollisionComponents.Add(CollisionComp);

		CollisionComp->RenderComponent = this;
		CollisionComp->SetSectionBase(GetSectionBase());
		CollisionComp->CollisionSizeQuads = CollisionSubsectionSizeQuads * NumSubsections;
		CollisionComp->CollisionScale = (float)(ComponentSizeQuads) / (float)(CollisionComp->CollisionSizeQuads);
		CollisionComp->SimpleCollisionSizeQuads = bUsingSimpleCollision ? SimpleCollisionSubsectionSizeQuads * NumSubsections : 0;
		CollisionComp->CachedLocalBox = CachedLocalBox;
		CollisionComp->SetGenerateOverlapEvents(Proxy->bGenerateOverlapEvents);
		CreatedNew = true;

		// Reallocate raw collision data
		CollisionComp->CollisionHeightData.Lock(LOCK_READ_WRITE);
		CollisionHeightData = (uint16*)CollisionComp->CollisionHeightData.Realloc(TotalCollisionSize);
		FMemory::Memzero(CollisionHeightData, TotalCollisionSize * sizeof(uint16));

		if (XYOffsetmapTexture && MeshCollisionComponent)
		{
			// Need XYOffsetData for Collision Component
			MeshCollisionComponent->CollisionXYOffsetData.Lock(LOCK_READ_WRITE);
			CollisionXYOffsetData = (uint16*)MeshCollisionComponent->CollisionXYOffsetData.Realloc(TotalCollisionSize * 2);
			FMemory::Memzero(CollisionXYOffsetData, TotalCollisionSize * 2 * sizeof(uint16));
		}

		if (DominantLayerData.Num())
		{
			CollisionComp->DominantLayerData.Lock(LOCK_READ_WRITE);
			uint8* DestDominantLayerData = (uint8*)CollisionComp->DominantLayerData.Realloc(TotalCollisionSize);
			FMemory::Memcpy(DestDominantLayerData, DominantLayerData.GetData(), TotalCollisionSize * sizeof(uint8));
			CollisionComp->DominantLayerData.Unlock();
		}

		if (LayerInfos.Num())
		{
			CollisionComp->ComponentLayerInfos = MoveTemp(LayerInfos);
		}
	}

	const int32 HeightmapSizeU = GetHeightmap()->Source.GetSizeX();
	const int32 HeightmapSizeV = GetHeightmap()->Source.GetSizeY();
	const int32 MipSizeU = HeightmapSizeU >> CollisionMipLevel;
	const int32 MipSizeV = HeightmapSizeV >> CollisionMipLevel;

	const int32 XYMipSizeU = XYOffsetmapTexture ? XYOffsetmapTexture->Source.GetSizeX() >> CollisionMipLevel : 0;
	const int32 XYMipSizeV = XYOffsetmapTexture ? XYOffsetmapTexture->Source.GetSizeY() >> CollisionMipLevel : 0;

	// Ratio to convert update region coordinate to collision mip coordinates
	const float CollisionQuadRatio = (float)CollisionSubsectionSizeQuads / (float)SubsectionSizeQuads;

	// XY offset into heightmap mip data
	const int32 HeightmapOffsetX = FMath::RoundToInt(HeightmapScaleBias.Z * (float)HeightmapSizeU) >> CollisionMipLevel;
	const int32 HeightmapOffsetY = FMath::RoundToInt(HeightmapScaleBias.W * (float)HeightmapSizeV) >> CollisionMipLevel;

	//int32 WeightmapOffsetX = FMath::RoundToInt(WeightmapScaleBias.Z * (float)XYMipSizeU) >> CollisionMipLevel;
	//int32 WeightmapOffsetY = FMath::RoundToInt(WeightmapScaleBias.W * (float)XYMipSizeV) >> CollisionMipLevel;

	// Handle Material WPO baked into heightfield collision
	// Material WPO is not currently supported for mesh collision components
	const bool bUsingGrassMapHeights = Proxy->bBakeMaterialPositionOffsetIntoCollision && !MeshCollisionComponent && GrassData->HasData() && !IsGrassMapOutdated();
	const uint16* GrassHeights = nullptr;
	if (bUsingGrassMapHeights)
	{
		if (CollisionMipLevel == 0)
		{
			GrassHeights = GrassData->HeightData.GetData();
		}
		else
		{
			if (GrassData->HeightMipData.Contains(CollisionMipLevel))
			{
				GrassHeights = GrassData->HeightMipData[CollisionMipLevel].GetData();
			}
		}
	}

	const int32 SubSectionX1 = FMath::Max(0, FMath::DivideAndRoundDown(ComponentX1 - 1, SubsectionSizeQuads));
	const int32 SubSectionY1 = FMath::Max(0, FMath::DivideAndRoundDown(ComponentY1 - 1, SubsectionSizeQuads));
	const int32 SubSectionX2 = FMath::Min(FMath::DivideAndRoundUp(ComponentX2 + 1, SubsectionSizeQuads), NumSubsections);
	const int32 SubSectionY2 = FMath::Min(FMath::DivideAndRoundUp(ComponentY2 + 1, SubsectionSizeQuads), NumSubsections);
	for (int32 SubsectionY = SubSectionY1; SubsectionY < SubSectionY2; ++SubsectionY)
	{
		for (int32 SubsectionX = SubSectionX1; SubsectionX < SubSectionX2; ++SubsectionX)
		{
			// Area to update in subsection coordinates
			const int32 SubX1 = ComponentX1 - SubsectionSizeQuads * SubsectionX;
			const int32 SubY1 = ComponentY1 - SubsectionSizeQuads * SubsectionY;
			const int32 SubX2 = ComponentX2 - SubsectionSizeQuads * SubsectionX;
			const int32 SubY2 = ComponentY2 - SubsectionSizeQuads * SubsectionY;

			// Area to update in collision mip level coords
			const int32 CollisionSubX1 = FMath::FloorToInt((float)SubX1 * CollisionQuadRatio);
			const int32 CollisionSubY1 = FMath::FloorToInt((float)SubY1 * CollisionQuadRatio);
			const int32 CollisionSubX2 = FMath::CeilToInt( (float)SubX2 * CollisionQuadRatio);
			const int32 CollisionSubY2 = FMath::CeilToInt( (float)SubY2 * CollisionQuadRatio);

			// Clamp area to update
			const int32 VertX1 = FMath::Clamp<int32>(CollisionSubX1, 0, CollisionSubsectionSizeQuads);
			const int32 VertY1 = FMath::Clamp<int32>(CollisionSubY1, 0, CollisionSubsectionSizeQuads);
			const int32 VertX2 = FMath::Clamp<int32>(CollisionSubX2, 0, CollisionSubsectionSizeQuads);
			const int32 VertY2 = FMath::Clamp<int32>(CollisionSubY2, 0, CollisionSubsectionSizeQuads);

			for (int32 VertY = VertY1; VertY <= VertY2; VertY++)
			{
				for (int32 VertX = VertX1; VertX <= VertX2; VertX++)
				{
					// this uses Quads as we don't want the duplicated vertices
					const int32 CompVertX = CollisionSubsectionSizeQuads * SubsectionX + VertX;
					const int32 CompVertY = CollisionSubsectionSizeQuads * SubsectionY + VertY;

					if (GrassHeights)
					{
						uint16& CollisionHeight = CollisionHeightData[CompVertX + CompVertY * CollisionSizeVerts];
						const uint16& NewHeight = GrassHeights[CompVertX + CompVertY * CollisionSizeVerts];
						CollisionHeight = NewHeight;
					}
					else
					{
						// X/Y of the vertex we're looking indexed into the texture data
						const int32 TexX = HeightmapOffsetX + CollisionSubsectionSizeVerts * SubsectionX + VertX;
						const int32 TexY = HeightmapOffsetY + CollisionSubsectionSizeVerts * SubsectionY + VertY;
						const FColor& TexData = HeightmapTextureMipData[TexX + TexY * MipSizeU];

						// Copy collision data
						uint16& CollisionHeight = CollisionHeightData[CompVertX + CompVertY * CollisionSizeVerts];
						const uint16 NewHeight = TexData.R << 8 | TexData.G;

						CollisionHeight = NewHeight;
					}

					if (XYOffsetmapTexture && XYOffsetTextureMipData && CollisionXYOffsetData)
					{
						const int32 TexX = CollisionSubsectionSizeVerts * SubsectionX + VertX;
						const int32 TexY = CollisionSubsectionSizeVerts * SubsectionY + VertY;
						const FColor& TexData = XYOffsetTextureMipData[TexX + TexY * XYMipSizeU];

						// Copy collision data
						const uint16 NewXOffset = TexData.R << 8 | TexData.G;
						const uint16 NewYOffset = TexData.B << 8 | TexData.A;

						const int32 XYIndex = CompVertX + CompVertY * CollisionSizeVerts;
						CollisionXYOffsetData[XYIndex * 2] = NewXOffset;
						CollisionXYOffsetData[XYIndex * 2 + 1] = NewYOffset;
					}
				}
			}
		}
	}

	if (bUsingSimpleCollision)
	{
		const int32 SimpleMipSizeU = HeightmapSizeU >> SimpleCollisionMipLevel;
		const int32 SimpleMipSizeV = HeightmapSizeV >> SimpleCollisionMipLevel;

		// Ratio to convert update region coordinate to collision mip coordinates
		const float SimpleCollisionQuadRatio = (float)SimpleCollisionSubsectionSizeQuads / (float)SubsectionSizeQuads;

		// XY offset into heightmap mip data
		const int32 SimpleHeightmapOffsetX = FMath::RoundToInt(HeightmapScaleBias.Z * (float)HeightmapSizeU) >> SimpleCollisionMipLevel;
		const int32 SimpleHeightmapOffsetY = FMath::RoundToInt(HeightmapScaleBias.W * (float)HeightmapSizeV) >> SimpleCollisionMipLevel;

		const uint16* SimpleCollisionGrassHeights = bUsingGrassMapHeights && GrassData->HeightMipData.Contains(SimpleCollisionMipLevel) ? GrassData->HeightMipData[SimpleCollisionMipLevel].GetData() : nullptr;
		uint16* const SimpleCollisionHeightData = CollisionHeightData + FMath::Square(CollisionSizeVerts);

		for (int32 SubsectionY = SubSectionY1; SubsectionY < SubSectionY2; ++SubsectionY)
		{
			for (int32 SubsectionX = SubSectionX1; SubsectionX < SubSectionX2; ++SubsectionX)
			{
				// Area to update in subsection coordinates
				const int32 SubX1 = ComponentX1 - SubsectionSizeQuads * SubsectionX;
				const int32 SubY1 = ComponentY1 - SubsectionSizeQuads * SubsectionY;
				const int32 SubX2 = ComponentX2 - SubsectionSizeQuads * SubsectionX;
				const int32 SubY2 = ComponentY2 - SubsectionSizeQuads * SubsectionY;

				// Area to update in simple collision mip level coords
				const int32 SimpleCollisionSubX1 = FMath::FloorToInt((float)SubX1 * SimpleCollisionQuadRatio);
				const int32 SimpleCollisionSubY1 = FMath::FloorToInt((float)SubY1 * SimpleCollisionQuadRatio);
				const int32 SimpleCollisionSubX2 = FMath::CeilToInt((float)SubX2 * SimpleCollisionQuadRatio);
				const int32 SimpleCollisionSubY2 = FMath::CeilToInt((float)SubY2 * SimpleCollisionQuadRatio);

				// Clamp area to update
				const int32 SimpleVertX1 = FMath::Clamp<int32>(SimpleCollisionSubX1, 0, SimpleCollisionSubsectionSizeQuads);
				const int32 SimpleVertY1 = FMath::Clamp<int32>(SimpleCollisionSubY1, 0, SimpleCollisionSubsectionSizeQuads);
				const int32 SimpleVertX2 = FMath::Clamp<int32>(SimpleCollisionSubX2, 0, SimpleCollisionSubsectionSizeQuads);
				const int32 SimpleVertY2 = FMath::Clamp<int32>(SimpleCollisionSubY2, 0, SimpleCollisionSubsectionSizeQuads);

				for (int32 VertY = SimpleVertY1; VertY <= SimpleVertY2; VertY++)
				{
					for (int32 VertX = SimpleVertX1; VertX <= SimpleVertX2; VertX++)
					{
						// this uses Quads as we don't want the duplicated vertices
						const int32 CompVertX = SimpleCollisionSubsectionSizeQuads * SubsectionX + VertX;
						const int32 CompVertY = SimpleCollisionSubsectionSizeQuads * SubsectionY + VertY;

						if (SimpleCollisionGrassHeights)
						{
							uint16& CollisionHeight = SimpleCollisionHeightData[CompVertX + CompVertY * SimpleCollisionSizeVerts];
							const uint16& NewHeight = SimpleCollisionGrassHeights[CompVertX + CompVertY * SimpleCollisionSizeVerts];
							CollisionHeight = NewHeight;
						}
						else
						{
							// X/Y of the vertex we're looking indexed into the texture data
							const int32 TexX = SimpleHeightmapOffsetX + SimpleCollisionSubsectionSizeVerts * SubsectionX + VertX;
							const int32 TexY = SimpleHeightmapOffsetY + SimpleCollisionSubsectionSizeVerts * SubsectionY + VertY;
							const FColor& TexData = SimpleCollisionHeightmapTextureData[TexX + TexY * SimpleMipSizeU];

							// Copy collision data
							uint16& CollisionHeight = SimpleCollisionHeightData[CompVertX + CompVertY * SimpleCollisionSizeVerts];
							uint16 NewHeight = TexData.R << 8 | TexData.G;

							CollisionHeight = NewHeight;
						}

						// not supported
						// if (XYOffsetmapTexture && XYOffsetTextureMipData && CollisionXYOffsetData)
					}
				}
			}
		}
	}

	CollisionComp->CollisionHeightData.Unlock();

	if (XYOffsetmapTexture && MeshCollisionComponent)
	{
		MeshCollisionComponent->CollisionXYOffsetData.Unlock();
	}

	// If we updated an existing component, we need to update the PhysX copy of the data
	if (!CreatedNew)
	{
		if (MeshCollisionComponent)
		{
			// Will be done once for XY Offset data update in FXYOffsetmapAccessor() destructor with UpdateCachedBounds()
			//MeshCollisionComponent->RecreateCollision();
		}
		else if (CollisionMipLevel == 0)
		{
			CollisionComp->UpdateHeightfieldRegion(ComponentX1, ComponentY1, ComponentX2, ComponentY2);
		}
		else
		{
			const int32 CollisionCompX1 = FMath::FloorToInt((float)ComponentX1 * CollisionQuadRatio);
			const int32 CollisionCompY1 = FMath::FloorToInt((float)ComponentY1 * CollisionQuadRatio);
			const int32 CollisionCompX2 = FMath::CeilToInt( (float)ComponentX2 * CollisionQuadRatio);
			const int32 CollisionCompY2 = FMath::CeilToInt( (float)ComponentY2 * CollisionQuadRatio);
			CollisionComp->UpdateHeightfieldRegion(CollisionCompX1, CollisionCompY1, CollisionCompX2, CollisionCompY2);
		}
	}

	{
		// set relevancy for navigation system
		ACyLandProxy* CyLandProxy = CollisionComp->GetCyLandProxy();
		CollisionComp->SetCanEverAffectNavigation(CyLandProxy ? CyLandProxy->bUsedForNavigation : false);
	}

	// Move any foliage instances if we created a new collision component.
	if (OldCollisionComponent && OldCollisionComponent != CollisionComp)
	{
		AInstancedFoliageActor::MoveInstancesToNewComponent(Proxy->GetWorld(), OldCollisionComponent, CollisionComp);
	}

	// Set new collision component to pointer
	CollisionComponent = CollisionComp;

	if (CreatedNew && !ChangeType)
	{
		UpdateCollisionLayerData();
	}

	if (CreatedNew && Proxy->GetRootComponent()->IsRegistered())
	{
		CollisionComp->RegisterComponent();
	}
}

void UCyLandComponent::UpdateCollisionData(bool bRebuild)
{
	UCyLandHeightfieldCollisionComponent* CollisionComp = CollisionComponent.Get();
	if (CollisionComp && bRebuild)
	{
		CollisionComp->DestroyComponent();
		CollisionComponent = CollisionComp = nullptr;
	}

	TArray<uint8> CollisionMipData;
	TArray<uint8> SimpleCollisionMipData;
	TArray<uint8> XYOffsetMipData;

	GetHeightmap()->Source.GetMipData(CollisionMipData, CollisionMipLevel);
	if (SimpleCollisionMipLevel > CollisionMipLevel)
	{
		GetHeightmap()->Source.GetMipData(SimpleCollisionMipData, SimpleCollisionMipLevel);
	}
	if (XYOffsetmapTexture)
	{
		XYOffsetmapTexture->Source.GetMipData(XYOffsetMipData, CollisionMipLevel);
	}

	UpdateCollisionHeightData(
		(FColor*)CollisionMipData.GetData(),
		SimpleCollisionMipLevel > CollisionMipLevel ? (FColor*)SimpleCollisionMipData.GetData() : nullptr,
		0, 0, MAX_int32, MAX_int32, true,
		XYOffsetmapTexture ? (FColor*)XYOffsetMipData.GetData() : nullptr);
}

void UCyLandComponent::UpdateCollisionLayerData(const FColor* const* const WeightmapTextureMipData, const FColor* const* const SimpleCollisionWeightmapTextureMipData, int32 ComponentX1, int32 ComponentY1, int32 ComponentX2, int32 ComponentY2)
{
	UCyLandInfo* Info = GetCyLandInfo();
	ACyLandProxy* Proxy = GetCyLandProxy();
	FIntPoint ComponentKey = GetSectionBase() / ComponentSizeQuads;

	UCyLandHeightfieldCollisionComponent* CollisionComp = CollisionComponent.Get();

	if (CollisionComp)
	{
		CollisionComp->Modify();

		// Simple collision is not currently supported with mesh collision components
		const bool bUsingSimpleCollision = (SimpleCollisionMipLevel > CollisionMipLevel && SimpleCollisionWeightmapTextureMipData && !XYOffsetmapTexture);

		TArray<UCyLandLayerInfoObject*> CandidateLayers;
		TArray<uint8*> CandidateDataPtrs;
		TArray<uint8*> SimpleCollisionDataPtrs;

		bool bExistingLayerMismatch = false;
		int32 DataLayerIdx = INDEX_NONE;

		// Find the layers we're interested in
		for (int32 AllocIdx = 0; AllocIdx < WeightmapLayerAllocations.Num(); AllocIdx++)
		{
			FCyWeightmapLayerAllocationInfo& AllocInfo = WeightmapLayerAllocations[AllocIdx];
			UCyLandLayerInfoObject* LayerInfo = AllocInfo.LayerInfo;
			if (LayerInfo == ACyLandProxy::VisibilityLayer || LayerInfo != nullptr)
			{
				int32 Idx = CandidateLayers.Add(LayerInfo);
				CandidateDataPtrs.Add(((uint8*)WeightmapTextureMipData[AllocInfo.WeightmapTextureIndex]) + ChannelOffsets[AllocInfo.WeightmapTextureChannel]);

				if (bUsingSimpleCollision)
				{
					SimpleCollisionDataPtrs.Add(((uint8*)SimpleCollisionWeightmapTextureMipData[AllocInfo.WeightmapTextureIndex]) + ChannelOffsets[AllocInfo.WeightmapTextureChannel]);
				}

				// Check if we still match the collision component.
				if (!CollisionComp->ComponentLayerInfos.IsValidIndex(Idx) || CollisionComp->ComponentLayerInfos[Idx] != LayerInfo)
				{
					bExistingLayerMismatch = true;
				}

				if (LayerInfo == ACyLandProxy::VisibilityLayer)
				{
					DataLayerIdx = Idx;
					bExistingLayerMismatch = true; // always rebuild whole component for hole
				}
			}
		}

		if (CandidateLayers.Num() == 0)
		{
			// No layers, so don't update any weights
			CollisionComp->DominantLayerData.RemoveBulkData();
			CollisionComp->ComponentLayerInfos.Empty();
		}
		else
		{
			const int32 CollisionSubsectionSizeVerts = ((SubsectionSizeQuads + 1) >> CollisionMipLevel);
			const int32 CollisionSubsectionSizeQuads = CollisionSubsectionSizeVerts - 1;
			const int32 CollisionSizeVerts = NumSubsections * CollisionSubsectionSizeQuads + 1;
			const int32 SimpleCollisionSubsectionSizeVerts = bUsingSimpleCollision ? ((SubsectionSizeQuads + 1) >> SimpleCollisionMipLevel) : 0;
			const int32 SimpleCollisionSubsectionSizeQuads = bUsingSimpleCollision ? SimpleCollisionSubsectionSizeVerts - 1 : 0;
			const int32 SimpleCollisionSizeVerts = bUsingSimpleCollision ? NumSubsections * SimpleCollisionSubsectionSizeQuads + 1 : 0;

			const int32 TotalCollisionSize = FMath::Square(CollisionSizeVerts) + FMath::Square(SimpleCollisionSizeVerts);

			uint8* DominantLayerData = nullptr;

			// If there's no existing data, or the layer allocations have changed, we need to update the data for the whole component.
			if (bExistingLayerMismatch || CollisionComp->DominantLayerData.GetElementCount() == 0)
			{
				ComponentX1 = 0;
				ComponentY1 = 0;
				ComponentX2 = ComponentSizeQuads;
				ComponentY2 = ComponentSizeQuads;

				CollisionComp->DominantLayerData.Lock(LOCK_READ_WRITE);
				DominantLayerData = (uint8*)CollisionComp->DominantLayerData.Realloc(TotalCollisionSize);
				FMemory::Memzero(DominantLayerData, TotalCollisionSize);

				CollisionComp->ComponentLayerInfos = CandidateLayers;
			}
			else
			{
				ComponentX1 = FMath::Min(ComponentX1, ComponentSizeQuads);
				ComponentY1 = FMath::Min(ComponentY1, ComponentSizeQuads);
				ComponentX2 = FMath::Max(ComponentX2, 0);
				ComponentY2 = FMath::Max(ComponentY2, 0);

				DominantLayerData = (uint8*)CollisionComp->DominantLayerData.Lock(LOCK_READ_WRITE);
			}

			const int32 WeightmapSizeU = WeightmapTextures[0]->Source.GetSizeX();
			const int32 MipSizeU = WeightmapSizeU >> CollisionMipLevel;

			// Ratio to convert update region coordinate to collision mip coordinates
			const float CollisionQuadRatio = (float)CollisionSubsectionSizeQuads / (float)SubsectionSizeQuads;

			// gmartin: WeightmapScaleBias not handled?

			const int32 SubSectionX1 = FMath::Max(0, FMath::DivideAndRoundDown(ComponentX1 - 1, SubsectionSizeQuads));
			const int32 SubSectionY1 = FMath::Max(0, FMath::DivideAndRoundDown(ComponentY1 - 1, SubsectionSizeQuads));
			const int32 SubSectionX2 = FMath::Min(FMath::DivideAndRoundUp(ComponentX2 + 1, SubsectionSizeQuads), NumSubsections);
			const int32 SubSectionY2 = FMath::Min(FMath::DivideAndRoundUp(ComponentY2 + 1, SubsectionSizeQuads), NumSubsections);
			for (int32 SubsectionY = SubSectionY1; SubsectionY < SubSectionY2; ++SubsectionY)
			{
				for (int32 SubsectionX = SubSectionX1; SubsectionX < SubSectionX2; ++SubsectionX)
				{
					// Area to update in subsection coordinates
					const int32 SubX1 = ComponentX1 - SubsectionSizeQuads * SubsectionX;
					const int32 SubY1 = ComponentY1 - SubsectionSizeQuads * SubsectionY;
					const int32 SubX2 = ComponentX2 - SubsectionSizeQuads * SubsectionX;
					const int32 SubY2 = ComponentY2 - SubsectionSizeQuads * SubsectionY;

					// Area to update in collision mip level coords
					const int32 CollisionSubX1 = FMath::FloorToInt((float)SubX1 * CollisionQuadRatio);
					const int32 CollisionSubY1 = FMath::FloorToInt((float)SubY1 * CollisionQuadRatio);
					const int32 CollisionSubX2 = FMath::CeilToInt( (float)SubX2 * CollisionQuadRatio);
					const int32 CollisionSubY2 = FMath::CeilToInt( (float)SubY2 * CollisionQuadRatio);

					// Clamp area to update
					const int32 VertX1 = FMath::Clamp<int32>(CollisionSubX1, 0, CollisionSubsectionSizeQuads);
					const int32 VertY1 = FMath::Clamp<int32>(CollisionSubY1, 0, CollisionSubsectionSizeQuads);
					const int32 VertX2 = FMath::Clamp<int32>(CollisionSubX2, 0, CollisionSubsectionSizeQuads);
					const int32 VertY2 = FMath::Clamp<int32>(CollisionSubY2, 0, CollisionSubsectionSizeQuads);

					for (int32 VertY = VertY1; VertY <= VertY2; VertY++)
					{
						for (int32 VertX = VertX1; VertX <= VertX2; VertX++)
						{
							// X/Y of the vertex we're looking indexed into the texture data
							const int32 TexX = CollisionSubsectionSizeVerts * SubsectionX + VertX;
							const int32 TexY = CollisionSubsectionSizeVerts * SubsectionY + VertY;
							const int32 DataOffset = (TexX + TexY * MipSizeU) * sizeof(FColor);

							uint8 DominantLayer = 255; // 255 as invalid value
							int32 DominantWeight = 0;
							for (int32 LayerIdx = 0; LayerIdx < CandidateDataPtrs.Num(); LayerIdx++)
							{
								const uint8 LayerWeight = CandidateDataPtrs[LayerIdx][DataOffset];

								if (LayerIdx == DataLayerIdx) // Override value for hole
								{
									if (LayerWeight > 170) // 255 * 0.66...
									{
										DominantLayer = LayerIdx;
										DominantWeight = INT_MAX;
									}
								}
								else if (LayerWeight > DominantWeight)
								{
									DominantLayer = LayerIdx;
									DominantWeight = LayerWeight;
								}
							}

							// this uses Quads as we don't want the duplicated vertices
							const int32 CompVertX = CollisionSubsectionSizeQuads * SubsectionX + VertX;
							const int32 CompVertY = CollisionSubsectionSizeQuads * SubsectionY + VertY;

							// Set collision data
							DominantLayerData[CompVertX + CompVertY*CollisionSizeVerts] = DominantLayer;
						}
					}
				}
			}

			if (bUsingSimpleCollision)
			{
				const int32 SimpleMipSizeU = WeightmapSizeU >> SimpleCollisionMipLevel;

				// Ratio to convert update region coordinate to collision mip coordinates
				const float SimpleCollisionQuadRatio = (float)SimpleCollisionSubsectionSizeQuads / (float)SubsectionSizeQuads;

				// gmartin: WeightmapScaleBias not handled?

				uint8* const SimpleCollisionHeightData = DominantLayerData + FMath::Square(CollisionSizeVerts);

				for (int32 SubsectionY = SubSectionY1; SubsectionY < SubSectionY2; ++SubsectionY)
				{
					for (int32 SubsectionX = SubSectionX1; SubsectionX < SubSectionX2; ++SubsectionX)
					{
						// Area to update in subsection coordinates
						const int32 SubX1 = ComponentX1 - SubsectionSizeQuads * SubsectionX;
						const int32 SubY1 = ComponentY1 - SubsectionSizeQuads * SubsectionY;
						const int32 SubX2 = ComponentX2 - SubsectionSizeQuads * SubsectionX;
						const int32 SubY2 = ComponentY2 - SubsectionSizeQuads * SubsectionY;

						// Area to update in simple collision mip level coords
						const int32 SimpleCollisionSubX1 = FMath::FloorToInt((float)SubX1 * SimpleCollisionQuadRatio);
						const int32 SimpleCollisionSubY1 = FMath::FloorToInt((float)SubY1 * SimpleCollisionQuadRatio);
						const int32 SimpleCollisionSubX2 = FMath::CeilToInt( (float)SubX2 * SimpleCollisionQuadRatio);
						const int32 SimpleCollisionSubY2 = FMath::CeilToInt( (float)SubY2 * SimpleCollisionQuadRatio);

						// Clamp area to update
						const int32 SimpleVertX1 = FMath::Clamp<int32>(SimpleCollisionSubX1, 0, SimpleCollisionSubsectionSizeQuads);
						const int32 SimpleVertY1 = FMath::Clamp<int32>(SimpleCollisionSubY1, 0, SimpleCollisionSubsectionSizeQuads);
						const int32 SimpleVertX2 = FMath::Clamp<int32>(SimpleCollisionSubX2, 0, SimpleCollisionSubsectionSizeQuads);
						const int32 SimpleVertY2 = FMath::Clamp<int32>(SimpleCollisionSubY2, 0, SimpleCollisionSubsectionSizeQuads);

						for (int32 VertY = SimpleVertY1; VertY <= SimpleVertY2; VertY++)
						{
							for (int32 VertX = SimpleVertX1; VertX <= SimpleVertX2; VertX++)
							{
								// X/Y of the vertex we're looking indexed into the texture data
								const int32 TexX = SimpleCollisionSubsectionSizeVerts * SubsectionX + VertX;
								const int32 TexY = SimpleCollisionSubsectionSizeVerts * SubsectionY + VertY;
								const int32 DataOffset = (TexX + TexY * SimpleMipSizeU) * sizeof(FColor);

								uint8 DominantLayer = 255; // 255 as invalid value
								int32 DominantWeight = 0;
								for (int32 LayerIdx = 0; LayerIdx < CandidateDataPtrs.Num(); LayerIdx++)
								{
									const uint8 LayerWeight = SimpleCollisionDataPtrs[LayerIdx][DataOffset];

									if (LayerIdx == DataLayerIdx) // Override value for hole
									{
										if (LayerWeight > 170) // 255 * 0.66...
										{
											DominantLayer = LayerIdx;
											DominantWeight = INT_MAX;
										}
									}
									else if (LayerWeight > DominantWeight)
									{
										DominantLayer = LayerIdx;
										DominantWeight = LayerWeight;
									}
								}

								// this uses Quads as we don't want the duplicated vertices
								const int32 CompVertX = SimpleCollisionSubsectionSizeQuads * SubsectionX + VertX;
								const int32 CompVertY = SimpleCollisionSubsectionSizeQuads * SubsectionY + VertY;

								// Set collision data
								DominantLayerData[CompVertX + CompVertY * SimpleCollisionSizeVerts] = DominantLayer;
							}
						}
					}
				}
			}

			CollisionComp->DominantLayerData.Unlock();
		}

		// We do not force an update of the physics data here. We don't need the layer information in the editor and it
		// causes problems if we update it multiple times in a single frame.
	}
}


void UCyLandComponent::UpdateCollisionLayerData()
{
	// Generate the dominant layer data
	TArray<FColor*> WeightmapTextureMipData;
	TArray<TArray<uint8>> CachedWeightmapTextureMipData;

	WeightmapTextureMipData.Empty(WeightmapTextures.Num());
	CachedWeightmapTextureMipData.Empty(WeightmapTextures.Num());
	for (int32 WeightmapIdx = 0; WeightmapIdx < WeightmapTextures.Num(); ++WeightmapIdx)
	{
		TArray<uint8> MipData;
		WeightmapTextures[WeightmapIdx]->Source.GetMipData(MipData, CollisionMipLevel);
		WeightmapTextureMipData.Add((FColor*)MipData.GetData());
		CachedWeightmapTextureMipData.Add(MoveTemp(MipData));
	}

	TArray<FColor*> SimpleCollisionWeightmapMipData;
	TArray<TArray<uint8>> SimpleCollisionCachedWeightmapTextureMipData;
	if (SimpleCollisionMipLevel > CollisionMipLevel)
	{
		for (int32 WeightmapIdx = 0; WeightmapIdx < WeightmapTextures.Num(); ++WeightmapIdx)
		{
			TArray<uint8> MipData;
			WeightmapTextures[WeightmapIdx]->Source.GetMipData(MipData, SimpleCollisionMipLevel);
			SimpleCollisionWeightmapMipData.Add((FColor*)MipData.GetData());
			SimpleCollisionCachedWeightmapTextureMipData.Add(MoveTemp(MipData));
		}
	}

	UpdateCollisionLayerData(WeightmapTextureMipData.GetData(), SimpleCollisionWeightmapMipData.GetData());
}




void UCyLandComponent::GenerateHeightmapMips(TArray<FColor*>& HeightmapTextureMipData, int32 ComponentX1/*=0*/, int32 ComponentY1/*=0*/, int32 ComponentX2/*=MAX_int32*/, int32 ComponentY2/*=MAX_int32*/, FCyLandTextureDataInfo* TextureDataInfo/*=nullptr*/)
{
	bool EndX = false;
	bool EndY = false;

	if (ComponentX1 == MAX_int32)
	{
		EndX = true;
		ComponentX1 = 0;
	}

	if (ComponentY1 == MAX_int32)
	{
		EndY = true;
		ComponentY1 = 0;
	}

	if (ComponentX2 == MAX_int32)
	{
		ComponentX2 = ComponentSizeQuads;
	}
	if (ComponentY2 == MAX_int32)
	{
		ComponentY2 = ComponentSizeQuads;
	}

	int32 HeightmapSizeU = GetHeightmap()->Source.GetSizeX();
	int32 HeightmapSizeV = GetHeightmap()->Source.GetSizeY();

	int32 HeightmapOffsetX = FMath::RoundToInt(HeightmapScaleBias.Z * (float)HeightmapSizeU);
	int32 HeightmapOffsetY = FMath::RoundToInt(HeightmapScaleBias.W * (float)HeightmapSizeV);

	for (int32 SubsectionY = 0; SubsectionY < NumSubsections; SubsectionY++)
	{
		// Check if subsection is fully above or below the area we are interested in
		if ((ComponentY2 < SubsectionSizeQuads*SubsectionY) ||		// above
			(ComponentY1 > SubsectionSizeQuads*(SubsectionY + 1)))	// below
		{
			continue;
		}

		for (int32 SubsectionX = 0; SubsectionX < NumSubsections; SubsectionX++)
		{
			// Check if subsection is fully to the left or right of the area we are interested in
			if ((ComponentX2 < SubsectionSizeQuads*SubsectionX) ||		// left
				(ComponentX1 > SubsectionSizeQuads*(SubsectionX + 1)))	// right
			{
				continue;
			}

			// Area to update in previous mip level coords
			int32 PrevMipSubX1 = ComponentX1 - SubsectionSizeQuads*SubsectionX;
			int32 PrevMipSubY1 = ComponentY1 - SubsectionSizeQuads*SubsectionY;
			int32 PrevMipSubX2 = ComponentX2 - SubsectionSizeQuads*SubsectionX;
			int32 PrevMipSubY2 = ComponentY2 - SubsectionSizeQuads*SubsectionY;

			int32 PrevMipSubsectionSizeQuads = SubsectionSizeQuads;
			float InvPrevMipSubsectionSizeQuads = 1.0f / (float)SubsectionSizeQuads;

			int32 PrevMipSizeU = HeightmapSizeU;
			int32 PrevMipSizeV = HeightmapSizeV;

			int32 PrevMipHeightmapOffsetX = HeightmapOffsetX;
			int32 PrevMipHeightmapOffsetY = HeightmapOffsetY;

			for (int32 Mip = 1; Mip < HeightmapTextureMipData.Num(); Mip++)
			{
				int32 MipSizeU = HeightmapSizeU >> Mip;
				int32 MipSizeV = HeightmapSizeV >> Mip;

				int32 MipSubsectionSizeQuads = ((SubsectionSizeQuads + 1) >> Mip) - 1;
				float InvMipSubsectionSizeQuads = 1.0f / (float)MipSubsectionSizeQuads;

				int32 MipHeightmapOffsetX = HeightmapOffsetX >> Mip;
				int32 MipHeightmapOffsetY = HeightmapOffsetY >> Mip;

				// Area to update in current mip level coords
				int32 MipSubX1 = FMath::FloorToInt((float)MipSubsectionSizeQuads * (float)PrevMipSubX1 * InvPrevMipSubsectionSizeQuads);
				int32 MipSubY1 = FMath::FloorToInt((float)MipSubsectionSizeQuads * (float)PrevMipSubY1 * InvPrevMipSubsectionSizeQuads);
				int32 MipSubX2 = FMath::CeilToInt((float)MipSubsectionSizeQuads * (float)PrevMipSubX2 * InvPrevMipSubsectionSizeQuads);
				int32 MipSubY2 = FMath::CeilToInt((float)MipSubsectionSizeQuads * (float)PrevMipSubY2 * InvPrevMipSubsectionSizeQuads);

				// Clamp area to update
				int32 VertX1 = FMath::Clamp<int32>(MipSubX1, 0, MipSubsectionSizeQuads);
				int32 VertY1 = FMath::Clamp<int32>(MipSubY1, 0, MipSubsectionSizeQuads);
				int32 VertX2 = FMath::Clamp<int32>(MipSubX2, 0, MipSubsectionSizeQuads);
				int32 VertY2 = FMath::Clamp<int32>(MipSubY2, 0, MipSubsectionSizeQuads);

				for (int32 VertY = VertY1; VertY <= VertY2; VertY++)
				{
					for (int32 VertX = VertX1; VertX <= VertX2; VertX++)
					{
						// Convert VertX/Y into previous mip's coords
						float PrevMipVertX = (float)PrevMipSubsectionSizeQuads * (float)VertX * InvMipSubsectionSizeQuads;
						float PrevMipVertY = (float)PrevMipSubsectionSizeQuads * (float)VertY * InvMipSubsectionSizeQuads;

#if 0
						// Validate that the vertex we skip wouldn't use the updated data in the parent mip.
						// Note this validation is doesn't do anything unless you change the VertY/VertX loops 
						// above to process all verts from 0 .. MipSubsectionSizeQuads.
						if (VertX < VertX1 || VertX > VertX2)
						{
							check(FMath::CeilToInt(PrevMipVertX) < PrevMipSubX1 || FMath::FloorToInt(PrevMipVertX) > PrevMipSubX2);
							continue;
						}

						if (VertY < VertY1 || VertY > VertY2)
						{
							check(FMath::CeilToInt(PrevMipVertY) < PrevMipSubY1 || FMath::FloorToInt(PrevMipVertY) > PrevMipSubY2);
							continue;
						}
#endif

						// X/Y of the vertex we're looking indexed into the texture data
						int32 TexX = (MipHeightmapOffsetX)+(MipSubsectionSizeQuads + 1) * SubsectionX + VertX;
						int32 TexY = (MipHeightmapOffsetY)+(MipSubsectionSizeQuads + 1) * SubsectionY + VertY;

						float fPrevMipTexX = (float)(PrevMipHeightmapOffsetX)+(float)((PrevMipSubsectionSizeQuads + 1) * SubsectionX) + PrevMipVertX;
						float fPrevMipTexY = (float)(PrevMipHeightmapOffsetY)+(float)((PrevMipSubsectionSizeQuads + 1) * SubsectionY) + PrevMipVertY;

						int32 PrevMipTexX = FMath::FloorToInt(fPrevMipTexX);
						float fPrevMipTexFracX = FMath::Fractional(fPrevMipTexX);
						int32 PrevMipTexY = FMath::FloorToInt(fPrevMipTexY);
						float fPrevMipTexFracY = FMath::Fractional(fPrevMipTexY);

						checkSlow(TexX >= 0 && TexX < MipSizeU);
						checkSlow(TexY >= 0 && TexY < MipSizeV);
						checkSlow(PrevMipTexX >= 0 && PrevMipTexX < PrevMipSizeU);
						checkSlow(PrevMipTexY >= 0 && PrevMipTexY < PrevMipSizeV);

						int32 PrevMipTexX1 = FMath::Min<int32>(PrevMipTexX + 1, PrevMipSizeU - 1);
						int32 PrevMipTexY1 = FMath::Min<int32>(PrevMipTexY + 1, PrevMipSizeV - 1);

						// Padding for missing data for MIP 0
						if (Mip == 1)
						{
							if (EndX && SubsectionX == NumSubsections - 1 && VertX == VertX2)
							{
								for (int32 PaddingIdx = PrevMipTexX + PrevMipTexY * PrevMipSizeU; PaddingIdx + 1 < PrevMipTexY1 * PrevMipSizeU; ++PaddingIdx)
								{
									HeightmapTextureMipData[Mip - 1][PaddingIdx + 1] = HeightmapTextureMipData[Mip - 1][PaddingIdx];
								}
							}

							if (EndY && SubsectionX == NumSubsections - 1 && SubsectionY == NumSubsections - 1 && VertY == VertY2 && VertX == VertX2)
							{
								for (int32 PaddingYIdx = PrevMipTexY; PaddingYIdx + 1 < PrevMipSizeV; ++PaddingYIdx)
								{
									for (int32 PaddingXIdx = 0; PaddingXIdx < PrevMipSizeU; ++PaddingXIdx)
									{
										HeightmapTextureMipData[Mip - 1][PaddingXIdx + (PaddingYIdx + 1) * PrevMipSizeU] = HeightmapTextureMipData[Mip - 1][PaddingXIdx + PaddingYIdx * PrevMipSizeU];
									}
								}
							}
						}

						FColor* TexData = &(HeightmapTextureMipData[Mip])[TexX + TexY * MipSizeU];
						FColor *PreMipTexData00 = &(HeightmapTextureMipData[Mip - 1])[PrevMipTexX + PrevMipTexY * PrevMipSizeU];
						FColor *PreMipTexData01 = &(HeightmapTextureMipData[Mip - 1])[PrevMipTexX + PrevMipTexY1 * PrevMipSizeU];
						FColor *PreMipTexData10 = &(HeightmapTextureMipData[Mip - 1])[PrevMipTexX1 + PrevMipTexY * PrevMipSizeU];
						FColor *PreMipTexData11 = &(HeightmapTextureMipData[Mip - 1])[PrevMipTexX1 + PrevMipTexY1 * PrevMipSizeU];

						// Lerp height values
						uint16 PrevMipHeightValue00 = PreMipTexData00->R << 8 | PreMipTexData00->G;
						uint16 PrevMipHeightValue01 = PreMipTexData01->R << 8 | PreMipTexData01->G;
						uint16 PrevMipHeightValue10 = PreMipTexData10->R << 8 | PreMipTexData10->G;
						uint16 PrevMipHeightValue11 = PreMipTexData11->R << 8 | PreMipTexData11->G;
						uint16 HeightValue = FMath::RoundToInt(
							FMath::Lerp(
							FMath::Lerp((float)PrevMipHeightValue00, (float)PrevMipHeightValue10, fPrevMipTexFracX),
							FMath::Lerp((float)PrevMipHeightValue01, (float)PrevMipHeightValue11, fPrevMipTexFracX),
							fPrevMipTexFracY));

						TexData->R = HeightValue >> 8;
						TexData->G = HeightValue & 255;

						// Lerp tangents
						TexData->B = FMath::RoundToInt(
							FMath::Lerp(
							FMath::Lerp((float)PreMipTexData00->B, (float)PreMipTexData10->B, fPrevMipTexFracX),
							FMath::Lerp((float)PreMipTexData01->B, (float)PreMipTexData11->B, fPrevMipTexFracX),
							fPrevMipTexFracY));

						TexData->A = FMath::RoundToInt(
							FMath::Lerp(
							FMath::Lerp((float)PreMipTexData00->A, (float)PreMipTexData10->A, fPrevMipTexFracX),
							FMath::Lerp((float)PreMipTexData01->A, (float)PreMipTexData11->A, fPrevMipTexFracX),
							fPrevMipTexFracY));

						// Padding for missing data
						if (EndX && SubsectionX == NumSubsections - 1 && VertX == VertX2)
						{
							for (int32 PaddingIdx = TexX + TexY * MipSizeU; PaddingIdx + 1 < (TexY + 1) * MipSizeU; ++PaddingIdx)
							{
								HeightmapTextureMipData[Mip][PaddingIdx + 1] = HeightmapTextureMipData[Mip][PaddingIdx];
							}
						}

						if (EndY && SubsectionX == NumSubsections - 1 && SubsectionY == NumSubsections - 1 && VertY == VertY2 && VertX == VertX2)
						{
							for (int32 PaddingYIdx = TexY; PaddingYIdx + 1 < MipSizeV; ++PaddingYIdx)
							{
								for (int32 PaddingXIdx = 0; PaddingXIdx < MipSizeU; ++PaddingXIdx)
								{
									HeightmapTextureMipData[Mip][PaddingXIdx + (PaddingYIdx + 1) * MipSizeU] = HeightmapTextureMipData[Mip][PaddingXIdx + PaddingYIdx * MipSizeU];
								}
							}
						}

					}
				}

				// Record the areas we updated
				if (TextureDataInfo)
				{
					int32 TexX1 = (MipHeightmapOffsetX)+(MipSubsectionSizeQuads + 1) * SubsectionX + VertX1;
					int32 TexY1 = (MipHeightmapOffsetY)+(MipSubsectionSizeQuads + 1) * SubsectionY + VertY1;
					int32 TexX2 = (MipHeightmapOffsetX)+(MipSubsectionSizeQuads + 1) * SubsectionX + VertX2;
					int32 TexY2 = (MipHeightmapOffsetY)+(MipSubsectionSizeQuads + 1) * SubsectionY + VertY2;
					TextureDataInfo->AddMipUpdateRegion(Mip, TexX1, TexY1, TexX2, TexY2);
				}

				// Copy current mip values to prev as we move to the next mip.
				PrevMipSubsectionSizeQuads = MipSubsectionSizeQuads;
				InvPrevMipSubsectionSizeQuads = InvMipSubsectionSizeQuads;

				PrevMipSizeU = MipSizeU;
				PrevMipSizeV = MipSizeV;

				PrevMipHeightmapOffsetX = MipHeightmapOffsetX;
				PrevMipHeightmapOffsetY = MipHeightmapOffsetY;

				// Use this mip's area as we move to the next mip
				PrevMipSubX1 = MipSubX1;
				PrevMipSubY1 = MipSubY1;
				PrevMipSubX2 = MipSubX2;
				PrevMipSubY2 = MipSubY2;
			}
		}
	}
}

void UCyLandComponent::CreateEmptyTextureMips(UTexture2D* Texture, bool bClear /*= false*/)
{
	ETextureSourceFormat WeightmapFormat = Texture->Source.GetFormat();
	int32 WeightmapSizeU = Texture->Source.GetSizeX();
	int32 WeightmapSizeV = Texture->Source.GetSizeY();

	if (bClear)
	{
		Texture->Source.Init2DWithMipChain(WeightmapSizeU, WeightmapSizeV, WeightmapFormat);
		int32 NumMips = Texture->Source.GetNumMips();
		for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
		{
			uint8* MipData = Texture->Source.LockMip(MipIndex);
			FMemory::Memzero(MipData, Texture->Source.CalcMipSize(MipIndex));
			Texture->Source.UnlockMip(MipIndex);
		}
	}
	else
	{
		TArray<uint8> TopMipData;
		Texture->Source.GetMipData(TopMipData, 0);
		Texture->Source.Init2DWithMipChain(WeightmapSizeU, WeightmapSizeV, WeightmapFormat);
		int32 NumMips = Texture->Source.GetNumMips();
		uint8* MipData = Texture->Source.LockMip(0);
		FMemory::Memcpy(MipData, TopMipData.GetData(), TopMipData.Num());
		Texture->Source.UnlockMip(0);
	}
}

template<typename DataType>
void UCyLandComponent::GenerateMipsTempl(int32 InNumSubsections, int32 InSubsectionSizeQuads, UTexture2D* Texture, DataType* BaseMipData)
{
	// Stores pointers to the locked mip data
	TArray<DataType*> MipData;
	MipData.Add(BaseMipData);
	for (int32 MipIndex = 1; MipIndex < Texture->Source.GetNumMips(); ++MipIndex)
	{
		MipData.Add((DataType*)Texture->Source.LockMip(MipIndex));
	}

	// Update the newly created mips
	UpdateMipsTempl<DataType>(InNumSubsections, InSubsectionSizeQuads, Texture, MipData);

	// Unlock all the new mips, but not the base mip's data
	for (int32 i = 1; i < MipData.Num(); i++)
	{
		Texture->Source.UnlockMip(i);
	}
}

void UCyLandComponent::GenerateWeightmapMips(int32 InNumSubsections, int32 InSubsectionSizeQuads, UTexture2D* WeightmapTexture, FColor* BaseMipData)
{
	GenerateMipsTempl<FColor>(InNumSubsections, InSubsectionSizeQuads, WeightmapTexture, BaseMipData);
}

namespace
{
	template<typename DataType>
	void BiLerpTextureData(DataType* Output, const DataType* Data00, const DataType* Data10, const DataType* Data01, const DataType* Data11, float FracX, float FracY)
	{
		*Output = FMath::RoundToInt(
			FMath::Lerp(
			FMath::Lerp((float)*Data00, (float)*Data10, FracX),
			FMath::Lerp((float)*Data01, (float)*Data11, FracX),
			FracY));
	}

	template<>
	void BiLerpTextureData(FColor* Output, const FColor* Data00, const FColor* Data10, const FColor* Data01, const FColor* Data11, float FracX, float FracY)
	{
		Output->R = FMath::RoundToInt(
			FMath::Lerp(
			FMath::Lerp((float)Data00->R, (float)Data10->R, FracX),
			FMath::Lerp((float)Data01->R, (float)Data11->R, FracX),
			FracY));
		Output->G = FMath::RoundToInt(
			FMath::Lerp(
			FMath::Lerp((float)Data00->G, (float)Data10->G, FracX),
			FMath::Lerp((float)Data01->G, (float)Data11->G, FracX),
			FracY));
		Output->B = FMath::RoundToInt(
			FMath::Lerp(
			FMath::Lerp((float)Data00->B, (float)Data10->B, FracX),
			FMath::Lerp((float)Data01->B, (float)Data11->B, FracX),
			FracY));
		Output->A = FMath::RoundToInt(
			FMath::Lerp(
			FMath::Lerp((float)Data00->A, (float)Data10->A, FracX),
			FMath::Lerp((float)Data01->A, (float)Data11->A, FracX),
			FracY));
	}

	template<typename DataType>
	void AverageTexData(DataType* Output, const DataType* Data00, const DataType* Data10, const DataType* Data01, const DataType* Data11)
	{
		*Output = (((int32)(*Data00) + (int32)(*Data10) + (int32)(*Data01) + (int32)(*Data11)) >> 2);
	}

	template<>
	void AverageTexData(FColor* Output, const FColor* Data00, const FColor* Data10, const FColor* Data01, const FColor* Data11)
	{
		Output->R = (((int32)Data00->R + (int32)Data10->R + (int32)Data01->R + (int32)Data11->R) >> 2);
		Output->G = (((int32)Data00->G + (int32)Data10->G + (int32)Data01->G + (int32)Data11->G) >> 2);
		Output->B = (((int32)Data00->B + (int32)Data10->B + (int32)Data01->B + (int32)Data11->B) >> 2);
		Output->A = (((int32)Data00->A + (int32)Data10->A + (int32)Data01->A + (int32)Data11->A) >> 2);
	}

};

template<typename DataType>
void UCyLandComponent::UpdateMipsTempl(int32 InNumSubsections, int32 InSubsectionSizeQuads, UTexture2D* Texture, TArray<DataType*>& TextureMipData, int32 ComponentX1/*=0*/, int32 ComponentY1/*=0*/, int32 ComponentX2/*=MAX_int32*/, int32 ComponentY2/*=MAX_int32*/, struct FCyLandTextureDataInfo* TextureDataInfo/*=nullptr*/)
{
	int32 WeightmapSizeU = Texture->Source.GetSizeX();
	int32 WeightmapSizeV = Texture->Source.GetSizeY();

	// Find the maximum mip where each texel's data comes from just one subsection.
	int32 MaxWholeSubsectionMip = FMath::FloorLog2(InSubsectionSizeQuads + 1) - 1;

	// Update the mip where each texel's data comes from just one subsection.
	for (int32 SubsectionY = 0; SubsectionY < InNumSubsections; SubsectionY++)
	{
		// Check if subsection is fully above or below the area we are interested in
		if ((ComponentY2 < InSubsectionSizeQuads*SubsectionY) ||	// above
			(ComponentY1 > InSubsectionSizeQuads*(SubsectionY + 1)))	// below
		{
			continue;
		}

		for (int32 SubsectionX = 0; SubsectionX < InNumSubsections; SubsectionX++)
		{
			// Check if subsection is fully to the left or right of the area we are interested in
			if ((ComponentX2 < InSubsectionSizeQuads*SubsectionX) ||	// left
				(ComponentX1 > InSubsectionSizeQuads*(SubsectionX + 1)))	// right
			{
				continue;
			}

			// Area to update in previous mip level coords
			int32 PrevMipSubX1 = ComponentX1 - InSubsectionSizeQuads*SubsectionX;
			int32 PrevMipSubY1 = ComponentY1 - InSubsectionSizeQuads*SubsectionY;
			int32 PrevMipSubX2 = ComponentX2 - InSubsectionSizeQuads*SubsectionX;
			int32 PrevMipSubY2 = ComponentY2 - InSubsectionSizeQuads*SubsectionY;

			int32 PrevMipSubsectionSizeQuads = InSubsectionSizeQuads;
			float InvPrevMipSubsectionSizeQuads = 1.0f / (float)InSubsectionSizeQuads;

			int32 PrevMipSizeU = WeightmapSizeU;
			int32 PrevMipSizeV = WeightmapSizeV;

			for (int32 Mip = 1; Mip <= MaxWholeSubsectionMip; Mip++)
			{
				int32 MipSizeU = WeightmapSizeU >> Mip;
				int32 MipSizeV = WeightmapSizeV >> Mip;

				int32 MipSubsectionSizeQuads = ((InSubsectionSizeQuads + 1) >> Mip) - 1;
				float InvMipSubsectionSizeQuads = 1.0f / (float)MipSubsectionSizeQuads;

				// Area to update in current mip level coords
				int32 MipSubX1 = FMath::FloorToInt((float)MipSubsectionSizeQuads * (float)PrevMipSubX1 * InvPrevMipSubsectionSizeQuads);
				int32 MipSubY1 = FMath::FloorToInt((float)MipSubsectionSizeQuads * (float)PrevMipSubY1 * InvPrevMipSubsectionSizeQuads);
				int32 MipSubX2 = FMath::CeilToInt((float)MipSubsectionSizeQuads * (float)PrevMipSubX2 * InvPrevMipSubsectionSizeQuads);
				int32 MipSubY2 = FMath::CeilToInt((float)MipSubsectionSizeQuads * (float)PrevMipSubY2 * InvPrevMipSubsectionSizeQuads);

				// Clamp area to update
				int32 VertX1 = FMath::Clamp<int32>(MipSubX1, 0, MipSubsectionSizeQuads);
				int32 VertY1 = FMath::Clamp<int32>(MipSubY1, 0, MipSubsectionSizeQuads);
				int32 VertX2 = FMath::Clamp<int32>(MipSubX2, 0, MipSubsectionSizeQuads);
				int32 VertY2 = FMath::Clamp<int32>(MipSubY2, 0, MipSubsectionSizeQuads);

				for (int32 VertY = VertY1; VertY <= VertY2; VertY++)
				{
					for (int32 VertX = VertX1; VertX <= VertX2; VertX++)
					{
						// Convert VertX/Y into previous mip's coords
						float PrevMipVertX = (float)PrevMipSubsectionSizeQuads * (float)VertX * InvMipSubsectionSizeQuads;
						float PrevMipVertY = (float)PrevMipSubsectionSizeQuads * (float)VertY * InvMipSubsectionSizeQuads;

						// X/Y of the vertex we're looking indexed into the texture data
						int32 TexX = (MipSubsectionSizeQuads + 1) * SubsectionX + VertX;
						int32 TexY = (MipSubsectionSizeQuads + 1) * SubsectionY + VertY;

						float fPrevMipTexX = (float)((PrevMipSubsectionSizeQuads + 1) * SubsectionX) + PrevMipVertX;
						float fPrevMipTexY = (float)((PrevMipSubsectionSizeQuads + 1) * SubsectionY) + PrevMipVertY;

						int32 PrevMipTexX = FMath::FloorToInt(fPrevMipTexX);
						float fPrevMipTexFracX = FMath::Fractional(fPrevMipTexX);
						int32 PrevMipTexY = FMath::FloorToInt(fPrevMipTexY);
						float fPrevMipTexFracY = FMath::Fractional(fPrevMipTexY);

						check(TexX >= 0 && TexX < MipSizeU);
						check(TexY >= 0 && TexY < MipSizeV);
						check(PrevMipTexX >= 0 && PrevMipTexX < PrevMipSizeU);
						check(PrevMipTexY >= 0 && PrevMipTexY < PrevMipSizeV);

						int32 PrevMipTexX1 = FMath::Min<int32>(PrevMipTexX + 1, PrevMipSizeU - 1);
						int32 PrevMipTexY1 = FMath::Min<int32>(PrevMipTexY + 1, PrevMipSizeV - 1);

						DataType* TexData = &(TextureMipData[Mip])[TexX + TexY * MipSizeU];
						DataType *PreMipTexData00 = &(TextureMipData[Mip - 1])[PrevMipTexX + PrevMipTexY * PrevMipSizeU];
						DataType *PreMipTexData01 = &(TextureMipData[Mip - 1])[PrevMipTexX + PrevMipTexY1 * PrevMipSizeU];
						DataType *PreMipTexData10 = &(TextureMipData[Mip - 1])[PrevMipTexX1 + PrevMipTexY * PrevMipSizeU];
						DataType *PreMipTexData11 = &(TextureMipData[Mip - 1])[PrevMipTexX1 + PrevMipTexY1 * PrevMipSizeU];

						// Lerp weightmap data
						BiLerpTextureData<DataType>(TexData, PreMipTexData00, PreMipTexData10, PreMipTexData01, PreMipTexData11, fPrevMipTexFracX, fPrevMipTexFracY);
					}
				}

				// Record the areas we updated
				if (TextureDataInfo)
				{
					int32 TexX1 = (MipSubsectionSizeQuads + 1) * SubsectionX + VertX1;
					int32 TexY1 = (MipSubsectionSizeQuads + 1) * SubsectionY + VertY1;
					int32 TexX2 = (MipSubsectionSizeQuads + 1) * SubsectionX + VertX2;
					int32 TexY2 = (MipSubsectionSizeQuads + 1) * SubsectionY + VertY2;
					TextureDataInfo->AddMipUpdateRegion(Mip, TexX1, TexY1, TexX2, TexY2);
				}

				// Copy current mip values to prev as we move to the next mip.
				PrevMipSubsectionSizeQuads = MipSubsectionSizeQuads;
				InvPrevMipSubsectionSizeQuads = InvMipSubsectionSizeQuads;

				PrevMipSizeU = MipSizeU;
				PrevMipSizeV = MipSizeV;

				// Use this mip's area as we move to the next mip
				PrevMipSubX1 = MipSubX1;
				PrevMipSubY1 = MipSubY1;
				PrevMipSubX2 = MipSubX2;
				PrevMipSubY2 = MipSubY2;
			}
		}
	}

	// Handle mips that have texels from multiple subsections
	// not valid weight data, so just average the texels of the previous mip.
	for (int32 Mip = MaxWholeSubsectionMip + 1;; ++Mip)
	{
		int32 MipSubsectionSizeQuads = ((InSubsectionSizeQuads + 1) >> Mip) - 1;
		checkSlow(MipSubsectionSizeQuads <= 0);

		int32 MipSizeU = FMath::Max<int32>(WeightmapSizeU >> Mip, 1);
		int32 MipSizeV = FMath::Max<int32>(WeightmapSizeV >> Mip, 1);

		int32 PrevMipSizeU = FMath::Max<int32>(WeightmapSizeU >> (Mip - 1), 1);
		int32 PrevMipSizeV = FMath::Max<int32>(WeightmapSizeV >> (Mip - 1), 1);

		for (int32 Y = 0; Y < MipSizeV; Y++)
		{
			for (int32 X = 0; X < MipSizeU; X++)
			{
				DataType* TexData = &(TextureMipData[Mip])[X + Y * MipSizeU];

				DataType *PreMipTexData00 = &(TextureMipData[Mip - 1])[(X * 2 + 0) + (Y * 2 + 0)  * PrevMipSizeU];
				DataType *PreMipTexData01 = &(TextureMipData[Mip - 1])[(X * 2 + 0) + (Y * 2 + 1)  * PrevMipSizeU];
				DataType *PreMipTexData10 = &(TextureMipData[Mip - 1])[(X * 2 + 1) + (Y * 2 + 0)  * PrevMipSizeU];
				DataType *PreMipTexData11 = &(TextureMipData[Mip - 1])[(X * 2 + 1) + (Y * 2 + 1)  * PrevMipSizeU];

				AverageTexData<DataType>(TexData, PreMipTexData00, PreMipTexData10, PreMipTexData01, PreMipTexData11);
			}
		}

		if (TextureDataInfo)
		{
			// These mip sizes are small enough that we may as well just update the whole mip.
			TextureDataInfo->AddMipUpdateRegion(Mip, 0, 0, MipSizeU - 1, MipSizeV - 1);
		}

		if (MipSizeU == 1 && MipSizeV == 1)
		{
			break;
		}
	}
}

void UCyLandComponent::UpdateWeightmapMips(int32 InNumSubsections, int32 InSubsectionSizeQuads, UTexture2D* WeightmapTexture, TArray<FColor*>& WeightmapTextureMipData, int32 ComponentX1/*=0*/, int32 ComponentY1/*=0*/, int32 ComponentX2/*=MAX_int32*/, int32 ComponentY2/*=MAX_int32*/, struct FCyLandTextureDataInfo* TextureDataInfo/*=nullptr*/)
{
	UpdateMipsTempl<FColor>(InNumSubsections, InSubsectionSizeQuads, WeightmapTexture, WeightmapTextureMipData, ComponentX1, ComponentY1, ComponentX2, ComponentY2, TextureDataInfo);
}

void UCyLandComponent::UpdateDataMips(int32 InNumSubsections, int32 InSubsectionSizeQuads, UTexture2D* Texture, TArray<uint8*>& TextureMipData, int32 ComponentX1/*=0*/, int32 ComponentY1/*=0*/, int32 ComponentX2/*=MAX_int32*/, int32 ComponentY2/*=MAX_int32*/, struct FCyLandTextureDataInfo* TextureDataInfo/*=nullptr*/)
{
	UpdateMipsTempl<uint8>(InNumSubsections, InSubsectionSizeQuads, Texture, TextureMipData, ComponentX1, ComponentY1, ComponentX2, ComponentY2, TextureDataInfo);
}

float UCyLandComponent::GetLayerWeightAtLocation(const FVector& InLocation, UCyLandLayerInfoObject* LayerInfo, TArray<uint8>* LayerCache)
{
	// Allocate and discard locally if no external cache is passed in.
	TArray<uint8> LocalCache;
	if (LayerCache == nullptr)
	{
		LayerCache = &LocalCache;
	}

	// Fill the cache if necessary
	if (LayerCache->Num() == 0)
	{
		FCyLandComponentDataInterface CDI(this);
		if (!CDI.GetWeightmapTextureData(LayerInfo, *LayerCache))
		{
			// no data for this layer for this component.
			return 0.0f;
		}
	}

	// Find location
	// TODO: Root landscape isn't always loaded, would Proxy suffice?
	if (ACyLand* CyLand = GetCyLandActor())
	{
		const FVector DrawScale = CyLand->GetRootComponent()->RelativeScale3D;
		float TestX = (InLocation.X - CyLand->GetActorLocation().X) / DrawScale.X - (float)GetSectionBase().X;
		float TestY = (InLocation.Y - CyLand->GetActorLocation().Y) / DrawScale.Y - (float)GetSectionBase().Y;

		// Abort if the test location is not on this component
		if (TestX < 0 || TestY < 0 || TestX > ComponentSizeQuads || TestY > ComponentSizeQuads)
		{
			return 0.0f;
		}

		// Find data
		int32 X1 = FMath::FloorToInt(TestX);
		int32 Y1 = FMath::FloorToInt(TestY);
		int32 X2 = FMath::CeilToInt(TestX);
		int32 Y2 = FMath::CeilToInt(TestY);

		int32 Stride = (SubsectionSizeQuads + 1) * NumSubsections;

		// Min is to prevent the sampling of the final column from overflowing
		int32 IdxX1 = FMath::Min<int32>(((X1 / SubsectionSizeQuads) * (SubsectionSizeQuads + 1)) + (X1 % SubsectionSizeQuads), Stride - 1);
		int32 IdxY1 = FMath::Min<int32>(((Y1 / SubsectionSizeQuads) * (SubsectionSizeQuads + 1)) + (Y1 % SubsectionSizeQuads), Stride - 1);
		int32 IdxX2 = FMath::Min<int32>(((X2 / SubsectionSizeQuads) * (SubsectionSizeQuads + 1)) + (X2 % SubsectionSizeQuads), Stride - 1);
		int32 IdxY2 = FMath::Min<int32>(((Y2 / SubsectionSizeQuads) * (SubsectionSizeQuads + 1)) + (Y2 % SubsectionSizeQuads), Stride - 1);

		// sample
		float Sample11 = (float)((*LayerCache)[IdxX1 + Stride*IdxY1]) / 255.0f;
		float Sample21 = (float)((*LayerCache)[IdxX2 + Stride*IdxY1]) / 255.0f;
		float Sample12 = (float)((*LayerCache)[IdxX1 + Stride*IdxY2]) / 255.0f;
		float Sample22 = (float)((*LayerCache)[IdxX2 + Stride*IdxY2]) / 255.0f;

		float LerpX = FMath::Fractional(TestX);
		float LerpY = FMath::Fractional(TestY);

		// Bilinear interpolate
		return FMath::Lerp(
			FMath::Lerp(Sample11, Sample21, LerpX),
			FMath::Lerp(Sample12, Sample22, LerpX),
			LerpY);
	}
	
	return 0.f;	//if landscape is null we just return 0 instead of crashing. Seen cases where this happens, seems like a bug?
}

void UCyLandComponent::GetComponentExtent(int32& MinX, int32& MinY, int32& MaxX, int32& MaxY) const
{
	MinX = FMath::Min(SectionBaseX, MinX);
	MinY = FMath::Min(SectionBaseY, MinY);
	MaxX = FMath::Max(SectionBaseX + ComponentSizeQuads, MaxX);
	MaxY = FMath::Max(SectionBaseY + ComponentSizeQuads, MaxY);
}

//
// ACyLand
//

#define MAX_LANDSCAPE_SUBSECTIONS 2

void UCyLandInfo::GetComponentsInRegion(int32 X1, int32 Y1, int32 X2, int32 Y2, TSet<UCyLandComponent*>& OutComponents, bool bOverlap) const
{
	// Find component range for this block of data
	// X2/Y2 Coordinates are "inclusive" max values
	int32 ComponentIndexX1, ComponentIndexY1, ComponentIndexX2, ComponentIndexY2;
	if (bOverlap)
	{
		ACyLand::CalcComponentIndicesOverlap(X1, Y1, X2, Y2, ComponentSizeQuads, ComponentIndexX1, ComponentIndexY1, ComponentIndexX2, ComponentIndexY2);
	}
	else
	{
		ACyLand::CalcComponentIndicesNoOverlap(X1, Y1, X2, Y2, ComponentSizeQuads, ComponentIndexX1, ComponentIndexY1, ComponentIndexX2, ComponentIndexY2);
	}

	for (int32 ComponentIndexY = ComponentIndexY1; ComponentIndexY <= ComponentIndexY2; ComponentIndexY++)
	{
		for (int32 ComponentIndexX = ComponentIndexX1; ComponentIndexX <= ComponentIndexX2; ComponentIndexX++)
		{
			UCyLandComponent* Component = XYtoComponentMap.FindRef(FIntPoint(ComponentIndexX, ComponentIndexY));
			if (Component && !FLevelUtils::IsLevelLocked(Component->GetCyLandProxy()->GetLevel()) && FLevelUtils::IsLevelVisible(Component->GetCyLandProxy()->GetLevel()))
			{
				OutComponents.Add(Component);
			}
		}
	}
}

// A struct to remember where we have spare texture channels.
struct FWeightmapTextureAllocation
{
	int32 X;
	int32 Y;
	int32 ChannelsInUse;
	UTexture2D* Texture;
	FColor* TextureData;

	FWeightmapTextureAllocation(int32 InX, int32 InY, int32 InChannels, UTexture2D* InTexture, FColor* InTextureData)
		: X(InX)
		, Y(InY)
		, ChannelsInUse(InChannels)
		, Texture(InTexture)
		, TextureData(InTextureData)
	{}
};

// A struct to hold the info about each texture chunk of the total heightmap
struct FHeightmapInfo
{
	int32 HeightmapSizeU;
	int32 HeightmapSizeV;
	UTexture2D* HeightmapTexture;
	TArray<FColor*> HeightmapTextureMipData;
};

TArray<FName> ACyLandProxy::GetLayersFromMaterial(UMaterialInterface* MaterialInterface)
{
	TArray<FName> Result;

	if (MaterialInterface)
	{
		TArray<FMaterialParameterInfo> OutParameterInfo;
		TArray<FGuid> Guids;
		if (UMaterialInstance* Instance = Cast<UMaterialInstance>(MaterialInterface))
		{
			Instance->GetAllParameterInfo<UMaterialExpressionLandscapeLayerBlend>(OutParameterInfo, Guids);
			Instance->GetAllParameterInfo<UMaterialExpressionLandscapeLayerWeight>(OutParameterInfo, Guids);
			Instance->GetAllParameterInfo<UMaterialExpressionLandscapeLayerSwitch>(OutParameterInfo, Guids);
			Instance->GetAllParameterInfo<UMaterialExpressionLandscapeLayerSample>(OutParameterInfo, Guids);
		}
		else if (UMaterial* Material = MaterialInterface->GetMaterial())
		{
			Material->GetAllParameterInfo<UMaterialExpressionLandscapeLayerBlend>(OutParameterInfo, Guids);
			Material->GetAllParameterInfo<UMaterialExpressionLandscapeLayerWeight>(OutParameterInfo, Guids);
			Material->GetAllParameterInfo<UMaterialExpressionLandscapeLayerSwitch>(OutParameterInfo, Guids);
			Material->GetAllParameterInfo<UMaterialExpressionLandscapeLayerSample>(OutParameterInfo, Guids);
		}

		for (const FMaterialParameterInfo& ParameterInfo : OutParameterInfo)
		{
			Result.AddUnique(ParameterInfo.Name);
		}
	}

	return Result;
}

TArray<FName> ACyLandProxy::GetLayersFromMaterial() const
{
	return GetLayersFromMaterial(CyLandMaterial);
}

UCyLandLayerInfoObject* ACyLandProxy::CreateLayerInfo(const TCHAR* LayerName, ULevel* Level)
{
	FName LayerObjectName = FName(*FString::Printf(TEXT("LayerInfoObject_%s"), LayerName));
	FString Path = Level->GetOutermost()->GetName() + TEXT("_sharedassets/");
	if (Path.StartsWith("/Temp/"))
	{
		Path = FString("/Game/") + Path.RightChop(FString("/Temp/").Len());
	}
	FString PackageName = Path + LayerObjectName.ToString();
	FString PackageFilename;
	int32 Suffix = 1;
	while (FPackageName::DoesPackageExist(PackageName, nullptr, &PackageFilename))
	{
		LayerObjectName = FName(*FString::Printf(TEXT("LayerInfoObject_%s_%d"), LayerName, Suffix));
		PackageName = Path + LayerObjectName.ToString();
		Suffix++;
	}
	UPackage* Package = CreatePackage(nullptr, *PackageName);
	UCyLandLayerInfoObject* LayerInfo = NewObject<UCyLandLayerInfoObject>(Package, LayerObjectName, RF_Public | RF_Standalone | RF_Transactional);
	LayerInfo->LayerName = LayerName;

	return LayerInfo;
}

UCyLandLayerInfoObject* ACyLandProxy::CreateLayerInfo(const TCHAR* LayerName)
{
	UCyLandLayerInfoObject* LayerInfo = ACyLandProxy::CreateLayerInfo(LayerName, GetLevel());

	check(LayerInfo);

	UCyLandInfo* CyLandInfo = GetCyLandInfo();
	if (CyLandInfo)
	{
		int32 Index = CyLandInfo->GetLayerInfoIndex(LayerName, this);
		if (Index == INDEX_NONE)
		{
			CyLandInfo->Layers.Add(FCyLandInfoLayerSettings(LayerInfo, this));
		}
		else
		{
			CyLandInfo->Layers[Index].LayerInfoObj = LayerInfo;
		}
	}

	return LayerInfo;
}

#define HEIGHTDATA(X,Y) (HeightData[ FMath::Clamp<int32>(Y,0,VertsY) * VertsX + FMath::Clamp<int32>(X,0,VertsX) ])
CYLAND_API void ACyLandProxy::Imports(
	const FGuid Guid,
	const int32 MinX, const int32 MinY, const int32 MaxX, const int32 MaxY,
	const int32 InNumSubsections, const int32 InSubsectionSizeQuads,
	const uint16* const HeightData, const TCHAR* const HeightmapFileName,
	const TArray<FCyLandImportLayerInfo>& ImportLayerInfos, const ECyLandImportAlphamapType ImportLayerType)
{
	UE_LOG(LogCyLand, Warning, TEXT("ACyLandProxy Importing ... "));
	GWarn->BeginSlowTask(LOCTEXT("BeingImportingCyLandTask", "Importing CyLand"), true);

	const int32 VertsX = MaxX - MinX + 1;
	const int32 VertsY = MaxY - MinY + 1;

	ComponentSizeQuads = InNumSubsections * InSubsectionSizeQuads;
	NumSubsections = InNumSubsections;
	SubsectionSizeQuads = InSubsectionSizeQuads;
	CyLandGuid = Guid;

	Modify();

	// Create and initialize landscape info object
	UCyLandInfo* CyLandInfo = CreateCyLandInfo();

	const int32 NumPatchesX = (VertsX - 1);
	const int32 NumPatchesY = (VertsY - 1);

	const int32 NumComponentsX = NumPatchesX / ComponentSizeQuads;
	const int32 NumComponentsY = NumPatchesY / ComponentSizeQuads;

	// currently only support importing into a new/blank landscape actor/proxy
	check(CyLandComponents.Num() == 0);
	CyLandComponents.Empty(NumComponentsX * NumComponentsY);

	for (int32 Y = 0; Y < NumComponentsY; Y++)
	{
		for (int32 X = 0; X < NumComponentsX; X++)
		{
			const int32 BaseX = MinX + X * ComponentSizeQuads;
			const int32 BaseY = MinY + Y * ComponentSizeQuads;

			UCyLandComponent* CyLandComponent = NewObject<UCyLandComponent>(this, NAME_None, RF_Transactional);
			CyLandComponent->SetRelativeLocation(FVector(BaseX, BaseY, 0));
			CyLandComponent->SetupAttachment(GetRootComponent(), NAME_None);
			CyLandComponents.Add(CyLandComponent);
			CyLandComponent->Init(
				BaseX, BaseY,
				ComponentSizeQuads,
				NumSubsections,
				SubsectionSizeQuads
				);

			// Assign shared properties
			CyLandComponent->UpdatedSharedPropertiesFromActor();
		}
	}

	// Ensure that we don't pack so many heightmaps into a texture that their lowest LOD isn't guaranteed to be resident
#define MAX_HEIGHTMAP_TEXTURE_SIZE 512
	const int32 ComponentSizeVerts = NumSubsections * (SubsectionSizeQuads + 1);
	const int32 ComponentsPerHeightmap = FMath::Min(MAX_HEIGHTMAP_TEXTURE_SIZE / ComponentSizeVerts, 1 << (UTexture2D::GetMinTextureResidentMipCount() - 2));
	check(ComponentsPerHeightmap > 0);

	// Count how many heightmaps we need and the X dimension of the final heightmap
	int32 NumHeightmapsX = 1;
	int32 FinalComponentsX = NumComponentsX;
	while (FinalComponentsX > ComponentsPerHeightmap)
	{
		FinalComponentsX -= ComponentsPerHeightmap;
		NumHeightmapsX++;
	}
	// Count how many heightmaps we need and the Y dimension of the final heightmap
	int32 NumHeightmapsY = 1;
	int32 FinalComponentsY = NumComponentsY;
	while (FinalComponentsY > ComponentsPerHeightmap)
	{
		FinalComponentsY -= ComponentsPerHeightmap;
		NumHeightmapsY++;
	}

	TArray<FHeightmapInfo> HeightmapInfos;

	for (int32 HmY = 0; HmY < NumHeightmapsY; HmY++)
	{
		for (int32 HmX = 0; HmX < NumHeightmapsX; HmX++)
		{
			FHeightmapInfo& HeightmapInfo = HeightmapInfos[HeightmapInfos.AddZeroed()];

			// make sure the heightmap UVs are powers of two.
			HeightmapInfo.HeightmapSizeU = (1 << FMath::CeilLogTwo(((HmX == NumHeightmapsX - 1) ? FinalComponentsX : ComponentsPerHeightmap) * ComponentSizeVerts));
			HeightmapInfo.HeightmapSizeV = (1 << FMath::CeilLogTwo(((HmY == NumHeightmapsY - 1) ? FinalComponentsY : ComponentsPerHeightmap) * ComponentSizeVerts));

			// Construct the heightmap textures
			HeightmapInfo.HeightmapTexture = CreateCyLandTexture(HeightmapInfo.HeightmapSizeU, HeightmapInfo.HeightmapSizeV, TEXTUREGROUP_Terrain_Heightmap, TSF_BGRA8);

			int32 MipSubsectionSizeQuads = SubsectionSizeQuads;
			int32 MipSizeU = HeightmapInfo.HeightmapSizeU;
			int32 MipSizeV = HeightmapInfo.HeightmapSizeV;
			while (MipSizeU > 1 && MipSizeV > 1 && MipSubsectionSizeQuads >= 1)
			{
				int32 MipIndex = HeightmapInfo.HeightmapTextureMipData.Num();
				FColor* HeightmapTextureData = (FColor*)HeightmapInfo.HeightmapTexture->Source.LockMip(MipIndex);
				FMemory::Memzero(HeightmapTextureData, MipSizeU*MipSizeV*sizeof(FColor));
				HeightmapInfo.HeightmapTextureMipData.Add(HeightmapTextureData);

				MipSizeU >>= 1;
				MipSizeV >>= 1;

				MipSubsectionSizeQuads = ((MipSubsectionSizeQuads + 1) >> 1) - 1;
			}
		}
	}

	const FVector DrawScale3D = GetRootComponent()->RelativeScale3D;

	// Calculate the normals for each of the two triangles per quad.
	TArray<FVector> VertexNormals;
	VertexNormals.AddZeroed(VertsX * VertsY);
	for (int32 QuadY = 0; QuadY < NumPatchesY; QuadY++)
	{
		for (int32 QuadX = 0; QuadX < NumPatchesX; QuadX++)
		{
			const FVector Vert00 = FVector(0.0f, 0.0f, ((float)HEIGHTDATA(QuadX + 0, QuadY + 0) - 32768.0f)*LANDSCAPE_ZSCALE) * DrawScale3D;
			const FVector Vert01 = FVector(0.0f, 1.0f, ((float)HEIGHTDATA(QuadX + 0, QuadY + 1) - 32768.0f)*LANDSCAPE_ZSCALE) * DrawScale3D;
			const FVector Vert10 = FVector(1.0f, 0.0f, ((float)HEIGHTDATA(QuadX + 1, QuadY + 0) - 32768.0f)*LANDSCAPE_ZSCALE) * DrawScale3D;
			const FVector Vert11 = FVector(1.0f, 1.0f, ((float)HEIGHTDATA(QuadX + 1, QuadY + 1) - 32768.0f)*LANDSCAPE_ZSCALE) * DrawScale3D;

			const FVector FaceNormal1 = ((Vert00 - Vert10) ^ (Vert10 - Vert11)).GetSafeNormal();
			const FVector FaceNormal2 = ((Vert11 - Vert01) ^ (Vert01 - Vert00)).GetSafeNormal();

			// contribute to the vertex normals.
			VertexNormals[(QuadX + 1 + VertsX * (QuadY + 0))] += FaceNormal1;
			VertexNormals[(QuadX + 0 + VertsX * (QuadY + 1))] += FaceNormal2;
			VertexNormals[(QuadX + 0 + VertsX * (QuadY + 0))] += FaceNormal1 + FaceNormal2;
			VertexNormals[(QuadX + 1 + VertsX * (QuadY + 1))] += FaceNormal1 + FaceNormal2;
		}
	}

	// Weight values for each layer for each component.
	TArray<TArray<TArray<uint8>>> ComponentWeightValues;
	ComponentWeightValues.AddZeroed(NumComponentsX * NumComponentsY);

	for (int32 ComponentY = 0; ComponentY < NumComponentsY; ComponentY++)
	{
		for (int32 ComponentX = 0; ComponentX < NumComponentsX; ComponentX++)
		{
			UCyLandComponent* const CyLandComponent = CyLandComponents[ComponentX + ComponentY*NumComponentsX];
			TArray<TArray<uint8>>& WeightValues = ComponentWeightValues[ComponentX + ComponentY*NumComponentsX];

			// Import alphamap data into local array and check for unused layers for this component.
			TArray<FCyLandComponentAlphaInfo, TInlineAllocator<16>> EditingAlphaLayerData;
			for (int32 LayerIndex = 0; LayerIndex < ImportLayerInfos.Num(); LayerIndex++)
			{
				FCyLandComponentAlphaInfo* NewAlphaInfo = new(EditingAlphaLayerData) FCyLandComponentAlphaInfo(CyLandComponent, LayerIndex);

				if (ImportLayerInfos[LayerIndex].LayerData.Num())
				{
					for (int32 AlphaY = 0; AlphaY <= CyLandComponent->ComponentSizeQuads; AlphaY++)
					{
						const uint8* const OldAlphaRowStart = &ImportLayerInfos[LayerIndex].LayerData[(AlphaY + CyLandComponent->GetSectionBase().Y - MinY) * VertsX + (CyLandComponent->GetSectionBase().X - MinX)];
						uint8* const NewAlphaRowStart = &NewAlphaInfo->AlphaValues[AlphaY * (CyLandComponent->ComponentSizeQuads + 1)];
						FMemory::Memcpy(NewAlphaRowStart, OldAlphaRowStart, CyLandComponent->ComponentSizeQuads + 1);
					}
				}
			}

			for (int32 AlphaMapIndex = 0; AlphaMapIndex < EditingAlphaLayerData.Num(); AlphaMapIndex++)
			{
				if (EditingAlphaLayerData[AlphaMapIndex].IsLayerAllZero())
				{
					EditingAlphaLayerData.RemoveAt(AlphaMapIndex);
					AlphaMapIndex--;
				}
			}


			UE_LOG(LogCyLand, Log, TEXT("%s needs %d alphamaps"), *CyLandComponent->GetName(), EditingAlphaLayerData.Num());

			// Calculate weightmap weights for this component
			WeightValues.Empty(EditingAlphaLayerData.Num());
			WeightValues.AddZeroed(EditingAlphaLayerData.Num());
			CyLandComponent->WeightmapLayerAllocations.Empty(EditingAlphaLayerData.Num());

			TArray<bool, TInlineAllocator<16>> IsNoBlendArray;
			IsNoBlendArray.Empty(EditingAlphaLayerData.Num());
			IsNoBlendArray.AddZeroed(EditingAlphaLayerData.Num());

			for (int32 WeightLayerIndex = 0; WeightLayerIndex < WeightValues.Num(); WeightLayerIndex++)
			{
				// Lookup the original layer name
				WeightValues[WeightLayerIndex] = EditingAlphaLayerData[WeightLayerIndex].AlphaValues;
				new(CyLandComponent->WeightmapLayerAllocations) FCyWeightmapLayerAllocationInfo(ImportLayerInfos[EditingAlphaLayerData[WeightLayerIndex].LayerIndex].LayerInfo);
				IsNoBlendArray[WeightLayerIndex] = ImportLayerInfos[EditingAlphaLayerData[WeightLayerIndex].LayerIndex].LayerInfo->bNoWeightBlend;
			}

			// Discard the temporary alpha data
			EditingAlphaLayerData.Empty();

			if (ImportLayerType == ECyLandImportAlphamapType::Layered)
			{
				// For each layer...
				for (int32 WeightLayerIndex = WeightValues.Num() - 1; WeightLayerIndex >= 0; WeightLayerIndex--)
				{
					// ... multiply all lower layers'...
					for (int32 BelowWeightLayerIndex = WeightLayerIndex - 1; BelowWeightLayerIndex >= 0; BelowWeightLayerIndex--)
					{
						int32 TotalWeight = 0;

						if (IsNoBlendArray[BelowWeightLayerIndex])
						{
							continue; // skip no blend
						}

						// ... values by...
						for (int32 Idx = 0; Idx < WeightValues[WeightLayerIndex].Num(); Idx++)
						{
							// ... one-minus the current layer's values
							int32 NewValue = (int32)WeightValues[BelowWeightLayerIndex][Idx] * (int32)(255 - WeightValues[WeightLayerIndex][Idx]) / 255;
							WeightValues[BelowWeightLayerIndex][Idx] = (uint8)NewValue;
							TotalWeight += NewValue;
						}

						if (TotalWeight == 0)
						{
							// Remove the layer as it has no contribution
							WeightValues.RemoveAt(BelowWeightLayerIndex);
							CyLandComponent->WeightmapLayerAllocations.RemoveAt(BelowWeightLayerIndex);
							IsNoBlendArray.RemoveAt(BelowWeightLayerIndex);

							// The current layer has been re-numbered
							WeightLayerIndex--;
						}
					}
				}
			}

			// Weight normalization for total should be 255...
			if (WeightValues.Num())
			{
				for (int32 Idx = 0; Idx < WeightValues[0].Num(); Idx++)
				{
					int32 TotalWeight = 0;
					int32 MaxLayerIdx = -1;
					int32 MaxWeight = INT_MIN;

					for (int32 WeightLayerIndex = 0; WeightLayerIndex < WeightValues.Num(); WeightLayerIndex++)
					{
						if (!IsNoBlendArray[WeightLayerIndex])
						{
							int32 Weight = WeightValues[WeightLayerIndex][Idx];
							TotalWeight += Weight;
							if (MaxWeight < Weight)
							{
								MaxWeight = Weight;
								MaxLayerIdx = WeightLayerIndex;
							}
						}
					}

					if (TotalWeight == 0)
					{
						if (MaxLayerIdx >= 0)
						{
							WeightValues[MaxLayerIdx][Idx] = 255;
						}
					}
					else if (TotalWeight != 255)
					{
						// normalization...
						float Factor = 255.0f / TotalWeight;
						TotalWeight = 0;
						for (int32 WeightLayerIndex = 0; WeightLayerIndex < WeightValues.Num(); WeightLayerIndex++)
						{
							if (!IsNoBlendArray[WeightLayerIndex])
							{
								WeightValues[WeightLayerIndex][Idx] = (uint8)(Factor * WeightValues[WeightLayerIndex][Idx]);
								TotalWeight += WeightValues[WeightLayerIndex][Idx];
							}
						}

						if (255 - TotalWeight && MaxLayerIdx >= 0)
						{
							WeightValues[MaxLayerIdx][Idx] += 255 - TotalWeight;
						}
					}
				}
			}
		}
	}

	// Remember where we have spare texture channels.
	TArray<FWeightmapTextureAllocation> TextureAllocations;

	for (int32 ComponentY = 0; ComponentY < NumComponentsY; ComponentY++)
	{
		const int32 HmY = ComponentY / ComponentsPerHeightmap;
		const int32 HeightmapOffsetY = (ComponentY - ComponentsPerHeightmap*HmY) * NumSubsections * (SubsectionSizeQuads + 1);

		for (int32 ComponentX = 0; ComponentX < NumComponentsX; ComponentX++)
		{
			const int32 HmX = ComponentX / ComponentsPerHeightmap;
			const FHeightmapInfo& HeightmapInfo = HeightmapInfos[HmX + HmY * NumHeightmapsX];

			UCyLandComponent* CyLandComponent = CyLandComponents[ComponentX + ComponentY*NumComponentsX];

			// Lookup array of weight values for this component.
			const TArray<TArray<uint8>>& WeightValues = ComponentWeightValues[ComponentX + ComponentY*NumComponentsX];

			// Heightmap offsets
			const int32 HeightmapOffsetX = (ComponentX - ComponentsPerHeightmap*HmX) * NumSubsections * (SubsectionSizeQuads + 1);

			CyLandComponent->HeightmapScaleBias = FVector4(1.0f / (float)HeightmapInfo.HeightmapSizeU, 1.0f / (float)HeightmapInfo.HeightmapSizeV, (float)((HeightmapOffsetX)) / (float)HeightmapInfo.HeightmapSizeU, ((float)(HeightmapOffsetY)) / (float)HeightmapInfo.HeightmapSizeV);
			CyLandComponent->SetHeightmap(HeightmapInfo.HeightmapTexture);

			// Weightmap is sized the same as the component
			const int32 WeightmapSize = (SubsectionSizeQuads + 1) * NumSubsections;
			// Should be power of two
			check(FMath::IsPowerOfTwo(WeightmapSize));

			CyLandComponent->WeightmapScaleBias = FVector4(1.0f / (float)WeightmapSize, 1.0f / (float)WeightmapSize, 0.5f / (float)WeightmapSize, 0.5f / (float)WeightmapSize);
			CyLandComponent->WeightmapSubsectionOffset = (float)(SubsectionSizeQuads + 1) / (float)WeightmapSize;

			// Pointers to the texture data where we'll store each layer. Stride is 4 (FColor)
			TArray<uint8*> WeightmapTextureDataPointers;

			UE_LOG(LogCyLand, Log, TEXT("%s needs %d weightmap channels"), *CyLandComponent->GetName(), WeightValues.Num());

			// Find texture channels to store each layer.
			int32 LayerIndex = 0;
			while (LayerIndex < WeightValues.Num())
			{
				const int32 RemainingLayers = WeightValues.Num() - LayerIndex;

				int32 BestAllocationIndex = -1;

				// if we need less than 4 channels, try to find them somewhere to put all of them
				if (RemainingLayers < 4)
				{
					int32 BestDistSquared = MAX_int32;
					for (int32 TryAllocIdx = 0; TryAllocIdx < TextureAllocations.Num(); TryAllocIdx++)
					{
						if (TextureAllocations[TryAllocIdx].ChannelsInUse + RemainingLayers <= 4)
						{
							FWeightmapTextureAllocation& TryAllocation = TextureAllocations[TryAllocIdx];
							const int32 TryDistSquared = FMath::Square(TryAllocation.X - ComponentX) + FMath::Square(TryAllocation.Y - ComponentY);
							if (TryDistSquared < BestDistSquared)
							{
								BestDistSquared = TryDistSquared;
								BestAllocationIndex = TryAllocIdx;
							}
						}
					}
				}

				if (BestAllocationIndex != -1)
				{
					FWeightmapTextureAllocation& Allocation = TextureAllocations[BestAllocationIndex];
					FCyLandWeightmapUsage& WeightmapUsage = WeightmapUsageMap.FindChecked(Allocation.Texture);

					UE_LOG(LogCyLand, Log, TEXT("  ==> Storing %d channels starting at %s[%d]"), RemainingLayers, *Allocation.Texture->GetName(), Allocation.ChannelsInUse);

					for (int32 i = 0; i < RemainingLayers; i++)
					{
						CyLandComponent->WeightmapLayerAllocations[LayerIndex + i].WeightmapTextureIndex = CyLandComponent->WeightmapTextures.Num();
						CyLandComponent->WeightmapLayerAllocations[LayerIndex + i].WeightmapTextureChannel = Allocation.ChannelsInUse;
						WeightmapUsage.ChannelUsage[Allocation.ChannelsInUse] = CyLandComponent;
						switch (Allocation.ChannelsInUse)
						{
						case 1:
							WeightmapTextureDataPointers.Add((uint8*)&Allocation.TextureData->G);
							break;
						case 2:
							WeightmapTextureDataPointers.Add((uint8*)&Allocation.TextureData->B);
							break;
						case 3:
							WeightmapTextureDataPointers.Add((uint8*)&Allocation.TextureData->A);
							break;
						default:
							// this should not occur.
							check(0);

						}
						Allocation.ChannelsInUse++;
					}

					LayerIndex += RemainingLayers;
					CyLandComponent->WeightmapTextures.Add(Allocation.Texture);
				}
				else
				{
					// We couldn't find a suitable place for these layers, so lets make a new one.
					UTexture2D* const WeightmapTexture = CreateCyLandTexture(WeightmapSize, WeightmapSize, TEXTUREGROUP_Terrain_Weightmap, TSF_BGRA8);
					FColor* const MipData = (FColor*)WeightmapTexture->Source.LockMip(0);

					const int32 ThisAllocationLayers = FMath::Min<int32>(RemainingLayers, 4);
					new(TextureAllocations) FWeightmapTextureAllocation(ComponentX, ComponentY, ThisAllocationLayers, WeightmapTexture, MipData);
					FCyLandWeightmapUsage& WeightmapUsage = WeightmapUsageMap.Add(WeightmapTexture, FCyLandWeightmapUsage());

					UE_LOG(LogCyLand, Log, TEXT("  ==> Storing %d channels in new texture %s"), ThisAllocationLayers, *WeightmapTexture->GetName());

					WeightmapTextureDataPointers.Add((uint8*)&MipData->R);
					CyLandComponent->WeightmapLayerAllocations[LayerIndex + 0].WeightmapTextureIndex = CyLandComponent->WeightmapTextures.Num();
					CyLandComponent->WeightmapLayerAllocations[LayerIndex + 0].WeightmapTextureChannel = 0;
					WeightmapUsage.ChannelUsage[0] = CyLandComponent;

					if (ThisAllocationLayers > 1)
					{
						WeightmapTextureDataPointers.Add((uint8*)&MipData->G);
						CyLandComponent->WeightmapLayerAllocations[LayerIndex + 1].WeightmapTextureIndex = CyLandComponent->WeightmapTextures.Num();
						CyLandComponent->WeightmapLayerAllocations[LayerIndex + 1].WeightmapTextureChannel = 1;
						WeightmapUsage.ChannelUsage[1] = CyLandComponent;

						if (ThisAllocationLayers > 2)
						{
							WeightmapTextureDataPointers.Add((uint8*)&MipData->B);
							CyLandComponent->WeightmapLayerAllocations[LayerIndex + 2].WeightmapTextureIndex = CyLandComponent->WeightmapTextures.Num();
							CyLandComponent->WeightmapLayerAllocations[LayerIndex + 2].WeightmapTextureChannel = 2;
							WeightmapUsage.ChannelUsage[2] = CyLandComponent;

							if (ThisAllocationLayers > 3)
							{
								WeightmapTextureDataPointers.Add((uint8*)&MipData->A);
								CyLandComponent->WeightmapLayerAllocations[LayerIndex + 3].WeightmapTextureIndex = CyLandComponent->WeightmapTextures.Num();
								CyLandComponent->WeightmapLayerAllocations[LayerIndex + 3].WeightmapTextureChannel = 3;
								WeightmapUsage.ChannelUsage[3] = CyLandComponent;
							}
						}
					}
					CyLandComponent->WeightmapTextures.Add(WeightmapTexture);

					LayerIndex += ThisAllocationLayers;
				}
			}
			check(WeightmapTextureDataPointers.Num() == WeightValues.Num());

			FBox LocalBox(ForceInit);
			for (int32 SubsectionY = 0; SubsectionY < NumSubsections; SubsectionY++)
			{
				for (int32 SubsectionX = 0; SubsectionX < NumSubsections; SubsectionX++)
				{
					for (int32 SubY = 0; SubY <= SubsectionSizeQuads; SubY++)
					{
						for (int32 SubX = 0; SubX <= SubsectionSizeQuads; SubX++)
						{
							// X/Y of the vertex we're looking at in component's coordinates.
							const int32 CompX = SubsectionSizeQuads * SubsectionX + SubX;
							const int32 CompY = SubsectionSizeQuads * SubsectionY + SubY;

							// X/Y of the vertex we're looking indexed into the texture data
							const int32 TexX = (SubsectionSizeQuads + 1) * SubsectionX + SubX;
							const int32 TexY = (SubsectionSizeQuads + 1) * SubsectionY + SubY;

							const int32 WeightSrcDataIdx = CompY * (ComponentSizeQuads + 1) + CompX;
							const int32 HeightTexDataIdx = (HeightmapOffsetX + TexX) + (HeightmapOffsetY + TexY) * (HeightmapInfo.HeightmapSizeU);

							const int32 WeightTexDataIdx = (TexX)+(TexY)* (WeightmapSize);

							// copy height and normal data
							const uint16 HeightValue = HEIGHTDATA(CompX + CyLandComponent->GetSectionBase().X - MinX, CompY + CyLandComponent->GetSectionBase().Y - MinY);
							const FVector Normal = VertexNormals[CompX + CyLandComponent->GetSectionBase().X - MinX + VertsX * (CompY + CyLandComponent->GetSectionBase().Y - MinY)].GetSafeNormal();

							HeightmapInfo.HeightmapTextureMipData[0][HeightTexDataIdx].R = HeightValue >> 8;
							HeightmapInfo.HeightmapTextureMipData[0][HeightTexDataIdx].G = HeightValue & 255;
							HeightmapInfo.HeightmapTextureMipData[0][HeightTexDataIdx].B = FMath::RoundToInt(127.5f * (Normal.X + 1.0f));
							HeightmapInfo.HeightmapTextureMipData[0][HeightTexDataIdx].A = FMath::RoundToInt(127.5f * (Normal.Y + 1.0f));

							for (int32 WeightmapIndex = 0; WeightmapIndex < WeightValues.Num(); WeightmapIndex++)
							{
								WeightmapTextureDataPointers[WeightmapIndex][WeightTexDataIdx * 4] = WeightValues[WeightmapIndex][WeightSrcDataIdx];
							}

							// Get local space verts
							const FVector LocalVertex(CompX, CompY, CyLandDataAccess::GetLocalHeight(HeightValue));
							LocalBox += LocalVertex;
						}
					}
				}
			}

			CyLandComponent->CachedLocalBox = LocalBox;
		}
	}

	TArray<UTexture2D*> PendingTexturePlatformDataCreation;

	// Unlock the weightmaps' base mips
	for (int32 AllocationIndex = 0; AllocationIndex < TextureAllocations.Num(); AllocationIndex++)
	{
		UTexture2D* const WeightmapTexture = TextureAllocations[AllocationIndex].Texture;
		FColor* const BaseMipData = TextureAllocations[AllocationIndex].TextureData;

		// Generate mips for weightmaps
		UCyLandComponent::GenerateWeightmapMips(NumSubsections, SubsectionSizeQuads, WeightmapTexture, BaseMipData);

		WeightmapTexture->Source.UnlockMip(0);

		WeightmapTexture->BeginCachePlatformData();
		WeightmapTexture->ClearAllCachedCookedPlatformData();
		PendingTexturePlatformDataCreation.Add(WeightmapTexture);
	}

	// Generate mipmaps for the components, and create the collision components
	for (int32 ComponentY = 0; ComponentY < NumComponentsY; ComponentY++)
	{
		for (int32 ComponentX = 0; ComponentX < NumComponentsX; ComponentX++)
		{
			const int32 HmX = ComponentX / ComponentsPerHeightmap;
			const int32 HmY = ComponentY / ComponentsPerHeightmap;
			FHeightmapInfo& HeightmapInfo = HeightmapInfos[HmX + HmY * NumHeightmapsX];

			UCyLandComponent* CyLandComponent = CyLandComponents[ComponentX + ComponentY*NumComponentsX];
			CyLandComponent->GenerateHeightmapMips(HeightmapInfo.HeightmapTextureMipData, ComponentX == NumComponentsX - 1 ? MAX_int32 : 0, ComponentY == NumComponentsY - 1 ? MAX_int32 : 0);
			CyLandComponent->UpdateCollisionHeightData(
				HeightmapInfo.HeightmapTextureMipData[CyLandComponent->CollisionMipLevel],
				CyLandComponent->SimpleCollisionMipLevel > CyLandComponent->CollisionMipLevel ? HeightmapInfo.HeightmapTextureMipData[CyLandComponent->SimpleCollisionMipLevel] : nullptr);
			CyLandComponent->UpdateCollisionLayerData();
		}
	}

	for (int32 HmIdx = 0; HmIdx < HeightmapInfos.Num(); HmIdx++)
	{
		FHeightmapInfo& HeightmapInfo = HeightmapInfos[HmIdx];

		// Add remaining mips down to 1x1 to heightmap texture. These do not represent quads and are just a simple averages of the previous mipmaps. 
		// These mips are not used for sampling in the vertex shader but could be sampled in the pixel shader.
		int32 Mip = HeightmapInfo.HeightmapTextureMipData.Num();
		int32 MipSizeU = (HeightmapInfo.HeightmapTexture->Source.GetSizeX()) >> Mip;
		int32 MipSizeV = (HeightmapInfo.HeightmapTexture->Source.GetSizeY()) >> Mip;
		while (MipSizeU > 1 && MipSizeV > 1)
		{
			HeightmapInfo.HeightmapTextureMipData.Add((FColor*)HeightmapInfo.HeightmapTexture->Source.LockMip(Mip));
			const int32 PrevMipSizeU = (HeightmapInfo.HeightmapTexture->Source.GetSizeX()) >> (Mip - 1);
			const int32 PrevMipSizeV = (HeightmapInfo.HeightmapTexture->Source.GetSizeY()) >> (Mip - 1);

			for (int32 Y = 0; Y < MipSizeV; Y++)
			{
				for (int32 X = 0; X < MipSizeU; X++)
				{
					FColor* const TexData = &(HeightmapInfo.HeightmapTextureMipData[Mip])[X + Y * MipSizeU];

					const FColor* const PreMipTexData00 = &(HeightmapInfo.HeightmapTextureMipData[Mip - 1])[(X * 2 + 0) + (Y * 2 + 0) * PrevMipSizeU];
					const FColor* const PreMipTexData01 = &(HeightmapInfo.HeightmapTextureMipData[Mip - 1])[(X * 2 + 0) + (Y * 2 + 1) * PrevMipSizeU];
					const FColor* const PreMipTexData10 = &(HeightmapInfo.HeightmapTextureMipData[Mip - 1])[(X * 2 + 1) + (Y * 2 + 0) * PrevMipSizeU];
					const FColor* const PreMipTexData11 = &(HeightmapInfo.HeightmapTextureMipData[Mip - 1])[(X * 2 + 1) + (Y * 2 + 1) * PrevMipSizeU];

					TexData->R = (((int32)PreMipTexData00->R + (int32)PreMipTexData01->R + (int32)PreMipTexData10->R + (int32)PreMipTexData11->R) >> 2);
					TexData->G = (((int32)PreMipTexData00->G + (int32)PreMipTexData01->G + (int32)PreMipTexData10->G + (int32)PreMipTexData11->G) >> 2);
					TexData->B = (((int32)PreMipTexData00->B + (int32)PreMipTexData01->B + (int32)PreMipTexData10->B + (int32)PreMipTexData11->B) >> 2);
					TexData->A = (((int32)PreMipTexData00->A + (int32)PreMipTexData01->A + (int32)PreMipTexData10->A + (int32)PreMipTexData11->A) >> 2);
				}
			}
			Mip++;
			MipSizeU >>= 1;
			MipSizeV >>= 1;
		}

		for (int32 i = 0; i < HeightmapInfo.HeightmapTextureMipData.Num(); i++)
		{
			HeightmapInfo.HeightmapTexture->Source.UnlockMip(i);
		}

		HeightmapInfo.HeightmapTexture->BeginCachePlatformData();
		HeightmapInfo.HeightmapTexture->ClearAllCachedCookedPlatformData();
		PendingTexturePlatformDataCreation.Add(HeightmapInfo.HeightmapTexture);
	}

	for (UTexture2D* Texture : PendingTexturePlatformDataCreation)
	{
		Texture->FinishCachePlatformData();
		Texture->PostEditChange();
	}

	// Update MaterialInstances (must be done after textures are fully initialized)
	UpdateAllComponentMaterialInstances();

	if (GetMutableDefault<UEditorExperimentalSettings>()->bProceduralLandscape)
	{
		SetupProceduralLayers(NumComponentsX, NumComponentsY);
		
		FEditorDelegates::PreSaveWorld.AddUObject(GetCyLandActor(), &ACyLand::OnPreSaveWorld);
		FEditorDelegates::PostSaveWorld.AddUObject(GetCyLandActor(), &ACyLand::OnPostSaveWorld);
	}

	if (GetLevel()->bIsVisible)
	{
		// Update our new components
		ReregisterAllComponents();
	}

	ReimportHeightmapFilePath = HeightmapFileName;

	CyLandInfo->UpdateLayerInfoMap();
	CyLandInfo->RecreateCollisionComponents();
	CyLandInfo->UpdateAllAddCollisions();

	GWarn->EndSlowTask();
}

bool ACyLandProxy::ExportToRawMesh(int32 InExportLOD, FMeshDescription& OutRawMesh) const
{
	FBoxSphereBounds GarbageBounds;
	return ExportToRawMesh(InExportLOD, OutRawMesh, GarbageBounds, true);
}

bool ACyLandProxy::ExportToRawMesh(int32 InExportLOD, FMeshDescription& OutRawMesh, const FBoxSphereBounds& InBounds, bool bIgnoreBounds /*= false*/) const
{
	TInlineComponentArray<UCyLandComponent*> RegisteredCyLandComponents;
	GetComponents<UCyLandComponent>(RegisteredCyLandComponents);

	const FIntRect CyLandSectionRect = GetBoundingRect();
	const FVector2D CyLandUVScale = FVector2D(1.0f, 1.0f) / FVector2D(CyLandSectionRect.Size());

	TVertexAttributesRef<FVector> VertexPositions = OutRawMesh.VertexAttributes().GetAttributesRef<FVector>(MeshAttribute::Vertex::Position);
	TEdgeAttributesRef<bool> EdgeHardnesses = OutRawMesh.EdgeAttributes().GetAttributesRef<bool>(MeshAttribute::Edge::IsHard);
	TEdgeAttributesRef<float> EdgeCreaseSharpnesses = OutRawMesh.EdgeAttributes().GetAttributesRef<float>(MeshAttribute::Edge::CreaseSharpness);
	TPolygonGroupAttributesRef<FName> PolygonGroupImportedMaterialSlotNames = OutRawMesh.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
	TVertexInstanceAttributesRef<FVector> VertexInstanceNormals = OutRawMesh.VertexInstanceAttributes().GetAttributesRef<FVector>(MeshAttribute::VertexInstance::Normal);
	TVertexInstanceAttributesRef<FVector> VertexInstanceTangents = OutRawMesh.VertexInstanceAttributes().GetAttributesRef<FVector>(MeshAttribute::VertexInstance::Tangent);
	TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = OutRawMesh.VertexInstanceAttributes().GetAttributesRef<float>(MeshAttribute::VertexInstance::BinormalSign);
	TVertexInstanceAttributesRef<FVector4> VertexInstanceColors = OutRawMesh.VertexInstanceAttributes().GetAttributesRef<FVector4>(MeshAttribute::VertexInstance::Color);
	TVertexInstanceAttributesRef<FVector2D> VertexInstanceUVs = OutRawMesh.VertexInstanceAttributes().GetAttributesRef<FVector2D>(MeshAttribute::VertexInstance::TextureCoordinate);

	if (VertexInstanceUVs.GetNumIndices() < 2)
	{
		VertexInstanceUVs.SetNumIndices(2);
	}

	// User specified LOD to export
	int32 CyLandLODToExport = ExportLOD;
	if (InExportLOD != INDEX_NONE)
	{
		CyLandLODToExport = FMath::Clamp<int32>(InExportLOD, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
	}

	// Export data for each component
	for (auto It = RegisteredCyLandComponents.CreateConstIterator(); It; ++It)
	{
		UCyLandComponent* Component = (*It);

		// Early out if the CyLand bounds and given bounds do not overlap at all
		if (!bIgnoreBounds && !FBoxSphereBounds::SpheresIntersect(Component->Bounds, InBounds))
		{
			continue;
		}

		FCyLandComponentDataInterface CDI(Component, CyLandLODToExport);
		const int32 ComponentSizeQuadsLOD = ((Component->ComponentSizeQuads + 1) >> CyLandLODToExport) - 1;
		const int32 SubsectionSizeQuadsLOD = ((Component->SubsectionSizeQuads + 1) >> CyLandLODToExport) - 1;
		const FIntPoint ComponentOffsetQuads = Component->GetSectionBase() - CyLandSectionOffset - CyLandSectionRect.Min;
		const FVector2D ComponentUVOffsetLOD = FVector2D(ComponentOffsetQuads)*((float)ComponentSizeQuadsLOD / ComponentSizeQuads);
		const FVector2D ComponentUVScaleLOD = CyLandUVScale*((float)ComponentSizeQuads / ComponentSizeQuadsLOD);

		const int32 NumFaces = FMath::Square(ComponentSizeQuadsLOD) * 2;
		const int32 NumVertices = NumFaces * 3;

		OutRawMesh.ReserveNewVertices(NumVertices);
		OutRawMesh.ReserveNewPolygons(NumFaces);
		OutRawMesh.ReserveNewVertexInstances(NumVertices);
		OutRawMesh.ReserveNewEdges(NumVertices);

		FPolygonGroupID PolygonGroupID = FPolygonGroupID::Invalid;
		if (OutRawMesh.PolygonGroups().Num() < 1)
		{
			PolygonGroupID = OutRawMesh.CreatePolygonGroup();
			PolygonGroupImportedMaterialSlotNames[PolygonGroupID] = FName(TEXT("CyLandMat_0"));
		}
		else
		{
			PolygonGroupID = OutRawMesh.PolygonGroups().GetFirstValidID();
		}

		// Check if there are any holes
		const int32 VisThreshold = 170;
		TArray<uint8> VisDataMap;

		for (int32 AllocIdx = 0; AllocIdx < Component->WeightmapLayerAllocations.Num(); AllocIdx++)
		{
			FCyWeightmapLayerAllocationInfo& AllocInfo = Component->WeightmapLayerAllocations[AllocIdx];
			if (AllocInfo.LayerInfo == ACyLandProxy::VisibilityLayer)
			{
				CDI.GetWeightmapTextureData(AllocInfo.LayerInfo, VisDataMap);
			}
		}

		const FIntPoint QuadPattern[6] =
		{
			//face 1
			FIntPoint(0, 0),
			FIntPoint(0, 1),
			FIntPoint(1, 1),
			//face 2
			FIntPoint(0, 0),
			FIntPoint(1, 1),
			FIntPoint(1, 0),
		};

		const int32 WeightMapSize = (SubsectionSizeQuadsLOD + 1) * Component->NumSubsections;

		const float SquaredSphereRadius = FMath::Square(InBounds.SphereRadius);

		//We need to not duplicate the vertex position, so we use the FIndexAndZ to achieve fast result
		TArray<FIndexAndZ> VertIndexAndZ;
		VertIndexAndZ.Reserve(ComponentSizeQuadsLOD*ComponentSizeQuadsLOD*ARRAY_COUNT(QuadPattern));
		int32 CurrentIndex = 0;
		TMap<int32, FVector> IndexToPosition;
		IndexToPosition.Reserve(ComponentSizeQuadsLOD*ComponentSizeQuadsLOD*ARRAY_COUNT(QuadPattern));
		for (int32 y = 0; y < ComponentSizeQuadsLOD; y++)
		{
			for (int32 x = 0; x < ComponentSizeQuadsLOD; x++)
			{
				for (int32 i = 0; i < ARRAY_COUNT(QuadPattern); i++)
				{
					int32 VertexX = x + QuadPattern[i].X;
					int32 VertexY = y + QuadPattern[i].Y;
					FVector Position = CDI.GetWorldVertex(VertexX, VertexY);

					// If at least one vertex is within the given bounds we should process the quad  
					new(VertIndexAndZ)FIndexAndZ(CurrentIndex, Position);
					IndexToPosition.Add(CurrentIndex, Position);
					CurrentIndex++;
				}
			}
		}
		// Sort the vertices by z value
		VertIndexAndZ.Sort(FCompareIndexAndZ());

		auto FindPreviousIndex = [&VertIndexAndZ, &IndexToPosition](int32 Index)->int32
		{
			const FVector& PositionA = IndexToPosition[Index];
			FIndexAndZ CompressPosition(0, PositionA);
			// Search for lowest index duplicates
			int32 BestIndex = MAX_int32;
			for (int32 i = 0; i < IndexToPosition.Num(); i++)
			{
				if (CompressPosition.Z > (VertIndexAndZ[i].Z + SMALL_NUMBER))
				{
					//We will not find anything there is no point searching more
					break;
				}
				const FVector& PositionB = IndexToPosition[VertIndexAndZ[i].Index];
				if (PointsEqual(PositionA, PositionB, SMALL_NUMBER))
				{
					if (VertIndexAndZ[i].Index < BestIndex)
					{
						BestIndex = VertIndexAndZ[i].Index;
					}
				}
			}
			return BestIndex < MAX_int32 ? BestIndex : Index;
		};

		// Export to MeshDescription
		TMap<int32, FVertexID> IndexToVertexID;
		IndexToVertexID.Reserve(CurrentIndex);
		CurrentIndex = 0;
		for (int32 y = 0; y < ComponentSizeQuadsLOD; y++)
		{
			for (int32 x = 0; x < ComponentSizeQuadsLOD; x++)
			{
				FVector Positions[ARRAY_COUNT(QuadPattern)];
				bool bProcess = bIgnoreBounds;

				// Fill positions
				for (int32 i = 0; i < ARRAY_COUNT(QuadPattern); i++)
				{
					int32 VertexX = x + QuadPattern[i].X;
					int32 VertexY = y + QuadPattern[i].Y;
					Positions[i] = CDI.GetWorldVertex(VertexX, VertexY);

					// If at least one vertex is within the given bounds we should process the quad  
					if (!bProcess && InBounds.ComputeSquaredDistanceFromBoxToPoint(Positions[i]) < SquaredSphereRadius)
					{
						bProcess = true;
					}
				}

				if (bProcess)
				{
					//Fill the vertexID we need
					TArray<FVertexID> VertexIDs;
					VertexIDs.Reserve(ARRAY_COUNT(QuadPattern));
					TArray<FVertexInstanceID> VertexInstanceIDs;
					VertexInstanceIDs.Reserve(ARRAY_COUNT(QuadPattern));
					// Fill positions
					for (int32 i = 0; i < ARRAY_COUNT(QuadPattern); i++)
					{
						int32 DuplicateLowestIndex = FindPreviousIndex(CurrentIndex);
						FVertexID VertexID;
						if (DuplicateLowestIndex < CurrentIndex)
						{
							VertexID = IndexToVertexID[DuplicateLowestIndex];
						}
						else
						{
							VertexID = OutRawMesh.CreateVertex();
							VertexPositions[VertexID] = Positions[i];
						}
						IndexToVertexID.Add(CurrentIndex, VertexID);
						VertexIDs.Add(VertexID);
						CurrentIndex++;
					}

					// Create triangle
					{
						// Whether this vertex is in hole
						bool bInvisible = false;
						if (VisDataMap.Num())
						{
							int32 TexelX, TexelY;
							CDI.VertexXYToTexelXY(x, y, TexelX, TexelY);
							bInvisible = (VisDataMap[CDI.TexelXYToIndex(TexelX, TexelY)] >= VisThreshold);
						}
						//Add vertexInstance and polygon only if we are visible
						if (!bInvisible)
						{
							VertexInstanceIDs.Add(OutRawMesh.CreateVertexInstance(VertexIDs[0]));
							VertexInstanceIDs.Add(OutRawMesh.CreateVertexInstance(VertexIDs[1]));
							VertexInstanceIDs.Add(OutRawMesh.CreateVertexInstance(VertexIDs[2]));

							VertexInstanceIDs.Add(OutRawMesh.CreateVertexInstance(VertexIDs[3]));
							VertexInstanceIDs.Add(OutRawMesh.CreateVertexInstance(VertexIDs[4]));
							VertexInstanceIDs.Add(OutRawMesh.CreateVertexInstance(VertexIDs[5]));

							// Fill other vertex data
							for (int32 i = 0; i < ARRAY_COUNT(QuadPattern); i++)
							{
								int32 VertexX = x + QuadPattern[i].X;
								int32 VertexY = y + QuadPattern[i].Y;

								FVector LocalTangentX, LocalTangentY, LocalTangentZ;
								CDI.GetLocalTangentVectors(VertexX, VertexY, LocalTangentX, LocalTangentY, LocalTangentZ);

								VertexInstanceTangents[VertexInstanceIDs[i]] = LocalTangentX;
								VertexInstanceBinormalSigns[VertexInstanceIDs[i]] = GetBasisDeterminantSign(LocalTangentX, LocalTangentY, LocalTangentZ);
								VertexInstanceNormals[VertexInstanceIDs[i]] = LocalTangentZ;

								FVector2D UV = (ComponentUVOffsetLOD + FVector2D(VertexX, VertexY))*ComponentUVScaleLOD;
								VertexInstanceUVs.Set(VertexInstanceIDs[i], 0, UV);
								// Add lightmap UVs
								VertexInstanceUVs.Set(VertexInstanceIDs[i], 1, UV);
							}
							auto AddTriangle = [&OutRawMesh, &EdgeHardnesses, &EdgeCreaseSharpnesses, &PolygonGroupID, &VertexIDs, &VertexInstanceIDs](int32 BaseIndex)
							{
								//Create a polygon from this triangle
								TArray<FVertexInstanceID> PerimeterVertexInstances;
								PerimeterVertexInstances.SetNum(3);
								for (int32 Corner = 0; Corner < 3; ++Corner)
								{
									PerimeterVertexInstances[Corner] = VertexInstanceIDs[BaseIndex + Corner];
								}
								// Insert a polygon into the mesh
								TArray<FEdgeID> NewEdgeIDs;
								const FPolygonID NewPolygonID = OutRawMesh.CreatePolygon(PolygonGroupID, PerimeterVertexInstances, &NewEdgeIDs);
								for (const FEdgeID NewEdgeID : NewEdgeIDs)
								{
									EdgeHardnesses[NewEdgeID] = false;
									EdgeCreaseSharpnesses[NewEdgeID] = 0.0f;
								}
								//Triangulate the polygon
								FMeshPolygon& Polygon = OutRawMesh.GetPolygon(NewPolygonID);
								OutRawMesh.ComputePolygonTriangulation(NewPolygonID, Polygon.Triangles);
							};
							AddTriangle(0);
							AddTriangle(3);
						}
					}
				}
				else
				{
					CurrentIndex += ARRAY_COUNT(QuadPattern);
				}
			}
		}
	}

	//Compact the MeshDescription, if there was visibility mask or some bounding box clip, it need to be compacted so the sparse array are from 0 to n with no invalid data in between. 
	FElementIDRemappings ElementIDRemappings;
	OutRawMesh.Compact(ElementIDRemappings);
	return OutRawMesh.Polygons().Num() > 0;
}


FIntRect ACyLandProxy::GetBoundingRect() const
{
	FIntRect Rect(MAX_int32, MAX_int32, MIN_int32, MIN_int32);

	for (int32 CompIdx = 0; CompIdx < CyLandComponents.Num(); CompIdx++)
	{
		Rect.Include(CyLandComponents[CompIdx]->GetSectionBase());
	}

	if (CyLandComponents.Num() > 0)
	{
		Rect.Max += FIntPoint(ComponentSizeQuads, ComponentSizeQuads);
		Rect -= CyLandSectionOffset;
	}
	else
	{
		Rect = FIntRect();
	}

	return Rect;
}

bool ACyLand::HasAllComponent()
{
	UCyLandInfo* Info = GetCyLandInfo();
	if (Info && Info->XYtoComponentMap.Num() == CyLandComponents.Num())
	{
		// all components are owned by this CyLand actor (no CyLand Proxies)
		return true;
	}
	return false;
}

bool UCyLandInfo::GetCyLandExtent(int32& MinX, int32& MinY, int32& MaxX, int32& MaxY) const
{
	MinX = MAX_int32;
	MinY = MAX_int32;
	MaxX = MIN_int32;
	MaxY = MIN_int32;

	// Find range of entire landscape
	for (auto& XYComponentPair : XYtoComponentMap)
	{
		const UCyLandComponent* Comp = XYComponentPair.Value;
		Comp->GetComponentExtent(MinX, MinY, MaxX, MaxY);
	}
	return (MinX != MAX_int32);
}

bool UCyLandInfo::GetSelectedExtent(int32& MinX, int32& MinY, int32& MaxX, int32& MaxY) const
{
	MinX = MinY = MAX_int32;
	MaxX = MaxY = MIN_int32;
	for (auto& SelectedPointPair : SelectedRegion)
	{
		const FIntPoint Key = SelectedPointPair.Key;
		if (MinX > Key.X) MinX = Key.X;
		if (MaxX < Key.X) MaxX = Key.X;
		if (MinY > Key.Y) MinY = Key.Y;
		if (MaxY < Key.Y) MaxY = Key.Y;
	}
	if (MinX != MAX_int32)
	{
		return true;
	}
	// if SelectedRegion is empty, try SelectedComponents
	for (const UCyLandComponent* Comp : SelectedComponents)
	{
		Comp->GetComponentExtent(MinX, MinY, MaxX, MaxY);
	}
	return MinX != MAX_int32;
}

FVector UCyLandInfo::GetCyLandCenterPos(float& LengthZ, int32 MinX /*= MAX_INT*/, int32 MinY /*= MAX_INT*/, int32 MaxX /*= MIN_INT*/, int32 MaxY /*= MIN_INT*/)
{
	// MinZ, MaxZ is Local coordinate
	float MaxZ = -HALF_WORLD_MAX, MinZ = HALF_WORLD_MAX;
	const float ScaleZ = DrawScale.Z;

	if (MinX == MAX_int32)
	{
		// Find range of entire landscape
		for (auto It = XYtoComponentMap.CreateIterator(); It; ++It)
		{
			UCyLandComponent* Comp = It.Value();
			Comp->GetComponentExtent(MinX, MinY, MaxX, MaxY);
		}

		const int32 Dist = (ComponentSizeQuads + 1) >> 1; // Should be same in ACyLandGizmoActiveActor::SetTargetCyLand
		FVector2D MidPoint(((float)(MinX + MaxX)) / 2.0f, ((float)(MinY + MaxY)) / 2.0f);
		MinX = FMath::FloorToInt(MidPoint.X) - Dist;
		MaxX = FMath::CeilToInt(MidPoint.X) + Dist;
		MinY = FMath::FloorToInt(MidPoint.Y) - Dist;
		MaxY = FMath::CeilToInt(MidPoint.Y) + Dist;
		check(MidPoint.X == ((float)(MinX + MaxX)) / 2.0f && MidPoint.Y == ((float)(MinY + MaxY)) / 2.0f);
	}

	check(MinX != MAX_int32);
	//if (MinX != MAX_int32)
	{
		int32 CompX1, CompX2, CompY1, CompY2;
		ACyLand::CalcComponentIndicesOverlap(MinX, MinY, MaxX, MaxY, ComponentSizeQuads, CompX1, CompY1, CompX2, CompY2);
		for (int32 IndexY = CompY1; IndexY <= CompY2; ++IndexY)
		{
			for (int32 IndexX = CompX1; IndexX <= CompX2; ++IndexX)
			{
				UCyLandComponent* Comp = XYtoComponentMap.FindRef(FIntPoint(IndexX, IndexY));
				if (Comp)
				{
					UCyLandHeightfieldCollisionComponent* CollisionComp = Comp->CollisionComponent.Get();
					if (CollisionComp)
					{
						uint16* Heights = (uint16*)CollisionComp->CollisionHeightData.Lock(LOCK_READ_ONLY);
						int32 CollisionSizeVerts = CollisionComp->CollisionSizeQuads + 1;

						int32 StartX = FMath::Max(0, MinX - CollisionComp->GetSectionBase().X);
						int32 StartY = FMath::Max(0, MinY - CollisionComp->GetSectionBase().Y);
						int32 EndX = FMath::Min(CollisionSizeVerts, MaxX - CollisionComp->GetSectionBase().X + 1);
						int32 EndY = FMath::Min(CollisionSizeVerts, MaxY - CollisionComp->GetSectionBase().Y + 1);

						for (int32 Y = StartY; Y < EndY; ++Y)
						{
							for (int32 X = StartX; X < EndX; ++X)
							{
								float Height = CyLandDataAccess::GetLocalHeight(Heights[X + Y*CollisionSizeVerts]);
								MaxZ = FMath::Max(Height, MaxZ);
								MinZ = FMath::Min(Height, MinZ);
							}
						}
						CollisionComp->CollisionHeightData.Unlock();
					}
				}
			}
		}
	}

	const float MarginZ = 3;
	if (MaxZ < MinZ)
	{
		MaxZ = +MarginZ;
		MinZ = -MarginZ;
	}
	LengthZ = (MaxZ - MinZ + 2 * MarginZ) * ScaleZ;

	const FVector LocalPosition(((float)(MinX + MaxX)) / 2.0f, ((float)(MinY + MaxY)) / 2.0f, MinZ - MarginZ);
	return GetCyLandProxy()->CyLandActorToWorld().TransformPosition(LocalPosition);
}

bool UCyLandInfo::IsValidPosition(int32 X, int32 Y)
{
	int32 CompX1, CompX2, CompY1, CompY2;
	ACyLand::CalcComponentIndicesOverlap(X, Y, X, Y, ComponentSizeQuads, CompX1, CompY1, CompX2, CompY2);
	if (XYtoComponentMap.FindRef(FIntPoint(CompX1, CompY1)))
	{
		return true;
	}
	if (XYtoComponentMap.FindRef(FIntPoint(CompX2, CompY2)))
	{
		return true;
	}
	return false;
}

void UCyLandInfo::ExportHeightmap(const FString& Filename)
{
	int32 MinX = MAX_int32;
	int32 MinY = MAX_int32;
	int32 MaxX = -MAX_int32;
	int32 MaxY = -MAX_int32;

	if (!GetCyLandExtent(MinX, MinY, MaxX, MaxY))
	{
		return;
	}

	GWarn->BeginSlowTask(LOCTEXT("BeginExportingCyLandHeightmapTask", "Exporting CyLand Heightmap"), true);

	ICyLandEditorModule& CyLandEditorModule = FModuleManager::GetModuleChecked<ICyLandEditorModule>("CyLandEditor");
	FCyLandEditDataInterface CyLandEdit(this);

	TArray<uint16> HeightData;
	HeightData.AddZeroed((MaxX - MinX + 1) * (MaxY - MinY + 1));
	CyLandEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0);

	const ICyLandHeightmapFileFormat* HeightmapFormat = CyLandEditorModule.GetHeightmapFormatByExtension(*FPaths::GetExtension(Filename, true));
	if (HeightmapFormat)
	{
		HeightmapFormat->Export(*Filename, HeightData, {(uint32)(MaxX - MinX + 1), (uint32)(MaxY - MinY + 1)}, DrawScale * FVector(1, 1, LANDSCAPE_ZSCALE));
	}

	GWarn->EndSlowTask();
}

void UCyLandInfo::ExportLayer(UCyLandLayerInfoObject* LayerInfo, const FString& Filename)
{
	check(LayerInfo);

	int32 MinX = MAX_int32;
	int32 MinY = MAX_int32;
	int32 MaxX = -MAX_int32;
	int32 MaxY = -MAX_int32;

	if (!GetCyLandExtent(MinX, MinY, MaxX, MaxY))
	{
		return;
	}

	GWarn->BeginSlowTask(LOCTEXT("BeginExportingCyLandWeightmapTask", "Exporting CyLand Layer Weightmap"), true);

	ICyLandEditorModule& CyLandEditorModule = FModuleManager::GetModuleChecked<ICyLandEditorModule>("CyLandEditor");

	TArray<uint8> WeightData;
	WeightData.AddZeroed((MaxX - MinX + 1) * (MaxY - MinY + 1));

	FCyLandEditDataInterface CyLandEdit(this);
	CyLandEdit.GetWeightDataFast(LayerInfo, MinX, MinY, MaxX, MaxY, WeightData.GetData(), 0);

	const ICyLandWeightmapFileFormat* WeightmapFormat = CyLandEditorModule.GetWeightmapFormatByExtension(*FPaths::GetExtension(Filename, true));
	if (WeightmapFormat)
	{
		WeightmapFormat->Export(*Filename, LayerInfo->LayerName, WeightData, {(uint32)(MaxX - MinX + 1), (uint32)(MaxY - MinY + 1)});
	}

	GWarn->EndSlowTask();
}

void UCyLandInfo::DeleteLayer(UCyLandLayerInfoObject* LayerInfo, const FName& LayerName)
{
	GWarn->BeginSlowTask(LOCTEXT("BeginDeletingLayerTask", "Deleting Layer"), true);

	// Remove data from all components
	FCyLandEditDataInterface CyLandEdit(this);
	CyLandEdit.DeleteLayer(LayerInfo);

	// Remove from layer settings array
	{
		int32 LayerIndex = Layers.IndexOfByPredicate([LayerInfo, LayerName](const FCyLandInfoLayerSettings& LayerSettings) { return LayerSettings.LayerInfoObj == LayerInfo && LayerSettings.LayerName == LayerName; });
		if (LayerIndex != INDEX_NONE)
		{
			Layers.RemoveAt(LayerIndex);
		}
	}

	ForAllCyLandProxies([LayerInfo](ACyLandProxy* Proxy)
	{
		Proxy->Modify();
		int32 Index = Proxy->EditorLayerSettings.IndexOfByKey(LayerInfo);
		if (Index != INDEX_NONE)
		{
			Proxy->EditorLayerSettings.RemoveAt(Index);
		}
	});

	//UpdateLayerInfoMap();

	GWarn->EndSlowTask();
}

void UCyLandInfo::ReplaceLayer(UCyLandLayerInfoObject* FromLayerInfo, UCyLandLayerInfoObject* ToLayerInfo)
{
	if (ensure(FromLayerInfo != ToLayerInfo))
	{
		GWarn->BeginSlowTask(LOCTEXT("BeginReplacingLayerTask", "Replacing Layer"), true);

		// Remove data from all components
		FCyLandEditDataInterface CyLandEdit(this);
		CyLandEdit.ReplaceLayer(FromLayerInfo, ToLayerInfo);

		// Convert array
		for (int32 j = 0; j < Layers.Num(); j++)
		{
			if (Layers[j].LayerInfoObj == FromLayerInfo)
			{
				Layers[j].LayerInfoObj = ToLayerInfo;
			}
		}

		ForAllCyLandProxies([FromLayerInfo, ToLayerInfo](ACyLandProxy* Proxy)
		{
			Proxy->Modify();
			FCyLandEditorLayerSettings* ToEditorLayerSettings = Proxy->EditorLayerSettings.FindByKey(ToLayerInfo);
			if (ToEditorLayerSettings != nullptr)
			{
				// If the new layer already exists, simple remove the old layer
				int32 Index = Proxy->EditorLayerSettings.IndexOfByKey(FromLayerInfo);
				if (Index != INDEX_NONE)
				{
					Proxy->EditorLayerSettings.RemoveAt(Index);
				}
			}
			else
			{
				FCyLandEditorLayerSettings* FromEditorLayerSettings = Proxy->EditorLayerSettings.FindByKey(FromLayerInfo);
				if (FromEditorLayerSettings != nullptr)
				{
					// If only the old layer exists (most common case), change it to point to the new layer info
					FromEditorLayerSettings->LayerInfoObj = ToLayerInfo;
				}
				else
				{
					// If neither exists in the EditorLayerSettings cache, add it
					Proxy->EditorLayerSettings.Add(FCyLandEditorLayerSettings(ToLayerInfo));
				}
			}
		});

		//UpdateLayerInfoMap();

		GWarn->EndSlowTask();
	}
}

void ACyLandProxy::EditorApplyScale(const FVector& DeltaScale, const FVector* PivotLocation, bool bAltDown, bool bShiftDown, bool bCtrlDown)
{
	FVector ModifiedScale = DeltaScale;

	// Lock X and Y scaling to the same value
	ModifiedScale.X = ModifiedScale.Y = (FMath::Abs(DeltaScale.X) > FMath::Abs(DeltaScale.Y)) ? DeltaScale.X : DeltaScale.Y;

	// Correct for attempts to scale to 0 on any axis
	FVector CurrentScale = GetRootComponent()->RelativeScale3D;
	if (AActor::bUsePercentageBasedScaling)
	{
		if (ModifiedScale.X == -1)
		{
			ModifiedScale.X = ModifiedScale.Y = -(CurrentScale.X - 1) / CurrentScale.X;
		}
		if (ModifiedScale.Z == -1)
		{
			ModifiedScale.Z = -(CurrentScale.Z - 1) / CurrentScale.Z;
		}
	}
	else
	{
		if (ModifiedScale.X == -CurrentScale.X)
		{
			CurrentScale.X += 1;
			CurrentScale.Y += 1;
		}
		if (ModifiedScale.Z == -CurrentScale.Z)
		{
			CurrentScale.Z += 1;
		}
	}

	Super::EditorApplyScale(ModifiedScale, PivotLocation, bAltDown, bShiftDown, bCtrlDown);

	// We need to regenerate collision objects, they depend on scale value 
	for (UCyLandHeightfieldCollisionComponent* Comp : CollisionComponents)
	{
		if (Comp)
		{
			Comp->RecreateCollision();
		}
	}
}

void ACyLandProxy::EditorApplyMirror(const FVector& MirrorScale, const FVector& PivotLocation)
{
	Super::EditorApplyMirror(MirrorScale, PivotLocation);

	// We need to regenerate collision objects, they depend on scale value 
	for (UCyLandHeightfieldCollisionComponent* Comp : CollisionComponents)
	{
		if (Comp)
		{
			Comp->RecreateCollision();
		}
	}
}

void ACyLandProxy::PostEditMove(bool bFinished)
{
	// This point is only reached when Copy and Pasted
	Super::PostEditMove(bFinished);

	if (bFinished && !GetWorld()->IsGameWorld())
	{
		//fatal
		if(CyLandGuid.IsValid())
		UCyLandInfo::RecreateCyLandInfo(GetWorld(), true);
		RecreateComponentsState();

		if (SplineComponent)
		{
			SplineComponent->CheckSplinesValid();
		}
	}
}

void ACyLandProxy::PostEditImport()
{
	Super::PostEditImport();

	// during import this gets called multiple times, without a valid guid the first time
	if (CyLandGuid.IsValid())
	{
		CreateCyLandInfo();
	}

	for (int32 ComponentIndex = 0; ComponentIndex < CyLandComponents.Num(); ++ComponentIndex)
	{
		UCyLandComponent* Comp = CyLandComponents[ComponentIndex];
		if (Comp)
		{
			// Update the MIC
			Comp->UpdateMaterialInstances();
		}
	}
}

void ACyLand::PostEditMove(bool bFinished)
{
	UE_LOG(LogCyLand, Warning, TEXT("ACyLand PostEditMove"));
	if (bFinished && !GetWorld()->IsGameWorld())
	{
		// align all proxies to landscape actor
		auto* CyLandInfo = GetCyLandInfo();
		if (CyLandInfo)
		{
			CyLandInfo->FixupProxiesTransform();
		}
	}

	Super::PostEditMove(bFinished);
}

bool ACyLand::ShouldImport(FString* ActorPropString, bool IsMovingLevel)
{
	return GetWorld() != nullptr && !GetWorld()->IsGameWorld();
}

void ACyLand::PostEditImport()
{
	UE_LOG(LogCyLand, Warning, TEXT("ACyLand PostEditImport"));
	check(GetWorld() && !GetWorld()->IsGameWorld());

	for (ACyLand* CyLand : TActorRange<ACyLand>(GetWorld()))
	{
		if (CyLand && CyLand != this && !CyLand->HasAnyFlags(RF_BeginDestroyed) && CyLand->CyLandGuid == CyLandGuid)
		{
			// Copy/Paste case, need to generate new GUID
			CyLandGuid = FGuid::NewGuid();
			break;
		}
	}

	Super::PostEditImport();
}

void ACyLand::PostDuplicate(bool bDuplicateForPIE)
{
	if (!bDuplicateForPIE)
	{
		// Need to generate new GUID when duplicating
		CyLandGuid = FGuid::NewGuid();
	}

	Super::PostDuplicate(bDuplicateForPIE);
}
#endif	//WITH_EDITOR

UCyLandLayerInfoObject::UCyLandLayerInfoObject(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
#if WITH_EDITORONLY_DATA
	, IsReferencedFromLoadedData(false)
#endif // WITH_EDITORONLY_DATA
{
	Hardness = 0.5f;
#if WITH_EDITORONLY_DATA
	bNoWeightBlend = false;
#endif // WITH_EDITORONLY_DATA

	// Assign initial LayerUsageDebugColor
	if (!IsTemplate())
	{
		uint8 Hash[20];
		FString PathNameString = GetPathName();
		FSHA1::HashBuffer(*PathNameString, PathNameString.Len() * sizeof(PathNameString[0]), Hash);
		LayerUsageDebugColor = FLinearColor(float(Hash[0]) / 255.f, float(Hash[1]) / 255.f, float(Hash[2]) / 255.f, 1.f);
	}
}

#if WITH_EDITOR
void UCyLandLayerInfoObject::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	static const FName NAME_Hardness = FName(TEXT("Hardness"));
	static const FName NAME_PhysMaterial = FName(TEXT("PhysMaterial"));
	static const FName NAME_LayerUsageDebugColor = FName(TEXT("LayerUsageDebugColor"));
	static const FName NAME_R = FName(TEXT("R"));
	static const FName NAME_G = FName(TEXT("G"));
	static const FName NAME_B = FName(TEXT("B"));
	static const FName NAME_A = FName(TEXT("A"));

	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	if (GIsEditor)
	{
		if (PropertyName == NAME_Hardness)
		{
			Hardness = FMath::Clamp<float>(Hardness, 0.0f, 1.0f);
		}
		else if (PropertyName == NAME_PhysMaterial)
		{
			for (TObjectIterator<ACyLandProxy> It; It; ++It)
			{
				ACyLandProxy* Proxy = *It;
				if (Proxy->GetWorld() && !Proxy->GetWorld()->IsPlayInEditor())
				{
					UCyLandInfo* Info = Proxy->GetCyLandInfo();
					if (Info)
					{
						for (int32 i = 0; i < Info->Layers.Num(); ++i)
						{
							if (Info->Layers[i].LayerInfoObj == this)
							{
								Proxy->ChangedPhysMaterial();
								break;
							}
						}
					}
				}
			}
		}
		else if (PropertyName == NAME_LayerUsageDebugColor || PropertyName == NAME_R || PropertyName == NAME_G || PropertyName == NAME_B || PropertyName == NAME_A)
		{
			LayerUsageDebugColor.A = 1.0f;
			for (TObjectIterator<ACyLandProxy> It; It; ++It)
			{
				ACyLandProxy* Proxy = *It;
				if (Proxy->GetWorld() && !Proxy->GetWorld()->IsPlayInEditor())
				{
					Proxy->MarkComponentsRenderStateDirty();
				}
			}
		}
	}
}

void UCyLandLayerInfoObject::PostLoad()
{
	Super::PostLoad();
	if (GIsEditor)
	{
		if (!HasAnyFlags(RF_Standalone))
		{
			SetFlags(RF_Standalone);
		}
		Hardness = FMath::Clamp<float>(Hardness, 0.0f, 1.0f);
	}
}

void ACyLandProxy::RemoveXYOffsets()
{
	bool bFoundXYOffset = false;

	for (int32 i = 0; i < CyLandComponents.Num(); ++i)
	{
		UCyLandComponent* Comp = CyLandComponents[i];
		if (Comp && Comp->XYOffsetmapTexture)
		{
			Comp->XYOffsetmapTexture->SetFlags(RF_Transactional);
			Comp->XYOffsetmapTexture->Modify();
			Comp->XYOffsetmapTexture->MarkPackageDirty();
			Comp->XYOffsetmapTexture->ClearFlags(RF_Standalone);
			Comp->Modify();
			Comp->MarkPackageDirty();
			Comp->XYOffsetmapTexture = nullptr;
			Comp->MarkRenderStateDirty();
			bFoundXYOffset = true;
		}
	}

	if (bFoundXYOffset)
	{
		RecreateCollisionComponents();
	}
}



void ACyLandProxy::RecreateCollisionComponents()
{
	// We can assume these are all junk; they recreate as needed
	FlushGrassComponents();

	// Clear old CollisionComponent containers
	CollisionComponents.Empty();

	// Destroy any owned collision components
	TInlineComponentArray<UCyLandHeightfieldCollisionComponent*> CollisionComps;
	GetComponents(CollisionComps);
	for (UCyLandHeightfieldCollisionComponent* Component : CollisionComps)
	{
		Component->DestroyComponent();
	}

	TArray<USceneComponent*> AttachedCollisionComponents = RootComponent->GetAttachChildren().FilterByPredicate(
		[](USceneComponent* Component)
	{
		return Cast<UCyLandHeightfieldCollisionComponent>(Component);
	});

	// Destroy any attached but un-owned collision components
	for (USceneComponent* Component : AttachedCollisionComponents)
	{
		Component->DestroyComponent();
	}

	// Recreate collision
	CollisionMipLevel = FMath::Clamp<int32>(CollisionMipLevel, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
	SimpleCollisionMipLevel = FMath::Clamp<int32>(SimpleCollisionMipLevel, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
	for (UCyLandComponent* Comp : CyLandComponents)
	{
		if (Comp)
		{
			Comp->CollisionMipLevel = CollisionMipLevel;
			Comp->SimpleCollisionMipLevel = SimpleCollisionMipLevel;
			Comp->UpdateCollisionData(true);
		}
	}
}

void UCyLandInfo::RecreateCollisionComponents()
{
	ForAllCyLandProxies([](ACyLandProxy* Proxy)
	{
		Proxy->RecreateCollisionComponents();
	});
}

void UCyLandInfo::RemoveXYOffsets()
{
	ForAllCyLandProxies([](ACyLandProxy* Proxy)
	{
		Proxy->RemoveXYOffsets();
	});
}

void UCyLandInfo::PostponeTextureBaking()
{
	static const int32 PostponeValue = 60; //frames
	
	ForAllCyLandProxies([](ACyLandProxy* Proxy)
	{
		Proxy->UpdateBakedTexturesCountdown = PostponeValue;
	});
}

namespace
{
	inline float AdjustStaticLightingResolution(float StaticLightingResolution, int32 NumSubsections, int32 SubsectionSizeQuads, int32 ComponentSizeQuads)
	{
		// Change Lighting resolution to proper one...
		if (StaticLightingResolution > 1.0f)
		{
			StaticLightingResolution = (int32)StaticLightingResolution;
		}
		else if (StaticLightingResolution < 1.0f)
		{
			// Restrict to 1/16
			if (StaticLightingResolution < 0.0625)
			{
				StaticLightingResolution = 0.0625;
			}

			// Adjust to 1/2^n
			int32 i = 2;
			int32 LightmapSize = (NumSubsections * (SubsectionSizeQuads + 1)) >> 1;
			while (StaticLightingResolution < (1.0f / i) && LightmapSize > 4)
			{
				i <<= 1;
				LightmapSize >>= 1;
			}
			StaticLightingResolution = 1.0f / i;

			int32 PixelPaddingX = GPixelFormats[PF_DXT1].BlockSizeX;

			int32 DestSize = (int32)((2 * PixelPaddingX + ComponentSizeQuads + 1) * StaticLightingResolution);
			StaticLightingResolution = (float)DestSize / (2 * PixelPaddingX + ComponentSizeQuads + 1);
		}

		return StaticLightingResolution;
	}
};

void ACyLandProxy::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
	const FName SubPropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	if (PropertyName == FName(TEXT("RelativeScale3D")))
	{
		// RelativeScale3D isn't even a property of ACyLandProxy, it's a property of the root component
		if (RootComponent)
		{
			FVector ModifiedScale = RootComponent->RelativeScale3D;

			// Lock X and Y scaling to the same value
			if (SubPropertyName == FName("Y"))
			{
				ModifiedScale.X = FMath::Abs(RootComponent->RelativeScale3D.Y)*FMath::Sign(ModifiedScale.X);
			}
			else
			{
				// There's no "if name == X" here so that if we can't tell which has changed out of X and Y, we just use X
				ModifiedScale.Y = FMath::Abs(RootComponent->RelativeScale3D.X)*FMath::Sign(ModifiedScale.Y);
			}

			UCyLandInfo* Info = GetCyLandInfo();

			// Correct for attempts to scale to 0 on any axis
			if (ModifiedScale.X == 0)
			{
				if (Info && Info->DrawScale.X < 0)
				{
					ModifiedScale.Y = ModifiedScale.X = -1;
				}
				else
				{
					ModifiedScale.Y = ModifiedScale.X = 1;
				}
			}
			if (ModifiedScale.Z == 0)
			{
				if (Info && Info->DrawScale.Z < 0)
				{
					ModifiedScale.Z = -1;
				}
				else
				{
					ModifiedScale.Z = 1;
				}
			}

			RootComponent->SetRelativeScale3D(ModifiedScale);

			// Update UCyLandInfo cached DrawScale
			if (Info)
			{
				Info->DrawScale = ModifiedScale;
			}

			// We need to regenerate collision objects, they depend on scale value
			if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
			{
				for (int32 ComponentIndex = 0; ComponentIndex < CollisionComponents.Num(); ComponentIndex++)
				{
					UCyLandHeightfieldCollisionComponent* Comp = CollisionComponents[ComponentIndex];
					if (Comp)
					{
						Comp->RecreateCollision();
					}
				}
			}
		}
	}

	if (GIsEditor && PropertyName == FName(TEXT("StreamingDistanceMultiplier")))
	{
		// Recalculate in a few seconds.
		GetWorld()->TriggerStreamingDataRebuild();
	}
	else if (GIsEditor && PropertyName == FName(TEXT("DefaultPhysMaterial")))
	{
		ChangedPhysMaterial();
	}
	else if (GIsEditor &&
		(PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, CollisionMipLevel) ||
		 PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, SimpleCollisionMipLevel) ||
		 PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, CollisionThickness) ||
		 PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, bBakeMaterialPositionOffsetIntoCollision) ||
		 PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, bGenerateOverlapEvents)))
	{
		if (bBakeMaterialPositionOffsetIntoCollision)
		{
			MarkComponentsRenderStateDirty();
		}
		if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
		{
			RecreateCollisionComponents();
		}
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, TessellationComponentScreenSize))
	{
		ChangeTessellationComponentScreenSize(TessellationComponentScreenSize);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, ComponentScreenSizeToUseSubSections))
	{
		ChangeComponentScreenSizeToUseSubSections(ComponentScreenSizeToUseSubSections);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, UseTessellationComponentScreenSizeFalloff))
	{
		ChangeUseTessellationComponentScreenSizeFalloff(UseTessellationComponentScreenSizeFalloff);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, TessellationComponentScreenSizeFalloff))
	{
		ChangeTessellationComponentScreenSizeFalloff(TessellationComponentScreenSizeFalloff);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, LODDistributionSetting)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, LOD0DistributionSetting))
	{		
		MarkComponentsRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, bUseMaterialPositionOffsetInStaticLighting))
	{
		InvalidateLightingCache();
	}
	else if(PropertyName == FName(TEXT("bCastStaticShadow")) ||
		PropertyName == FName(TEXT("bCastShadowAsTwoSided")) ||
		PropertyName == FName(TEXT("bCastFarShadow")) ||
		PropertyName == FName(TEXT("bAffectDistanceFieldLighting")) ||
		PropertyName == FName(TEXT("bRenderCustomDepth")) ||
		PropertyName == FName(TEXT("CustomDepthStencilValue")) ||
		PropertyName == FName(TEXT("LightingChannels")) ||
		PropertyName == FName(TEXT("LDMaxDrawDistance")))
	{
		// Replicate shared properties to all components.
		for (int32 ComponentIndex = 0; ComponentIndex < CyLandComponents.Num(); ComponentIndex++)
		{
			UCyLandComponent* Comp = CyLandComponents[ComponentIndex];
			if (Comp)
			{
				Comp->UpdatedSharedPropertiesFromActor();
			}
		}
	}
	else if (GIsEditor && PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, OccluderGeometryLOD))
	{
		CheckGenerateCyLandPlatformData(false, nullptr);
		MarkComponentsRenderStateDirty();
	}
	else if (PropertyName == FName(TEXT("bUseDynamicMaterialInstance")))
	{
		MarkComponentsRenderStateDirty();
	}
	
	// Remove null layer infos
	EditorLayerSettings.RemoveAll([](const FCyLandEditorLayerSettings& Entry) { return Entry.LayerInfoObj == nullptr; });

	// Remove any null landscape components
	CyLandComponents.RemoveAll([](const UCyLandComponent* Component) { return Component == nullptr; });

	UCyLandInfo* Info = GetCyLandInfo();
	bool bRemovedAnyLayers = false;
	for (UCyLandComponent* Component : CyLandComponents)
	{
		int32 NumNullLayers = Algo::CountIf(Component->WeightmapLayerAllocations, [](const FCyWeightmapLayerAllocationInfo& Allocation) { return Allocation.LayerInfo == nullptr; });
		if (NumNullLayers > 0)
		{
			FCyLandEditDataInterface CyLandEdit(Info);
			for (int32 i = 0; i < NumNullLayers; ++i)
			{
				// DeleteLayer doesn't expect duplicates, so we need to call it once for each null
				Component->DeleteLayer(nullptr, CyLandEdit);
			}
			bRemovedAnyLayers = true;
		}
	}
	if (bRemovedAnyLayers)
	{
		// Flush dynamic data (e.g. grass)
		TSet<UCyLandComponent*> Components;
		Components.Append(CyLandComponents);
		ACyLandProxy::InvalidateGeneratedComponentData(Components);
	}

	// Must do this *after* correcting the scale or reattaching the landscape components will crash!
	// Must do this *after* clamping values / propogating values to components
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void ACyLandStreamingProxy::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;

	if (PropertyName == FName(TEXT("CyLandActor")))
	{
		if (CyLandActor && IsValidCyLandActor(CyLandActor.Get()))
		{
			CyLandGuid = CyLandActor->GetCyLandGuid();
			if (GIsEditor && GetWorld() && !GetWorld()->IsPlayInEditor())
			{
				// TODO - only need to refresh the old and new landscape info
				UCyLandInfo::RecreateCyLandInfo(GetWorld(), false);
			}
		}
		else
		{
			CyLandActor = nullptr;
		}
	}
	else if (PropertyName == FName(TEXT("CyLandMaterial")) || PropertyName == FName(TEXT("CyLandHoleMaterial")) || PropertyName == FName(TEXT("CyLandMaterialsOverride")))
	{
		bool RecreateMaterialInstances = true;

		if (PropertyName == FName(TEXT("CyLandMaterialsOverride")) && PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd)
		{
			RecreateMaterialInstances = false;
		}

		if (RecreateMaterialInstances)
		{
			{
				FMaterialUpdateContext MaterialUpdateContext;
				GetCyLandInfo()->UpdateLayerInfoMap(/*this*/);

				// Clear the parents out of combination material instances
				for (const auto& MICPair : MaterialInstanceConstantMap)
				{
					UMaterialInstanceConstant* MaterialInstance = MICPair.Value;
					MaterialInstance->BasePropertyOverrides.bOverride_BlendMode = false;
					MaterialInstance->SetParentEditorOnly(nullptr);
					MaterialUpdateContext.AddMaterialInstance(MaterialInstance);
				}

				// Remove our references to any material instances
				MaterialInstanceConstantMap.Empty();
			}

			UpdateAllComponentMaterialInstances();

			UWorld* World = GetWorld();

			if (World != nullptr && World->FeatureLevel <= ERHIFeatureLevel::ES3_1)
			{
				for (UCyLandComponent * Component : CyLandComponents)
				{
					if (Component != nullptr)
					{
						Component->CheckGenerateCyLandPlatformData(false, nullptr);
					}
				}
			}
		}
	}

	// Must do this *after* clamping values
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void ACyLand::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	const FName MemberPropertyName = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;

	bool ChangedMaterial = false;
	bool bNeedsRecalcBoundingBox = false;
	bool bChangedLighting = false;
	bool bChangedNavRelevance = false;
	bool bPropagateToProxies = false;

	UCyLandInfo* Info = GetCyLandInfo();

	if (PropertyName == FName(TEXT("CyLandMaterial")) || PropertyName == FName(TEXT("CyLandHoleMaterial")) || MemberPropertyName == FName(TEXT("CyLandMaterialsOverride")))
	{
		bool RecreateMaterialInstances = true;

		if (PropertyName == FName(TEXT("CyLandMaterialsOverride")) && PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd)
		{
			RecreateMaterialInstances = false;
		}		

		if (Info != nullptr && RecreateMaterialInstances)
		{
			FMaterialUpdateContext MaterialUpdateContext;
			Info->UpdateLayerInfoMap(/*this*/);

			ChangedMaterial = true;

			// Clear the parents out of combination material instances
			for (const auto& MICPair : MaterialInstanceConstantMap)
			{
				UMaterialInstanceConstant* MaterialInstance = MICPair.Value;
				MaterialInstance->BasePropertyOverrides.bOverride_BlendMode = false;
				MaterialInstance->SetParentEditorOnly(nullptr);
				MaterialUpdateContext.AddMaterialInstance(MaterialInstance);
			}

			// Remove our references to any material instances
			MaterialInstanceConstantMap.Empty();
		}
	}
	else if (PropertyName == FName(TEXT("RelativeScale3D")) ||
		PropertyName == FName(TEXT("RelativeLocation")) ||
		PropertyName == FName(TEXT("RelativeRotation")))
	{
		if (Info != nullptr)
		{
			// update transformations for all linked proxies 
			Info->FixupProxiesTransform();
			bNeedsRecalcBoundingBox = true;
		}
	}
	else if (GIsEditor && PropertyName == FName(TEXT("MaxLODLevel")))
	{
		MaxLODLevel = FMath::Clamp<int32>(MaxLODLevel, -1, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
		bPropagateToProxies = true;
	}
	else if (PropertyName == FName(TEXT("TessellationComponentScreenSize")))
	{
		TessellationComponentScreenSize = FMath::Clamp<float>(TessellationComponentScreenSize, 0.01f, 1.0f);
		bPropagateToProxies = true;
	}
	else if (PropertyName == FName(TEXT("ComponentScreenSizeToUseSubSections")))
	{
		ComponentScreenSizeToUseSubSections = FMath::Clamp<float>(ComponentScreenSizeToUseSubSections, 0.01f, 1.0f);
		bPropagateToProxies = true;
	}
	else if (PropertyName == FName(TEXT("UseTessellationComponentScreenSizeFalloff")))
	{
		bPropagateToProxies = true;
	}
	else if (PropertyName == FName(TEXT("TessellationComponentScreenSizeFalloff")))
	{
		TessellationComponentScreenSizeFalloff = FMath::Clamp<float>(TessellationComponentScreenSizeFalloff, 0.01f, 1.0f);
		bPropagateToProxies = true;
	}
	else if (PropertyName == FName(TEXT("LODDistributionSetting")))
	{
		LODDistributionSetting = FMath::Clamp<float>(LODDistributionSetting, 1.0f, 10.0f);
		bPropagateToProxies = true;
	}
	else if (PropertyName == FName(TEXT("LOD0DistributionSetting")))
	{
		LOD0DistributionSetting = FMath::Clamp<float>(LOD0DistributionSetting, 1.0f, 10.0f);
		bPropagateToProxies = true;
	}
	else if (PropertyName == FName(TEXT("CollisionMipLevel")))
	{
		CollisionMipLevel = FMath::Clamp<int32>(CollisionMipLevel, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
		bPropagateToProxies = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, SimpleCollisionMipLevel))
	{
		SimpleCollisionMipLevel = FMath::Clamp<int32>(SimpleCollisionMipLevel, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
		bPropagateToProxies = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, bBakeMaterialPositionOffsetIntoCollision))
	{
		bPropagateToProxies = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, OccluderGeometryLOD))
	{
		bPropagateToProxies = true;
	}
	else if (GIsEditor && PropertyName == FName(TEXT("StaticLightingResolution")))
	{
		StaticLightingResolution = ::AdjustStaticLightingResolution(StaticLightingResolution, NumSubsections, SubsectionSizeQuads, ComponentSizeQuads);
		bChangedLighting = true;
	}
	else if (GIsEditor && PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, StaticLightingLOD))
	{
		StaticLightingLOD = FMath::Clamp<int32>(StaticLightingLOD, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
		bChangedLighting = true;
	}
	else if (GIsEditor && PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, ExportLOD))
	{
		ExportLOD = FMath::Clamp<int32>(ExportLOD, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
	}
	else if (GIsEditor && PropertyName == GET_MEMBER_NAME_CHECKED(ACyLandProxy, bUsedForNavigation))
	{
		bChangedNavRelevance = true;
	}

	// Must do this *after* clamping values
	Super::PostEditChangeProperty(PropertyChangedEvent);

	bPropagateToProxies = bPropagateToProxies || bNeedsRecalcBoundingBox || bChangedLighting;

	if (Info != nullptr)
	{
		if (bPropagateToProxies)
		{
			// Propagate Event to Proxies...
			for (ACyLandProxy* Proxy : Info->Proxies)
			{
				Proxy->GetSharedProperties(this);
				Proxy->PostEditChangeProperty(PropertyChangedEvent);
			}
		}

		// Update normals if DrawScale3D is changed
		if (PropertyName == FName(TEXT("RelativeScale3D")))
		{
			FCyLandEditDataInterface CyLandEdit(Info);
			CyLandEdit.RecalculateNormals();
		}

		if (bNeedsRecalcBoundingBox || ChangedMaterial || bChangedLighting || bChangedNavRelevance)
		{
			// We cannot iterate the XYtoComponentMap directly because reregistering components modifies the array.
			TArray<UCyLandComponent*> AllComponents;
			Info->XYtoComponentMap.GenerateValueArray(AllComponents);
			for (UCyLandComponent* Comp : AllComponents)
			{
				if (ensure(Comp))
				{
					Comp->Modify();

					if (bNeedsRecalcBoundingBox)
					{
						Comp->UpdateCachedBounds();
						Comp->UpdateBounds();
					}

					if (bChangedLighting)
					{
						Comp->InvalidateLightingCache();
					}

					if (bChangedNavRelevance)
					{
						Comp->UpdateNavigationRelevance();
					}
				}
			}

			if (ChangedMaterial)
			{
				UpdateAllComponentMaterialInstances();

				UWorld* World = GetWorld();

				if (World != nullptr && World->FeatureLevel <= ERHIFeatureLevel::ES3_1)
				{
					for (UCyLandComponent * Component : CyLandComponents)
					{
						if (Component != nullptr)
						{
							Component->CheckGenerateCyLandPlatformData(false, nullptr);
						}
					}
				}
			}
		}

		// Need to update Gizmo scene proxy
		if (bNeedsRecalcBoundingBox && GetWorld())
		{
			for (ACyLandGizmoActiveActor* Gizmo : TActorRange<ACyLandGizmoActiveActor>(GetWorld()))
			{
				Gizmo->MarkComponentsRenderStateDirty();
			}
		}

		// Must be done after the AActor::PostEditChange as we depend on the relinking of the landscapeInfo->CyLandActor
		if (ChangedMaterial)
		{
			CyLandMaterialChangedDelegate.Broadcast();
		}
	}
}

void ACyLandProxy::ChangedPhysMaterial()
{
	UCyLandInfo* CyLandInfo = GetCyLandInfo();
	if (!CyLandInfo) return;
	for (auto It = CyLandInfo->XYtoComponentMap.CreateIterator(); It; ++It)
	{
		UCyLandComponent* Comp = It.Value();
		if (Comp)
		{
			UCyLandHeightfieldCollisionComponent* CollisionComponent = Comp->CollisionComponent.Get();
			if (CollisionComponent)
			{
				Comp->UpdateCollisionLayerData();
				// Physical materials cooked into collision object, so we need to recreate it
				CollisionComponent->RecreateCollision();
			}
		}
	}
}

void UCyLandComponent::SetLOD(bool bForcedLODChanged, int32 InLODValue)
{
	if (bForcedLODChanged)
	{
		ForcedLOD = InLODValue;
		if (ForcedLOD >= 0)
		{
			ForcedLOD = FMath::Clamp<int32>(ForcedLOD, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
		}
		else
		{
			ForcedLOD = -1;
		}
	}
	else
	{
		int32 MaxLOD = FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1;
		LODBias = FMath::Clamp<int32>(InLODValue, -MaxLOD, MaxLOD);
	}

	InvalidateLightingCache();
	MarkRenderStateDirty();

	// Update neighbor components
	UCyLandInfo* Info = GetCyLandInfo();
	if (Info)
	{
		FIntPoint ComponentBase = GetSectionBase() / ComponentSizeQuads;
		FIntPoint CyLandKey[8] =
		{
			ComponentBase + FIntPoint(-1, -1),
			ComponentBase + FIntPoint(+0, -1),
			ComponentBase + FIntPoint(+1, -1),
			ComponentBase + FIntPoint(-1, +0),
			ComponentBase + FIntPoint(+1, +0),
			ComponentBase + FIntPoint(-1, +1),
			ComponentBase + FIntPoint(+0, +1),
			ComponentBase + FIntPoint(+1, +1)
		};

		for (int32 Idx = 0; Idx < 8; ++Idx)
		{
			UCyLandComponent* Comp = Info->XYtoComponentMap.FindRef(CyLandKey[Idx]);
			if (Comp)
			{
				Comp->Modify();
				Comp->InvalidateLightingCache();
				Comp->MarkRenderStateDirty();
			}
		}
	}
}

void UCyLandComponent::PreEditChange(UProperty* PropertyThatWillChange)
{
	Super::PreEditChange(PropertyThatWillChange);
	if (GIsEditor && PropertyThatWillChange && (PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(UCyLandComponent, ForcedLOD) || PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(UCyLandComponent, LODBias)))
	{
		// PreEdit unregister component and re-register after PostEdit so we will lose XYtoComponentMap for this component
		UCyLandInfo* Info = GetCyLandInfo();
		if (Info)
		{
			FIntPoint ComponentKey = GetSectionBase() / ComponentSizeQuads;
			auto RegisteredComponent = Info->XYtoComponentMap.FindRef(ComponentKey);

			if (RegisteredComponent == nullptr)
			{
				Info->XYtoComponentMap.Add(ComponentKey, this);
			}
		}
	}
}

void UCyLandComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	const FName MemberPropertyName = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;

	if (PropertyName == FName(TEXT("OverrideMaterial")) || MemberPropertyName == FName(TEXT("OverrideMaterials")))
	{
		bool RecreateMaterialInstances = true;

		if (PropertyName == FName(TEXT("OverrideMaterials")) && PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd)
		{
			RecreateMaterialInstances = false;
		}

		if (RecreateMaterialInstances)
		{
			UpdateMaterialInstances();

			UWorld* World = GetWorld();

			if (World != nullptr && World->FeatureLevel <= ERHIFeatureLevel::ES3_1)
			{
				CheckGenerateCyLandPlatformData(false, nullptr);
			}
		}
	}
	else if (GIsEditor && (PropertyName == FName(TEXT("ForcedLOD")) || PropertyName == FName(TEXT("LODBias"))))
	{
		bool bForcedLODChanged = PropertyName == FName(TEXT("ForcedLOD"));
		SetLOD(bForcedLODChanged, bForcedLODChanged ? ForcedLOD : LODBias);
	}
	else if (GIsEditor && PropertyName == FName(TEXT("StaticLightingResolution")))
	{
		if (StaticLightingResolution > 0.0f)
		{
			StaticLightingResolution = ::AdjustStaticLightingResolution(StaticLightingResolution, NumSubsections, SubsectionSizeQuads, ComponentSizeQuads);
		}
		else
		{
			StaticLightingResolution = 0;
		}
		InvalidateLightingCache();
	}
	else if (GIsEditor && PropertyName == FName(TEXT("LightingLODBias")))
	{
		int32 MaxLOD = FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1;
		LightingLODBias = FMath::Clamp<int32>(LightingLODBias, -1, MaxLOD);
		InvalidateLightingCache();
	}
	else if (GIsEditor &&
		(PropertyName == GET_MEMBER_NAME_CHECKED(UCyLandComponent, CollisionMipLevel) ||
		 PropertyName == GET_MEMBER_NAME_CHECKED(UCyLandComponent, SimpleCollisionMipLevel)))
	{
		CollisionMipLevel = FMath::Clamp<int32>(CollisionMipLevel, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
		SimpleCollisionMipLevel = FMath::Clamp<int32>(SimpleCollisionMipLevel, 0, FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1);
		if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
		{
			UpdateCollisionData(true); // Rebuild for new CollisionMipLevel
		}
	}

	// Must do this *after* clamping values
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

TSet<class UCyLandComponent*> UCyLandInfo::GetSelectedComponents() const
{
	return SelectedComponents;
}

TSet<class UCyLandComponent*> UCyLandInfo::GetSelectedRegionComponents() const
{
	return SelectedRegionComponents;
}

void UCyLandInfo::UpdateSelectedComponents(TSet<UCyLandComponent*>& NewComponents, bool bIsComponentwise /*=true*/)
{
	int32 InSelectType = bIsComponentwise ? FCyLandEditToolRenderData::ST_COMPONENT : FCyLandEditToolRenderData::ST_REGION;

	if (bIsComponentwise)
	{
		for (TSet<UCyLandComponent*>::TIterator It(NewComponents); It; ++It)
		{
			UCyLandComponent* Comp = *It;
			if ((Comp->EditToolRenderData.SelectedType & InSelectType) == 0)
			{
				Comp->Modify();
				int32 SelectedType = Comp->EditToolRenderData.SelectedType;
				SelectedType |= InSelectType;
				Comp->EditToolRenderData.UpdateSelectionMaterial(SelectedType, Comp);
				Comp->UpdateEditToolRenderData();
			}
		}

		// Remove the material from any old components that are no longer in the region
		TSet<UCyLandComponent*> RemovedComponents = SelectedComponents.Difference(NewComponents);
		for (TSet<UCyLandComponent*>::TIterator It(RemovedComponents); It; ++It)
		{
			UCyLandComponent* Comp = *It;
			Comp->Modify();
			int32 SelectedType = Comp->EditToolRenderData.SelectedType;
			SelectedType &= ~InSelectType;
			Comp->EditToolRenderData.UpdateSelectionMaterial(SelectedType, Comp);
			Comp->UpdateEditToolRenderData();
		}
		SelectedComponents = NewComponents;
	}
	else
	{
		// Only add components...
		if (NewComponents.Num())
		{
			for (TSet<UCyLandComponent*>::TIterator It(NewComponents); It; ++It)
			{
				UCyLandComponent* Comp = *It;
				if ((Comp->EditToolRenderData.SelectedType & InSelectType) == 0)
				{
					Comp->Modify();
					int32 SelectedType = Comp->EditToolRenderData.SelectedType;
					SelectedType |= InSelectType;
					Comp->EditToolRenderData.UpdateSelectionMaterial(SelectedType, Comp);
					Comp->UpdateEditToolRenderData();
				}

				SelectedRegionComponents.Add(*It);
			}
		}
		else
		{
			// Remove the material from any old components that are no longer in the region
			for (TSet<UCyLandComponent*>::TIterator It(SelectedRegionComponents); It; ++It)
			{
				UCyLandComponent* Comp = *It;
				Comp->Modify();
				int32 SelectedType = Comp->EditToolRenderData.SelectedType;
				SelectedType &= ~InSelectType;
				Comp->EditToolRenderData.UpdateSelectionMaterial(SelectedType, Comp);
				Comp->UpdateEditToolRenderData();
			}
			SelectedRegionComponents = NewComponents;
		}
	}
}

void UCyLandInfo::SortSelectedComponents()
{
	struct FCompareUCyLandComponentBySectionBase
	{
		FORCEINLINE bool operator()(const UCyLandComponent& A, const UCyLandComponent& B) const
		{
			return (A.GetSectionBase().X == B.GetSectionBase().X) ? (A.GetSectionBase().Y < B.GetSectionBase().Y) : (A.GetSectionBase().X < B.GetSectionBase().X);
		}
	};
	SelectedComponents.Sort(FCompareUCyLandComponentBySectionBase());
}

void UCyLandInfo::ClearSelectedRegion(bool bIsComponentwise /*= true*/)
{
	TSet<UCyLandComponent*> NewComponents;
	UpdateSelectedComponents(NewComponents, bIsComponentwise);
	if (!bIsComponentwise)
	{
		SelectedRegion.Empty();
	}
}

void UCyLandComponent::ReallocateWeightmaps(FCyLandEditDataInterface* DataInterface)
{
	ACyLandProxy* Proxy = GetCyLandProxy();

	int32 NeededNewChannels = 0;
	for (int32 LayerIdx = 0; LayerIdx < WeightmapLayerAllocations.Num(); LayerIdx++)
	{
		if (WeightmapLayerAllocations[LayerIdx].WeightmapTextureIndex == 255)
		{
			NeededNewChannels++;
		}
	}

	// All channels allocated!
	if (NeededNewChannels == 0)
	{
		return;
	}

	Modify();
	//CyLand->Modify();
	Proxy->Modify();

	// UE_LOG(LogCyLand, Log, TEXT("----------------------"));
	// UE_LOG(LogCyLand, Log, TEXT("Component %s needs %d layers (%d new)"), *GetName(), WeightmapLayerAllocations.Num(), NeededNewChannels);

	// See if our existing textures have sufficient space
	int32 ExistingTexAvailableChannels = 0;
	for (int32 TexIdx = 0; TexIdx < WeightmapTextures.Num(); TexIdx++)
	{
		FCyLandWeightmapUsage* Usage = Proxy->WeightmapUsageMap.Find(WeightmapTextures[TexIdx]);
		check(Usage);

		ExistingTexAvailableChannels += Usage->CyFreeChannelCount();

		if (ExistingTexAvailableChannels >= NeededNewChannels)
		{
			break;
		}
	}

	if (ExistingTexAvailableChannels >= NeededNewChannels)
	{
		// UE_LOG(LogCyLand, Log, TEXT("Existing texture has available channels"));

		// Allocate using our existing textures' spare channels.
		for (int32 TexIdx = 0; TexIdx < WeightmapTextures.Num(); TexIdx++)
		{
			FCyLandWeightmapUsage* Usage = Proxy->WeightmapUsageMap.Find(WeightmapTextures[TexIdx]);

			for (int32 ChanIdx = 0; ChanIdx < 4; ChanIdx++)
			{
				if (Usage->ChannelUsage[ChanIdx] == nullptr)
				{
					for (int32 LayerIdx = 0; LayerIdx < WeightmapLayerAllocations.Num(); LayerIdx++)
					{
						FCyWeightmapLayerAllocationInfo& AllocInfo = WeightmapLayerAllocations[LayerIdx];
						if (AllocInfo.WeightmapTextureIndex == 255)
						{
							// Zero out the data for this texture channel
							if (DataInterface)
							{
								DataInterface->ZeroTextureChannel(WeightmapTextures[TexIdx], ChanIdx);
							}

							AllocInfo.WeightmapTextureIndex = TexIdx;
							AllocInfo.WeightmapTextureChannel = ChanIdx;
							Usage->ChannelUsage[ChanIdx] = this;
							NeededNewChannels--;

							if (NeededNewChannels == 0)
							{
								return;
							}
						}
					}
				}
			}
		}
		// we should never get here.
		check(false);
	}

	// UE_LOG(LogCyLand, Log, TEXT("Reallocating."));

	// We are totally reallocating the weightmap
	int32 TotalNeededChannels = WeightmapLayerAllocations.Num();
	int32 CurrentLayer = 0;
	TArray<UTexture2D*> NewWeightmapTextures;
	while (TotalNeededChannels > 0)
	{
		// UE_LOG(LogCyLand, Log, TEXT("Still need %d channels"), TotalNeededChannels);

		UTexture2D* CurrentWeightmapTexture = nullptr;
		FCyLandWeightmapUsage* CurrentWeightmapUsage = nullptr;

		if (TotalNeededChannels < 4)
		{
			// UE_LOG(LogCyLand, Log, TEXT("Looking for nearest"));

			// see if we can find a suitable existing weightmap texture with sufficient channels
			int32 BestDistanceSquared = MAX_int32;
			for (TMap<UTexture2D*, struct FCyLandWeightmapUsage>::TIterator It(Proxy->WeightmapUsageMap); It; ++It)
			{
				FCyLandWeightmapUsage* TryWeightmapUsage = &It.Value();
				if (TryWeightmapUsage->CyFreeChannelCount() >= TotalNeededChannels)
				{
					// See if this candidate is closer than any others we've found
					for (int32 ChanIdx = 0; ChanIdx < 4; ChanIdx++)
					{
						if (TryWeightmapUsage->ChannelUsage[ChanIdx] != nullptr)
						{
							int32 TryDistanceSquared = (TryWeightmapUsage->ChannelUsage[ChanIdx]->GetSectionBase() - GetSectionBase()).SizeSquared();
							if (TryDistanceSquared < BestDistanceSquared)
							{
								CurrentWeightmapTexture = It.Key();
								CurrentWeightmapUsage = TryWeightmapUsage;
								BestDistanceSquared = TryDistanceSquared;
							}
						}
					}
				}
			}
		}

		bool NeedsUpdateResource = false;
		// No suitable weightmap texture
		if (CurrentWeightmapTexture == nullptr)
		{
			MarkPackageDirty();

			// Weightmap is sized the same as the component
			int32 WeightmapSize = (SubsectionSizeQuads + 1) * NumSubsections;

			// We need a new weightmap texture
			CurrentWeightmapTexture = GetCyLandProxy()->CreateCyLandTexture(WeightmapSize, WeightmapSize, TEXTUREGROUP_Terrain_Weightmap, TSF_BGRA8);
			// Alloc dummy mips
			CreateEmptyTextureMips(CurrentWeightmapTexture);
			CurrentWeightmapTexture->PostEditChange();

			// Store it in the usage map
			CurrentWeightmapUsage = &Proxy->WeightmapUsageMap.Add(CurrentWeightmapTexture, FCyLandWeightmapUsage());

			// UE_LOG(LogCyLand, Log, TEXT("Making a new texture %s"), *CurrentWeightmapTexture->GetName());
		}

		NewWeightmapTextures.Add(CurrentWeightmapTexture);

		for (int32 ChanIdx = 0; ChanIdx < 4 && TotalNeededChannels > 0; ChanIdx++)
		{
			// UE_LOG(LogCyLand, Log, TEXT("Finding allocation for layer %d"), CurrentLayer);

			if (CurrentWeightmapUsage->ChannelUsage[ChanIdx] == nullptr)
			{
				// Use this allocation
				FCyWeightmapLayerAllocationInfo& AllocInfo = WeightmapLayerAllocations[CurrentLayer];

				if (AllocInfo.WeightmapTextureIndex == 255)
				{
					// New layer - zero out the data for this texture channel
					if (DataInterface)
					{
						DataInterface->ZeroTextureChannel(CurrentWeightmapTexture, ChanIdx);
						// UE_LOG(LogCyLand, Log, TEXT("Zeroing out channel %s.%d"), *CurrentWeightmapTexture->GetName(), ChanIdx);
					}
				}
				else
				{
					UTexture2D* OldWeightmapTexture = WeightmapTextures[AllocInfo.WeightmapTextureIndex];

					// Copy the data
					if (ensure(DataInterface != nullptr)) // it's not safe to skip the copy
					{
						DataInterface->CopyTextureChannel(CurrentWeightmapTexture, ChanIdx, OldWeightmapTexture, AllocInfo.WeightmapTextureChannel);
						DataInterface->ZeroTextureChannel(OldWeightmapTexture, AllocInfo.WeightmapTextureChannel);
						// UE_LOG(LogCyLand, Log, TEXT("Copying old channel (%s).%d to new channel (%s).%d"), *OldWeightmapTexture->GetName(), AllocInfo.WeightmapTextureChannel, *CurrentWeightmapTexture->GetName(), ChanIdx);
					}

					// Remove the old allocation
					FCyLandWeightmapUsage* OldWeightmapUsage = Proxy->WeightmapUsageMap.Find(OldWeightmapTexture);
					OldWeightmapUsage->ChannelUsage[AllocInfo.WeightmapTextureChannel] = nullptr;
				}

				// Assign the new allocation
				CurrentWeightmapUsage->ChannelUsage[ChanIdx] = this;
				AllocInfo.WeightmapTextureIndex = NewWeightmapTextures.Num() - 1;
				AllocInfo.WeightmapTextureChannel = ChanIdx;
				CurrentLayer++;
				TotalNeededChannels--;
			}
		}
	}

	// Replace the weightmap textures
	WeightmapTextures = MoveTemp(NewWeightmapTextures);

	if (DataInterface)
	{
		// Update the mipmaps for the textures we edited
		for (int32 Idx = 0; Idx < WeightmapTextures.Num(); Idx++)
		{
			UTexture2D* WeightmapTexture = WeightmapTextures[Idx];
			FCyLandTextureDataInfo* WeightmapDataInfo = DataInterface->GetTextureDataInfo(WeightmapTexture);

			int32 NumMips = WeightmapTexture->Source.GetNumMips();
			TArray<FColor*> WeightmapTextureMipData;
			WeightmapTextureMipData.AddUninitialized(NumMips);
			for (int32 MipIdx = 0; MipIdx < NumMips; MipIdx++)
			{
				WeightmapTextureMipData[MipIdx] = (FColor*)WeightmapDataInfo->GetMipData(MipIdx);
			}

			UCyLandComponent::UpdateWeightmapMips(NumSubsections, SubsectionSizeQuads, WeightmapTexture, WeightmapTextureMipData, 0, 0, MAX_int32, MAX_int32, WeightmapDataInfo);
		}
	}
}

void ACyLandProxy::RemoveInvalidWeightmaps()
{
	if (GIsEditor)
	{
		for (TMap< UTexture2D*, struct FCyLandWeightmapUsage >::TIterator It(WeightmapUsageMap); It; ++It)
		{
			UTexture2D* Tex = It.Key();
			FCyLandWeightmapUsage& Usage = It.Value();
			if (Usage.CyFreeChannelCount() == 4) // Invalid Weight-map
			{
				if (Tex)
				{
					Tex->SetFlags(RF_Transactional);
					Tex->Modify();
					Tex->MarkPackageDirty();
					Tex->ClearFlags(RF_Standalone);
				}
				WeightmapUsageMap.Remove(Tex);
			}
		}

		// Remove Unused Weightmaps...
		for (int32 Idx = 0; Idx < CyLandComponents.Num(); ++Idx)
		{
			UCyLandComponent* Component = CyLandComponents[Idx];
			Component->RemoveInvalidWeightmaps();
		}
	}
}

void UCyLandComponent::RemoveInvalidWeightmaps()
{
	// Adjust WeightmapTextureIndex index for other layers
	TSet<int32> UnUsedTextureIndices;
	{
		TSet<int32> UsedTextureIndices;
		for (int32 LayerIdx = 0; LayerIdx < WeightmapLayerAllocations.Num(); LayerIdx++)
		{
			UsedTextureIndices.Add(WeightmapLayerAllocations[LayerIdx].WeightmapTextureIndex);
		}

		for (int32 WeightIdx = 0; WeightIdx < WeightmapTextures.Num(); ++WeightIdx)
		{
			if (!UsedTextureIndices.Contains(WeightIdx))
			{
				UnUsedTextureIndices.Add(WeightIdx);
			}
		}
	}

	int32 RemovedTextures = 0;
	for (int32 UnusedIndex : UnUsedTextureIndices)
	{
		int32 WeightmapTextureIndexToRemove = UnusedIndex - RemovedTextures;
		WeightmapTextures[WeightmapTextureIndexToRemove]->SetFlags(RF_Transactional);
		WeightmapTextures[WeightmapTextureIndexToRemove]->Modify();
		WeightmapTextures[WeightmapTextureIndexToRemove]->MarkPackageDirty();
		WeightmapTextures[WeightmapTextureIndexToRemove]->ClearFlags(RF_Standalone);
		WeightmapTextures.RemoveAt(WeightmapTextureIndexToRemove);

		// Adjust WeightmapTextureIndex index for other layers
		for (int32 LayerIdx = 0; LayerIdx < WeightmapLayerAllocations.Num(); LayerIdx++)
		{
			FCyWeightmapLayerAllocationInfo& Allocation = WeightmapLayerAllocations[LayerIdx];

			if (Allocation.WeightmapTextureIndex > WeightmapTextureIndexToRemove)
			{
				Allocation.WeightmapTextureIndex--;
			}

			checkSlow(Allocation.WeightmapTextureIndex < WeightmapTextures.Num());
		}
		RemovedTextures++;
	}
}

void UCyLandComponent::InitHeightmapData(TArray<FColor>& Heights, bool bUpdateCollision)
{
	int32 ComponentSizeVerts = NumSubsections * (SubsectionSizeQuads + 1);

	if (Heights.Num() != FMath::Square(ComponentSizeVerts))
	{
		return;
	}

	// Handling old Height map....
	if (HeightmapTexture && HeightmapTexture->GetOutermost() != GetTransientPackage()
		&& HeightmapTexture->GetOutermost() == GetOutermost()
		&& HeightmapTexture->Source.GetSizeX() >= ComponentSizeVerts) // if Height map is not valid...
	{
		HeightmapTexture->SetFlags(RF_Transactional);
		HeightmapTexture->Modify();
		HeightmapTexture->MarkPackageDirty();
		HeightmapTexture->ClearFlags(RF_Standalone); // Delete if no reference...
	}

	// New Height map
	TArray<FColor*> HeightmapTextureMipData;
	// make sure the heightmap UVs are powers of two.
	int32 HeightmapSizeU = (1 << FMath::CeilLogTwo(ComponentSizeVerts));
	int32 HeightmapSizeV = (1 << FMath::CeilLogTwo(ComponentSizeVerts));

	// Height map construction
	SetHeightmap(GetCyLandProxy()->CreateCyLandTexture(HeightmapSizeU, HeightmapSizeV, TEXTUREGROUP_Terrain_Heightmap, TSF_BGRA8));

	int32 MipSubsectionSizeQuads = SubsectionSizeQuads;
	int32 MipSizeU = HeightmapSizeU;
	int32 MipSizeV = HeightmapSizeV;

	HeightmapScaleBias = FVector4(1.0f / (float)HeightmapSizeU, 1.0f / (float)HeightmapSizeV, 0.0f, 0.0f);

	int32 Mip = 0;
	while (MipSizeU > 1 && MipSizeV > 1 && MipSubsectionSizeQuads >= 1)
	{
		FColor* HeightmapTextureData = (FColor*)GetHeightmap()->Source.LockMip(Mip);
		if (Mip == 0)
		{
			FMemory::Memcpy(HeightmapTextureData, Heights.GetData(), MipSizeU*MipSizeV*sizeof(FColor));
		}
		else
		{
			FMemory::Memzero(HeightmapTextureData, MipSizeU*MipSizeV*sizeof(FColor));
		}
		HeightmapTextureMipData.Add(HeightmapTextureData);

		MipSizeU >>= 1;
		MipSizeV >>= 1;
		Mip++;

		MipSubsectionSizeQuads = ((MipSubsectionSizeQuads + 1) >> 1) - 1;
	}
	UCyLandComponent::GenerateHeightmapMips(HeightmapTextureMipData);

	if (bUpdateCollision)
	{
		UpdateCollisionHeightData(
			HeightmapTextureMipData[CollisionMipLevel],
			SimpleCollisionMipLevel > CollisionMipLevel ? HeightmapTextureMipData[SimpleCollisionMipLevel] : nullptr);
	}

	for (int32 i = 0; i < HeightmapTextureMipData.Num(); i++)
	{
		GetHeightmap()->Source.UnlockMip(i);
	}
	GetHeightmap()->PostEditChange();
}

void UCyLandComponent::InitWeightmapData(TArray<UCyLandLayerInfoObject*>& LayerInfos, TArray<TArray<uint8> >& WeightmapData)
{
	if (LayerInfos.Num() != WeightmapData.Num() || LayerInfos.Num() <= 0)
	{
		return;
	}

	int32 ComponentSizeVerts = NumSubsections * (SubsectionSizeQuads + 1);

	// Validation..
	for (int32 Idx = 0; Idx < WeightmapData.Num(); ++Idx)
	{
		if (WeightmapData[Idx].Num() != FMath::Square(ComponentSizeVerts))
		{
			return;
		}
	}

	for (int32 Idx = 0; Idx < WeightmapTextures.Num(); ++Idx)
	{
		if (WeightmapTextures[Idx] && WeightmapTextures[Idx]->GetOutermost() != GetTransientPackage()
			&& WeightmapTextures[Idx]->GetOutermost() == GetOutermost()
			&& WeightmapTextures[Idx]->Source.GetSizeX() == ComponentSizeVerts)
		{
			WeightmapTextures[Idx]->SetFlags(RF_Transactional);
			WeightmapTextures[Idx]->Modify();
			WeightmapTextures[Idx]->MarkPackageDirty();
			WeightmapTextures[Idx]->ClearFlags(RF_Standalone); // Delete if no reference...
		}
	}
	WeightmapTextures.Empty();

	WeightmapLayerAllocations.Empty(LayerInfos.Num());
	for (int32 Idx = 0; Idx < LayerInfos.Num(); ++Idx)
	{
		new (WeightmapLayerAllocations)FCyWeightmapLayerAllocationInfo(LayerInfos[Idx]);
	}

	ReallocateWeightmaps(nullptr);

	check(WeightmapLayerAllocations.Num() > 0 && WeightmapTextures.Num() > 0);

	int32 WeightmapSize = ComponentSizeVerts;
	WeightmapScaleBias = FVector4(1.0f / (float)WeightmapSize, 1.0f / (float)WeightmapSize, 0.5f / (float)WeightmapSize, 0.5f / (float)WeightmapSize);
	WeightmapSubsectionOffset = (float)(SubsectionSizeQuads + 1) / (float)WeightmapSize;

	TArray<void*> WeightmapDataPtrs;
	WeightmapDataPtrs.AddUninitialized(WeightmapTextures.Num());
	for (int32 WeightmapIdx = 0; WeightmapIdx < WeightmapTextures.Num(); ++WeightmapIdx)
	{
		WeightmapDataPtrs[WeightmapIdx] = WeightmapTextures[WeightmapIdx]->Source.LockMip(0);
	}

	for (int32 LayerIdx = 0; LayerIdx < WeightmapLayerAllocations.Num(); ++LayerIdx)
	{
		void* DestDataPtr = WeightmapDataPtrs[WeightmapLayerAllocations[LayerIdx].WeightmapTextureIndex];
		uint8* DestTextureData = (uint8*)DestDataPtr + ChannelOffsets[WeightmapLayerAllocations[LayerIdx].WeightmapTextureChannel];
		uint8* SrcTextureData = (uint8*)&WeightmapData[LayerIdx][0];

		for (int32 i = 0; i < WeightmapData[LayerIdx].Num(); i++)
		{
			DestTextureData[i * 4] = SrcTextureData[i];
		}
	}

	for (int32 Idx = 0; Idx < WeightmapTextures.Num(); Idx++)
	{
		UTexture2D* WeightmapTexture = WeightmapTextures[Idx];
		WeightmapTexture->Source.UnlockMip(0);
	}

	for (int32 Idx = 0; Idx < WeightmapTextures.Num(); Idx++)
	{
		UTexture2D* WeightmapTexture = WeightmapTextures[Idx];
		{
			FCyLandTextureDataInfo WeightmapDataInfo(WeightmapTexture);

			int32 NumMips = WeightmapTexture->Source.GetNumMips();
			TArray<FColor*> WeightmapTextureMipData;
			WeightmapTextureMipData.AddUninitialized(NumMips);
			for (int32 MipIdx = 0; MipIdx < NumMips; MipIdx++)
			{
				WeightmapTextureMipData[MipIdx] = (FColor*)WeightmapDataInfo.GetMipData(MipIdx);
			}

			UCyLandComponent::UpdateWeightmapMips(NumSubsections, SubsectionSizeQuads, WeightmapTexture, WeightmapTextureMipData, 0, 0, MAX_int32, MAX_int32, &WeightmapDataInfo);
		}

		WeightmapTexture->PostEditChange();
	}

	FlushRenderingCommands();

	MaterialInstances.Empty(1);
	MaterialInstances.Add(nullptr);

	LODIndexToMaterialIndex.Empty(1);
	LODIndexToMaterialIndex.Add(0);
}

#define MAX_LANDSCAPE_EXPORT_COMPONENTS_NUM		16
#define MAX_LANDSCAPE_PROP_TEXT_LENGTH			1024*1024*16


bool ACyLandProxy::ShouldExport()
{
	if (!bIsMovingToLevel && CyLandComponents.Num() > MAX_LANDSCAPE_EXPORT_COMPONENTS_NUM)
	{
		// Prompt to save startup packages
		if (EAppReturnType::Yes == FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(
			NSLOCTEXT("UnrealEd", "CyLandExport_Warning", "CyLand has large number({0}) of components, so it will use large amount memory to copy it to the clipboard. Do you want to proceed?"), FText::AsNumber(CyLandComponents.Num()))))
		{
			return true;
		}
		else
		{
			return false;
		}
	}
	return true;
}

bool ACyLandProxy::ShouldImport(FString* ActorPropString, bool IsMovingToLevel)
{
	bIsMovingToLevel = IsMovingToLevel;
	if (!bIsMovingToLevel && ActorPropString && ActorPropString->Len() > MAX_LANDSCAPE_PROP_TEXT_LENGTH)
	{
		// Prompt to save startup packages
		if (EAppReturnType::Yes == FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(
			NSLOCTEXT("UnrealEd", "CyLandImport_Warning", "CyLand is about to import large amount memory ({0}MB) from the clipboard, which will take some time. Do you want to proceed?"), FText::AsNumber(ActorPropString->Len() >> 20))))
		{
			return true;
		}
		else
		{
			return false;
		}
	}
	return true;
}

void UCyLandComponent::ExportCustomProperties(FOutputDevice& Out, uint32 Indent)
{
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}
	// Height map
	int32 NumVertices = FMath::Square(NumSubsections*(SubsectionSizeQuads + 1));
	FCyLandComponentDataInterface DataInterface(this);
	TArray<FColor> Heightmap;
	DataInterface.GetHeightmapTextureData(Heightmap);
	check(Heightmap.Num() == NumVertices);

	Out.Logf(TEXT("%sCustomProperties CyLandHeightData "), FCString::Spc(Indent));
	for (int32 i = 0; i < NumVertices; i++)
	{
		Out.Logf(TEXT("%x "), Heightmap[i].DWColor());
	}

	TArray<uint8> Weightmap;
	// Weight map
	Out.Logf(TEXT("LayerNum=%d "), WeightmapLayerAllocations.Num());
	for (int32 i = 0; i < WeightmapLayerAllocations.Num(); i++)
	{
		if (DataInterface.GetWeightmapTextureData(WeightmapLayerAllocations[i].LayerInfo, Weightmap))
		{
			Out.Logf(TEXT("LayerInfo=%s "), *WeightmapLayerAllocations[i].LayerInfo->GetPathName());
			for (int32 VertexIndex = 0; VertexIndex < NumVertices; VertexIndex++)
			{
				Out.Logf(TEXT("%x "), Weightmap[VertexIndex]);
			}
		}
	}

	Out.Logf(TEXT("\r\n"));
}


void UCyLandComponent::ImportCustomProperties(const TCHAR* SourceText, FFeedbackContext* Warn)
{
	if (FParse::Command(&SourceText, TEXT("CyLandHeightData")))
	{
		int32 NumVertices = FMath::Square(NumSubsections*(SubsectionSizeQuads + 1));

		TArray<FColor> Heights;
		Heights.Empty(NumVertices);
		Heights.AddZeroed(NumVertices);

		FParse::Next(&SourceText);
		int32 i = 0;
		TCHAR* StopStr;
		while (FChar::IsHexDigit(*SourceText))
		{
			if (i < NumVertices)
			{
				Heights[i++].DWColor() = FCString::Strtoi(SourceText, &StopStr, 16);
				while (FChar::IsHexDigit(*SourceText))
				{
					SourceText++;
				}
			}

			FParse::Next(&SourceText);
		}

		if (i != NumVertices)
		{
			Warn->Log(*NSLOCTEXT("Core", "SyntaxError", "Syntax Error").ToString());
		}

		int32 ComponentSizeVerts = NumSubsections * (SubsectionSizeQuads + 1);

		InitHeightmapData(Heights, false);

		// Weight maps
		int32 LayerNum = 0;
		if (FParse::Value(SourceText, TEXT("LayerNum="), LayerNum))
		{
			while (*SourceText && (!FChar::IsWhitespace(*SourceText)))
			{
				++SourceText;
			}
			FParse::Next(&SourceText);
		}

		if (LayerNum <= 0)
		{
			return;
		}

		// Init memory
		TArray<UCyLandLayerInfoObject*> LayerInfos;
		LayerInfos.Empty(LayerNum);
		TArray<TArray<uint8>> WeightmapData;
		for (int32 LayerIndex = 0; LayerIndex < LayerNum; ++LayerIndex)
		{
			TArray<uint8> Weights;
			Weights.Empty(NumVertices);
			Weights.AddUninitialized(NumVertices);
			WeightmapData.Add(Weights);
		}

		int32 LayerIdx = 0;
		FString LayerInfoPath;
		while (*SourceText)
		{
			if (FParse::Value(SourceText, TEXT("LayerInfo="), LayerInfoPath))
			{
				LayerInfos.Add(LoadObject<UCyLandLayerInfoObject>(nullptr, *LayerInfoPath));

				while (*SourceText && (!FChar::IsWhitespace(*SourceText)))
				{
					++SourceText;
				}
				FParse::Next(&SourceText);
				check(*SourceText);

				i = 0;
				while (FChar::IsHexDigit(*SourceText))
				{
					if (i < NumVertices)
					{
						(WeightmapData[LayerIdx])[i++] = (uint8)FCString::Strtoi(SourceText, &StopStr, 16);
						while (FChar::IsHexDigit(*SourceText))
						{
							SourceText++;
						}
					}
					FParse::Next(&SourceText);
				}

				if (i != NumVertices)
				{
					Warn->Log(*NSLOCTEXT("Core", "SyntaxError", "Syntax Error").ToString());
				}
				LayerIdx++;
			}
			else
			{
				break;
			}
		}

		InitWeightmapData(LayerInfos, WeightmapData);
	}
}

bool ACyLandStreamingProxy::IsValidCyLandActor(ACyLand* CyLand)
{
	if (CyLand)
	{
		if (!CyLand->HasAnyFlags(RF_BeginDestroyed))
		{
			if (CyLandActor.IsNull() && !CyLandGuid.IsValid())
			{
				return true; // always valid for newly created Proxy
			}
			if (((CyLandActor && CyLandActor == CyLand)
				|| (CyLandActor.IsNull() && CyLandGuid.IsValid() && CyLandGuid == CyLand->GetCyLandGuid()))
				&& ComponentSizeQuads == CyLand->ComponentSizeQuads
				&& NumSubsections == CyLand->NumSubsections
				&& SubsectionSizeQuads == CyLand->SubsectionSizeQuads)
			{
				return true;
			}
		}
	}
	return false;
}

/* Returns the list of layer names relevant to mobile platforms. Walks the material tree following feature level switch nodes. */
static void GetAllMobileRelevantLayerNames(TSet<FName>& OutLayerNames, UMaterial* InMaterial)
{
	TArray<FMaterialParameterInfo> ParameterInfos;
	TArray<FGuid> ParameterIds;

	TArray<UMaterialExpression*> ES2MobileExpressions;
	InMaterial->GetAllReferencedExpressions(ES2MobileExpressions, nullptr, ERHIFeatureLevel::ES2);
	TArray<UMaterialExpression*> ES31Expressions;
	InMaterial->GetAllReferencedExpressions(ES31Expressions, nullptr, ERHIFeatureLevel::ES3_1);

	TArray<UMaterialExpression*> MobileExpressions = MoveTemp(ES2MobileExpressions);
	for (UMaterialExpression* Expression : ES31Expressions)
	{
		MobileExpressions.AddUnique(Expression);
	}

	for (UMaterialExpression* Expression : MobileExpressions)
	{
		UMaterialExpressionLandscapeLayerWeight* LayerWeightExpression = Cast<UMaterialExpressionLandscapeLayerWeight>(Expression);
		UMaterialExpressionLandscapeLayerSwitch* LayerSwitchExpression = Cast<UMaterialExpressionLandscapeLayerSwitch>(Expression);
		UMaterialExpressionLandscapeLayerSample* LayerSampleExpression = Cast<UMaterialExpressionLandscapeLayerSample>(Expression);
		UMaterialExpressionLandscapeLayerBlend*	LayerBlendExpression = Cast<UMaterialExpressionLandscapeLayerBlend>(Expression);
		UMaterialExpressionLandscapeVisibilityMask* VisibilityMaskExpression = Cast<UMaterialExpressionLandscapeVisibilityMask>(Expression);

		FMaterialParameterInfo BaseParameterInfo;
		BaseParameterInfo.Association = EMaterialParameterAssociation::GlobalParameter;
		BaseParameterInfo.Index = INDEX_NONE;

		if(LayerWeightExpression != nullptr)
		{
			LayerWeightExpression->GetAllParameterInfo(ParameterInfos, ParameterIds, BaseParameterInfo);
		}
		if (LayerSwitchExpression != nullptr)
		{
			LayerSwitchExpression->GetAllParameterInfo(ParameterInfos, ParameterIds, BaseParameterInfo);
		}
		if (LayerSampleExpression != nullptr)
		{
			LayerSampleExpression->GetAllParameterInfo(ParameterInfos, ParameterIds, BaseParameterInfo);
		}
		if (LayerBlendExpression != nullptr)
		{
			LayerBlendExpression->GetAllParameterInfo(ParameterInfos, ParameterIds, BaseParameterInfo);
		}
		if (VisibilityMaskExpression != nullptr)
		{
			VisibilityMaskExpression->GetAllParameterInfo(ParameterInfos, ParameterIds, BaseParameterInfo);
		}
	}

	for (FMaterialParameterInfo& Info : ParameterInfos)
	{
		OutLayerNames.Add(Info.Name);
	}
}

void UCyLandComponent::GenerateMobileWeightmapLayerAllocations()
{
	TSet<FName> LayerNames;
	GetAllMobileRelevantLayerNames(LayerNames, GetCyLandMaterial()->GetMaterial());
	MobileWeightmapLayerAllocations = WeightmapLayerAllocations.FilterByPredicate([&](const FCyWeightmapLayerAllocationInfo& Allocation) -> bool 
		{
			return Allocation.LayerInfo && LayerNames.Contains(Allocation.LayerInfo == ACyLandProxy::VisibilityLayer ? UMaterialExpressionLandscapeVisibilityMask::ParameterName : Allocation.GetLayerName());
		}
	);
	MobileWeightmapLayerAllocations.StableSort(([&](const FCyWeightmapLayerAllocationInfo& A, const FCyWeightmapLayerAllocationInfo& B) -> bool
	{
		UCyLandLayerInfoObject* LhsLayerInfo = A.LayerInfo;
		UCyLandLayerInfoObject* RhsLayerInfo = B.LayerInfo;

		if (!LhsLayerInfo && !RhsLayerInfo) return false; // equally broken :P
		if (!LhsLayerInfo && RhsLayerInfo) return false; // broken layers sort to the end
		if (!RhsLayerInfo && LhsLayerInfo) return true;

		// Sort visibility layer to the front
		if (LhsLayerInfo == ACyLandProxy::VisibilityLayer && RhsLayerInfo != ACyLandProxy::VisibilityLayer) return true;
		if (RhsLayerInfo == ACyLandProxy::VisibilityLayer && LhsLayerInfo != ACyLandProxy::VisibilityLayer) return false;

		// Sort non-weight blended layers to the front so if we have exactly 3 layers, the 3rd is definitely weight-based.
		if (LhsLayerInfo->bNoWeightBlend && !RhsLayerInfo->bNoWeightBlend) return true;
		if (RhsLayerInfo->bNoWeightBlend && !LhsLayerInfo->bNoWeightBlend) return false;

		return false; // equal, preserve order
	}));
}

void UCyLandComponent::GeneratePlatformPixelData()
{
	check(!IsTemplate());

	GenerateMobileWeightmapLayerAllocations();

	int32 WeightmapSize = (SubsectionSizeQuads + 1) * NumSubsections;

	MobileWeightmapTextures.Empty();

    UTexture2D* MobileWeightNormalmapTexture = GetCyLandProxy()->CreateCyLandTexture(WeightmapSize, WeightmapSize, TEXTUREGROUP_Terrain_Weightmap, TSF_BGRA8, nullptr, GMobileCompressCyLandWeightMaps ? true : false );
	CreateEmptyTextureMips(MobileWeightNormalmapTexture);

	{
		FCyLandTextureDataInterface CyLandData;

		// copy normals into B/A channels
		CyLandData.CopyTextureFromHeightmap(MobileWeightNormalmapTexture, 2, this, 2);
		CyLandData.CopyTextureFromHeightmap(MobileWeightNormalmapTexture, 3, this, 3);

		UTexture2D* CurrentWeightmapTexture = MobileWeightNormalmapTexture;
		MobileWeightmapTextures.Add(CurrentWeightmapTexture);
		int32 CurrentChannel = 0;
		int32 RemainingChannels = 2;

		MobileBlendableLayerMask = 0;

		bool bAtLeastOneWeightBasedBlend = MobileWeightmapLayerAllocations.FindByPredicate([&](const FCyWeightmapLayerAllocationInfo& Allocation) -> bool { return !Allocation.LayerInfo->bNoWeightBlend; }) != nullptr;

		for (auto& Allocation : MobileWeightmapLayerAllocations)
		{
			if (Allocation.LayerInfo)
			{
				// If we can pack into 2 channels with the 3rd implied, track the mask for the weight blendable layers
				if (bAtLeastOneWeightBasedBlend && MobileWeightmapLayerAllocations.Num() <= 3)
				{
					MobileBlendableLayerMask |= (!Allocation.LayerInfo->bNoWeightBlend ? (1 << CurrentChannel) : 0);

					// we don't need to create a new texture for the 3rd layer
					if (RemainingChannels == 0)
					{
						Allocation.WeightmapTextureIndex = 0;
						Allocation.WeightmapTextureChannel = 2; // not a valid texture channel, but used for the mask.
						break;
					}
				}

				if (RemainingChannels == 0)
				{

					// create a new weightmap texture if we've run out of channels
					CurrentChannel = 0;
					RemainingChannels = 4;
                    CurrentWeightmapTexture = GetCyLandProxy()->CreateCyLandTexture(WeightmapSize, WeightmapSize, TEXTUREGROUP_Terrain_Weightmap, TSF_BGRA8, nullptr, GMobileCompressCyLandWeightMaps ? true : false);
					CreateEmptyTextureMips(CurrentWeightmapTexture);
					MobileWeightmapTextures.Add(CurrentWeightmapTexture);
				}

				CyLandData.CopyTextureFromWeightmap(CurrentWeightmapTexture, CurrentChannel, this, Allocation.LayerInfo);
				// update Allocation
				Allocation.WeightmapTextureIndex = MobileWeightmapTextures.Num() - 1;
				Allocation.WeightmapTextureChannel = CurrentChannel;
				CurrentChannel++;
				RemainingChannels--;
			}
		}
	}

	for (int TextureIdx = 0; TextureIdx < MobileWeightmapTextures.Num(); TextureIdx++)
	{
		MobileWeightmapTextures[TextureIdx]->PostEditChange();
	}

	FLinearColor Masks[4];
	Masks[0] = FLinearColor(1, 0, 0, 0);
	Masks[1] = FLinearColor(0, 1, 0, 0);
	Masks[2] = FLinearColor(0, 0, 1, 0);
	Masks[3] = FLinearColor(0, 0, 0, 1);


	if (!GIsEditor)
	{
		// This path is used by game mode running with uncooked data, eg standalone executable Mobile Preview.
		// Game mode cannot create MICs, so we use a MaterialInstanceDynamic here.
		
		// Fallback to use non mobile materials if there is no mobile one
		if (MobileCombinationMaterialInstances.Num() == 0)
		{
			MobileCombinationMaterialInstances.Append(MaterialInstances);
		}

		MobileMaterialInterfaces.Reset();
		MobileMaterialInterfaces.Reserve(MobileCombinationMaterialInstances.Num());

		for (int32 MaterialIndex = 0; MaterialIndex < MobileCombinationMaterialInstances.Num(); ++MaterialIndex)
		{
			UMaterialInstanceDynamic* NewMobileMaterialInstance = UMaterialInstanceDynamic::Create(MobileCombinationMaterialInstances[MaterialIndex], GetOutermost());

			// Set the layer mask
			for (const auto& Allocation : MobileWeightmapLayerAllocations)
			{
				if (Allocation.LayerInfo)
				{
					FName LayerName = Allocation.LayerInfo == ACyLandProxy::VisibilityLayer ? UMaterialExpressionLandscapeVisibilityMask::ParameterName : Allocation.LayerInfo->LayerName;
					NewMobileMaterialInstance->SetVectorParameterValue(FName(*FString::Printf(TEXT("LayerMask_%s"), *LayerName.ToString())), Masks[Allocation.WeightmapTextureChannel]);
				}
			}

			for (int TextureIdx = 0; TextureIdx < MobileWeightmapTextures.Num(); TextureIdx++)
			{
				NewMobileMaterialInstance->SetTextureParameterValue(FName(*FString::Printf(TEXT("Weightmap%d"), TextureIdx)), MobileWeightmapTextures[TextureIdx]);
			}

			MobileMaterialInterfaces.Add(NewMobileMaterialInstance);
		}
	}
	else
	{
		// When cooking, we need to make a persistent MIC. In the editor we also do so in
		// case we start a Cook in Editor operation, which will reuse the MIC we create now.

		check(LODIndexToMaterialIndex.Num() > 0);		

		if (MaterialPerLOD.Num() == 0)
		{
			int32 MaxLOD = FMath::CeilLogTwo(SubsectionSizeQuads + 1) - 1;

			for (int32 LODIndex = 0; LODIndex <= MaxLOD; ++LODIndex)
			{
				UMaterialInterface* CurrentMaterial = GetCyLandMaterial(LODIndex);

				if (MaterialPerLOD.Find(CurrentMaterial) == nullptr)
				{
					MaterialPerLOD.Add(CurrentMaterial, LODIndex);
				}
			}
		}

		MobileCombinationMaterialInstances.SetNumZeroed(MaterialPerLOD.Num());
		MobileMaterialInterfaces.Reset();
		MobileMaterialInterfaces.Reserve(MaterialPerLOD.Num());
		int8 MaterialIndex = 0;

		for (auto It = MaterialPerLOD.CreateConstIterator(); It; ++It)
		{
			const int8 MaterialLOD = It.Value();

			// Find or set a matching MIC in the CyLand's map.
			MobileCombinationMaterialInstances[MaterialIndex] = GetCombinationMaterial(nullptr, MobileWeightmapLayerAllocations, MaterialLOD, true);
			check(MobileCombinationMaterialInstances[MaterialIndex] != nullptr);

			UMaterialInstanceConstant* NewMobileMaterialInstance = NewObject<UCyLandMaterialInstanceConstant>(GetOutermost());

			NewMobileMaterialInstance->SetParentEditorOnly(MobileCombinationMaterialInstances[MaterialIndex]);

			// Set the layer mask
			for (const auto& Allocation : MobileWeightmapLayerAllocations)
			{
				if (Allocation.LayerInfo)
				{
					FName LayerName = Allocation.LayerInfo == ACyLandProxy::VisibilityLayer ? UMaterialExpressionLandscapeVisibilityMask::ParameterName : Allocation.LayerInfo->LayerName;
					NewMobileMaterialInstance->SetVectorParameterValueEditorOnly(FName(*FString::Printf(TEXT("LayerMask_%s"), *LayerName.ToString())), Masks[Allocation.WeightmapTextureChannel]);
				}
			}

			for (int TextureIdx = 0; TextureIdx < MobileWeightmapTextures.Num(); TextureIdx++)
			{
				NewMobileMaterialInstance->SetTextureParameterValueEditorOnly(FName(*FString::Printf(TEXT("Weightmap%d"), TextureIdx)), MobileWeightmapTextures[TextureIdx]);
			}

			NewMobileMaterialInstance->PostEditChange();

			MobileMaterialInterfaces.Add(NewMobileMaterialInstance);
			++MaterialIndex;
		}
	}
}

//
// Generates vertex buffer data from the component's heightmap texture, for use on platforms without vertex texture fetch
//
void UCyLandComponent::GeneratePlatformVertexData(const ITargetPlatform* TargetPlatform)
{
	if (IsTemplate())
	{
		return;
	}
	check(GetHeightmap());
	check(GetHeightmap()->Source.GetFormat() == TSF_BGRA8);

	TArray<uint8> NewPlatformData;
	FMemoryWriter PlatformAr(NewPlatformData);

	int32 SubsectionSizeVerts = SubsectionSizeQuads + 1;
	int32 MaxLOD = FMath::CeilLogTwo(SubsectionSizeVerts) - 1;

	float HeightmapSubsectionOffsetU = (float)(SubsectionSizeVerts) / (float)GetHeightmap()->Source.GetSizeX();
	float HeightmapSubsectionOffsetV = (float)(SubsectionSizeVerts) / (float)GetHeightmap()->Source.GetSizeY();
	
	// Get the required mip data
	TArray<TArray<uint8>> HeightmapMipRawData;
	TArray<FColor*> HeightmapMipData;
	for (int32 MipIdx = 0; MipIdx < FMath::Min(LANDSCAPE_MAX_ES_LOD, GetHeightmap()->Source.GetNumMips()); MipIdx++)
	{
		int32 MipSubsectionSizeVerts = (SubsectionSizeVerts) >> MipIdx;
		if (MipSubsectionSizeVerts > 1)
		{
			new(HeightmapMipRawData) TArray<uint8>();
			GetHeightmap()->Source.GetMipData(HeightmapMipRawData.Last(), MipIdx);
			HeightmapMipData.Add((FColor*)HeightmapMipRawData.Last().GetData());
		}
	}

	TMap<uint64, int32> VertexMap;
	TArray<FCyLandVertexRef> VertexOrder;
	VertexOrder.Empty(FMath::Square(SubsectionSizeVerts * NumSubsections));

	// Layout index buffer to determine best vertex order
	for (int32 Mip = MaxLOD; Mip >= 0; Mip--)
	{
		int32 LodSubsectionSizeQuads = (SubsectionSizeVerts >> Mip) - 1;
		float MipRatio = (float)SubsectionSizeQuads / (float)LodSubsectionSizeQuads; // Morph current MIP to base MIP

		for (int32 SubY = 0; SubY < NumSubsections; SubY++)
		{
			for (int32 SubX = 0; SubX < NumSubsections; SubX++)
			{
				for (int32 y = 0; y < LodSubsectionSizeQuads; y++)
				{
					for (int32 x = 0; x < LodSubsectionSizeQuads; x++)
					{
						int32 x0 = FMath::RoundToInt((float)x * MipRatio);
						int32 y0 = FMath::RoundToInt((float)y * MipRatio);
						int32 x1 = FMath::RoundToInt((float)(x + 1) * MipRatio);
						int32 y1 = FMath::RoundToInt((float)(y + 1) * MipRatio);

						FCyLandVertexRef V1(x0, y0, SubX, SubY);
						FCyLandVertexRef V2(x1, y0, SubX, SubY);
						FCyLandVertexRef V3(x1, y1, SubX, SubY);
						FCyLandVertexRef V4(x0, y1, SubX, SubY);

						uint64 Key1 = V1.MakeKey();
						if (VertexMap.Find(Key1) == nullptr)
						{
							VertexMap.Add(Key1, VertexOrder.Num());
							VertexOrder.Add(V1);
						}
						uint64 Key2 = V2.MakeKey();
						if (VertexMap.Find(Key2) == nullptr)
						{
							VertexMap.Add(Key2, VertexOrder.Num());
							VertexOrder.Add(V2);
						}
						uint64 Key3 = V3.MakeKey();
						if (VertexMap.Find(Key3) == nullptr)
						{
							VertexMap.Add(Key3, VertexOrder.Num());
							VertexOrder.Add(V3);
						}
						uint64 Key4 = V4.MakeKey();
						if (VertexMap.Find(Key4) == nullptr)
						{
							VertexMap.Add(Key4, VertexOrder.Num());
							VertexOrder.Add(V4);
						}
					}
				}
			}
		}
	}

	if (VertexOrder.Num() != FMath::Square(SubsectionSizeVerts) * FMath::Square(NumSubsections)) 
	{
		UE_LOG(LogCyLand, Warning, TEXT("VertexOrder count of %d did not match expected size of %d"), 
			VertexOrder.Num(), FMath::Square(SubsectionSizeVerts) * FMath::Square(NumSubsections));
	}

	int32 NumMobileVerices = FMath::Square(SubsectionSizeVerts * NumSubsections);
	TArray<FCyLandMobileVertex> MobileVertices;
	MobileVertices.AddZeroed(NumMobileVerices);
	FCyLandMobileVertex* DstVert = MobileVertices.GetData();

	// Fill in the vertices in the specified order
	for (int32 Idx = 0; Idx < VertexOrder.Num(); Idx++)
	{
		int32 X = VertexOrder[Idx].X;
		int32 Y = VertexOrder[Idx].Y;
		int32 SubX = VertexOrder[Idx].SubX;
		int32 SubY = VertexOrder[Idx].SubY;

		float HeightmapScaleBiasZ = HeightmapScaleBias.Z + HeightmapSubsectionOffsetU * (float)SubX;
		float HeightmapScaleBiasW = HeightmapScaleBias.W + HeightmapSubsectionOffsetV * (float)SubY;
		int32 BaseMipOfsX = FMath::RoundToInt(HeightmapScaleBiasZ * (float)GetHeightmap()->Source.GetSizeX());
		int32 BaseMipOfsY = FMath::RoundToInt(HeightmapScaleBiasW * (float)GetHeightmap()->Source.GetSizeY());

		DstVert->Position[0] = X;
		DstVert->Position[1] = Y;
		DstVert->Position[2] = SubX;
		DstVert->Position[3] = SubY;

		TArray<int32> MipHeights;
		MipHeights.AddZeroed(HeightmapMipData.Num());
		int32 LastIndex = 0;
		uint16 MaxHeight = 0, MinHeight = 65535;

		for (int32 Mip = 0; Mip < HeightmapMipData.Num(); ++Mip)
		{
			int32 MipSizeX = GetHeightmap()->Source.GetSizeX() >> Mip;

			int32 CurrentMipOfsX = BaseMipOfsX >> Mip;
			int32 CurrentMipOfsY = BaseMipOfsY >> Mip;

			int32 MipX = X >> Mip;
			int32 MipY = Y >> Mip;

			FColor* CurrentMipSrcRow = HeightmapMipData[Mip] + (CurrentMipOfsY + MipY) * MipSizeX + CurrentMipOfsX;
			uint16 Height = CurrentMipSrcRow[MipX].R << 8 | CurrentMipSrcRow[MipX].G;

			MipHeights[Mip] = Height;
			MaxHeight = FMath::Max(MaxHeight, Height);
			MinHeight = FMath::Min(MinHeight, Height);
		}

		DstVert->LODHeights[0] = MinHeight >> 8;
		DstVert->LODHeights[1] = (MinHeight & 255);
		DstVert->LODHeights[2] = MaxHeight >> 8;
		DstVert->LODHeights[3] = (MaxHeight & 255);

		for (int32 Mip = 0; Mip < HeightmapMipData.Num(); ++Mip)
		{
			if (Mip < 4)
			{
				DstVert->LODHeights[4 + Mip] = FMath::RoundToInt(float(MipHeights[Mip] - MinHeight) / (MaxHeight - MinHeight) * 255);
			}
			else // Mip 4 5 packed into SubX, SubY
			{
				DstVert->Position[Mip - 2] += (FMath::RoundToInt(float(MipHeights[Mip] - MinHeight) / (MaxHeight - MinHeight) * 255)) & (0xfffe);
			}
		}

		DstVert++;
	}

	PlatformAr << NumMobileVerices;
	PlatformAr.Serialize(MobileVertices.GetData(), NumMobileVerices*sizeof(FCyLandMobileVertex));
	
	// Generate occlusion mesh
	TArray<FVector> OccluderVertices;
	const int32 OcclusionMeshMip = FMath::Clamp<int32>(GetCyLandProxy()->OccluderGeometryLOD, -1, MaxLOD);

	if (OcclusionMeshMip >= 0 && (!TargetPlatform || TargetPlatform->SupportsFeature(ETargetPlatformFeatures::SoftwareOcclusion)))
	{
		int32 LodSubsectionSizeQuads = (SubsectionSizeVerts >> OcclusionMeshMip) - 1;
		float MipRatio = (float)SubsectionSizeQuads / (float)LodSubsectionSizeQuads;
		
		for (int32 SubY = 0; SubY < NumSubsections; SubY++)
		{
			for (int32 SubX = 0; SubX < NumSubsections; SubX++)
			{
				float HeightmapScaleBiasZ = HeightmapScaleBias.Z + HeightmapSubsectionOffsetU * (float)SubX;
				float HeightmapScaleBiasW = HeightmapScaleBias.W + HeightmapSubsectionOffsetV * (float)SubY;
				int32 BaseMipOfsX = FMath::RoundToInt(HeightmapScaleBiasZ * (float)GetHeightmap()->Source.GetSizeX());
				int32 BaseMipOfsY = FMath::RoundToInt(HeightmapScaleBiasW * (float)GetHeightmap()->Source.GetSizeY());

				for (int32 y = 0; y <= LodSubsectionSizeQuads; y++)
				{
					for (int32 x = 0; x <= LodSubsectionSizeQuads; x++)
					{
						int32 MipSizeX = GetHeightmap()->Source.GetSizeX() >> OcclusionMeshMip;

						int32 CurrentMipOfsX = BaseMipOfsX >> OcclusionMeshMip;
						int32 CurrentMipOfsY = BaseMipOfsY >> OcclusionMeshMip;
												
						FColor* CurrentMipSrcRow = HeightmapMipData[OcclusionMeshMip] + (CurrentMipOfsY + y) * MipSizeX + CurrentMipOfsX;
						uint16 Height = CurrentMipSrcRow[x].R << 8 | CurrentMipSrcRow[x].G;

						FVector VtxPos = FVector(x*MipRatio, y*MipRatio, ((float)Height - 32768.f) * LANDSCAPE_ZSCALE);
						OccluderVertices.Add(VtxPos);
					}
				}
			}
		}
	}

	int32 NumOccluderVerices = OccluderVertices.Num();
	PlatformAr << NumOccluderVerices;
	PlatformAr.Serialize(OccluderVertices.GetData(), NumOccluderVerices*sizeof(FVector));
	
	// Copy to PlatformData as Compressed
	PlatformData.InitializeFromUncompressedData(NewPlatformData);
}

UTexture2D* ACyLandProxy::CreateCyLandTexture(int32 InSizeX, int32 InSizeY, TextureGroup InLODGroup, ETextureSourceFormat InFormat, UObject* OptionalOverrideOuter, bool bCompress) const
{
	UObject* TexOuter = OptionalOverrideOuter ? OptionalOverrideOuter : GetOutermost();
	UTexture2D* NewTexture = NewObject<UTexture2D>(TexOuter);
	NewTexture->Source.Init2DWithMipChain(InSizeX, InSizeY, InFormat);
	NewTexture->SRGB = false;
	NewTexture->CompressionNone = !bCompress;
	NewTexture->MipGenSettings = TMGS_LeaveExistingMips;
	NewTexture->AddressX = TA_Clamp;
	NewTexture->AddressY = TA_Clamp;
	NewTexture->LODGroup = InLODGroup;

	return NewTexture;
}

void ACyLandProxy::RemoveOverlappingComponent(UCyLandComponent* Component)
{
	Modify();
	Component->Modify();
	if (Component->CollisionComponent.IsValid() && (Component->CollisionComponent->RenderComponent.Get() == Component || Component->CollisionComponent->RenderComponent.IsNull()))
	{
		Component->CollisionComponent->Modify();
		CollisionComponents.Remove(Component->CollisionComponent.Get());
		Component->CollisionComponent.Get()->DestroyComponent();
	}
	CyLandComponents.Remove(Component);
	Component->DestroyComponent();
}

TArray<FLinearColor> ACyLandProxy::SampleRTData(UTextureRenderTarget2D* InRenderTarget, FLinearColor InRect)
{

	if (!InRenderTarget)
	{
		FMessageLog("Blueprint").Warning(LOCTEXT("SampleRTData_InvalidRenderTarget", "SampleRTData: Render Target must be non-null."));
		return { FLinearColor(0,0,0,0) };
	}
	else if (!InRenderTarget->Resource)
	{
		FMessageLog("Blueprint").Warning(LOCTEXT("SampleRTData_ReleasedRenderTarget", "SampleRTData: Render Target has been released."));
		return { FLinearColor(0,0,0,0) };
	}
	else
	{
		ETextureRenderTargetFormat format = (InRenderTarget->RenderTargetFormat);

		if ((format == (RTF_RGBA16f)) || (format == (RTF_RGBA32f)) || (format == (RTF_RGBA8)))
		{

			FTextureRenderTargetResource* RTResource = InRenderTarget->GameThread_GetRenderTargetResource();

			InRect.R = FMath::Clamp(int(InRect.R), 0, InRenderTarget->SizeX - 1);
			InRect.G = FMath::Clamp(int(InRect.G), 0, InRenderTarget->SizeY - 1);
			InRect.B = FMath::Clamp(int(InRect.B), int(InRect.R + 1), InRenderTarget->SizeX);
			InRect.A = FMath::Clamp(int(InRect.A), int(InRect.G + 1), InRenderTarget->SizeY);
			FIntRect Rect = FIntRect(InRect.R, InRect.G, InRect.B, InRect.A);

			FReadSurfaceDataFlags ReadPixelFlags(RCM_MinMax);

			TArray<FColor> OutLDR;
			TArray<FLinearColor> OutHDR;

			TArray<FLinearColor> OutVals;

			bool ishdr = ((format == (RTF_R16f)) || (format == (RTF_RG16f)) || (format == (RTF_RGBA16f)) || (format == (RTF_R32f)) || (format == (RTF_RG32f)) || (format == (RTF_RGBA32f)));

			if (!ishdr)
			{
				RTResource->ReadPixels(OutLDR, ReadPixelFlags, Rect);
				for (auto i : OutLDR)
				{
					OutVals.Add(FLinearColor(float(i.R), float(i.G), float(i.B), float(i.A)) / 255.0f);
				}
			}
			else
			{
				RTResource->ReadLinearColorPixels(OutHDR, ReadPixelFlags, Rect);
				return OutHDR;
			}

			return OutVals;
		}
	}
	FMessageLog("Blueprint").Warning(LOCTEXT("SampleRTData_InvalidTexture", "SampleRTData: Currently only 4 channel formats are supported: RTF_RGBA8, RTF_RGBA16f, and RTF_RGBA32f."));

	return { FLinearColor(0,0,0,0) };
}

bool ACyLandProxy::CyLandImportHeightmapFromRenderTarget(UTextureRenderTarget2D* InRenderTarget, bool InImportHeightFromRGChannel)
{
	uint64 StartCycle = FPlatformTime::Cycles64();

	ACyLand* CyLand = GetCyLandActor();
	if (CyLand == nullptr)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_NullCyLand", "CyLandImportHeightmapFromRenderTarget: CyLand must be non-null."));
		return false;
	}

	int32 MinX, MinY, MaxX, MaxY;
	UCyLandInfo* CyLandInfo = CyLand->GetCyLandInfo();

	if (!CyLandInfo->GetCyLandExtent(MinX, MinY, MaxX, MaxY))
	{
		FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_InvalidCyLandExtends", "CyLandImportHeightmapFromRenderTarget: The landscape min extends are invalid."));
		return false;
	}

	if (InRenderTarget == nullptr || InRenderTarget->Resource == nullptr)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_InvalidRT", "CyLandImportHeightmapFromRenderTarget: Render Target must be non null and not released."));
		return false;
	}

	FTextureRenderTargetResource* RenderTargetResource = InRenderTarget->GameThread_GetRenderTargetResource();
	FIntRect SampleRect = FIntRect(0, 0, FMath::Min(1 + MaxX - MinX, InRenderTarget->SizeX), FMath::Min(1 + MaxY - MinY, InRenderTarget->SizeY));

	TArray<uint16> HeightData;

	switch (InRenderTarget->RenderTargetFormat)
	{
		case RTF_RGBA16f:
		case RTF_RGBA32f:
		{
			TArray<FLinearColor> OutputRTHeightmap;
			OutputRTHeightmap.Reserve(SampleRect.Width() * SampleRect.Height());

			RenderTargetResource->ReadLinearColorPixels(OutputRTHeightmap, FReadSurfaceDataFlags(RCM_MinMax, CubeFace_MAX), SampleRect);
			HeightData.Reserve(OutputRTHeightmap.Num());

			for (auto LinearColor : OutputRTHeightmap)
			{
				if (InImportHeightFromRGChannel)
				{
					FColor Color = LinearColor.ToFColor(false);
					uint16 Height = ((Color.R << 8) | Color.G);
					HeightData.Add(Height);
				}
				else
				{
					HeightData.Add((uint16)LinearColor.R);
				}
			}
		}
		break;			

		case RTF_RGBA8:
		{
			TArray<FColor> OutputRTHeightmap;
			OutputRTHeightmap.Reserve(SampleRect.Width() * SampleRect.Height());

			RenderTargetResource->ReadPixels(OutputRTHeightmap, FReadSurfaceDataFlags(RCM_MinMax, CubeFace_MAX), SampleRect);
			HeightData.Reserve(OutputRTHeightmap.Num());

			for (FColor Color : OutputRTHeightmap)
			{
				uint16 Height = ((Color.R << 8) | Color.G);
				HeightData.Add(Height);
			}
		}
		break;

		default:
		{
			FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_InvalidRTFormat", "CyLandImportHeightmapFromRenderTarget: The Render Target format is invalid. We only support RTF_RGBA16f, RTF_RGBA32f, RTF_RGBA8"));
			return false;
		}
	}	

	FScopedTransaction Transaction(LOCTEXT("Undo_ImportHeightmap", "Importing CyLand Heightmap"));

	FHeightmapAccessor<false> HeightmapAccessor(CyLandInfo);
	HeightmapAccessor.SetData(MinX, MinY, SampleRect.Width() - 1, SampleRect.Height() - 1, HeightData.GetData());

	double SecondsTaken = FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - StartCycle);
	UE_LOG(LogCyLandBP, Display, TEXT("Took %f seconds to import heightmap from render target."), SecondsTaken);

	return true;
}

bool ACyLandProxy::CyLandExportHeightmapToRenderTarget(UTextureRenderTarget2D* InRenderTarget, bool InExportHeightIntoRGChannel)
{
	uint64 StartCycle = FPlatformTime::Cycles64();

	ACyLand* CyLand = GetCyLandActor();
	if (CyLand == nullptr)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("CyLandExportHeightmapToRenderTarget_NullCyLand.", "CyLandExportHeightmapToRenderTarget: CyLand must be non-null."));
		return false;
	}

	UMaterial* HeightmapRenderMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EditorLandscapeResources/Landscape_Heightmap_To_RenderTarget2D.Landscape_Heightmap_To_RenderTarget2D"));

	if (HeightmapRenderMaterial == nullptr)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("CyLandExportHeightmapToRenderTarget_CyLand_Heightmap_To_RenderTarget2D.", "CyLandExportHeightmapToRenderTarget: Material CyLand_Heightmap_To_RenderTarget2D not found in engine content."));
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	FTextureRenderTargetResource* RenderTargetResource = InRenderTarget->GameThread_GetRenderTargetResource();

	// Create a canvas for the render target and clear it to black
	FCanvas Canvas(RenderTargetResource, nullptr, 0, 0, 0, World->FeatureLevel);
	Canvas.Clear(FLinearColor::Black);

	UCyLandInfo* CyLandInfo = CyLand->GetCyLandInfo();

	TArray<UCyLandComponent*> CyLandComponentsToExport;
	CyLandComponentsToExport.Append(CyLand->CyLandComponents);

	// When run on the landscape actor, we will also export all proxies otherwise, we only export the component linked to the specified proxy
	if (CyLand == CyLandInfo->CyLandActor.Get())
	{
		for (ACyLandProxy* Proxy : CyLandInfo->Proxies)
		{
			CyLandComponentsToExport.Append(Proxy->CyLandComponents);
		}
	}

	struct FTrianglePerMID
	{
		UMaterialInstanceDynamic* HeightmapMID;
		TArray<FCanvasUVTri> TriangleList;
	};

	TMap<UTexture*, FTrianglePerMID> TrianglesPerHeightmap;

	for (const UCyLandComponent* Component : CyLandComponentsToExport)
	{
		FTrianglePerMID* TrianglesPerMID = TrianglesPerHeightmap.Find(Component->GetHeightmap());

		if (TrianglesPerMID == nullptr)
		{
			FTrianglePerMID Data;
			Data.HeightmapMID = UMaterialInstanceDynamic::Create(HeightmapRenderMaterial, this);
			Data.HeightmapMID->SetTextureParameterValue(TEXT("Heightmap"), Component->GetHeightmap());
			Data.HeightmapMID->SetScalarParameterValue(TEXT("ExportHeightIntoRGChannel"), InExportHeightIntoRGChannel);
			TrianglesPerMID = &TrianglesPerHeightmap.Add(Component->GetHeightmap(), Data);
		}

		FIntPoint ComponentSectionBase = Component->GetSectionBase();
		FIntPoint ComponentHeightmapTextureSize(Component->GetHeightmap()->Source.GetSizeX(), Component->GetHeightmap()->Source.GetSizeY());
		int32 SubsectionSizeVerts = Component->SubsectionSizeQuads + 1;
		float HeightmapSubsectionOffsetU = (float)(SubsectionSizeVerts) / (float)ComponentHeightmapTextureSize.X;
		float HeightmapSubsectionOffsetV = (float)(SubsectionSizeVerts) / (float)ComponentHeightmapTextureSize.Y;

		for (int8 SubY = 0; SubY < NumSubsections; ++SubY)
		{
			for (int8 SubX = 0; SubX < NumSubsections; ++SubX)
			{
				FIntPoint SubSectionSectionBase = ComponentSectionBase;
				SubSectionSectionBase.X = ComponentSectionBase.X + Component->SubsectionSizeQuads * SubX;
				SubSectionSectionBase.Y = ComponentSectionBase.Y + Component->SubsectionSizeQuads * SubY;

				// Offset for this component's data in heightmap texture
				float HeightmapOffsetU = Component->HeightmapScaleBias.Z + HeightmapSubsectionOffsetU * (float)SubX;
				float HeightmapOffsetV = Component->HeightmapScaleBias.W + HeightmapSubsectionOffsetV * (float)SubY;

				FCanvasUVTri Tri1;
				Tri1.V0_Pos = FVector2D(SubSectionSectionBase.X, SubSectionSectionBase.Y);
				Tri1.V1_Pos = FVector2D(SubSectionSectionBase.X + SubsectionSizeVerts, SubSectionSectionBase.Y);
				Tri1.V2_Pos = FVector2D(SubSectionSectionBase.X + SubsectionSizeVerts, SubSectionSectionBase.Y + SubsectionSizeVerts);

				Tri1.V0_UV = FVector2D(HeightmapOffsetU, HeightmapOffsetV);
				Tri1.V1_UV = FVector2D(HeightmapOffsetU + HeightmapSubsectionOffsetU, HeightmapOffsetV);
				Tri1.V2_UV = FVector2D(HeightmapOffsetU + HeightmapSubsectionOffsetU, HeightmapOffsetV + HeightmapSubsectionOffsetV);
				TrianglesPerMID->TriangleList.Add(Tri1);

				FCanvasUVTri Tri2;
				Tri2.V0_Pos = FVector2D(SubSectionSectionBase.X + SubsectionSizeVerts, SubSectionSectionBase.Y + SubsectionSizeVerts);
				Tri2.V1_Pos = FVector2D(SubSectionSectionBase.X, SubSectionSectionBase.Y + SubsectionSizeVerts);
				Tri2.V2_Pos = FVector2D(SubSectionSectionBase.X, SubSectionSectionBase.Y);

				Tri2.V0_UV = FVector2D(HeightmapOffsetU + HeightmapSubsectionOffsetU, HeightmapOffsetV + HeightmapSubsectionOffsetV);
				Tri2.V1_UV = FVector2D(HeightmapOffsetU, HeightmapOffsetV + HeightmapSubsectionOffsetV);
				Tri2.V2_UV = FVector2D(HeightmapOffsetU, HeightmapOffsetV);

				TrianglesPerMID->TriangleList.Add(Tri2);
			}
		}
	}

	for (auto& TriangleList : TrianglesPerHeightmap)
	{
		FCanvasTriangleItem TriItemList(MoveTemp(TriangleList.Value.TriangleList), nullptr);
		TriItemList.MaterialRenderProxy = TriangleList.Value.HeightmapMID->GetRenderProxy();
		TriItemList.BlendMode = SE_BLEND_Opaque;
		TriItemList.SetColor(FLinearColor::White);

		TriItemList.Draw(&Canvas);
	}

	TrianglesPerHeightmap.Reset();

	// Tell the rendering thread to draw any remaining batched elements
	Canvas.Flush_GameThread(true);

	ENQUEUE_RENDER_COMMAND(DrawHeightmapRTCommand)(
		[RenderTargetResource](FRHICommandListImmediate& RHICmdList)
		{
			// Copy (resolve) the rendered image from the frame buffer to its render target texture
			RHICmdList.CopyToResolveTarget(
				RenderTargetResource->GetRenderTargetTexture(),		// Source texture
				RenderTargetResource->TextureRHI,					// Dest texture
				FResolveParams());									// Resolve parameters
		});


	FlushRenderingCommands();

	double SecondsTaken = FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - StartCycle);
	UE_LOG(LogCyLandBP, Display, TEXT("Took %f seconds to export heightmap to render target."), SecondsTaken);

	return true;
}

bool ACyLandProxy::CyLandImportWeightmapFromRenderTarget(UTextureRenderTarget2D* InRenderTarget, FName InLayerName)
{
	ACyLand* CyLand = GetCyLandActor();
	if (CyLand != nullptr)
	{
		UCyLandInfo* CyLandInfo = CyLand->GetCyLandInfo();

		int32 MinX, MinY, MaxX, MaxY;
		if (CyLandInfo && CyLandInfo->GetCyLandExtent(MinX, MinY, MaxX, MaxY))
		{
			const uint32 CyLandWidth = (uint32)(1 + MaxX - MinX);
			const uint32 CyLandHeight = (uint32)(1 + MaxY - MinY);
			FLinearColor SampleRect = FLinearColor(0, 0, CyLandWidth, CyLandHeight);

			const uint32 RTWidth = InRenderTarget->SizeX;
			const uint32 RTHeight = InRenderTarget->SizeY;
			ETextureRenderTargetFormat format = (InRenderTarget->RenderTargetFormat);

			if (RTWidth >= CyLandWidth && RTHeight >= CyLandHeight)
			{
				TArray<FLinearColor> RTData;
				RTData = SampleRTData(InRenderTarget, SampleRect);

				TArray<uint8> LayerData;

				for (auto i : RTData)
				{
					LayerData.Add((uint8)(FMath::Clamp((float)i.R, 0.0f, 1.0f) * 255));
				}

				FCyLandInfoLayerSettings CurWeightmapInfo;

				int32 Index = CyLandInfo->GetLayerInfoIndex(InLayerName, CyLandInfo->GetCyLandProxy());

				if (ensure(Index != INDEX_NONE))
				{
					CurWeightmapInfo = CyLandInfo->Layers[Index];
				}

				if (CurWeightmapInfo.LayerInfoObj == nullptr)
				{
					FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportRenderTarget_InvalidLayerInfoObject", "CyLandImportWeightmapFromRenderTarget: Layers must first have Layer Info Objects assigned before importing."));
					return false;
				}

				FScopedTransaction Transaction(LOCTEXT("Undo_ImportWeightmap", "Importing CyLand Layer"));

				FAlphamapAccessor<false, false> AlphamapAccessor(CyLandInfo, CurWeightmapInfo.LayerInfoObj);
				AlphamapAccessor.SetData(MinX, MinY, MaxX, MaxY, LayerData.GetData(), ECyLandLayerPaintingRestriction::None);

				uint64 CycleEnd = FPlatformTime::Cycles64();
				UE_LOG(LogCyLand, Log, TEXT("Took %f seconds to import heightmap from render target"), FPlatformTime::ToSeconds64(CycleEnd));

				return true;
			}
			else
			{
				FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportRenderTarget_InvalidRenderTarget", "CyLandImportWeightmapFromRenderTarget: Render target must be at least as large as landscape on each axis."));
				return false;
			}
		}
		else
		{
			return false;
		}
	}
	FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportRenderTarget_NullCyLand.", "CyLandImportWeightmapFromRenderTarget: CyLand must be non-null."));
	return false;
}

bool ACyLandProxy::CyLandExportWeightmapToRenderTarget(UTextureRenderTarget2D* InRenderTarget, FName InLayerName)
{
	return false;
}

#endif //WITH_EDITOR


bool ACyLandProxy::CyLandImportHeightmapFromRenderTargetmy(UTextureRenderTarget2D* InRenderTarget, bool InImportHeightFromRGChannel)
{
	uint64 StartCycle = FPlatformTime::Cycles64();

	ACyLand* CyLand = GetCyLandActor();
	if (CyLand == nullptr)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_NullCyLand", "CyLandImportHeightmapFromRenderTarget: CyLand must be non-null."));
		return false;
	}

	int32 MinX, MinY, MaxX, MaxY;
	UCyLandInfo* CyLandInfo = CyLand->GetCyLandInfo();

	//if (!CyLandInfo->GetCyLandExtent(MinX, MinY, MaxX, MaxY))
	//{
	//	FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_InvalidCyLandExtends", "CyLandImportHeightmapFromRenderTarget: The landscape min extends are invalid."));
	//	return false;
	//}
	if (!CyLandInfo) {
		CyLandInfo = CyLand->CreateCyLandInfo();
		CyLandInfo->ComponentNumSubsections = CyLand->NumSubsections;
		CyLandInfo->ComponentSizeQuads = CyLand->ComponentSizeQuads;
		CyLandInfo->SubsectionSizeQuads = CyLand->SubsectionSizeQuads;
		CyLandInfo->DrawScale = CyLand->GetActorScale();
		CyLandInfo->CyLandGuid = CyLand->GetCyLandGuid();
		CyLandInfo->CyLandActor = CyLand;
	}

	MinX = MAX_int32;
	MinY = MAX_int32;
	MaxX = MIN_int32;
	MaxY = MIN_int32;
	for (auto component : CyLand->CyLandComponents) {
		component->GetComponentExtent(MinX, MinY, MaxX, MaxY);
		if(MinX == MAX_int32)
		{
			FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_InvalidCyLandExtends", "CyLandImportHeightmapFromRenderTarget: The landscape min extends are invalid."));
			return false;
		}
	}

	if (InRenderTarget == nullptr || InRenderTarget->Resource == nullptr)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_InvalidRT", "CyLandImportHeightmapFromRenderTarget: Render Target must be non null and not released."));
		return false;
	}

	FTextureRenderTargetResource* RenderTargetResource = InRenderTarget->GameThread_GetRenderTargetResource();
	FIntRect SampleRect = FIntRect(0, 0, FMath::Min(1 + MaxX - MinX, InRenderTarget->SizeX), FMath::Min(1 + MaxY - MinY, InRenderTarget->SizeY));

	TArray<uint16> HeightData;

	switch (InRenderTarget->RenderTargetFormat)
	{
	case RTF_RGBA16f:
	case RTF_RGBA32f:
	{
		TArray<FLinearColor> OutputRTHeightmap;
		OutputRTHeightmap.Reserve(SampleRect.Width() * SampleRect.Height());

		RenderTargetResource->ReadLinearColorPixels(OutputRTHeightmap, FReadSurfaceDataFlags(RCM_MinMax, CubeFace_MAX), SampleRect);
		HeightData.Reserve(OutputRTHeightmap.Num());

		for (auto LinearColor : OutputRTHeightmap)
		{
			if (InImportHeightFromRGChannel)
			{
				FColor Color = LinearColor.ToFColor(false);
				uint16 Height = ((Color.R << 8) | Color.G);
				HeightData.Add(Height);
			}
			else
			{
				HeightData.Add((uint16)LinearColor.R);
			}
		}
	}
	break;

	case RTF_RGBA8:
	{
		TArray<FColor> OutputRTHeightmap;
		OutputRTHeightmap.Reserve(SampleRect.Width() * SampleRect.Height());

		RenderTargetResource->ReadPixels(OutputRTHeightmap, FReadSurfaceDataFlags(RCM_MinMax, CubeFace_MAX), SampleRect);
		HeightData.Reserve(OutputRTHeightmap.Num());

		for (FColor Color : OutputRTHeightmap)
		{
			uint16 Height = ((Color.R << 8) | Color.G);
			HeightData.Add(Height);
		}
	}
	break;

	default:
	{
		FMessageLog("Blueprint").Error(LOCTEXT("CyLandImportHeightmapFromRenderTarget_InvalidRTFormat", "CyLandImportHeightmapFromRenderTarget: The Render Target format is invalid. We only support RTF_RGBA16f, RTF_RGBA32f, RTF_RGBA8"));
		return false;
	}
	}

	FScopedTransaction Transaction(LOCTEXT("Undo_ImportHeightmap", "Importing CyLand Heightmap"));

	FHeightmapAccessor<false> HeightmapAccessor(CyLandInfo);
	HeightmapAccessor.SetData(*CyLand, MinX, MinY, SampleRect.Width() - 1, SampleRect.Height() - 1, HeightData.GetData());

	double SecondsTaken = FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - StartCycle);
	UE_LOG(LogCyLandBP, Display, TEXT("Took %f seconds to import heightmap from render target."), SecondsTaken);

	return true;
}

template<bool bInUseInterp>
void FHeightmapAccessor<bInUseInterp>::SetData(const ACyLand& land, int32 X1, int32 Y1, int32 X2, int32 Y2, const uint16* Data, ECyLandLayerPaintingRestriction PaintingRestriction)
{
	TSet<UCyLandComponent*> Components;
	for (auto comp : land.CyLandComponents) {
		Components.Add(comp);
	}
	if (CyLandInfo)
	{
		// Update data
		ChangedComponents.Append(Components);

		for (UCyLandComponent* Component : Components)
		{
			Component->InvalidateLightingCache();
		}

		// Flush dynamic foliage (grass)
		ACyLandProxy::InvalidateGeneratedComponentData(Components);

		// Notify foliage to move any attached instances
		bool bUpdateFoliage = false;
		for (UCyLandComponent* Component : Components)
		{
			UCyLandHeightfieldCollisionComponent* CollisionComponent = Component->CollisionComponent.Get();
			if (CollisionComponent && AInstancedFoliageActor::HasFoliageAttached(CollisionComponent))
			{
				bUpdateFoliage = true;
				break;
			}
		}

		if (bUpdateFoliage)
		{
			// Calculate landscape local-space bounding box of old data, to look for foliage instances.
			TArray<UCyLandHeightfieldCollisionComponent*> CollisionComponents;
			CollisionComponents.Empty(Components.Num());
			TArray<FBox> PreUpdateLocalBoxes;
			PreUpdateLocalBoxes.Empty(Components.Num());

			for (UCyLandComponent* Component : Components)
			{
				UCyLandHeightfieldCollisionComponent* CollisionComponent = Component->CollisionComponent.Get();
				if (CollisionComponent)
				{
					CollisionComponents.Add(CollisionComponent);
					PreUpdateLocalBoxes.Add(FBox(FVector((float)X1, (float)Y1, Component->CachedLocalBox.Min.Z), FVector((float)X2, (float)Y2, Component->CachedLocalBox.Max.Z)));
				}
			}

			// Update landscape.
			CyLandEdit->SetHeightData(X1, Y1, X2, Y2, Data, 0, true);

			// Snap foliage for each component.
			for (int32 Index = 0; Index < CollisionComponents.Num(); ++Index)
			{
				UCyLandHeightfieldCollisionComponent* CollisionComponent = CollisionComponents[Index];
				CollisionComponent->SnapFoliageInstances(PreUpdateLocalBoxes[Index].TransformBy(CyLandInfo->GetCyLandProxy()->CyLandActorToWorld().ToMatrixWithScale()).ExpandBy(1.0f));
			}
		}
		else
		{
			// No foliage, just update landscape.
			CyLandEdit->SetHeightData(X1, Y1, X2, Y2, Data, 0, true);
		}
	}
}

#undef LOCTEXT_NAMESPACE
