// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "CyLandEditorDetails.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxDefs.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SComboButton.h"
#include "CyLandEdMode.h"
#include "CyLandEditorDetailCustomization_NewCyLand.h"
#include "CyLandEditorDetailCustomization_ResizeCyLand.h"
#include "CyLandEditorDetailCustomization_CopyPaste.h"
#include "CyLandEditorDetailCustomization_MiscTools.h"
#include "CyLandEditorDetailCustomization_AlphaBrush.h"
#include "DetailWidgetRow.h"
#include "CyLandEditorDetailCustomization_TargetLayers.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"

#include "CyLandEditorCommands.h"
#include "CyLandEditorDetailWidgets.h"
#include "CyLandEditorDetailCustomization_ProceduralBrushStack.h"
#include "Settings/EditorExperimentalSettings.h"

#define LOCTEXT_NAMESPACE "CyLandEditor"


TSharedRef<IDetailCustomization> FCyLandEditorDetails::MakeInstance()
{
	return MakeShareable(new FCyLandEditorDetails);
}

void FCyLandEditorDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	CommandList = CyLandEdMode->GetUICommandList();

	static const FLinearColor BorderColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.2f);
	static const FSlateBrush* BorderStyle = FEditorStyle::GetBrush("DetailsView.GroupSection");

	IDetailCategoryBuilder& CyLandEditorCategory = DetailBuilder.EditCategory("CyLandEditor", FText::GetEmpty(), ECategoryPriority::TypeSpecific);

	CyLandEditorCategory.AddCustomRow(FText::GetEmpty())
	.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateStatic(&FCyLandEditorDetails::GetTargetCyLandSelectorVisibility)))
	[
		SNew(SComboButton)
		.OnGetMenuContent_Static(&FCyLandEditorDetails::GetTargetCyLandMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text_Static(&FCyLandEditorDetails::GetTargetCyLandName)
		]
	];

	FToolSelectorBuilder ToolBrushSelectorButtons(CommandList, FMultiBoxCustomization::None);
	{
		FUIAction ToolSelectorUIAction;
		//ToolSelectorUIAction.IsActionVisibleDelegate.BindSP(this, &FCyLandEditorDetails::GetToolSelectorIsVisible);
		ToolBrushSelectorButtons.AddComboButton(
			ToolSelectorUIAction,
			FOnGetContent::CreateSP(this, &FCyLandEditorDetails::GetToolSelector),
			LOCTEXT("ToolSelector", "Tool"),
			TAttribute<FText>(this, &FCyLandEditorDetails::GetCurrentToolName),
			LOCTEXT("ToolSelector.Tooltip", "Select Tool"),
			TAttribute<FSlateIcon>(this, &FCyLandEditorDetails::GetCurrentToolIcon)
			);

		FUIAction BrushSelectorUIAction;
		BrushSelectorUIAction.IsActionVisibleDelegate.BindSP(this, &FCyLandEditorDetails::GetBrushSelectorIsVisible);
		ToolBrushSelectorButtons.AddComboButton(
			BrushSelectorUIAction,
			FOnGetContent::CreateSP(this, &FCyLandEditorDetails::GetBrushSelector),
			LOCTEXT("BrushSelector", "Brush"),
			TAttribute<FText>(this, &FCyLandEditorDetails::GetCurrentBrushName),
			LOCTEXT("BrushSelector.Tooltip", "Select Brush"),
			TAttribute<FSlateIcon>(this, &FCyLandEditorDetails::GetCurrentBrushIcon)
			);

		FUIAction BrushFalloffSelectorUIAction;
		BrushFalloffSelectorUIAction.IsActionVisibleDelegate.BindSP(this, &FCyLandEditorDetails::GetBrushFalloffSelectorIsVisible);
		ToolBrushSelectorButtons.AddComboButton(
			BrushFalloffSelectorUIAction,
			FOnGetContent::CreateSP(this, &FCyLandEditorDetails::GetBrushFalloffSelector),
			LOCTEXT("BrushFalloffSelector", "Falloff"),
			TAttribute<FText>(this, &FCyLandEditorDetails::GetCurrentBrushFalloffName),
			LOCTEXT("BrushFalloffSelector.Tooltip", "Select Brush Falloff Type"),
			TAttribute<FSlateIcon>(this, &FCyLandEditorDetails::GetCurrentBrushFalloffIcon)
			);
	}

	CyLandEditorCategory.AddCustomRow(FText::GetEmpty())
	.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateSP(this, &FCyLandEditorDetails::GetToolSelectorVisibility)))
	[
		ToolBrushSelectorButtons.MakeWidget()
	];

	// Tools:
	Customization_NewCyLand = MakeShareable(new FCyLandEditorDetailCustomization_NewCyLand);
	Customization_NewCyLand->CustomizeDetails(DetailBuilder);
	Customization_ResizeCyLand = MakeShareable(new FCyLandEditorDetailCustomization_ResizeCyLand);
	Customization_ResizeCyLand->CustomizeDetails(DetailBuilder);
	Customization_CopyPaste = MakeShareable(new FCyLandEditorDetailCustomization_CopyPaste);
	Customization_CopyPaste->CustomizeDetails(DetailBuilder);
	Customization_MiscTools = MakeShareable(new FCyLandEditorDetailCustomization_MiscTools);
	Customization_MiscTools->CustomizeDetails(DetailBuilder);

	// Brushes:
	Customization_AlphaBrush = MakeShareable(new FCyLandEditorDetailCustomization_AlphaBrush);
	Customization_AlphaBrush->CustomizeDetails(DetailBuilder);

	// Target Layers:
	Customization_TargetLayers = MakeShareable(new FCyLandEditorDetailCustomization_TargetLayers);
	Customization_TargetLayers->CustomizeDetails(DetailBuilder);

	if (GetMutableDefault<UEditorExperimentalSettings>()->bProceduralLandscape)
	{
		// Brush Stack
		Customization_ProceduralBrushStack = MakeShareable(new FCyLandEditorDetailCustomization_ProceduralBrushStack);
		Customization_ProceduralBrushStack->CustomizeDetails(DetailBuilder);

		// Procedural Layers
		Customization_ProceduralLayers = MakeShareable(new FCyLandEditorDetailCustomization_ProceduralLayers);
		Customization_ProceduralLayers->CustomizeDetails(DetailBuilder);
	}
}

