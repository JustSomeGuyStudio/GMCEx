﻿// Fill out your copyright notice in the Description page of Project Settings.


#include "Effects/GMCAbilityEffect.h"

#include "GMCAbilitySystem.h"
#include "Components/GMCAbilityComponent.h"
#include "Kismet/KismetSystemLibrary.h"


void UGMCAbilityEffect::InitializeEffect(FGMCAbilityEffectData InitializationData)
{
	EffectData = InitializationData;
	
	OwnerAbilityComponent = EffectData.OwnerAbilityComponent;
	SourceAbilityComponent = EffectData.SourceAbilityComponent;

	if (OwnerAbilityComponent == nullptr)
	{
		UE_LOG(LogGMCAbilitySystem, Error, TEXT("OwnerAbilityComponent is null in UGMCAbilityEffect::InitializeEffect"));
		return;
	}
	
	ClientEffectApplicationTime = OwnerAbilityComponent->ActionTimer;

	// If server sends times, use those
	// Only used in the case of a non predicted effect
	if (InitializationData.StartTime != 0)
	{
		EffectData.StartTime = InitializationData.StartTime;
	}
	else
	{
		EffectData.StartTime = OwnerAbilityComponent->ActionTimer + EffectData.Delay;
	}
	
	if (InitializationData.EndTime != 0)
	{
		EffectData.EndTime = InitializationData.EndTime;
	}
	else
	{
		EffectData.EndTime = EffectData.StartTime + EffectData.Duration;
	}

	// Start Immediately
	if (EffectData.Delay == 0)
	{
		StartEffect();
	}
}


void UGMCAbilityEffect::StartEffect()
{
	bHasStarted = true;

	// Ensure tag requirements are met before applying the effect
	if( ( EffectData.ApplicationMustHaveTags.Num() > 0 && !DoesOwnerHaveTagFromContainer(EffectData.ApplicationMustHaveTags) ) ||
	DoesOwnerHaveTagFromContainer(EffectData.ApplicationMustNotHaveTags) ||
	( EffectData.MustHaveTags.Num() > 0 && !DoesOwnerHaveTagFromContainer(EffectData.MustHaveTags) ) ||
	DoesOwnerHaveTagFromContainer(EffectData.MustNotHaveTags) )
	{
		EndEffect();
		return;
	}
	
	// Effect Query
	if (!EffectData.ActivationQuery.IsEmpty() && !EffectData.ActivationQuery.Matches(OwnerAbilityComponent->GetActiveTags()))
		{
		EndEffect();
		return;
	}

	AddTagsToOwner();
	AddAbilitiesToOwner();
	EndActiveAbilitiesFromOwner(EffectData.CancelAbilityOnActivation);

	EndActiveAbilitiesByDefinitionQuery(EffectData.EndAbilityOnActivationQuery);

	bHasAppliedEffect = true;

	// Instant effects modify base value and end instantly
	if (EffectData.bIsInstant)
	{
		for (const FGMCAttributeModifier& Modifier : EffectData.Modifiers)
		{
			OwnerAbilityComponent->ApplyAbilityEffectModifier(Modifier, true, false, SourceAbilityComponent);
		}
		EndEffect();
		return;
	}

	// Duration Effects that aren't periodic alter modifiers, not base
	if (!EffectData.bIsInstant && EffectData.Period == 0)
	{
		EffectData.bNegateEffectAtEnd = true;
		for (const FGMCAttributeModifier& Modifier : EffectData.Modifiers)
		{
			OwnerAbilityComponent->ApplyAbilityEffectModifier(Modifier, false, false, SourceAbilityComponent);
		}
	}

	// Tick period at start
	if (EffectData.bPeriodTickAtStart && EffectData.Period > 0)
	{
		PeriodTick();
	}
				
	// Instant effects instantly end
	if (EffectData.bIsInstant)
	{
		EndEffect();
	}

	StartEffectEvent();

	UpdateState(EGMASEffectState::Started, true);
}


void UGMCAbilityEffect::EndEffect()
{
	// Prevent EndEffect from being called multiple times
	if (bCompleted) return;
	
	bCompleted = true;
	if (CurrentState != EGMASEffectState::Ended)
	{
		UpdateState(EGMASEffectState::Ended, true);
	}

	// Only remove tags and abilities if the effect has started and applied
	if (!bHasStarted || !bHasAppliedEffect) return;

	if (EffectData.bNegateEffectAtEnd)
	{
		for (const FGMCAttributeModifier& Modifier : EffectData.Modifiers)
		{
			OwnerAbilityComponent->ApplyAbilityEffectModifier(Modifier, false, true);
		}
	}
	
	EndActiveAbilitiesByDefinitionQuery(EffectData.EndAbilityOnEndQuery);

	EndActiveAbilitiesFromOwner(EffectData.CancelAbilityOnEnd);
	RemoveTagsFromOwner(EffectData.bPreserveGrantedTagsIfMultiple);
	RemoveAbilitiesFromOwner();
	
	EndEffectEvent();
}


