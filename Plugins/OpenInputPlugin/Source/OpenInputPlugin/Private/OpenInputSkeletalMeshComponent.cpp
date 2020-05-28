// Fill out your copyright notice in the Description page of Project Settings.
#include "OpenInputSkeletalMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "MotionControllerComponent.h"

#if USE_WITH_VR_EXPANSION
#include "GripMotionControllerComponent.h"
#include "VRGlobalSettings.h"
#endif

#include "XRMotionControllerBase.h" // for GetHandEnumForSourceName()
//#include "EngineMinimal.h"

UOpenInputSkeletalMeshComponent::UOpenInputSkeletalMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	//PrimaryComponentTick.bCanEverTick = false;
	//PrimaryComponentTick.bStartWithTickEnabled = false;

	ReplicationRateForSkeletalAnimations = 10.f;
	bReplicateSkeletalData = false;
	bSmoothReplicatedSkeletalData = true;
	ReplicationType = EVRSkeletalReplicationType::Rep_SteamVRCompressedTransforms;
	bOffsetByControllerProfile = true;
	SkeletalNetUpdateCount = 0.f;
	bDetectGestures = true;
	SetIsReplicatedByDefault(true);
}

void UOpenInputSkeletalMeshComponent::GetLifetimeReplicatedProps(TArray< class FLifetimeProperty > & OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Skipping the owner with this as the owner will use the controllers location directly
	DOREPLIFETIME_CONDITION(UOpenInputSkeletalMeshComponent, LeftHandRep, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(UOpenInputSkeletalMeshComponent, RightHandRep, COND_SkipOwner);
}

void UOpenInputSkeletalMeshComponent::Server_SendSkeletalTransforms_Implementation(const FBPSkeletalRepContainer& SkeletalInfo)
{
	for (int i = 0; i < HandSkeletalActions.Num(); i++)
	{
		if (HandSkeletalActions[i].SkeletalData.TargetHand == SkeletalInfo.TargetHand)
		{
			if(SkeletalInfo.ReplicationType != EVRSkeletalReplicationType::Rep_SteamVRCompressedTransforms)
				HandSkeletalActions[i].OldSkeletalTransforms = HandSkeletalActions[i].SkeletalData.SkeletalTransforms;

			FBPSkeletalRepContainer::CopyReplicatedTo(SkeletalInfo, HandSkeletalActions[i]);

			if (HandSkeletalActions[i].CompressedTransforms.Num() > 0)
			{
				UOpenInputFunctionLibrary::DecompressSkeletalData(HandSkeletalActions[i], GetWorld());
				HandSkeletalActions[i].CompressedTransforms.Reset();
			}

			if (SkeletalInfo.TargetHand == EVRActionHand::EActionHand_Left)
			{
				LeftHandRep = SkeletalInfo;
				if (bSmoothReplicatedSkeletalData)
					LeftHandRepManager.NotifyNewData(HandSkeletalActions[i], ReplicationRateForSkeletalAnimations);
			}
			else
			{
				RightHandRep = SkeletalInfo;
				if (bSmoothReplicatedSkeletalData)
					RightHandRepManager.NotifyNewData(HandSkeletalActions[i], ReplicationRateForSkeletalAnimations);
			}

			break;
		}
	}
}

bool UOpenInputSkeletalMeshComponent::Server_SendSkeletalTransforms_Validate(const FBPSkeletalRepContainer& SkeletalInfo)
{
	return true;
}


void UOpenInputSkeletalMeshComponent::NewControllerProfileLoaded()
{
#if USE_WITH_VR_EXPANSION
	GetCurrentProfileTransform(false);
#endif
}

void UOpenInputSkeletalMeshComponent::GetCurrentProfileTransform(bool bBindToNoticationDelegate)
{
#if USE_WITH_VR_EXPANSION
	// Don't run this logic if we aren't parented to a controller
	if(!GetAttachParent() || !GetAttachParent()->IsA(UMotionControllerComponent::StaticClass()))
		return;

	// Need to rep this offset to the server and then down to remote clients as well, otherwise it will not perform correctly
	if (bOffsetByControllerProfile && HandSkeletalActions.Num())
	{
		UVRGlobalSettings* VRSettings = GetMutableDefault<UVRGlobalSettings>();

		if (VRSettings == nullptr)
			return;

		FTransform NewControllerProfileTransform = FTransform::Identity;

		if (HandSkeletalActions[0].SkeletalData.TargetHand == EVRActionHand::EActionHand_Left || !VRSettings->bUseSeperateHandTransforms)
		{
			NewControllerProfileTransform = VRSettings->CurrentControllerProfileTransform;
		}
		else if (HandSkeletalActions[0].SkeletalData.TargetHand == EVRActionHand::EActionHand_Right)
		{
			NewControllerProfileTransform = VRSettings->CurrentControllerProfileTransformRight;
		}

		if (bBindToNoticationDelegate && !NewControllerProfileEvent_Handle.IsValid())
		{
			NewControllerProfileEvent_Handle = VRSettings->OnControllerProfileChangedEvent.AddUObject(this, &UOpenInputSkeletalMeshComponent::NewControllerProfileLoaded);
		}

		if (!NewControllerProfileTransform.Equals(CurrentControllerProfileTransform))
		{
			FTransform OriginalControllerProfileTransform = CurrentControllerProfileTransform;
			CurrentControllerProfileTransform = NewControllerProfileTransform;

			// Auto adjust ourselves
			this->SetRelativeTransform((OriginalControllerProfileTransform.Inverse() * this->GetRelativeTransform()) * CurrentControllerProfileTransform.Inverse());
		}
	}
#endif
}


void FOpenInputAnimInstanceProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	Super::PreUpdate(InAnimInstance, DeltaSeconds);

	if (UOpenInputSkeletalMeshComponent * OwningMesh = Cast<UOpenInputSkeletalMeshComponent>(InAnimInstance->GetOwningComponent()))
	{
		if (HandSkeletalActionData.Num() != OwningMesh->HandSkeletalActions.Num())
		{
			HandSkeletalActionData.Empty(OwningMesh->HandSkeletalActions.Num());
			
			for(FBPOpenVRActionInfo &actionInfo : OwningMesh->HandSkeletalActions)
			{
				HandSkeletalActionData.Add(actionInfo.SkeletalData);
			}
		}
		else
		{
			for (int i = 0; i < OwningMesh->HandSkeletalActions.Num(); ++i)
			{
				HandSkeletalActionData[i] = OwningMesh->HandSkeletalActions[i].SkeletalData;
			}
		}
	}
}