FText FCyLandEditorDetails::GetLocalizedName(FString Name)
{
	static bool bInitialized = false;
	if (!bInitialized)
	{
		bInitialized = true;
		LOCTEXT("ToolSet_NewCyLand", "New CyLand");
		LOCTEXT("ToolSet_ResizeCyLand", "Change Component Size");
		LOCTEXT("ToolSet_Sculpt", "Sculpt");
		LOCTEXT("ToolSet_Paint", "Paint");
		LOCTEXT("ToolSet_Smooth", "Smooth");
		LOCTEXT("ToolSet_Flatten", "Flatten");
		LOCTEXT("ToolSet_Ramp", "Ramp");
		LOCTEXT("ToolSet_Erosion", "Erosion");
		LOCTEXT("ToolSet_HydraErosion", "HydroErosion");
		LOCTEXT("ToolSet_Noise", "Noise");
		LOCTEXT("ToolSet_Retopologize", "Retopologize");
		LOCTEXT("ToolSet_Visibility", "Visibility");

		if (GetMutableDefault<UEditorExperimentalSettings>()->bProceduralLandscape)
		{
			LOCTEXT("ToolSet_BPCustom", "Blueprint Custom");
		}

		LOCTEXT("ToolSet_Select", "Selection");
		LOCTEXT("ToolSet_AddComponent", "Add");
		LOCTEXT("ToolSet_DeleteComponent", "Delete");
		LOCTEXT("ToolSet_MoveToLevel", "Move to Level");

		LOCTEXT("ToolSet_Mask", "Selection");
		LOCTEXT("ToolSet_CopyPaste", "Copy/Paste");
		LOCTEXT("ToolSet_Mirror", "Mirror");

		LOCTEXT("ToolSet_Splines", "Edit Splines");

		LOCTEXT("BrushSet_Circle", "Circle");
		LOCTEXT("BrushSet_Alpha", "Alpha");
		LOCTEXT("BrushSet_Pattern", "Pattern");
		LOCTEXT("BrushSet_Component", "Component");
		LOCTEXT("BrushSet_Gizmo", "Gizmo");

		LOCTEXT("Circle_Smooth", "Smooth");
		LOCTEXT("Circle_Linear", "Linear");
		LOCTEXT("Circle_Spherical", "Spherical");
		LOCTEXT("Circle_Tip", "Tip");
	}

	FText Result;
	ensure(FText::FindText(TEXT("CyLandEditor"), Name, Result));
	return Result;
}

