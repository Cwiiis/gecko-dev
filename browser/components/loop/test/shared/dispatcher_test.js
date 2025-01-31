/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

var expect = chai.expect;

describe("loop.Dispatcher", function () {
  "use strict";

  var sharedActions = loop.shared.actions;
  var dispatcher, sandbox;

  beforeEach(function() {
    sandbox = sinon.sandbox.create();
    dispatcher = new loop.Dispatcher();
  });

  afterEach(function() {
    sandbox.restore();
  });

  describe("#register", function() {
    it("should register a store against an action name", function() {
      var object = { fake: true };

      dispatcher.register(object, ["gatherCallData"]);

      expect(dispatcher._eventData["gatherCallData"][0]).eql(object);
    });

    it("should register multiple store against an action name", function() {
      var object1 = { fake: true };
      var object2 = { fake2: true };

      dispatcher.register(object1, ["gatherCallData"]);
      dispatcher.register(object2, ["gatherCallData"]);

      expect(dispatcher._eventData["gatherCallData"][0]).eql(object1);
      expect(dispatcher._eventData["gatherCallData"][1]).eql(object2);
    });
  });

  describe("#dispatch", function() {
    var gatherStore1, gatherStore2, cancelStore1, connectStore1;
    var gatherAction, cancelAction, connectAction, resolveCancelStore1;

    beforeEach(function() {
      gatherAction = new sharedActions.GatherCallData({
        callId: "42",
        calleeId: null
      });

      cancelAction = new sharedActions.CancelCall();
      connectAction = new sharedActions.ConnectCall({
        sessionData: {}
      });

      gatherStore1 = {
        gatherCallData: sinon.stub()
      };
      gatherStore2 = {
        gatherCallData: sinon.stub()
      };
      cancelStore1 = {
        cancelCall: sinon.stub()
      };
      connectStore1 = {
        connectCall: function() {}
      };

      dispatcher.register(gatherStore1, ["gatherCallData"]);
      dispatcher.register(gatherStore2, ["gatherCallData"]);
      dispatcher.register(cancelStore1, ["cancelCall"]);
      dispatcher.register(connectStore1, ["connectCall"]);
    });

    it("should dispatch an action to the required object", function() {
      dispatcher.dispatch(cancelAction);

      sinon.assert.notCalled(gatherStore1.gatherCallData);

      sinon.assert.calledOnce(cancelStore1.cancelCall);
      sinon.assert.calledWithExactly(cancelStore1.cancelCall, cancelAction);

      sinon.assert.notCalled(gatherStore2.gatherCallData);
    });

    it("should dispatch actions to multiple objects", function() {
      dispatcher.dispatch(gatherAction);

      sinon.assert.calledOnce(gatherStore1.gatherCallData);
      sinon.assert.calledWithExactly(gatherStore1.gatherCallData, gatherAction);

      sinon.assert.notCalled(cancelStore1.cancelCall);

      sinon.assert.calledOnce(gatherStore2.gatherCallData);
      sinon.assert.calledWithExactly(gatherStore2.gatherCallData, gatherAction);
    });

    it("should dispatch multiple actions", function() {
      dispatcher.dispatch(cancelAction);
      dispatcher.dispatch(gatherAction);

      sinon.assert.calledOnce(cancelStore1.cancelCall);
      sinon.assert.calledOnce(gatherStore1.gatherCallData);
      sinon.assert.calledOnce(gatherStore2.gatherCallData);
    });

    describe("Queued actions", function() {
      beforeEach(function() {
        // Restore the stub, so that we can easily add a function to be
        // returned. Unfortunately, sinon doesn't make this easy.
        sandbox.stub(connectStore1, "connectCall", function() {
          dispatcher.dispatch(gatherAction);

          sinon.assert.notCalled(gatherStore1.gatherCallData);
          sinon.assert.notCalled(gatherStore2.gatherCallData);
        });
      });

      it("should not dispatch an action if the previous action hasn't finished", function() {
        // Dispatch the first action. The action handler dispatches the second
        // action - see the beforeEach above.
        dispatcher.dispatch(connectAction);

        sinon.assert.calledOnce(connectStore1.connectCall);
      });

      it("should dispatch an action when the previous action finishes", function() {
        // Dispatch the first action. The action handler dispatches the second
        // action - see the beforeEach above.
        dispatcher.dispatch(connectAction);

        sinon.assert.calledOnce(connectStore1.connectCall);
        // These should be called, because the dispatcher synchronously queues actions.
        sinon.assert.calledOnce(gatherStore1.gatherCallData);
        sinon.assert.calledOnce(gatherStore2.gatherCallData);
      });
    });
  });
});