void UGMCAbilityEffect::BeginDestroy() {


	// This is addition is mostly to catch ghost effect who are still in around.
	// it's a bug, and ideally should not happen but that happen. a check in engine is added to catch this, and an error log for packaged game.
	/*if (OwnerAbilityComponent) {
		for (TTuple<int, UGMCAbilityEffect*> Effect : OwnerAbilityComponent->GetActiveEffects())
		{
			if (Effect.Value == this) {
				UE_LOG(LogGMCAbilitySystem, Error, TEXT("Effect %s is still in the active effect list of %s"), *Effect.Value->EffectData.EffectTag.ToString(), *OwnerAbilityComponent->GetOwner()->GetName());
				
				if (!bCompleted) {
					UE_LOG(	LogGMCAbilitySystem, Error, TEXT("Effect %s is being destroyed without being completed"), *Effect.Value->EffectData.EffectTag.ToString());
					EndEffect();
				}
				
				Effect.Value = nullptr;
			}
		}
	}*/
	
	UObject::BeginDestroy();
}


void UGMCAbilityEffect::Tick(float DeltaTime)
{
	// Aherys : I'm not sure if this is correct. Sometime this is GC. We need to catch why, and when.
	if (bCompleted || IsUnreachable()) {
		if (IsUnreachable()) {
			UE_LOG(LogGMCAbilitySystem, Error, TEXT("Effect is unreachable : %s"), *EffectData.EffectTag.ToString());
			ensureMsgf(false, TEXT("Effect is being ticked after being completed or GC : %s"), *EffectData.EffectTag.ToString());
		}
		return;
	}
	
	EffectData.CurrentDuration += DeltaTime;
	TickEvent(DeltaTime);
	
	// Ensure tag requirements are met before applying the effect
	if( (EffectData.MustHaveTags.Num() > 0 && !DoesOwnerHaveTagFromContainer(EffectData.MustHaveTags) ) ||
		DoesOwnerHaveTagFromContainer(EffectData.MustNotHaveTags) )
	{
		EndEffect();
	}

	// query to maintain effect
	if ( !EffectData.MustMaintainQuery.IsEmpty() && EffectData.MustMaintainQuery.Matches(OwnerAbilityComponent->GetActiveTags()))
	{
		EndEffect();
	}

	// If there's a period, check to see if it's time to tick
	if (!IsPeriodPaused() && EffectData.Period > 0 && CurrentState == EGMASEffectState::Started)
	{
		const float Mod = FMath::Fmod(OwnerAbilityComponent->ActionTimer, EffectData.Period);
		if (Mod < PrevPeriodMod)
		{
			PeriodTick();
		}
		PrevPeriodMod = Mod;
	}
	
	CheckState();
}

void UGMCAbilityEffect::TickEvent_Implementation(float DeltaTime)
{
}


bool UGMCAbilityEffect::AttributeDynamicCondition_Implementation() const {
	return true;
}


void UGMCAbilityEffect::PeriodTick()
{
	if (AttributeDynamicCondition()) {
		for (const FGMCAttributeModifier& AttributeModifier : EffectData.Modifiers)
		{
			OwnerAbilityComponent->ApplyAbilityEffectModifier(AttributeModifier, true, false, SourceAbilityComponent);
		}
	}
	PeriodTickEvent();
}

void UGMCAbilityEffect::PeriodTickEvent_Implementation()
{
}

void UGMCAbilityEffect::UpdateState(EGMASEffectState State, bool Force)
{
	if (State == EGMASEffectState::Ended)
	{
	//	UE_LOG(LogGMCAbilitySystem, Warning, TEXT("Effect Ended"));
	}

	CurrentState = State;
}

bool UGMCAbilityEffect::IsPeriodPaused()
{
	return DoesOwnerHaveTagFromContainer(EffectData.PausePeriodicEffect);
}

void UGMCAbilityEffect::GetOwnerActor(AActor*& OutOwnerActor) const
{
	if (OwnerAbilityComponent)
	{
		OutOwnerActor = OwnerAbilityComponent->GetOwner();
	}
	else
	{
		OutOwnerActor = nullptr;
	}
}

