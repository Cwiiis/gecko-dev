/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsproxy_h
#define jsproxy_h

#include "mozilla/Maybe.h"

#include "jsfriendapi.h"

#include "js/CallNonGenericMethod.h"
#include "js/Class.h"

namespace js {

using JS::AutoIdVector;
using JS::CallArgs;
using JS::HandleId;
using JS::HandleObject;
using JS::HandleValue;
using JS::IsAcceptableThis;
using JS::MutableHandle;
using JS::MutableHandleObject;
using JS::MutableHandleValue;
using JS::NativeImpl;
using JS::PrivateValue;
using JS::Value;

class RegExpGuard;
class JS_FRIEND_API(Wrapper);

/*
 * A proxy is a JSObject that implements generic behavior by providing custom
 * implementations for each object trap. The implementation for each trap is
 * provided by a C++ object stored on the proxy, known as its handler.
 *
 * A major use case for proxies is to forward each trap to another object,
 * known as its target. The target can be an arbitrary C++ object. Not every
 * proxy has the notion of a target, however.
 *
 * Proxy traps are grouped into fundamental and derived traps. Every proxy has
 * to at least provide implementations for the fundamental traps, but the
 * derived traps can be implemented in terms of the fundamental ones
 * BaseProxyHandler provides implementations of the derived traps in terms of
 * the (pure virtual) fundamental traps.
 *
 * In addition to the normal traps, there are two models for proxy prototype
 * chains. First, proxies may opt to use the standard prototype mechanism used
 * throughout the engine. To do so, simply pass a prototype to NewProxyObject()
 * at creation time. All prototype accesses will then "just work" to treat the
 * proxy as a "normal" object. Alternatively, if instead the proxy wishes to
 * implement more complicated prototype semantics (if, for example, it wants to
 * delegate the prototype lookup to a wrapped object), it may pass Proxy::LazyProto
 * as the prototype at create time and opt in to the trapped prototype system,
 * which guarantees that their trap will be called on any and every prototype
 * chain access of the object.
 *
 * This system is implemented with two traps: {get,set}PrototypeOf. The default
 * implementation of setPrototypeOf throws a TypeError. Since it is not possible
 * to create an object without a sense of prototype chain, handler implementors
 * must provide a getPrototypeOf trap if opting in to the dynamic prototype system.
 *
 * To minimize code duplication, a set of abstract proxy handler classes is
 * provided, from which other handlers may inherit. These abstract classes
 * are organized in the following hierarchy:
 *
 * BaseProxyHandler
 * |
 * DirectProxyHandler
 * |
 * Wrapper
 */

/*
 * BaseProxyHandler is the most generic kind of proxy handler. It does not make
 * any assumptions about the target. Consequently, it does not provide any
 * default implementation for the fundamental traps. It does, however, implement
 * the derived traps in terms of the fundamental ones. This allows consumers of
 * this class to define any custom behavior they want.
 *
 * Important: If you add a trap here, you should probably also add a Proxy::foo
 * entry point with an AutoEnterPolicy. If you don't, you need an explicit
 * override for the trap in SecurityWrapper. See bug 945826 comment 0.
 */
class JS_FRIEND_API(BaseProxyHandler)
{
    /*
     * Sometimes it's desirable to designate groups of proxy handlers as "similar".
     * For this, we use the notion of a "family": A consumer-provided opaque pointer
     * that designates the larger group to which this proxy belongs.
     *
     * If it will never be important to differentiate this proxy from others as
     * part of a distinct group, nullptr may be used instead.
     */
    const void *mFamily;

    /*
     * Proxy handlers can use mHasPrototype to request the following special
     * treatment from the JS engine:
     *
     *   - When mHasPrototype is true, the engine never calls these methods:
     *     getPropertyDescriptor, has, set, enumerate, iterate.  Instead, for
     *     these operations, it calls the "own" traps like
     *     getOwnPropertyDescriptor, hasOwn, defineProperty, keys, etc., and
     *     consults the prototype chain if needed.
     *
     *   - When mHasPrototype is true, the engine calls handler->get() only if
     *     handler->hasOwn() says an own property exists on the proxy. If not,
     *     it consults the prototype chain.
     *
     * This is useful because it frees the ProxyHandler from having to implement
     * any behavior having to do with the prototype chain.
     */
    bool mHasPrototype;

    /*
     * All proxies indicate whether they have any sort of interesting security
     * policy that might prevent the caller from doing something it wants to
     * the object. In the case of wrappers, this distinction is used to
     * determine whether the caller may strip off the wrapper if it so desires.
     */
    bool mHasSecurityPolicy;

