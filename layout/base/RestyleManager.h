/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Code responsible for managing style changes: tracking what style
 * changes need to happen, scheduling them, and doing them.
 */

#ifndef mozilla_RestyleManager_h
#define mozilla_RestyleManager_h

#include "mozilla/RestyleLogging.h"
#include "nsISupportsImpl.h"
#include "nsChangeHint.h"
#include "RestyleTracker.h"
#include "nsPresContext.h"
#include "nsRefreshDriver.h"
#include "nsRefPtrHashtable.h"
#include "nsCSSPseudoElements.h"

class nsIFrame;
class nsStyleChangeList;
struct TreeMatchContext;

namespace mozilla {
  class EventStates;

namespace dom {
  class Element;
} // namespace dom

class RestyleManager MOZ_FINAL {
public:
  friend class ::nsRefreshDriver;
  friend class RestyleTracker;

  typedef mozilla::dom::Element Element;

  explicit RestyleManager(nsPresContext* aPresContext);

private:
  // Private destructor, to discourage deletion outside of Release():
  ~RestyleManager()
  {
    MOZ_ASSERT(!mReframingStyleContexts,
               "temporary member should be nulled out before destruction");
  }

public:
  NS_INLINE_DECL_REFCOUNTING(mozilla::RestyleManager)

  void Disconnect() {
    mPresContext = nullptr;
  }

  nsPresContext* PresContext() const {
    MOZ_ASSERT(mPresContext);
    return mPresContext;
  }

  nsCSSFrameConstructor* FrameConstructor() const
    { return PresContext()->FrameConstructor(); }

  // Should be called when a frame is going to be destroyed and
  // WillDestroyFrameTree hasn't been called yet.
  void NotifyDestroyingFrame(nsIFrame* aFrame);

  // Forwarded nsIDocumentObserver method, to handle restyling (and
  // passing the notification to the frame).
  nsresult ContentStateChanged(nsIContent*   aContent,
                               EventStates aStateMask);

  // Forwarded nsIMutationObserver method, to handle restyling.
  void AttributeWillChange(Element* aElement,
                           int32_t  aNameSpaceID,
                           nsIAtom* aAttribute,
                           int32_t  aModType);
  // Forwarded nsIMutationObserver method, to handle restyling (and
  // passing the notification to the frame).
  void AttributeChanged(Element* aElement,
                        int32_t  aNameSpaceID,
                        nsIAtom* aAttribute,
                        int32_t  aModType);

  // Get an integer that increments every time there is a style change
  // as a result of a change to the :hover content state.
  uint32_t GetHoverGeneration() const { return mHoverGeneration; }

  // Get a counter that increments on every style change, that we use to
  // track whether off-main-thread animations are up-to-date.
  uint64_t GetAnimationGeneration() const { return mAnimationGeneration; }

  // Whether rule matching should skip styles associated with animation
  bool SkipAnimationRules() const {
    MOZ_ASSERT(mSkipAnimationRules || !mPostAnimationRestyles,
               "inconsistent state");
    return mSkipAnimationRules;
  }

  // Whether rule matching should post animation restyles when it skips
  // styles associated with animation.  Only true when
  // SkipAnimationRules() is also true.
  bool PostAnimationRestyles() const {
    MOZ_ASSERT(mSkipAnimationRules || !mPostAnimationRestyles,
               "inconsistent state");
    return mPostAnimationRestyles;
  }

  // Whether we're currently in the animation phase of restyle
  // processing (to be eliminated in bug 960465)
  bool IsProcessingAnimationStyleChange() const {
    return mIsProcessingAnimationStyleChange;
  }

  /**
   * Reparent the style contexts of this frame subtree.  The parent frame of
   * aFrame must be changed to the new parent before this function is called;
   * the new parent style context will be automatically computed based on the
   * new position in the frame tree.
   *
   * @param aFrame the root of the subtree to reparent.  Must not be null.
   */
  nsresult ReparentStyleContext(nsIFrame* aFrame);

private:
  void ComputeAndProcessStyleChange(nsIFrame* aFrame,
                                    nsChangeHint aMinChange,
                                    RestyleTracker& aRestyleTracker,
                                    nsRestyleHint aRestyleHint);