EVisibility FCyLandEditorDetails::GetTargetCyLandSelectorVisibility()
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode && CyLandEdMode->GetCyLandList().Num() > 1)
	{
		return EVisibility::Visible;
	}

	return EVisibility::Collapsed;
}

FText FCyLandEditorDetails::GetTargetCyLandName()
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode)
	{
		UCyLandInfo* Info = CyLandEdMode->CurrentToolTarget.CyLandInfo.Get();
		if (Info)
		{
			ACyLandProxy* Proxy = Info->GetCyLandProxy();
			if (Proxy)
			{
				return FText::FromString(Proxy->GetActorLabel());
			}
		}
	}

	return FText();
}

TSharedRef<SWidget> FCyLandEditorDetails::GetTargetCyLandMenu()
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode)
	{
		FMenuBuilder MenuBuilder(true, NULL);

		const TArray<FCyLandListInfo>& CyLandList = CyLandEdMode->GetCyLandList();
		for (auto It = CyLandList.CreateConstIterator(); It; It++)
		{
			FUIAction Action = FUIAction(FExecuteAction::CreateStatic(&FCyLandEditorDetails::OnChangeTargetCyLand, MakeWeakObjectPtr(It->Info)));
			MenuBuilder.AddMenuEntry(FText::FromString(It->Info->GetCyLandProxy()->GetActorLabel()), FText(), FSlateIcon(), Action);
		}

		return MenuBuilder.MakeWidget();
	}

	return SNullWidget::NullWidget;
}

void FCyLandEditorDetails::OnChangeTargetCyLand(TWeakObjectPtr<UCyLandInfo> CyLandInfo)
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode)
	{
		// Unregister from old one
		if (CyLandEdMode->CurrentToolTarget.CyLandInfo.IsValid())
		{
			ACyLandProxy* CyLandProxy = CyLandEdMode->CurrentToolTarget.CyLandInfo->GetCyLandProxy();
			CyLandProxy->OnMaterialChangedDelegate().RemoveAll(CyLandEdMode);
		}

		CyLandEdMode->CurrentToolTarget.CyLandInfo = CyLandInfo.Get();
		CyLandEdMode->UpdateTargetList();
		// force a Leave and Enter the current tool, in case it has something about the current CyLand cached
		CyLandEdMode->SetCurrentTool(CyLandEdMode->CurrentToolIndex);
		if (CyLandEdMode->CurrentGizmoActor.IsValid())
		{
			CyLandEdMode->CurrentGizmoActor->SetTargetCyLand(CyLandEdMode->CurrentToolTarget.CyLandInfo.Get());
		}

		// register to new one
		if (CyLandEdMode->CurrentToolTarget.CyLandInfo.IsValid())
		{
			ACyLandProxy* CyLandProxy = CyLandEdMode->CurrentToolTarget.CyLandInfo->GetCyLandProxy();
			CyLandProxy->OnMaterialChangedDelegate().AddRaw(CyLandEdMode, &FEdModeCyLand::OnCyLandMaterialChangedDelegate);
		}

		CyLandEdMode->UpdateTargetList();
		CyLandEdMode->UpdateShownLayerList();
	}
}

FText FCyLandEditorDetails::GetCurrentToolName() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode != NULL && CyLandEdMode->CurrentTool != NULL)
	{
		const TCHAR* CurrentToolName = CyLandEdMode->CurrentTool->GetToolName();
		return GetLocalizedName(FString("ToolSet_") + CurrentToolName);
	}

	return LOCTEXT("Unknown", "Unknown");
}