  public:
    explicit MOZ_CONSTEXPR BaseProxyHandler(const void *aFamily, bool aHasPrototype = false,
                                            bool aHasSecurityPolicy = false)
      : mFamily(aFamily),
        mHasPrototype(aHasPrototype),
        mHasSecurityPolicy(aHasSecurityPolicy)
    { }

    bool hasPrototype() const {
        return mHasPrototype;
    }

    bool hasSecurityPolicy() const {
        return mHasSecurityPolicy;
    }

    inline const void *family() const {
        return mFamily;
    }
    static size_t offsetOfFamily() {
        return offsetof(BaseProxyHandler, mFamily);
    }

    virtual bool finalizeInBackground(Value priv) const {
        /*
         * Called on creation of a proxy to determine whether its finalize
         * method can be finalized on the background thread.
         */
        return true;
    }

    /* Policy enforcement traps.
     *
     * enter() allows the policy to specify whether the caller may perform |act|
     * on the proxy's |id| property. In the case when |act| is CALL, |id| is
     * generally JSID_VOID.
     *
     * The |act| parameter to enter() specifies the action being performed.
     * If |bp| is false, the trap suggests that the caller throw (though it
     * may still decide to squelch the error).
     *
     * We make these OR-able so that assertEnteredPolicy can pass a union of them.
     * For example, get{,Own}PropertyDescriptor is invoked by calls to ::get()
     * ::set(), in addition to being invoked on its own, so there are several
     * valid Actions that could have been entered.
     */
    typedef uint32_t Action;
    enum {
        NONE      = 0x00,
        GET       = 0x01,
        SET       = 0x02,
        CALL      = 0x04,
        ENUMERATE = 0x08,
        GET_PROPERTY_DESCRIPTOR = 0x10
    };

    virtual bool enter(JSContext *cx, HandleObject wrapper, HandleId id, Action act,
                       bool *bp) const;

    /* ES5 Harmony fundamental proxy traps. */
    virtual bool preventExtensions(JSContext *cx, HandleObject proxy) const = 0;
    virtual bool getPropertyDescriptor(JSContext *cx, HandleObject proxy, HandleId id,
                                       MutableHandle<JSPropertyDescriptor> desc) const = 0;
    virtual bool getOwnPropertyDescriptor(JSContext *cx, HandleObject proxy,
                                          HandleId id, MutableHandle<JSPropertyDescriptor> desc) const = 0;
    virtual bool defineProperty(JSContext *cx, HandleObject proxy, HandleId id,
                                MutableHandle<JSPropertyDescriptor> desc) const = 0;
    virtual bool getOwnPropertyNames(JSContext *cx, HandleObject proxy,
                                     AutoIdVector &props) const = 0;
    virtual bool delete_(JSContext *cx, HandleObject proxy, HandleId id, bool *bp) const = 0;
    virtual bool enumerate(JSContext *cx, HandleObject proxy, AutoIdVector &props) const = 0;

    /* ES5 Harmony derived proxy traps. */
    virtual bool has(JSContext *cx, HandleObject proxy, HandleId id, bool *bp) const;
    virtual bool hasOwn(JSContext *cx, HandleObject proxy, HandleId id, bool *bp) const;
    virtual bool get(JSContext *cx, HandleObject proxy, HandleObject receiver,
                     HandleId id, MutableHandleValue vp) const;
    virtual bool set(JSContext *cx, HandleObject proxy, HandleObject receiver,
                     HandleId id, bool strict, MutableHandleValue vp) const;
    virtual bool keys(JSContext *cx, HandleObject proxy, AutoIdVector &props) const;
    virtual bool iterate(JSContext *cx, HandleObject proxy, unsigned flags,
                         MutableHandleValue vp) const;