  /**
   * Re-resolve the style contexts for a frame tree, building
   * aChangeList based on the resulting style changes, plus aMinChange
   * applied to aFrame.
   */
  void
    ComputeStyleChangeFor(nsIFrame* aFrame,
                          nsStyleChangeList* aChangeList,
                          nsChangeHint aMinChange,
                          RestyleTracker& aRestyleTracker,
                          nsRestyleHint aRestyleHint);

public:

#ifdef DEBUG
  /**
   * DEBUG ONLY method to verify integrity of style tree versus frame tree
   */
  void DebugVerifyStyleTree(nsIFrame* aFrame);
#endif

  // Note: It's the caller's responsibility to make sure to wrap a
  // ProcessRestyledFrames call in a view update batch and a script blocker.
  // This function does not call ProcessAttachedQueue() on the binding manager.
  // If the caller wants that to happen synchronously, it needs to handle that
  // itself.
  nsresult ProcessRestyledFrames(nsStyleChangeList& aRestyleArray);

  /**
   * In order to start CSS transitions on elements that are being
   * reframed, we need to stash their style contexts somewhere during
   * the reframing process.
   *
   * In all cases, the content node in the hash table is the real
   * content node, not the anonymous content node we create for ::before
   * or ::after.  The content node passed to the Get and Put methods is,
   * however, the content node to be associate with the frame's style
   * context.
   */
  typedef nsRefPtrHashtable<nsRefPtrHashKey<nsIContent>, nsStyleContext>
            ReframingStyleContextTable;
  class ReframingStyleContexts {
  public:
    void Put(nsIContent* aContent, nsStyleContext* aStyleContext) {
      MOZ_ASSERT(aContent);
      nsCSSPseudoElements::Type pseudoType = aStyleContext->GetPseudoType();
      if (pseudoType == nsCSSPseudoElements::ePseudo_NotPseudoElement) {
        mElementContexts.Put(aContent, aStyleContext);
      } else if (pseudoType == nsCSSPseudoElements::ePseudo_before) {
        MOZ_ASSERT(aContent->Tag() == nsGkAtoms::mozgeneratedcontentbefore);
        mBeforePseudoContexts.Put(aContent->GetParent(), aStyleContext);
      } else if (pseudoType == nsCSSPseudoElements::ePseudo_after) {
        MOZ_ASSERT(aContent->Tag() == nsGkAtoms::mozgeneratedcontentafter);
        mAfterPseudoContexts.Put(aContent->GetParent(), aStyleContext);
      }
    }

    nsStyleContext* Get(nsIContent* aContent,
                        nsCSSPseudoElements::Type aPseudoType) {
      MOZ_ASSERT(aContent);
      if (aPseudoType == nsCSSPseudoElements::ePseudo_NotPseudoElement) {
        return mElementContexts.GetWeak(aContent);
      }
      if (aPseudoType == nsCSSPseudoElements::ePseudo_before) {
        MOZ_ASSERT(aContent->Tag() == nsGkAtoms::mozgeneratedcontentbefore);
        return mBeforePseudoContexts.GetWeak(aContent->GetParent());
      }
      if (aPseudoType == nsCSSPseudoElements::ePseudo_after) {
        MOZ_ASSERT(aContent->Tag() == nsGkAtoms::mozgeneratedcontentafter);
        return mAfterPseudoContexts.GetWeak(aContent->GetParent());
      }
      MOZ_ASSERT(false, "unexpected aPseudoType");
      return nullptr;
    }
  private:
    ReframingStyleContextTable mElementContexts;
    ReframingStyleContextTable mBeforePseudoContexts;
    ReframingStyleContextTable mAfterPseudoContexts;
  };

  /**
   * Return the current ReframingStyleContexts struct, or null if we're
   * not currently in a restyling operation.
   */
  ReframingStyleContexts* GetReframingStyleContexts() {
    return mReframingStyleContexts;
  }

