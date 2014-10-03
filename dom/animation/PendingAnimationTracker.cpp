/* vim: set shiftwidth=2 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PendingAnimationTracker.h"

#include "nsCycleCollectionParticipant.h"

using mozilla::dom::AnimationPlayer;

namespace mozilla {

NS_IMPL_CYCLE_COLLECTION(PendingAnimationTracker, mPendingPlayers)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(PendingAnimationTracker, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(PendingAnimationTracker, Release)

void
PendingAnimationTracker::AddPendingPlayer(AnimationPlayer& aPlayer)
{
  mPendingPlayers.PutEntry(&aPlayer);
}

void
PendingAnimationTracker::RemovePendingPlayer(AnimationPlayer& aPlayer)
{
  // XXX
}

bool
PendingAnimationTracker::IsPlayerPending(AnimationPlayer& aPlayer)
{
  return mPendingPlayers.Contains(&aPlayer);
}

void
PendingAnimationTracker::AddPausingPlayer(AnimationPlayer& aPlayer)
{
  mPausingPlayers.PutEntry(&aPlayer);
}

void
PendingAnimationTracker::RemovePausingPlayer(AnimationPlayer& aPlayer)
{
  // XXX
}

bool
PendingAnimationTracker::IsPlayerPausing(AnimationPlayer& aPlayer)
{
  return mPausingPlayers.Contains(&aPlayer);
}


PLDHashOperator
ResolveStartTimes(nsRefPtrHashKey<AnimationPlayer>* aKey, void* aReadyTime)
{
  AnimationPlayer* player = aKey->GetKey();
  player->ResolveStartTime(*static_cast<TimeStamp*>(aReadyTime));

  return PL_DHASH_NEXT;
}

PLDHashOperator
ResolvePauseTimes(nsRefPtrHashKey<AnimationPlayer>* aKey, void* aReadyTime)
{
  AnimationPlayer* player = aKey->GetKey();
  player->ResolvePauseTime(*static_cast<TimeStamp*>(aReadyTime));

  return PL_DHASH_NEXT;
}

void
PendingAnimationTracker::ResolvePendingPlayers(const TimeStamp& aReadyTime)
{
  mPendingPlayers.EnumerateEntries(ResolveStartTimes,
                                   const_cast<TimeStamp*>(&aReadyTime));
  mPendingPlayers.Clear();
  mPausingPlayers.EnumerateEntries(ResolvePauseTimes,
                                   const_cast<TimeStamp*>(&aReadyTime));
  mPausingPlayers.Clear();
}

} // namespace mozilla