FOpenInputAnimInstanceProxy::FOpenInputAnimInstanceProxy(UAnimInstance* InAnimInstance)
	: FAnimInstanceProxy(InAnimInstance)
{
}


void UOpenInputSkeletalMeshComponent::OnUnregister()
{
#if USE_WITH_VR_EXPANSION
	if (NewControllerProfileEvent_Handle.IsValid())
	{
		UVRGlobalSettings* VRSettings = GetMutableDefault<UVRGlobalSettings>();
		if (VRSettings != nullptr)
		{
			VRSettings->OnControllerProfileChangedEvent.Remove(NewControllerProfileEvent_Handle);
			NewControllerProfileEvent_Handle.Reset();
		}
	}
#endif

	Super::OnUnregister();
}

void UOpenInputSkeletalMeshComponent::BeginPlay()
{
	if (UMotionControllerComponent * MotionParent = Cast<UMotionControllerComponent>(GetAttachParent()))
	{
		EControllerHand HandType;
		if (!FXRMotionControllerBase::GetHandEnumForSourceName(MotionParent->MotionSource, HandType))
		{
			HandType = EControllerHand::Left;
		}

		for (int i = 0; i < HandSkeletalActions.Num(); i++)
		{
			if (HandType == EControllerHand::Left || HandType == EControllerHand::AnyHand)
				HandSkeletalActions[i].SkeletalData.TargetHand = EVRActionHand::EActionHand_Left;
			else
				HandSkeletalActions[i].SkeletalData.TargetHand = EVRActionHand::EActionHand_Right;
		}

	}

	Super::BeginPlay();
}

void UOpenInputSkeletalMeshComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}

void UOpenInputSkeletalMeshComponent::Activate(bool bReset)
{
	Super::Activate(bReset);
}

void UOpenInputSkeletalMeshComponent::Deactivate()
{
	Super::Deactivate();
}

void UOpenInputSkeletalMeshComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	if (!IsLocallyControlled())
	{
		if (bReplicateSkeletalData)
		{
			// Handle bone lerping here if we are replicating
			for (FBPOpenVRActionInfo& actionInfo : HandSkeletalActions)
			{
				if (bSmoothReplicatedSkeletalData)
				{
					if (actionInfo.SkeletalData.TargetHand == EVRActionHand::EActionHand_Left)
					{
						LeftHandRepManager.UpdateManager(DeltaTime, actionInfo);
					}
					else
					{
						RightHandRepManager.UpdateManager(DeltaTime, actionInfo);
					}
				}
			}

			
		}
	}
	else // Get data and process
	{

#if USE_WITH_VR_EXPANSION
		if (bOffsetByControllerProfile && !NewControllerProfileEvent_Handle.IsValid())
		{
			GetCurrentProfileTransform(true);
		}
#endif

		bool bGetCompressedTransforms = false;
		if (bReplicateSkeletalData && HandSkeletalActions.Num() > 0)
		{
			SkeletalNetUpdateCount += DeltaTime;
			if (SkeletalNetUpdateCount >= (1.0f / ReplicationRateForSkeletalAnimations))
			{
				SkeletalNetUpdateCount = 0.0f;
				bGetCompressedTransforms = true;
			}
		}

		for (FBPOpenVRActionInfo& actionInfo : HandSkeletalActions)
		{
			if (UOpenInputFunctionLibrary::GetActionPose(actionInfo, this, (bGetCompressedTransforms && ReplicationType == EVRSkeletalReplicationType::Rep_SteamVRCompressedTransforms)))
			{
				if (bGetCompressedTransforms)
				{
					if (GetNetMode() == NM_Client/* && !IsTornOff()*/)
					{
						if (actionInfo.bHasValidData)
						{
							FBPSkeletalRepContainer ContainerSend;
							ContainerSend.CopyForReplication(actionInfo, ReplicationType);
							Server_SendSkeletalTransforms(ContainerSend);
						}
					}
					else
					{
						if (actionInfo.bHasValidData)
						{
							if (actionInfo.SkeletalData.TargetHand == EVRActionHand::EActionHand_Left)
								LeftHandRep.CopyForReplication(actionInfo, ReplicationType);
							else
								RightHandRep.CopyForReplication(actionInfo, ReplicationType);
						}
					}
				}
			}

			if (bDetectGestures && actionInfo.bHasValidData && GesturesDB != nullptr && GesturesDB->Gestures.Num() > 0)
			{
				DetectCurrentPose(actionInfo);
			}
		}
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UOpenInputSkeletalMeshComponent::SaveCurrentPose(FName RecordingName, bool bUseFingerCurlOnly, EVRActionHand HandToSave)
{

	if (!HandSkeletalActions.Num())
		return;

	// Default to the first hand element so that single length arrays work as is.
	FBPOpenVRActionInfo & HandSkeletalAction = HandSkeletalActions[0];

	// Now check for the specific passed in hand if this is a multi hand
	for (int i = 0; i < HandSkeletalActions.Num(); ++i)
	{
		if (HandSkeletalActions[i].SkeletalData.TargetHand == HandToSave)
		{
			HandSkeletalAction = HandSkeletalActions[i];
			break;
		}
	}

	if (GesturesDB)
	{
		FOpenInputGesture NewGesture(bUseFingerCurlOnly);

		int i = 0;
		for (; i < HandSkeletalAction.PoseFingerData.PoseFingerCurls.Num() && i < NewGesture.FingerValues.Num(); ++i)
			NewGesture.FingerValues[i].Value = HandSkeletalAction.PoseFingerData.PoseFingerCurls[i];

		if (!bUseFingerCurlOnly && HandSkeletalAction.PoseFingerData.PoseFingerSplays.Num() > 0)
		{
			for (; (i - vr::VRFinger_Count) < HandSkeletalAction.PoseFingerData.PoseFingerSplays.Num() && i < NewGesture.FingerValues.Num(); ++i)
				NewGesture.FingerValues[i].Value = HandSkeletalAction.PoseFingerData.PoseFingerSplays[i - vr::VRFinger_Count];
		}

		NewGesture.bUseFingerCurlOnly = bUseFingerCurlOnly;
		NewGesture.Name = RecordingName;
		GesturesDB->Gestures.Add(NewGesture);
	}
}


bool UOpenInputSkeletalMeshComponent::K2_DetectCurrentPose(FBPOpenVRActionInfo &SkeletalAction, FOpenInputGesture & GestureOut)
{
	if (!GesturesDB || GesturesDB->Gestures.Num() < 1)
		return false;

	for (const FOpenInputGesture& Gesture : GesturesDB->Gestures)
	{
		// If not enough indexs to match curl values, or if this gesture requires finger splay and the controller can't do it
		if (Gesture.FingerValues.Num() < SkeletalAction.PoseFingerData.PoseFingerCurls.Num() ||
			(!Gesture.bUseFingerCurlOnly && SkeletalAction.SkeletalTrackingLevel == EVROpenInputSkeletalTrackingLevel::VRSkeletalTracking_Full)
			)
			continue;

		bool bDetectedPose = true;
		for (int i = 0; i < SkeletalAction.PoseFingerData.PoseFingerCurls.Num(); ++i)
		{

			if (!FMath::IsNearlyEqual(SkeletalAction.PoseFingerData.PoseFingerCurls[i], Gesture.FingerValues[i].Value, Gesture.FingerValues[i].Threshold))
			{
				bDetectedPose = false;
				break;
			}
		}

		if (bDetectedPose && !Gesture.bUseFingerCurlOnly && SkeletalAction.PoseFingerData.PoseFingerSplays.Num())
		{
			for (int i = 0; i < SkeletalAction.PoseFingerData.PoseFingerSplays.Num() && (i + vr::VRFinger_Count) < Gesture.FingerValues.Num(); ++i)
			{
				if (!FMath::IsNearlyEqual(SkeletalAction.PoseFingerData.PoseFingerCurls[i], Gesture.FingerValues[i + vr::VRFinger_Count].Value, Gesture.FingerValues[i + vr::VRFinger_Count].Threshold))
				{
					bDetectedPose = false;
					break;
				}
			}
		}

		if (bDetectedPose)
		{
			GestureOut = Gesture;
			return true;
		}
	}

	return false;
}

bool UOpenInputSkeletalMeshComponent::DetectCurrentPose(FBPOpenVRActionInfo &SkeletalAction)
{
	if (!GesturesDB || GesturesDB->Gestures.Num() < 1)
		return false;

	FTransform BoneTransform = FTransform::Identity;

	for (auto GestureIterator = GesturesDB->Gestures.CreateConstIterator(); GestureIterator; ++GestureIterator)
	{
		const FOpenInputGesture &Gesture = *GestureIterator;

		// If not enough indexs to match curl values, or if this gesture requires finger splay and the controller can't do it
		if (Gesture.FingerValues.Num() < SkeletalAction.PoseFingerData.PoseFingerCurls.Num() ||
			(!Gesture.bUseFingerCurlOnly && SkeletalAction.SkeletalTrackingLevel == EVROpenInputSkeletalTrackingLevel::VRSkeletalTracking_Full)
			)
			continue;

		bool bDetectedPose = true;
		for (int i = 0; i < SkeletalAction.PoseFingerData.PoseFingerCurls.Num(); ++i)
		{
			if (!FMath::IsNearlyEqual(SkeletalAction.PoseFingerData.PoseFingerCurls[i], Gesture.FingerValues[i].Value, Gesture.FingerValues[i].Threshold))
			{
				bDetectedPose = false;
				break;
			}
		}

		if (bDetectedPose && !Gesture.bUseFingerCurlOnly && SkeletalAction.PoseFingerData.PoseFingerSplays.Num())
		{
			for (int i = 0; i < SkeletalAction.PoseFingerData.PoseFingerSplays.Num() && (i + vr::VRFinger_Count) < Gesture.FingerValues.Num(); ++i)
			{
				if (!FMath::IsNearlyEqual(SkeletalAction.PoseFingerData.PoseFingerCurls[i], Gesture.FingerValues[i + vr::VRFinger_Count].Value, Gesture.FingerValues[i + vr::VRFinger_Count].Threshold))
				{
					bDetectedPose = false;
					break;
				}
			}
		}

		if (bDetectedPose)
		{
			if (SkeletalAction.LastHandGesture != Gesture.Name)
			{
				if (SkeletalAction.LastHandGesture != NAME_None)
					OnGestureEnded.Broadcast(SkeletalAction.LastHandGesture, SkeletalAction.LastHandGestureIndex, SkeletalAction.SkeletalData.TargetHand);

				SkeletalAction.LastHandGesture = Gesture.Name;
				SkeletalAction.LastHandGestureIndex = GestureIterator.GetIndex();
				OnNewGestureDetected.Broadcast(SkeletalAction.LastHandGesture, SkeletalAction.LastHandGestureIndex, SkeletalAction.SkeletalData.TargetHand);

				return true;
			}
			else
				return false; // Same gesture
		}
	}

	if (SkeletalAction.LastHandGesture != NAME_None)
	{
		OnGestureEnded.Broadcast(SkeletalAction.LastHandGesture, SkeletalAction.LastHandGestureIndex, SkeletalAction.SkeletalData.TargetHand);
		SkeletalAction.LastHandGesture = NAME_None;
		SkeletalAction.LastHandGestureIndex = INDEX_NONE;
	}
	return false;
}