    /* Spidermonkey extensions. */
    virtual bool isExtensible(JSContext *cx, HandleObject proxy, bool *extensible) const = 0;
    virtual bool call(JSContext *cx, HandleObject proxy, const CallArgs &args) const;
    virtual bool construct(JSContext *cx, HandleObject proxy, const CallArgs &args) const;
    virtual bool nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl, CallArgs args) const;
    virtual bool hasInstance(JSContext *cx, HandleObject proxy, MutableHandleValue v, bool *bp) const;
    virtual bool objectClassIs(HandleObject obj, ESClassValue classValue, JSContext *cx) const;
    virtual const char *className(JSContext *cx, HandleObject proxy) const;
    virtual JSString *fun_toString(JSContext *cx, HandleObject proxy, unsigned indent) const;
    virtual bool regexp_toShared(JSContext *cx, HandleObject proxy, RegExpGuard *g) const;
    virtual bool boxedValue_unbox(JSContext *cx, HandleObject proxy, MutableHandleValue vp) const;
    virtual bool defaultValue(JSContext *cx, HandleObject obj, JSType hint, MutableHandleValue vp) const;
    virtual void finalize(JSFreeOp *fop, JSObject *proxy) const;
    virtual void objectMoved(JSObject *proxy, const JSObject *old) const;
    virtual bool getPrototypeOf(JSContext *cx, HandleObject proxy, MutableHandleObject protop) const;
    virtual bool setPrototypeOf(JSContext *cx, HandleObject proxy, HandleObject proto, bool *bp) const;

    // Allow proxies, wrappers in particular, to specify callability at runtime.
    // Note: These do not take const JSObject *, but they do in spirit.
    //       We are not prepared to do this, as there's little const correctness
    //       in the external APIs that handle proxies.
    virtual bool isCallable(JSObject *obj) const;
    virtual bool isConstructor(JSObject *obj) const;

    // These two hooks must be overridden, or not overridden, in tandem -- no
    // overriding just one!
    virtual bool watch(JSContext *cx, JS::HandleObject proxy, JS::HandleId id,
                       JS::HandleObject callable) const;
    virtual bool unwatch(JSContext *cx, JS::HandleObject proxy, JS::HandleId id) const;

    virtual bool slice(JSContext *cx, HandleObject proxy, uint32_t begin, uint32_t end,
                       HandleObject result) const;

    /* See comment for weakmapKeyDelegateOp in js/Class.h. */
    virtual JSObject *weakmapKeyDelegate(JSObject *proxy) const;
    virtual bool isScripted() const { return false; }
};

/*
 * DirectProxyHandler includes a notion of a target object. All traps are
 * reimplemented such that they forward their behavior to the target. This
 * allows consumers of this class to forward to another object as transparently
 * and efficiently as possible.
 *
 * Important: If you add a trap implementation here, you probably also need to
 * add an override in CrossCompartmentWrapper. If you don't, you risk
 * compartment mismatches. See bug 945826 comment 0.
 */
class JS_PUBLIC_API(DirectProxyHandler) : public BaseProxyHandler
{
  public:
    explicit MOZ_CONSTEXPR DirectProxyHandler(const void *aFamily, bool aHasPrototype = false,
                                              bool aHasSecurityPolicy = false)
      : BaseProxyHandler(aFamily, aHasPrototype, aHasSecurityPolicy)
    { }

    /* ES5 Harmony fundamental proxy traps. */
    virtual bool preventExtensions(JSContext *cx, HandleObject proxy) const MOZ_OVERRIDE;
    virtual bool getPropertyDescriptor(JSContext *cx, HandleObject proxy, HandleId id,
                                       MutableHandle<JSPropertyDescriptor> desc) const MOZ_OVERRIDE;
    virtual bool getOwnPropertyDescriptor(JSContext *cx, HandleObject proxy, HandleId id,
                                          MutableHandle<JSPropertyDescriptor> desc) const MOZ_OVERRIDE;
    virtual bool defineProperty(JSContext *cx, HandleObject proxy, HandleId id,
                                MutableHandle<JSPropertyDescriptor> desc) const MOZ_OVERRIDE;
    virtual bool getOwnPropertyNames(JSContext *cx, HandleObject proxy,
                                     AutoIdVector &props) const MOZ_OVERRIDE;
    virtual bool delete_(JSContext *cx, HandleObject proxy, HandleId id,
                         bool *bp) const MOZ_OVERRIDE;
    virtual bool enumerate(JSContext *cx, HandleObject proxy,
                           AutoIdVector &props) const MOZ_OVERRIDE;

    /* ES5 Harmony derived proxy traps. */
    virtual bool has(JSContext *cx, HandleObject proxy, HandleId id,
                     bool *bp) const MOZ_OVERRIDE;
    virtual bool hasOwn(JSContext *cx, HandleObject proxy, HandleId id,
                        bool *bp) const MOZ_OVERRIDE;
    virtual bool get(JSContext *cx, HandleObject proxy, HandleObject receiver,
                     HandleId id, MutableHandleValue vp) const MOZ_OVERRIDE;
    virtual bool set(JSContext *cx, HandleObject proxy, HandleObject receiver,
                     HandleId id, bool strict, MutableHandleValue vp) const MOZ_OVERRIDE;
    virtual bool keys(JSContext *cx, HandleObject proxy,
                      AutoIdVector &props) const MOZ_OVERRIDE;
    virtual bool iterate(JSContext *cx, HandleObject proxy, unsigned flags,
                         MutableHandleValue vp) const MOZ_OVERRIDE;