  /**
   * Try starting a transition for an element or a ::before or ::after
   * pseudo-element, given an old and new style context.  This may
   * change the new style context if a transition is started.
   *
   * For the pseudo-elements, aContent must be the anonymous content
   * that we're creating for that pseudo-element, not the real element.
   */
  static void
  TryStartingTransition(nsPresContext* aPresContext, nsIContent* aContent,
                        nsStyleContext* aOldStyleContext,
                        nsRefPtr<nsStyleContext>* aNewStyleContext /* inout */);

private:
  void RestyleForEmptyChange(Element* aContainer);

public:
  // Restyling for a ContentInserted (notification after insertion) or
  // for a CharacterDataChanged.  |aContainer| must be non-null; when
  // the container is null, no work is needed.
  void RestyleForInsertOrChange(Element* aContainer, nsIContent* aChild);

  // This would be the same as RestyleForInsertOrChange if we got the
  // notification before the removal.  However, we get it after, so we need the
  // following sibling in addition to the old child.  |aContainer| must be
  // non-null; when the container is null, no work is needed.  aFollowingSibling
  // is the sibling that used to come after aOldChild before the removal.
  void RestyleForRemove(Element* aContainer,
                        nsIContent* aOldChild,
                        nsIContent* aFollowingSibling);

  // Same for a ContentAppended.  |aContainer| must be non-null; when
  // the container is null, no work is needed.
  void RestyleForAppend(Element* aContainer, nsIContent* aFirstNewContent);

  // Process any pending restyles. This should be called after
  // CreateNeededFrames.
  // Note: It's the caller's responsibility to make sure to wrap a
  // ProcessPendingRestyles call in a view update batch and a script blocker.
  // This function does not call ProcessAttachedQueue() on the binding manager.
  // If the caller wants that to happen synchronously, it needs to handle that
  // itself.
  void ProcessPendingRestyles();

  // Returns whether there are any pending restyles.
  bool HasPendingRestyles() { return mPendingRestyles.Count() != 0; }

  // ProcessPendingRestyles calls into one of our RestyleTracker
  // objects.  It then calls back to these functions at the beginning
  // and end of its work.
  void BeginProcessingRestyles();
  void EndProcessingRestyles();

  // Update styles for animations that are running on the compositor and
  // whose updating is suppressed on the main thread (to save
  // unnecessary work), while leaving all other aspects of style
  // out-of-date.
  //
  // Performs an animation-only style flush to make styles from
  // throttled transitions up-to-date prior to processing an unrelated
  // style change, so that any transitions triggered by that style
  // change produce correct results.
  //
  // In more detail:  when we're able to run animations on the
  // compositor, we sometimes "throttle" these animations by skipping
  // updating style data on the main thread.  However, whenever we
  // process a normal (non-animation) style change, any changes in
  // computed style on elements that have transition-* properties set
  // may need to trigger new transitions; this process requires knowing
  // both the old and new values of the property.  To do this correctly,
  // we need to have an up-to-date *old* value of the property on the
  // primary frame.  So the purpose of the mini-flush is to update the
  // style for all throttled transitions and animations to the current
  // animation state without making any other updates, so that when we
  // process the queued style updates we'll have correct old data to
  // compare against.  When we do this, we don't bother touching frames
  // other than primary frames.
  void UpdateOnlyAnimationStyles();

  bool ThrottledAnimationStyleIsUpToDate() const {
    return mLastUpdateForThrottledAnimations ==
             mPresContext->RefreshDriver()->MostRecentRefresh();
  }

  // Rebuilds all style data by throwing out the old rule tree and
  // building a new one, and additionally applying aExtraHint (which
  // must not contain nsChangeHint_ReconstructFrame) to the root frame.
  void RebuildAllStyleData(nsChangeHint aExtraHint);

  // Helper that does part of the work of RebuildAllStyleData, shared by
  // RestyleElement for 'rem' handling.
  void DoRebuildAllStyleData(RestyleTracker& aRestyleTracker,
                             nsChangeHint aExtraHint,
                             nsRestyleHint aRestyleHint);