FSlateIcon FCyLandEditorDetails::GetCurrentToolIcon() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode != NULL && CyLandEdMode->CurrentTool != NULL)
	{
		const TCHAR* CurrentToolName = CyLandEdMode->CurrentTool->GetToolName();
		return FCyLandEditorCommands::Get().NameToCommandMap.FindChecked(*(FString("Tool_") + CurrentToolName))->GetIcon();
	}

	return FSlateIcon(FEditorStyle::GetStyleSetName(), "Default");
}

TSharedRef<SWidget> FCyLandEditorDetails::GetToolSelector()
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode != NULL)
	{
		auto NameToCommandMap = FCyLandEditorCommands::Get().NameToCommandMap;

		FToolMenuBuilder MenuBuilder(true, CommandList);

		if (CyLandEdMode->CurrentToolMode->ToolModeName == "ToolMode_Manage")
		{
			MenuBuilder.BeginSection(NAME_None, LOCTEXT("NewCyLandToolsTitle", "New CyLand"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_NewCyLand"), NAME_None, LOCTEXT("Tool.NewCyLand", "New CyLand"), LOCTEXT("Tool.NewCyLand.Tooltip", "Create or import a new CyLand"));
			MenuBuilder.EndSection();

			MenuBuilder.BeginSection(NAME_None, LOCTEXT("ComponentToolsTitle", "Component Tools"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Select"), NAME_None, LOCTEXT("Tool.SelectComponent", "Selection"), LOCTEXT("Tool.SelectComponent.Tooltip", "Select components to use with other tools"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_AddComponent"), NAME_None, LOCTEXT("Tool.AddComponent", "Add"), LOCTEXT("Tool.AddComponent.Tooltip", "Add components to the CyLand"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_DeleteComponent"), NAME_None, LOCTEXT("Tool.DeleteComponent", "Delete"), LOCTEXT("Tool.DeleteComponent.Tooltip", "Delete components from the CyLand, leaving a hole"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_MoveToLevel"), NAME_None, LOCTEXT("Tool.MoveToLevel", "Move to Level"), LOCTEXT("Tool.MoveToLevel.Tooltip", "Move CyLand components to a CyLand proxy in the currently active streaming level, so that they can be streamed in/out independently of the rest of the CyLand"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_ResizeCyLand"), NAME_None, LOCTEXT("Tool.ResizeCyLand", "Change Component Size"), LOCTEXT("Tool.ResizeCyLand.Tooltip", "Change the size of the CyLand components"));
			MenuBuilder.EndSection();

			MenuBuilder.BeginSection(NAME_None, LOCTEXT("SplineToolsTitle", "Spline Tools"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Splines"), NAME_None, LOCTEXT("Tool.Spline", "Edit Splines"), LOCTEXT("Tool.Spline.Tooltip", "Ctrl+click to add control points\nHaving a control point selected when you ctrl+click will connect to the new control point with a segment\nSpline mesh settings can be found on the details panel when you have segments selected"));
			MenuBuilder.EndSection();
		}

		if (CyLandEdMode->CurrentToolMode->ToolModeName == "ToolMode_Sculpt")
		{
			MenuBuilder.BeginSection(NAME_None, LOCTEXT("SculptToolsTitle", "Sculpting Tools"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Sculpt"), NAME_None, LOCTEXT("Tool.Sculpt", "Sculpt"), LOCTEXT("Tool.Sculpt.Tooltip", "Sculpt height data.\nCtrl+Click to Raise, Ctrl+Shift+Click to lower"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Smooth"), NAME_None, LOCTEXT("Tool.Smooth", "Smooth"), LOCTEXT("Tool.Smooth.Tooltip", "Smooths heightmaps or blend layers"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Flatten"), NAME_None, LOCTEXT("Tool.Flatten", "Flatten"), LOCTEXT("Tool.Flatten.Tooltip", "Flattens an area of heightmap or blend layer"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Ramp"), NAME_None, LOCTEXT("Tool.Ramp", "Ramp"), LOCTEXT("Tool.Ramp.Tooltip", "Creates a ramp between two points"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Erosion"), NAME_None, LOCTEXT("Tool.Erosion", "Erosion"), LOCTEXT("Tool.Erosion.Tooltip", "Thermal Erosion - Simulates erosion caused by the movement of soil from higher areas to lower areas"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_HydraErosion"), NAME_None, LOCTEXT("Tool.HydroErosion", "Hydro Erosion"), LOCTEXT("Tool.HydroErosion.Tooltip", "Hydro Erosion - Simulates erosion caused by rainfall"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Noise"), NAME_None, LOCTEXT("Tool.Noise", "Noise"), LOCTEXT("Tool.Noise.Tooltip", "Adds noise to the heightmap or blend layer"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Retopologize"), NAME_None, LOCTEXT("Tool.Retopologize", "Retopologize"), LOCTEXT("Tool.Retopologize.Tooltip", "Automatically adjusts CyLand vertices with an X/Y offset map to improve vertex density on cliffs, reducing texture stretching.\nNote: An X/Y offset map makes the CyLand slower to render and paint on with other tools, so only use if needed"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Visibility"), NAME_None, LOCTEXT("Tool.Visibility", "Visibility"), LOCTEXT("Tool.Visibility.Tooltip", "Mask out individual quads in the CyLand, leaving a hole."));

			if (GetMutableDefault<UEditorExperimentalSettings>()->bProceduralLandscape)
			{
				MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_BPCustom"), NAME_None, LOCTEXT("Tool.SculptBPCustom", "Blueprint Custom"), LOCTEXT("Tool.SculptBPCustom.Tooltip", "Custom sculpting tools created using Blueprint."));
			}

			MenuBuilder.EndSection();

			MenuBuilder.BeginSection(NAME_None, LOCTEXT("RegionToolsTitle", "Region Tools"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Mask"), NAME_None, LOCTEXT("Tool.RegionSelect", "Selection"), LOCTEXT("Tool.RegionSelect.Tooltip", "Select a region of CyLand to use as a mask for other tools"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_CopyPaste"), NAME_None, LOCTEXT("Tool.RegionCopyPaste", "Copy/Paste"), LOCTEXT("Tool.RegionCopyPaste.Tooltip", "Copy/Paste areas of the CyLand, or import/export a copied area of CyLand from disk"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Mirror"), NAME_None, LOCTEXT("Tool.Mirror", "Mirror"), LOCTEXT("Tool.Mirror.Tooltip", "Copies one side of a CyLand to the other, to easily create a mirrored CyLand."));
			MenuBuilder.EndSection();
		}

		if (CyLandEdMode->CurrentToolMode->ToolModeName == "ToolMode_Paint")
		{
			MenuBuilder.BeginSection(NAME_None, LOCTEXT("PaintToolsTitle", "Paint Tools"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Paint"), NAME_None, LOCTEXT("Tool.Paint", "Paint"), LOCTEXT("Tool.Paint.Tooltip", "Paints weight data.\nCtrl+Click to paint, Ctrl+Shift+Click to erase"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Smooth"), NAME_None, LOCTEXT("Tool.Smooth", "Smooth"), LOCTEXT("Tool.Smooth.Tooltip", "Smooths heightmaps or blend layers"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Flatten"), NAME_None, LOCTEXT("Tool.Flatten", "Flatten"), LOCTEXT("Tool.Flatten.Tooltip", "Flattens an area of heightmap or blend layer"));
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_Noise"), NAME_None, LOCTEXT("Tool.Noise", "Noise"), LOCTEXT("Tool.Noise.Tooltip", "Adds noise to the heightmap or blend layer"));

			if (GetMutableDefault<UEditorExperimentalSettings>()->bProceduralLandscape)
			{
				MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("Tool_BPCustom"), NAME_None, LOCTEXT("Tool.PaintBPCustom", "Blueprint Custom"), LOCTEXT("Tool.PaintBPCustom.Tooltip", "Custom painting tools created using Blueprint."));
			}

			MenuBuilder.EndSection();
		}

		return MenuBuilder.MakeWidget();
	}

	return SNullWidget::NullWidget;
}

bool FCyLandEditorDetails::GetToolSelectorIsVisible() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode && CyLandEdMode->CurrentTool)
	{
		if (!IsToolActive("NewCyLand") || CyLandEdMode->GetCyLandList().Num() > 0)
		{
			return true;
		}
	}

	return false;
}

EVisibility FCyLandEditorDetails::GetToolSelectorVisibility() const
{
	if (GetToolSelectorIsVisible())
	{
		return EVisibility::Visible;
	}
	return EVisibility::Collapsed;
}

FText FCyLandEditorDetails::GetCurrentBrushName() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode != NULL && CyLandEdMode->CurrentBrush != NULL)
	{
		const FName CurrentBrushSetName = CyLandEdMode->CyLandBrushSets[CyLandEdMode->CurrentBrushSetIndex].BrushSetName;
		return GetLocalizedName(CurrentBrushSetName.ToString());
	}

	return LOCTEXT("Unknown", "Unknown");
}

FSlateIcon FCyLandEditorDetails::GetCurrentBrushIcon() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode != NULL && CyLandEdMode->CurrentBrush != NULL)
	{
		const FName CurrentBrushSetName = CyLandEdMode->CyLandBrushSets[CyLandEdMode->CurrentBrushSetIndex].BrushSetName;
		TSharedPtr<FUICommandInfo> Command = FCyLandEditorCommands::Get().NameToCommandMap.FindRef(CurrentBrushSetName);
		if (Command.IsValid())
		{
			return Command->GetIcon();
		}
	}

	return FSlateIcon(FEditorStyle::GetStyleSetName(), "Default");
}

TSharedRef<SWidget> FCyLandEditorDetails::GetBrushSelector()
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode && CyLandEdMode->CurrentTool)
	{
		auto NameToCommandMap = FCyLandEditorCommands::Get().NameToCommandMap;

		FToolMenuBuilder MenuBuilder(true, CommandList);
		MenuBuilder.BeginSection(NAME_None, LOCTEXT("BrushesTitle", "Brushes"));

		if (CyLandEdMode->CurrentTool->ValidBrushes.Contains("BrushSet_Circle"))
		{
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("BrushSet_Circle"), NAME_None, LOCTEXT("Brush.Circle", "Circle"), LOCTEXT("Brush.Circle.Brushtip", "Simple circular brush"));
		}
		if (CyLandEdMode->CurrentTool->ValidBrushes.Contains("BrushSet_Alpha"))
		{
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("BrushSet_Alpha"), NAME_None, LOCTEXT("Brush.Alpha.Alpha", "Alpha"), LOCTEXT("Brush.Alpha.Alpha.Tooltip", "Alpha brush, orients a mask image with the brush stroke"));
		}
		if (CyLandEdMode->CurrentTool->ValidBrushes.Contains("BrushSet_Pattern"))
		{
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("BrushSet_Pattern"), NAME_None, LOCTEXT("Brush.Alpha.Pattern", "Pattern"), LOCTEXT("Brush.Alpha.Pattern.Tooltip", "Pattern brush, tiles a mask image across the CyLand"));
		}
		if (CyLandEdMode->CurrentTool->ValidBrushes.Contains("BrushSet_Component"))
		{
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("BrushSet_Component"), NAME_None, LOCTEXT("Brush.Component", "Component"), LOCTEXT("Brush.Component.Brushtip", "Work with entire CyLand components"));
		}
		if (CyLandEdMode->CurrentTool->ValidBrushes.Contains("BrushSet_Gizmo"))
		{
			MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("BrushSet_Gizmo"), NAME_None, LOCTEXT("Brush.Gizmo", "Gizmo"), LOCTEXT("Brush.Gizmo.Brushtip", "Work with the CyLand gizmo, used for copy/pasting CyLand"));
		}
		//if (CyLandEdMode->CurrentTool->ValidBrushes.Contains("BrushSet_Splines"))
		//{
		//	MenuBuilder.AddToolButton(NameToCommandMap.FindChecked("BrushSet_Splines"), NAME_None, LOCTEXT("Brush.Splines", "Splines"), LOCTEXT("Brush.Splines.Brushtip", "Edit Splines"));
		//}
		MenuBuilder.EndSection();

		return MenuBuilder.MakeWidget();
	}

	return SNullWidget::NullWidget;
}

