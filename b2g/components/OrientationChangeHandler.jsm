/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

this.EXPORTED_SYMBOLS = [];

const Ci = Components.interfaces;
const Cu = Components.utils;

Cu.import("resource://gre/modules/Services.jsm");

let window = Services.wm.getMostRecentWindow("navigator:browser");
let system = window.document.getElementById("systemapp");

let OrientationChangeHandler = {
  // Clockwise orientations, looping
  orientations: ["portrait-primary", "landscape-secondary",
                 "portrait-secondary", "landscape-primary",
                 "portrait-primary"],

  lastOrientation: "portrait-primary",

  init: function() {
    window.screen.addEventListener("mozorientationchange", this, true);
  },

  handleEvent: function(evt) {
    let newOrientation = window.screen.mozOrientation;
    let orientationIndex = this.orientations.indexOf(this.lastOrientation);
    let nextClockwiseOrientation = this.orientations[orientationIndex + 1];
    let fullSwitch = (newOrientation.split("-")[0] ==
                      this.lastOrientation.split("-")[0]);

    this.lastOrientation = newOrientation;

    let angle, xFactor, yFactor;
    if (fullSwitch) {
      angle = 180;
      xFactor = 1;
    } else {
      angle = (nextClockwiseOrientation == newOrientation) ? 90 : -90;
      xFactor = window.innerWidth / window.innerHeight;
    }
    yFactor = 1 / xFactor;

    system.style.transition = "";
    system.style.transform = "rotate(" + angle + "deg)" +
                             "scale(" + xFactor + ", " + yFactor + ")";

    function trigger() {
      system.style.transition = "transform .25s cubic-bezier(.15, .7, .6, .9)";
      system.style.transform = "";
    }

    // 180deg rotation, no resize
    if (fullSwitch) {
      window.setTimeout(trigger);
      return;
    }

    let docViewer = window
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIWebNavigation)
      .QueryInterface(Ci.nsIDocShell)
      .contentViewer;

    window.addEventListener("resize", function waitForResize(e) {
      window.removeEventListener("resize", waitForResize);

      /*
      // Only pause painting after the next frame so that the animation
      // has a chance to begin.
      window.setTimeout(() => { trigger(); docViewer.pausePainting(); });

      // We won't get transitionend after pausing painting, so use a timeout
      // to resume.
      window.setTimeout(docViewer.resumePainting, 2000);
      */

      // Wait for the first paint after resize, then trigger the animation
      docViewer.pausePainting();
      window.addEventListener("MozAfterPaint", function waitForResizePaint(e) {
        window.removeEventListener("MozAfterPaint", waitForResizePaint);

        // Start the transition
        trigger();

        // Request a new frame to get the animation to start, then pause painting
        docViewer.resumePainting();
        window.requestAnimationFrame(() => {
          docViewer.pausePainting();

          // We won't get transitionend after pausing painting, so use a
          // timeout to resume instead.
          window.setTimeout(docViewer.resumePainting, 250);
        });
      });
    });
  }
};

OrientationChangeHandler.init();