    /* Spidermonkey extensions. */
    virtual bool isExtensible(JSContext *cx, HandleObject proxy, bool *extensible) const MOZ_OVERRIDE;
    virtual bool call(JSContext *cx, HandleObject proxy, const CallArgs &args) const MOZ_OVERRIDE;
    virtual bool construct(JSContext *cx, HandleObject proxy, const CallArgs &args) const MOZ_OVERRIDE;
    virtual bool nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl,
                            CallArgs args) const MOZ_OVERRIDE;
    virtual bool hasInstance(JSContext *cx, HandleObject proxy, MutableHandleValue v,
                             bool *bp) const MOZ_OVERRIDE;
    virtual bool getPrototypeOf(JSContext *cx, HandleObject proxy,
                                MutableHandleObject protop) const MOZ_OVERRIDE;
    virtual bool setPrototypeOf(JSContext *cx, HandleObject proxy, HandleObject proto,
                                bool *bp) const MOZ_OVERRIDE;
    virtual bool objectClassIs(HandleObject obj, ESClassValue classValue,
                               JSContext *cx) const MOZ_OVERRIDE;
    virtual const char *className(JSContext *cx, HandleObject proxy) const MOZ_OVERRIDE;
    virtual JSString *fun_toString(JSContext *cx, HandleObject proxy,
                                   unsigned indent) const MOZ_OVERRIDE;
    virtual bool regexp_toShared(JSContext *cx, HandleObject proxy,
                                 RegExpGuard *g) const MOZ_OVERRIDE;
    virtual bool boxedValue_unbox(JSContext *cx, HandleObject proxy, MutableHandleValue vp) const;
    virtual JSObject *weakmapKeyDelegate(JSObject *proxy) const MOZ_OVERRIDE;
    virtual bool isCallable(JSObject *obj) const MOZ_OVERRIDE;
};

extern JS_FRIEND_DATA(const js::Class* const) ProxyClassPtr;

inline bool IsProxy(JSObject *obj)
{
    return GetObjectClass(obj)->isProxy();
}

/*
 * These are part of the API.
 *
 * NOTE: PROXY_PRIVATE_SLOT is 0 because that way slot 0 is usable by API
 * clients for both proxy and non-proxy objects.  So an API client that only
 * needs to store one slot's worth of data doesn't need to branch on what sort
 * of object it has.
 */
const uint32_t PROXY_PRIVATE_SLOT   = 0;
const uint32_t PROXY_HANDLER_SLOT   = 1;
const uint32_t PROXY_EXTRA_SLOT     = 2;
const uint32_t PROXY_MINIMUM_SLOTS  = 4;

inline const BaseProxyHandler *
GetProxyHandler(JSObject *obj)
{
    MOZ_ASSERT(IsProxy(obj));
    return (const BaseProxyHandler *) GetReservedSlot(obj, PROXY_HANDLER_SLOT).toPrivate();
}

inline const Value &
GetProxyPrivate(JSObject *obj)
{
    MOZ_ASSERT(IsProxy(obj));
    return GetReservedSlot(obj, PROXY_PRIVATE_SLOT);
}

inline JSObject *
GetProxyTargetObject(JSObject *obj)
{
    MOZ_ASSERT(IsProxy(obj));
    return GetProxyPrivate(obj).toObjectOrNull();
}

inline const Value &
GetProxyExtra(JSObject *obj, size_t n)
{
    MOZ_ASSERT(IsProxy(obj));
    return GetReservedSlot(obj, PROXY_EXTRA_SLOT + n);
}

inline void
SetProxyHandler(JSObject *obj, BaseProxyHandler *handler)
{
    MOZ_ASSERT(IsProxy(obj));
    SetReservedSlot(obj, PROXY_HANDLER_SLOT, PrivateValue(handler));
}

inline void
SetProxyExtra(JSObject *obj, size_t n, const Value &extra)
{
    MOZ_ASSERT(IsProxy(obj));
    MOZ_ASSERT(n <= 1);
    SetReservedSlot(obj, PROXY_EXTRA_SLOT + n, extra);
}

inline bool
IsScriptedProxy(JSObject *obj)
{
    return IsProxy(obj) && GetProxyHandler(obj)->isScripted();
}

