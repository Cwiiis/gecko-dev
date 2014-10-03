/* vim: set shiftwidth=2 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_PendingAnimationTracker_h
#define mozilla_dom_PendingAnimationTracker_h

#include "nsCycleCollectionParticipant.h"
#include "nsIDocument.h"
#include "nsTHashtable.h"
#include "mozilla/dom/AnimationPlayer.h"

namespace mozilla {

class PendingAnimationTracker MOZ_FINAL {
public:
  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(PendingAnimationTracker)
  NS_DECL_CYCLE_COLLECTION_NATIVE_CLASS(PendingAnimationTracker)

  void AddPendingPlayer(dom::AnimationPlayer& aPlayer);
  void RemovePendingPlayer(dom::AnimationPlayer& aPlayer);
  bool IsPlayerPending(dom::AnimationPlayer& aPlayer);

  void AddPausingPlayer(dom::AnimationPlayer& aPlayer);
  void RemovePausingPlayer(dom::AnimationPlayer& aPlayer);
  bool IsPlayerPausing(dom::AnimationPlayer& aPlayer);

  void ResolvePendingPlayers(const TimeStamp& aReadyTime);

protected:
  // REVIEW: Should we use nsCheapSet here? It would save some space but
  // we'd need to define cycle collection for it and it would add a little
  // more indirection.
  typedef nsTHashtable<nsRefPtrHashKey<dom::AnimationPlayer>>
    AnimationPlayerSet;

  AnimationPlayerSet mPendingPlayers;
  AnimationPlayerSet mPausingPlayers;
};

} // namespace mozilla

#endif // mozilla_dom_PendingAnimationTracker_h