  // See PostRestyleEventCommon below.
  void PostRestyleEvent(Element* aElement,
                        nsRestyleHint aRestyleHint,
                        nsChangeHint aMinChangeHint)
  {
    if (mPresContext) {
      PostRestyleEventCommon(aElement, aRestyleHint, aMinChangeHint,
                             IsProcessingAnimationStyleChange());
    }
  }

  // See PostRestyleEventCommon below.
  void PostAnimationRestyleEvent(Element* aElement,
                                 nsRestyleHint aRestyleHint,
                                 nsChangeHint aMinChangeHint)
  {
    PostRestyleEventCommon(aElement, aRestyleHint, aMinChangeHint, true);
  }

  void PostRestyleEventForLazyConstruction()
  {
    PostRestyleEventInternal(true);
  }

  void FlushOverflowChangedTracker()
  {
    mOverflowChangedTracker.Flush();
  }

#ifdef DEBUG
  static nsCString RestyleHintToString(nsRestyleHint aHint);
  static nsCString ChangeHintToString(nsChangeHint aHint);
#endif

private:
  /**
   * Notify the frame constructor that an element needs to have its
   * style recomputed.
   * @param aElement: The element to be restyled.
   * @param aRestyleHint: Which nodes need to have selector matching run
   *                      on them.
   * @param aMinChangeHint: A minimum change hint for aContent and its
   *                        descendants.
   * @param aForAnimation: Whether the style should be computed with or
   *                       without animation data.  Animation code
   *                       sometimes needs to pass true; other code
   *                       should generally pass the the pres context's
   *                       IsProcessingAnimationStyleChange() value
   *                       (which is the default value).
   */
  void PostRestyleEventCommon(Element* aElement,
                              nsRestyleHint aRestyleHint,
                              nsChangeHint aMinChangeHint,
                              bool aForAnimation);
  void PostRestyleEventInternal(bool aForLazyConstruction);

public:
  /**
   * Asynchronously clear style data from the root frame downwards and ensure
   * it will all be rebuilt. This is safe to call anytime; it will schedule
   * a restyle and take effect next time style changes are flushed.
   * This method is used to recompute the style data when some change happens
   * outside of any style rules, like a color preference change or a change
   * in a system font size, or to fix things up when an optimization in the
   * style data has become invalid. We assume that the root frame will not
   * need to be reframed.
   */
  void PostRebuildAllStyleDataEvent(nsChangeHint aExtraHint);

#ifdef RESTYLE_LOGGING
  /**
   * Returns whether a restyle event currently being processed by this
   * RestyleManager should be logged.
   */
  bool ShouldLogRestyle() {
    return ShouldLogRestyle(mPresContext);
  }

  /**
   * Returns whether a restyle event currently being processed for the
   * document with the specified nsPresContext should be logged.
   */
  static bool ShouldLogRestyle(nsPresContext* aPresContext) {
    return aPresContext->RestyleLoggingEnabled() &&
           (!aPresContext->RestyleManager()->
               IsProcessingAnimationStyleChange() ||
            AnimationRestyleLoggingEnabled());
  }

  static bool RestyleLoggingInitiallyEnabled() {
    static bool enabled = getenv("MOZ_DEBUG_RESTYLE") != 0;
    return enabled;
  }

  static bool AnimationRestyleLoggingEnabled() {
    static bool animations = getenv("MOZ_DEBUG_RESTYLE_ANIMATIONS") != 0;
    return animations;
  }

  // Set MOZ_DEBUG_RESTYLE_STRUCTS to a comma-separated string of
  // style struct names -- such as "Font,SVGReset" -- to log the style context
  // tree and those cached struct pointers before each restyle.  This
  // function returns a bitfield of the structs named in the
  // environment variable.
  static uint32_t StructsToLog();

  static nsCString StructNamesToString(uint32_t aSIDs);
  int32_t& LoggingDepth() { return mLoggingDepth; }
#endif

private:
  /* aMinHint is the minimal change that should be made to the element */
  // XXXbz do we really need the aPrimaryFrame argument here?
  void RestyleElement(Element*        aElement,
                      nsIFrame*       aPrimaryFrame,
                      nsChangeHint    aMinHint,
                      RestyleTracker& aRestyleTracker,
                      nsRestyleHint   aRestyleHint);