void UGMCAbilityEffect::AddTagsToOwner()
{
	for (const FGameplayTag Tag : EffectData.GrantedTags)
	{
		OwnerAbilityComponent->AddActiveTag(Tag);
	}
}

void UGMCAbilityEffect::RemoveTagsFromOwner(bool bPreserveOnMultipleInstances)
{
	if (bPreserveOnMultipleInstances)
	{
		if (EffectData.EffectTag.IsValid()) {
			TArray<UGMCAbilityEffect*> ActiveEffect = OwnerAbilityComponent->GetActiveEffectsByTag(EffectData.EffectTag);
			
			if (ActiveEffect.Num() > 1) {
				return;
			}
		}
		else
		{
			UE_LOG(LogGMCAbilitySystem, Warning, TEXT("Effect Tag is not valid with PreserveMultipleInstances in UGMCAbilityEffect::RemoveTagsFromOwner"));
		}
	}


	
	for (const FGameplayTag Tag : EffectData.GrantedTags)
	{
		OwnerAbilityComponent->RemoveActiveTag(Tag);
	}
}

void UGMCAbilityEffect::AddAbilitiesToOwner()
{
	for (const FGameplayTag Tag : EffectData.GrantedAbilities)
	{
		OwnerAbilityComponent->GrantAbilityByTag(Tag);
	}
}

void UGMCAbilityEffect::RemoveAbilitiesFromOwner()
{
	for (const FGameplayTag Tag : EffectData.GrantedAbilities)
	{
		OwnerAbilityComponent->RemoveGrantedAbilityByTag(Tag);
	}
}


void UGMCAbilityEffect::EndActiveAbilitiesFromOwner(const FGameplayTagContainer& TagContainer) {
	
	for (const FGameplayTag Tag : TagContainer)
	{
		OwnerAbilityComponent->EndAbilitiesByTag(Tag);
	}
}


bool UGMCAbilityEffect::DoesOwnerHaveTagFromContainer(FGameplayTagContainer& TagContainer) const
{
	for (const FGameplayTag Tag : TagContainer)
	{
		if (OwnerAbilityComponent->HasActiveTag(Tag))
		{
			return true;
		}
	}
	return false;
}

bool UGMCAbilityEffect::DuplicateEffectAlreadyApplied()
{
	if (EffectData.EffectTag == FGameplayTag::EmptyTag)
	{
		return false;
	}
	
	for (const TPair<int, UGMCAbilityEffect*> Effect : OwnerAbilityComponent->GetActiveEffects())
	{
		if (Effect.Value->EffectData.EffectTag == this->EffectData.EffectTag && Effect.Value->bHasStarted)
		{
			return true;
		}
	}

	return false;
}

void UGMCAbilityEffect::CheckState()
{
	switch (CurrentState)
	{
		case EGMASEffectState::Initialized:
			if (OwnerAbilityComponent->ActionTimer >= EffectData.StartTime)
			{
				StartEffect();
				UpdateState(EGMASEffectState::Started, true);
			}
			break;
		case EGMASEffectState::Started:
			if (EffectData.Duration != 0 && OwnerAbilityComponent->ActionTimer >= EffectData.EndTime)
			{
				EndEffect();
			}
			break;
		case EGMASEffectState::Ended:
			break;
	default: break;
	}
}

void UGMCAbilityEffect::EndActiveAbilitiesByDefinitionQuery(FGameplayTagQuery EndAbilityOnActivationViaDefinitionQuery)
{

	if (EndAbilityOnActivationViaDefinitionQuery.IsEmpty()) return;

	int NumCancelled = OwnerAbilityComponent->EndAbilitiesByQuery(EndAbilityOnActivationViaDefinitionQuery);

	UE_LOG(LogGMCAbilitySystem, Verbose, TEXT("Effect %s cancelled %d ability(ies) via EffectDefinition query."),
		*EffectData.EffectTag.ToString(), NumCancelled);
}

void UGMCAbilityEffect::ModifyMustMaintainQuery(const FGameplayTagQuery& NewQuery)
{
	EffectData.MustMaintainQuery = NewQuery;
	UE_LOG(LogGMCAbilitySystem, Verbose, TEXT("MustMainQuery modified: %s"), *NewQuery.GetDescription());
}

void UGMCAbilityEffect::ModifyEndAbilitiesOnEndQuery(const FGameplayTagQuery& NewQuery)
{
	EffectData.EndAbilityOnEndQuery = NewQuery;
	UE_LOG(LogGMCAbilitySystem, Verbose, TEXT("EndAbilityOnEndViaDefinitionQuery modified: %s"), *NewQuery.GetDescription());
}
