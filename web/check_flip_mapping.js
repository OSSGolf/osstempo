#!/usr/bin/env node
/*
 * check_flip_mapping.js — guards the web page's half of the 180° flip.
 *
 * The page draws its own picture of the strip, so it mirrors independently of the firmware. The
 * trap it has to avoid is the same one the iOS visualizer had: `ledX` (index → x) and `xToLed`
 * (x → index) are a hand-rolled inverse PAIR, and the canvas reach-drag reads a pointer position
 * back through `xToLed`. Mirror only the forward mapping and every drag on a reversed strip maps
 * to the mirror index, clamps, and collapses reach to its minimum on first movement.
 *
 * Rather than re-implement the mapping here (which would prove nothing), this extracts the exact
 * source lines out of index.html and exercises them.
 *
 * Run:  node web/check_flip_mapping.js
 */
"use strict";

const fs = require("fs");
const path = require("path");

const html = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
const match = html.match(/const flipIdx[\s\S]*?const xToLed = [^\n]*\n/);
if (!match) {
  console.error("  FAIL could not find the flipIdx/ledX/xToLed block in index.html");
  process.exit(1);
}

const NUM = 144, padX = 18, cssW = 600;
const globals = { flip: 0 };
const { ledX, xToLed, flipIdx } =
  new Function("NUM", "padX", "cssW", "globals",
               match[0] + "; return { ledX, xToLed, flipIdx };")(NUM, padX, cssW, globals);

let failures = 0;
const check = (name, ok, detail = "") => {
  console.log((ok ? "  ok   " : "  FAIL ") + name.padEnd(52) + detail);
  if (!ok) failures++;
};

globals.flip = 0;
const n0 = ledX(0), n143 = ledX(143);
globals.flip = 1;
const f0 = ledX(0), f143 = ledX(143);
check("ends swap when reversed", Math.abs(n0 - f143) < 1e-9 && Math.abs(n143 - f0) < 1e-9,
      `0:${n0.toFixed(1)}->${f0.toFixed(1)}  143:${n143.toFixed(1)}->${f143.toFixed(1)}`);

let bad = null;
for (const flip of [0, 1]) {
  globals.flip = flip;
  for (let i = 0; i < NUM; i++) {
    if (xToLed(ledX(i)) !== i) { bad = `flip=${flip} led=${i} -> ${xToLed(ledX(i))}`; break; }
  }
  if (bad) break;
}
check("ledX/xToLed are exact inverses, both orientations", bad === null,
      bad || "all 144 indices x 2 orientations");

globals.flip = 0; const amberNormal = ledX(130);          // amber for the default reach of 58
globals.flip = 1; const amberFlipped = ledX(130);
const centre = padX + 0.5 * (cssW - 2 * padX);
check("amber crosses centre when reversed",
      (amberNormal - centre) * (amberFlipped - centre) < 0,
      `${amberNormal.toFixed(1)} -> ${amberFlipped.toFixed(1)} (centre ${centre.toFixed(1)})`);

check("mirror is an involution", flipIdx(flipIdx(77)) === 77);
globals.flip = 0;
check("unreversed is the identity", flipIdx(77) === 77);

console.log("\n" + (failures ? "WEB FLIP MAPPING FAILED" : "WEB FLIP MAPPING PASSED") +
            ` (${failures} failure${failures === 1 ? "" : "s"})`);
process.exit(failures ? 1 : 0);