  void StyleChangeReflow(nsIFrame* aFrame, nsChangeHint aHint);

  // Recursively add all the given frame and all children to the tracker.
  void AddSubtreeToOverflowTracker(nsIFrame* aFrame);

  // Returns true if this function managed to successfully move a frame, and
  // false if it could not process the position change, and a reflow should
  // be performed instead.
  bool RecomputePosition(nsIFrame* aFrame);

private:
  nsPresContext* mPresContext; // weak, disconnected in Disconnect

  bool mRebuildAllStyleData : 1;
  // True if we're already waiting for a refresh notification
  bool mObservingRefreshDriver : 1;
  // True if we're in the middle of a nsRefreshDriver refresh
  bool mInStyleRefresh : 1;
  // Whether rule matching should skip styles associated with animation
  bool mSkipAnimationRules : 1;
  // Whether rule matching should post animation restyles when it skips
  // styles associated with animation.  Only true when
  // mSkipAnimationRules is also true.
  bool mPostAnimationRestyles : 1;
  // Whether we're currently in the animation phase of restyle
  // processing (to be eliminated in bug 960465)
  bool mIsProcessingAnimationStyleChange : 1;

  uint32_t mHoverGeneration;
  nsChangeHint mRebuildAllExtraHint;

  mozilla::TimeStamp mLastUpdateForThrottledAnimations;

  OverflowChangedTracker mOverflowChangedTracker;

  // The total number of animation flushes by this frame constructor.
  // Used to keep the layer and animation manager in sync.
  uint64_t mAnimationGeneration;

  ReframingStyleContexts* mReframingStyleContexts;

  RestyleTracker mPendingRestyles;
  RestyleTracker mPendingAnimationRestyles;

#ifdef DEBUG
  bool mIsProcessingRestyles;
#endif

#ifdef RESTYLE_LOGGING
  int32_t mLoggingDepth;
#endif
};

/**
 * An ElementRestyler is created for *each* element in a subtree that we
 * recompute styles for.
 */
class ElementRestyler MOZ_FINAL {
public:
  typedef mozilla::dom::Element Element;

  // Construct for the root of the subtree that we're restyling.
  ElementRestyler(nsPresContext* aPresContext,
                  nsIFrame* aFrame,
                  nsStyleChangeList* aChangeList,
                  nsChangeHint aHintsHandledByAncestors,
                  RestyleTracker& aRestyleTracker,
                  TreeMatchContext& aTreeMatchContext,
                  nsTArray<nsIContent*>& aVisibleKidsOfHiddenElement);

  // Construct for an element whose parent is being restyled.
  enum ConstructorFlags {
    FOR_OUT_OF_FLOW_CHILD = 1<<0
  };
  ElementRestyler(const ElementRestyler& aParentRestyler,
                  nsIFrame* aFrame,
                  uint32_t aConstructorFlags);

  // Construct for a frame whose parent is being restyled, but whose
  // style context is the parent style context for its parent frame.
  // (This is only used for table frames, whose style contexts are used
  // as the parent style context for their outer table frame (table
  // wrapper frame).  We should probably try to get rid of this
  // exception and have the inheritance go the other way.)
  enum ParentContextFromChildFrame { PARENT_CONTEXT_FROM_CHILD_FRAME };
  ElementRestyler(ParentContextFromChildFrame,
                  const ElementRestyler& aParentFrameRestyler,
                  nsIFrame* aFrame);

  /**
   * Restyle our frame's element and its subtree.
   *
   * Use eRestyle_Self for the aRestyleHint argument to mean
   * "reresolve our style context but not kids", use eRestyle_Subtree
   * to mean "reresolve our style context and kids", and use
   * nsRestyleHint(0) to mean recompute a new style context for our
   * current parent and existing rulenode, and the same for kids.
   */
  void Restyle(nsRestyleHint aRestyleHint);