class MOZ_STACK_CLASS ProxyOptions {
  protected:
    /* protected constructor for subclass */
    explicit ProxyOptions(bool singletonArg)
      : singleton_(singletonArg),
        clasp_(ProxyClassPtr)
    {}

  public:
    ProxyOptions() : singleton_(false),
                     clasp_(ProxyClassPtr)
    {}

    bool singleton() const { return singleton_; }
    ProxyOptions &setSingleton(bool flag) {
        singleton_ = flag;
        return *this;
    }

    const Class *clasp() const {
        return clasp_;
    }
    ProxyOptions &setClass(const Class *claspArg) {
        clasp_ = claspArg;
        return *this;
    }

  private:
    bool singleton_;
    const Class *clasp_;
};

JS_FRIEND_API(JSObject *)
NewProxyObject(JSContext *cx, const BaseProxyHandler *handler, HandleValue priv,
               JSObject *proto, JSObject *parent, const ProxyOptions &options = ProxyOptions());

JSObject *
RenewProxyObject(JSContext *cx, JSObject *obj, BaseProxyHandler *handler, Value priv);

class JS_FRIEND_API(AutoEnterPolicy)
{
  public:
    typedef BaseProxyHandler::Action Action;
    AutoEnterPolicy(JSContext *cx, const BaseProxyHandler *handler,
                    HandleObject wrapper, HandleId id, Action act, bool mayThrow)
#ifdef JS_DEBUG
        : context(nullptr)
#endif
    {
        allow = handler->hasSecurityPolicy() ? handler->enter(cx, wrapper, id, act, &rv)
                                             : true;
        recordEnter(cx, wrapper, id, act);
        // We want to throw an exception if all of the following are true:
        // * The policy disallowed access.
        // * The policy set rv to false, indicating that we should throw.
        // * The caller did not instruct us to ignore exceptions.
        // * The policy did not throw itself.
        if (!allow && !rv && mayThrow)
            reportErrorIfExceptionIsNotPending(cx, id);
    }

    virtual ~AutoEnterPolicy() { recordLeave(); }
    inline bool allowed() { return allow; }
    inline bool returnValue() { MOZ_ASSERT(!allowed()); return rv; }

  protected:
    // no-op constructor for subclass
    AutoEnterPolicy()
#ifdef JS_DEBUG
        : context(nullptr)
        , enteredAction(BaseProxyHandler::NONE)
#endif
        {}
    void reportErrorIfExceptionIsNotPending(JSContext *cx, jsid id);
    bool allow;
    bool rv;

#ifdef JS_DEBUG
    JSContext *context;
    mozilla::Maybe<HandleObject> enteredProxy;
    mozilla::Maybe<HandleId> enteredId;
    Action                   enteredAction;

    // NB: We explicitly don't track the entered action here, because sometimes
    // SET traps do an implicit GET during their implementation, leading to
    // spurious assertions.
    AutoEnterPolicy *prev;
    void recordEnter(JSContext *cx, HandleObject proxy, HandleId id, Action act);
    void recordLeave();

    friend JS_FRIEND_API(void) assertEnteredPolicy(JSContext *cx, JSObject *proxy, jsid id, Action act);
#else
    inline void recordEnter(JSContext *cx, JSObject *proxy, jsid id, Action act) {}
    inline void recordLeave() {}
#endif

};

#ifdef JS_DEBUG
class JS_FRIEND_API(AutoWaivePolicy) : public AutoEnterPolicy {
public:
    AutoWaivePolicy(JSContext *cx, HandleObject proxy, HandleId id,
                    BaseProxyHandler::Action act)
    {
        allow = true;
        recordEnter(cx, proxy, id, act);
    }
};
#else
class JS_FRIEND_API(AutoWaivePolicy) {
  public:
    AutoWaivePolicy(JSContext *cx, HandleObject proxy, HandleId id,
                    BaseProxyHandler::Action act)
    {}
};
#endif

#ifdef JS_DEBUG
extern JS_FRIEND_API(void)
assertEnteredPolicy(JSContext *cx, JSObject *obj, jsid id,
                    BaseProxyHandler::Action act);
#else
inline void assertEnteredPolicy(JSContext *cx, JSObject *obj, jsid id,
                                BaseProxyHandler::Action act)
{}
#endif

} /* namespace js */

extern JS_FRIEND_API(JSObject *)
js_InitProxyClass(JSContext *cx, JS::HandleObject obj);

#endif /* jsproxy_h */