bool FCyLandEditorDetails::GetBrushSelectorIsVisible() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode && CyLandEdMode->CurrentTool)
	{
		if (CyLandEdMode->CurrentTool->ValidBrushes.Num() >= 2)
		{
			return true;
		}
	}

	return false;
}

FText FCyLandEditorDetails::GetCurrentBrushFalloffName() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode != NULL && CyLandEdMode->CurrentBrush != NULL)
	{
		const TCHAR* CurrentBrushName = CyLandEdMode->CurrentBrush->GetBrushName();
		return GetLocalizedName(CurrentBrushName);
	}

	return LOCTEXT("Unknown", "Unknown");
}

FSlateIcon FCyLandEditorDetails::GetCurrentBrushFalloffIcon() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode != NULL && CyLandEdMode->CurrentBrush != NULL)
	{
		const FName CurrentBrushName = CyLandEdMode->CurrentBrush->GetBrushName();
		TSharedPtr<FUICommandInfo> Command = FCyLandEditorCommands::Get().NameToCommandMap.FindRef(CurrentBrushName);
		if (Command.IsValid())
		{
			return Command->GetIcon();
		}
	}

	return FSlateIcon(FEditorStyle::GetStyleSetName(), "Default");
}

TSharedRef<SWidget> FCyLandEditorDetails::GetBrushFalloffSelector()
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode && CyLandEdMode->CurrentTool)
	{
		auto NameToCommandMap = FCyLandEditorCommands::Get().NameToCommandMap;

		FToolMenuBuilder MenuBuilder(true, CommandList);
		MenuBuilder.BeginSection(NAME_None, LOCTEXT("FalloffTitle", "Falloff"));
		MenuBuilder.AddToolButton(FCyLandEditorCommands::Get().CircleBrush_Smooth,    NAME_None, LOCTEXT("Brush.Circle.Smooth", "Smooth"),       LOCTEXT("Brush.Circle.Smooth.Tooltip", "Smooth falloff"));
		MenuBuilder.AddToolButton(FCyLandEditorCommands::Get().CircleBrush_Linear,    NAME_None, LOCTEXT("Brush.Circle.Linear", "Linear"),       LOCTEXT("Brush.Circle.Linear.Tooltip", "Sharp, linear falloff"));
		MenuBuilder.AddToolButton(FCyLandEditorCommands::Get().CircleBrush_Spherical, NAME_None, LOCTEXT("Brush.Circle.Spherical", "Spherical"), LOCTEXT("Brush.Circle.Spherical.Tooltip", "Spherical falloff, smooth at the center and sharp at the edge"));
		MenuBuilder.AddToolButton(FCyLandEditorCommands::Get().CircleBrush_Tip,       NAME_None, LOCTEXT("Brush.Circle.Tip", "Tip"),             LOCTEXT("Brush.Circle.Tip.Tooltip", "Tip falloff, sharp at the center and smooth at the edge"));
		MenuBuilder.EndSection();

		return MenuBuilder.MakeWidget();
	}

	return SNullWidget::NullWidget;
}

bool FCyLandEditorDetails::GetBrushFalloffSelectorIsVisible() const
{
	FEdModeCyLand* CyLandEdMode = GetEditorMode();
	if (CyLandEdMode && CyLandEdMode->CurrentBrush != NULL)
	{
		const FCyLandBrushSet& CurrentBrushSet = CyLandEdMode->CyLandBrushSets[CyLandEdMode->CurrentBrushSetIndex];

		if (CurrentBrushSet.Brushes.Num() >= 2)
		{
			return true;
		}
	}

	return false;
}

#undef LOCTEXT_NAMESPACE