  /**
   * mHintsHandled changes over time; it starts off as the hints that
   * have been handled by ancestors, and by the end of Restyle it
   * represents the hints that have been handled for this frame.  This
   * method is intended to be called after Restyle, to find out what
   * hints have been handled for this frame.
   */
  nsChangeHint HintsHandledForFrame() { return mHintsHandled; }

#ifdef RESTYLE_LOGGING
  bool ShouldLogRestyle() {
    return RestyleManager::ShouldLogRestyle(mPresContext);
  }
#endif

private:
  // Enum for the result of RestyleSelf, which indicates whether the
  // restyle procedure should continue to the children, and how.
  //
  // These values must be ordered so that later values imply that all
  // the work of the earlier values is also done.
  enum RestyleResult {

    // do not restyle children
    eRestyleResult_Stop = 1,

    // continue restyling children
    eRestyleResult_Continue,

    // continue restyling children with eRestyle_ForceDescendants set
    eRestyleResult_ContinueAndForceDescendants
  };

  /**
   * First half of Restyle().
   */
  RestyleResult RestyleSelf(nsIFrame* aSelf,
                            nsRestyleHint aRestyleHint,
                            uint32_t* aSwappedStructs);

  /**
   * Restyle the children of this frame (and, in turn, their children).
   *
   * Second half of Restyle().
   */
  void RestyleChildren(nsRestyleHint aChildRestyleHint);

  /**
   * Helpers for RestyleSelf().
   */
  void CaptureChange(nsStyleContext* aOldContext,
                     nsStyleContext* aNewContext,
                     nsChangeHint aChangeToAssume,
                     uint32_t* aEqualStructs);
  RestyleResult ComputeRestyleResultFromFrame(nsIFrame* aSelf);
  RestyleResult ComputeRestyleResultFromNewContext(nsIFrame* aSelf,
                                                   nsStyleContext* aNewContext);

  /**
   * Helpers for RestyleChildren().
   */
  void RestyleUndisplayedChildren(nsRestyleHint aChildRestyleHint);
  void MaybeReframeForBeforePseudo();
  void MaybeReframeForAfterPseudo(nsIFrame* aFrame);
  void RestyleContentChildren(nsIFrame* aParent,
                              nsRestyleHint aChildRestyleHint);
  void InitializeAccessibilityNotifications();
  void SendAccessibilityNotifications();

  enum DesiredA11yNotifications {
    eSkipNotifications,
    eSendAllNotifications,
    eNotifyIfShown
  };

  enum A11yNotificationType {
    eDontNotify,
    eNotifyShown,
    eNotifyHidden
  };

#ifdef RESTYLE_LOGGING
  int32_t& LoggingDepth() { return mLoggingDepth; }
#endif

#ifdef DEBUG
  static nsCString RestyleResultToString(RestyleResult aRestyleResult);
#endif

private:
  nsPresContext* const mPresContext;
  nsIFrame* const mFrame;
  nsIContent* const mParentContent;
  // |mContent| is the node that we used for rule matching of
  // normal elements (not pseudo-elements) and for which we generate
  // framechange hints if we need them.
  nsIContent* const mContent;
  nsStyleChangeList* const mChangeList;
  // We have already generated change list entries for hints listed in
  // mHintsHandled (initially it's those handled by ancestors, but by
  // the end of Restyle it is those handled for this frame as well).  We
  // need to generate a new change list entry for the frame when its
  // style comparision returns a hint other than one of these hints.
  nsChangeHint mHintsHandled;
  // See nsStyleContext::CalcStyleDifference
  nsChangeHint mParentFrameHintsNotHandledForDescendants;
  nsChangeHint mHintsNotHandledForDescendants;
  RestyleTracker& mRestyleTracker;
  TreeMatchContext& mTreeMatchContext;
  nsIFrame* mResolvedChild; // child that provides our parent style context

#ifdef ACCESSIBILITY
  const DesiredA11yNotifications mDesiredA11yNotifications;
  DesiredA11yNotifications mKidsDesiredA11yNotifications;
  A11yNotificationType mOurA11yNotification;
  nsTArray<nsIContent*>& mVisibleKidsOfHiddenElement;
  bool mWasFrameVisible;
#endif

#ifdef RESTYLE_LOGGING
  int32_t mLoggingDepth;
#endif
};

} // namespace mozilla

#endif /* mozilla_RestyleManager_h */
