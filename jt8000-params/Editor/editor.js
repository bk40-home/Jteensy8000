"use strict";
/* ===========================================================================
   JT-8000 LAYOUT EDITOR  -  manual placement edition

   The layout is now MANUAL. Positions were seeded once from pack.py and baked
   into layout.js; from here the editor is the source of truth and pack.py is
   retired. You place components freely (this doubles as the hardware panel
   drawing), so items carry absolute x,y,w,h on the panel.

   Merged from both prior editors: free-drag + snap/grid/align/delete from the
   original, shift facets + param catalogue + coverage + header/background from
   the blocks.json edition. Displays are a component type; any control can be
   converted to display-only.
   =========================================================================== */

if (typeof window.LAYOUT === "undefined") throw new Error("layout.js not loaded");
var DOC   = normalize(JSON.parse(JSON.stringify(window.LAYOUT)));

/* Flatten any grouped layout to the flat block -> items model. Groups were
   editor-only visual clustering; here every block's groups collapse into a
   single items[] (labels dropped), preserving each item verbatim (position,
   key, shift facet, conditional variants). Idempotent: a already-flat file
   (blocks have items[], no groups) passes through unchanged. */
function normalize(doc) {
  if (!doc || !Array.isArray(doc.blocks)) return doc;
  doc.blocks.forEach(function (b) {
    if (b.items === undefined) b.items = [];
    if (b.groups !== undefined) {
      b.groups.forEach(function (g) { (g.items || []).forEach(function (it) { b.items.push(it); }); });
      delete b.groups;
    }
  });
  return doc;
}
var ORIG  = JSON.stringify(window.LAYOUT);

/* --- state ----------------------------------------------------------------- */
var scale = 0.30;
var snap  = "grid";            // 'grid' | 'off'
var snapStepX = DOC.grid.colw / (DOC.grid.sub || 4);
var snapStepY = DOC.grid.row;
var gridOn = true;
var view  = "MAIN";
var shiftPreview = false;
var previewValues = {};   // { "osc1.wave": "SSAW", ... } drives conditional items

/* Resolve an item's EFFECTIVE fields under the current preview state. A plain
   item returns itself. A conditional item (has `when` + `variants`) returns its
   base fields overridden by the first variant whose `eq` matches the previewed
   value of the driver key. Geometry (x/y/w/h) never changes - a variant changes
   what the control does, not where it sits. Backward-compatible: items without
   `when` are untouched, so existing exports are unaffected. */
function effectiveItem(it) {
  if (!it.when || !it.variants || !it.variants.length) return it;
  var driven = previewValues[it.when], v = null;
  for (var i = 0; i < it.variants.length; i++) if (it.variants[i].eq === driven) { v = it.variants[i]; break; }
  if (!v) return it;
  return {
    k:   v.k   !== undefined ? v.k   : it.k,
    t:   v.t   !== undefined ? v.t   : it.t,
    key: v.key !== undefined ? v.key : it.key,
    lw:  v.lw  !== undefined ? v.lw  : it.lw,
    w:   it.w, h: it.h, x: it.x, y: it.y,
    opts: v.opts !== undefined ? v.opts : it.opts,
    when: it.when, variants: it.variants, sh: it.sh, _base: it
  };
}
var sel = [];                  // array of selected items (blocks, groups, or controls/displays)
var inside = null;             // block entered for item-level editing
var lastDown = { el: null, t: 0 };

var VIEWS = ["MAIN", "REVERB", "SEQ", "ARP", "SETUP"];
var LAYERMAP = { REVERB: "Reverb", SEQ: "Step Sequencer", ARP: "Arpeggiator", SETUP: "Setup" };

/* Header/background layer (edits the skin, exported as an RML/RCSS patch). */
var HEADER = window.LAYOUT.header || { h: 260, m: 40 };
var HDR = {
  band: { MAIN: "band_main", REVERB: "band_reverb", SEQ: "band_step", ARP: "band_arpeggiator", SETUP: "band_setup" },
  bg: "bg_base",
  texts: [
    { id: "lcd1", t: "INIT PATCH", x: 544, y: 102, w: 740, size: 56, color: "#ffa83a", align: "left" },
    { id: "lcd2", t: "", x: 544, y: 184, w: 932, size: 30, color: "#be6c1e", align: "left" }
  ]
};

/* --- catalogue ------------------------------------------------------------- */
var CAT = window.PARAMS || { keys: [], labels: {}, options: {} };
function optionSetForKey(key) {
  if (!key) return null;
  var o = CAT.options, stem = key.replace(/^[a-z]+\d*\./, "").replace(/\d+/g, ""), head = key.split(".")[0].replace(/\d+$/, "");
  var cands = [head + "_" + stem, stem, key.replace(/\./g, "_"), key.replace(/\d+/g, "")];
  for (var i = 0; i < cands.length; i++) if (o[cands[i]]) return cands[i];
  for (var kk in o) if (kk.indexOf(stem) >= 0) return kk;
  return null;
}
function optionsForKey(key) { var s = optionSetForKey(key); return s ? CAT.options[s] : null; }

/* --- undo ------------------------------------------------------------------ */
var undoStack = [], redoStack = [], HMAX = 80, coTimer = null;
function pushHistory() { undoStack.push(JSON.stringify(DOC)); if (undoStack.length > HMAX) undoStack.shift(); redoStack.length = 0; histBtns(); }
function pushCoalesced() { if (coTimer === null) pushHistory(); clearTimeout(coTimer); coTimer = setTimeout(function () { coTimer = null; }, 500); }
function undo() { if (!undoStack.length) return; redoStack.push(JSON.stringify(DOC)); DOC = JSON.parse(undoStack.pop()); sel = []; inside = null; histBtns(); render(); }
function redo() { if (!redoStack.length) return; undoStack.push(JSON.stringify(DOC)); DOC = JSON.parse(redoStack.pop()); sel = []; inside = null; histBtns(); render(); }
function histBtns() { var u = document.getElementById("undo"), r = document.getElementById("redo"); if (u) u.disabled = !undoStack.length; if (r) r.disabled = !redoStack.length; }

/* --- helpers --------------------------------------------------------------- */
var panel = document.getElementById("panel"), wrap = document.getElementById("wrap");
var LAYER_NAMES = DOC.blocks.filter(function (b) { return b.layer; }).map(function (b) { return b.name; });
function blockByName(n) { for (var i = 0; i < DOC.blocks.length; i++) if (DOC.blocks[i].name === n) return DOC.blocks[i]; return null; }
function isBlock(x) { return x && x.items !== undefined && x.name !== undefined; }
function isItem(x) { return x && x.k !== undefined; }
function isDisplay(x) { return x && x.k === "display"; }

/* controlBox — THE single source of truth for where a control and its label sit,
   in absolute panel dp. Both the editor preview and the RML export call this, so
   what you see is exactly what exports. The cell (it.x/y/w/h) is the bounding
   box; the control (cw/ch) sits inside it, and the label goes above / below /
   nowhere per it.labelPos (default "above"). Label band is LB dp tall. */
var LB = 30;   // label band height (dp)
function controlBox(it) {
  var cellX = it.x, cellY = it.y, cellW = (it.w !== undefined ? it.w : 144), cellH = (it.h !== undefined ? it.h : 174);
  var cw = (it.cw !== undefined ? it.cw : cellW);
  var ch = (it.ch !== undefined ? it.ch : cellH);
  var kind = it.k || "knob";
  var labelPos = it.labelPos || "above";   // "above" | "below" | "none"

  /* Vertical: reserve the label band on the chosen side; the control fills the
     rest, clamped to ch. */
  var availTop = cellY, availH = cellH;
  if (labelPos === "above") { availTop = cellY + LB; availH = cellH - LB; }
  else if (labelPos === "below") { availH = cellH - LB; }
  if (ch > availH) ch = availH;

  /* Horizontal: knobs/faders centre in the cell; combos/toggles fill from left. */
  var cx = cellX, ccw = cw;
  if (kind === "knob" || kind === "fader") {
    if (cw > cellW) ccw = cellW;
    cx = Math.round(cellX + (cellW - ccw) / 2);
  } else {
    ccw = cellW;   // combos/toggles span the cell width
  }

  /* Knobs stay square within the available box. */
  if (kind === "knob") { var d = Math.min(ccw, ch); ccw = d; ch = d; cx = Math.round(cellX + (cellW - d) / 2); }

  var cy = Math.round(availTop + (availH - ch) / 2);   // centre control in its band

  /* Label rect. */
  var lx = cellX, lw = cellW;
  var lalign = (kind === "knob" || kind === "fader") ? "center" : "left";
  var ly = (labelPos === "below") ? (cellY + cellH - LB) : cellY;

  return { cx: cx, cy: cy, cw: Math.round(ccw), ch: Math.round(ch),
           lx: lx, ly: ly, lw: lw, lalign: lalign, labelPos: labelPos };
}
function ownerOf(item) {
  for (var i = 0; i < DOC.blocks.length; i++) {
    var b = DOC.blocks[i];
    if (b.items.indexOf(item) >= 0) return b;
  }
  return null;
}
function allItems(b) { return b.items.map(function (it) { return { it: it }; }); }

function visible(b) {
  if (view === "MAIN") return !b.layer;
  if (b.layer) return LAYERMAP[view] === b.name;
  return false;   // in a layer view, hide main blocks for a clean surface
}

/* =========================================================================
   RENDER
   ========================================================================= */
function render() {
  var CW = DOC.canvas.w, CH = DOC.canvas.h;
  panel.style.width = Math.round(CW * scale) + "px";
  panel.style.height = Math.round(CH * scale) + "px";
  panel.innerHTML = "";

  var bg = window.SKIN && window.SKIN[HDR.bg];
  panel.style.background = bg ? ("#14171d url(" + bg + ") top center / 100% auto no-repeat") : "#14171d";
  if (gridOn && snap !== "off") {
    var dot = "radial-gradient(circle, rgba(120,190,255,.20) 1px, transparent 1.4px)";
    panel.style.backgroundImage = dot + (bg ? ",url(" + bg + ")" : "");
    panel.style.backgroundSize = (snapStepX * scale) + "px " + (snapStepY * scale) + "px" + (bg ? ",100% auto" : "");
    panel.style.backgroundPosition = (DOC.canvas.margin * scale) + "px 0,0 0";
  }

  drawHeader();

  DOC.blocks.forEach(function (b) {
    if (!visible(b)) return;
    var el = document.createElement("div");
    el.className = "blk" + (sel.indexOf(b) >= 0 && inside !== b ? " sel" : "")
                 + (b.layer ? " layer" : "") + (inside === b ? " inside" : "");
    el.style.cssText = pos(b.x, b.y, b.w, b.h);
    el.dataset.block = b.name;

    var title = document.createElement("div");
    title.className = "blk-title"; title.style.fontSize = px(46 * 0.9);
    title.textContent = b.name.toUpperCase();
    el.appendChild(title);
    if (sel.indexOf(b) >= 0 && inside !== b) addHandles(el);

    b.items.forEach(function (it, ii) {
      /* Effective view: shift preview wins if present; otherwise a conditional
         item resolves against the preview state. Real `it` keeps geometry and
         selection identity. */
      var eff = effectiveItem(it);
      var useShift = shiftPreview && it.sh;
      var cell = useShift ? it.sh : eff;
      var kind = cell.k || it.k, label = cell.t;
      var conditional = !!(it.when && it.variants && it.variants.length);

      var node = document.createElement("div");
      node.className = "item " + kind + (isDisplay(it) ? " display" : "")
        + (conditional ? " cond" : "") + (sel.indexOf(it) >= 0 ? " sel" : "");
      node.style.cssText = pos(it.x - b.x, it.y - b.y, it.w, it.h);
      node.title = cell.key || "";
      node.dataset.itemBlock = b.name;
      node.dataset.ii = ii;                        // exact identity for pointer

      var spr = document.createElement("div"); spr.className = "spr " + kind;
      var lab = document.createElement("div"); lab.className = "ilab" + (useShift ? " shift" : "");
      var wpx = Math.round(it.w * scale), hpx = Math.round(it.h * scale);
      /* Placement from the SHARED controlBox (absolute panel dp), converted to
         cell-relative pixels so the preview matches the exported RML exactly. */
      var cb = controlBox(it);
      var relCX = Math.round((cb.cx - it.x) * scale), relCY = Math.round((cb.cy - it.y) * scale);
      var cwpx2 = Math.round(cb.cw * scale), chpx2 = Math.round(cb.ch * scale);
      var relLY = Math.round((cb.ly - it.y) * scale);

      if (kind === "display") {
        spr.style.cssText = "left:0;top:" + Math.round(20 * scale) + "px;width:" + wpx + "px;height:" + (hpx - Math.round(20 * scale)) + "px;";
        lab.style.cssText = "left:0;top:0;font-size:" + px(26 * 0.9) + ";";
        lab.textContent = (it.role ? "[" + it.role + "] " : "") + (label || "");
      } else {
        spr.style.cssText = "width:" + cwpx2 + "px;height:" + chpx2 + "px;left:" + relCX + "px;top:" + relCY + "px;";
        lab.style.cssText = "left:0;top:" + relLY + "px;width:" + wpx + "px;text-align:" + cb.lalign + ";font-size:" + px(28 * 0.9) + ";"
          + (cb.labelPos === "none" ? "display:none;" : "");
      }
      if (kind !== "display") lab.textContent = (label || "") + (it.sh ? " *" : "") + (conditional ? " ▸" : "");
      node.appendChild(spr); node.appendChild(lab);
      if (sel.indexOf(it) >= 0) addHandles(node);
      el.appendChild(node);
    });
    panel.appendChild(el);
  });

  buildTree(); buildInspector(); buildCoverage(); buildPreviewBar();
  document.getElementById("info").textContent =
    Math.round(scale * 100) + "%   " + DOC.canvas.w + "x" + DOC.canvas.h + "   " + view
    + (inside ? "   INSIDE " + inside.name : "") + (shiftPreview ? "   SHIFT" : "")
    + "   " + sel.length + " sel";
}
function pos(x, y, w, h) { return "left:" + Math.round(x * scale) + "px;top:" + Math.round(y * scale) + "px;width:" + Math.round(w * scale) + "px;height:" + Math.round(h * scale) + "px;"; }
function px(v) { return Math.max(7, Math.round(v * scale)) + "px"; }

/* Draw a move grip (top-left) and a grow grip (bottom-right) on a selected
   node. The grips are the ONLY resize affordance; the body always moves. Their
   cursors (move / nwse-resize) make the action unambiguous before you click. */
function addHandles(node) {
  var mv = document.createElement("div"); mv.className = "h-move"; mv.title = "Move"; node.appendChild(mv);
  var gr = document.createElement("div"); gr.className = "h-grow"; gr.title = "Resize cell"; node.appendChild(gr);
  /* Control-size grip (bottom-left): resizes the control (cw/ch) inside its
     cell, separate from the cell box. Only on real controls, not displays. */
  if (node.classList.contains("item") && !node.classList.contains("display")) {
    var cs = document.createElement("div"); cs.className = "h-ctrl"; cs.title = "Resize control"; node.appendChild(cs);
  }
}

function drawHeader() {
  var M = HEADER.m, H = HEADER.h, CW = DOC.canvas.w;
  var img = window.SKIN && window.SKIN[HDR.band[view] || HDR.band.MAIN];
  var band = document.createElement("div"); band.className = "hdr-band" + (sel.indexOf("BAND") >= 0 ? " sel" : "");
  band.style.cssText = pos(M, M, CW - 2 * M, H) + (img ? "background:url(" + img + ") center/100% 100% no-repeat;" : "background:#22262e;");
  band.onclick = function (e) { e.stopPropagation(); sel = ["BAND"]; render(); };
  panel.appendChild(band);
  HDR.texts.forEach(function (t, i) {
    var d = document.createElement("div"); d.className = "hdr-text" + (sel.indexOf("TXT" + i) >= 0 ? " sel" : "");
    d.style.cssText = "left:" + Math.round((M + t.x) * scale) + "px;top:" + Math.round((M + t.y) * scale) + "px;width:" + Math.round(t.w * scale) + "px;font-size:" + px(t.size) + ";color:" + t.color + ";text-align:" + t.align + ";";
    d.textContent = t.t || "";
    d.onclick = function (e) { e.stopPropagation(); sel = ["TXT" + i]; render(); };
    panel.appendChild(d);
  });
}

/* =========================================================================
   POINTER  (free-drag with snap, from the original editor)
   ========================================================================= */
var drag = null;
function snapV(v, axis) {
  if (snap === "off") return Math.round(v);
  var step = axis === "x" ? snapStepX : snapStepY, base = axis === "x" ? DOC.canvas.margin : 0;
  return Math.round(base + Math.round((v - base) / step) * step);
}
panel.addEventListener("pointerdown", function (e) {
  var itemEl = e.target.closest(".item"), blkEl = e.target.closest(".blk");
  if (!blkEl && !itemEl) { if (!e.target.closest(".hdr-band,.hdr-text")) { sel = []; render(); } return; }
  var b = blockByName(blkEl.dataset.block);

  var item = b;
  if (itemEl) { item = findItem(b, itemEl); }

  /* double-click a block to enter it */
  var now = Date.now();
  if (blkEl && !itemEl) {
    if (lastDown.el === b && now - lastDown.t < 350 && inside !== b) { inside = b; sel = []; lastDown = { el: null, t: 0 }; render(); e.preventDefault(); return; }
    lastDown = { el: b, t: now };
  }

  if (e.shiftKey) { var k = sel.indexOf(item); if (k >= 0) sel.splice(k, 1); else sel.push(item); }
  else if (sel.indexOf(item) < 0) sel = [item];

  /* Mode comes from an explicit handle, not an invisible edge band. A grow
     handle (bottom-right) resizes; anything else moves. Handles are drawn only
     on the selected item/block, so the body is always a safe move target. */
  var mode = "move";
  var handle = e.target.closest(".h-grow, .h-move, .h-ctrl");
  if (handle && handle.classList.contains("h-grow")) mode = "wh";
  else if (handle && handle.classList.contains("h-ctrl")) mode = "cwh";

  drag = { mode: mode, sx: e.clientX, sy: e.clientY, moved: false, snap: JSON.stringify(DOC),
           start: sel.filter(function (s) { return typeof s !== "string" && s.x !== undefined; })
                     .map(function (it) { return { it: it, x: it.x, y: it.y, w: it.w || 0, h: it.h || 0,
                                                    cw: (it.cw !== undefined ? it.cw : (it.w || 0)),
                                                    ch: (it.ch !== undefined ? it.ch : (it.h || 0)) }; }) };
  panel.setPointerCapture(e.pointerId);
  document.body.style.cursor = mode === "move" ? "grabbing" : "nwse-resize";
  render(); e.preventDefault();
});
panel.addEventListener("pointermove", function (e) {
  if (!drag) return;
  var dx = (e.clientX - drag.sx) / scale, dy = (e.clientY - drag.sy) / scale;
  if (!drag.moved && (Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5)) { drag.moved = true; undoStack.push(drag.snap); if (undoStack.length > HMAX) undoStack.shift(); redoStack.length = 0; histBtns(); }
  drag.start.forEach(function (s) {
    if (drag.mode === "move") { s.it.x = snapV(s.x + dx, "x"); s.it.y = snapV(s.y + dy, "y"); }
    else if (drag.mode === "cwh") {
      /* Control-size grip: resize cw/ch independently of the cell. Minimum 8px.
         When the control's aspect is locked (it.lockAR, and always for knobs,
         which must stay round) both axes scale together from the starting ratio,
         driven by whichever axis you dragged further. */
      var locked = s.it.lockAR || (s.it.k || "knob") === "knob";
      if (locked && s.cw > 0 && s.ch > 0) {
        var ar = s.ch / s.cw;                         // preserve starting ratio
        var ncw = Math.abs(dx) >= Math.abs(dy) ? (s.cw + dx) : ((s.ch + dy) / ar);
        ncw = Math.max(8, Math.round(ncw));
        s.it.cw = ncw;
        s.it.ch = Math.max(8, Math.round(ncw * ar));
      } else {
        s.it.cw = Math.max(8, Math.round(s.cw + dx));
        s.it.ch = Math.max(8, Math.round(s.ch + dy));
      }
    }
    else {
      /* grow handle: resize width+height, snapping the far edge to the grid so
         sizes stay aligned. Minimum 20px. */
      var nx2 = snap === "off" ? Math.round(s.x + s.w + dx) : snapV(s.x + s.w + dx, "x");
      var ny2 = snap === "off" ? Math.round(s.y + s.h + dy) : snapV(s.y + s.h + dy, "y");
      s.it.w = Math.max(20, nx2 - s.x);
      s.it.h = Math.max(20, ny2 - s.y);
    }
  });
  render(); e.preventDefault();
});
panel.addEventListener("pointerup", function () { drag = null; document.body.style.cursor = ""; });
panel.addEventListener("pointercancel", function () { drag = null; document.body.style.cursor = ""; });

function findItem(b, itemEl) {
  /* Resolve directly from the item index stamped at render. */
  var ii = +itemEl.dataset.ii;
  if (b.items[ii]) return b.items[ii];
  return b;
}

/* =========================================================================
   KEYBOARD
   ========================================================================= */
window.addEventListener("keydown", function (e) {
  var tag = (e.target.tagName || "").toLowerCase();
  var mod = e.ctrlKey || e.metaKey;
  if (mod && (e.key === "z" || e.key === "Z")) { if (tag === "input" || tag === "textarea") return; e.shiftKey ? redo() : undo(); e.preventDefault(); return; }
  if (mod && (e.key === "y" || e.key === "Y")) { redo(); e.preventDefault(); return; }
  if (tag === "input" || tag === "textarea" || tag === "select") return;
  if (e.key === "Escape") { inside = null; sel = []; render(); return; }
  if (e.key === "Delete" || e.key === "Backspace") { if (sel.length) { deleteSel(); e.preventDefault(); } return; }
  var d = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[e.key];
  if (!d || !sel.length) return;
  pushCoalesced();
  var step = e.shiftKey ? 10 : 1;
  sel.forEach(function (it) { if (typeof it !== "string" && it.x !== undefined) { it.x += d[0] * step; it.y += d[1] * step; } });
  render(); e.preventDefault();
});

function deleteSel() {
  pushHistory();
  sel.forEach(function (it) {
    if (typeof it === "string") return;
    if (isBlock(it)) { var i = DOC.blocks.indexOf(it); if (i >= 0) DOC.blocks.splice(i, 1); if (inside === it) inside = null; return; }
    var own = ownerOf(it); if (!own) return;
    var k = own.items.indexOf(it); if (k >= 0) own.items.splice(k, 1);
  });
  sel = []; render();
}

/* =========================================================================
   TREE
   ========================================================================= */
var tree = document.getElementById("tree");
function buildTree() {
  tree.innerHTML = "";
  DOC.blocks.forEach(function (b) {
    var r = trow("t-blk", sel.indexOf(b) >= 0, function () { sel = [b]; inside = null; render(); });
    r.appendChild(tspan("", b.name)); if (b.layer) r.appendChild(tspan("badge", "LAYER"));
    tree.appendChild(r);
    b.items.forEach(function (it) {
      var ir = trow("t-itm" + (it.key || isDisplay(it) ? "" : " nokey"), sel.indexOf(it) >= 0, function () { inside = b; sel = [it]; render(); });
      ir.appendChild(tspan("dot" + (isDisplay(it) ? " disp" : ""), ""));
      ir.appendChild(tspan("", it.t || (isDisplay(it) ? "(display)" : "(item)")));
      if (isDisplay(it)) ir.appendChild(tspan("facet disp", "D"));
      else if (it.sh) ir.appendChild(tspan("facet", "S"));
      ir.appendChild(tspan("tk", isDisplay(it) ? (it.role || "display") : (it.key || "unassigned")));
      tree.appendChild(ir);
    });
  });
}
function trow(cls, on, fn) { var d = document.createElement("div"); d.className = "trow " + cls + (on ? " on" : ""); d.onclick = fn; return d; }
function tspan(cls, t) { var s = document.createElement("span"); if (cls) s.className = cls; s.textContent = t; return s; }

/* =========================================================================
   INSPECTOR
   ========================================================================= */
var insp = document.getElementById("insp");
var facetTab = "primary";
function buildInspector() {
  insp.innerHTML = "";
  var chip = document.getElementById("selchip");

  if (sel.length === 1 && typeof sel[0] === "string") { chip.textContent = "hdr"; return inspHeader(sel[0]); }
  if (!sel.length) { chip.textContent = "0"; return inspGrid(); }
  if (sel.length > 1) { chip.textContent = sel.length; return inspMulti(); }

  var it = sel[0];
  if (isBlock(it)) { chip.textContent = "block"; return inspBlock(it); }
  if (isDisplay(it)) { chip.textContent = "display"; return inspDisplay(it); }
  chip.textContent = "item"; return inspItem(it);
}
function ihead(k, n) { var h = document.createElement("div"); h.className = "ihead"; h.appendChild(sp("k", k)); h.appendChild(sp("", n || "")); insp.appendChild(h); }
function ig() { var g = document.createElement("div"); g.className = "igrid"; insp.appendChild(g); return g; }
function sp(c, t) { var s = document.createElement("span"); if (c) s.className = c; s.textContent = t; return s; }

function inspGrid() {
  ihead("grid", "Snap & canvas");
  var g = ig();
  gnum(g, "Snap X", snapStepX, function (v) { if (v > 0) snapStepX = v; });
  gnum(g, "Snap Y", snapStepY, function (v) { if (v > 0) snapStepY = v; });
  gnum(g, "Canvas W", DOC.canvas.w, function (v) { if (v > 0) DOC.canvas.w = v; });
  gnum(g, "Canvas H", DOC.canvas.h, function (v) { if (v > 0) DOC.canvas.h = v; });
  gnum(g, "Margin", DOC.canvas.margin, function (v) { DOC.canvas.margin = v; });
  var rb = document.createElement("div"); rb.className = "row-btns";
  rb.appendChild(mbtn(gridOn ? "Hide grid" : "Show grid", function () { gridOn = !gridOn; render(); }));
  rb.appendChild(mbtn(snap === "off" ? "Snap on" : "Snap off", function () { snap = snap === "off" ? "grid" : "off"; render(); }));
  insp.appendChild(rb);
  insp.appendChild(mbtn("+ Add block", function () { pushHistory(); DOC.blocks.push({ name: uniq("Block"), x: 80, y: 400, w: 500, h: 300, layer: false, items: [] }); sel = [DOC.blocks[DOC.blocks.length - 1]]; render(); }));
}
function inspBlock(b) {
  ihead("block", b.name);
  var g = ig();
  tfield(g, b, "name", "Name", true); gnum(g, "X", b.x, function (v) { b.x = v; }); gnum(g, "Y", b.y, function (v) { b.y = v; }); gnum(g, "W", b.w, function (v) { b.w = v; }); gnum(g, "H", b.h, function (v) { b.h = v; });
  cfield("Layer block", !!b.layer, function (on) { pushHistory(); b.layer = on; render(); });
  insp.appendChild(mbtn("+ Add control", function () { pushHistory(); b.items.push({ k: "knob", t: "NEW", key: "", x: b.x + 24, y: b.y + 60, w: 160, h: 194, lw: 60 }); inside = b; sel = [b.items[b.items.length - 1]]; render(); }));
  insp.appendChild(mbtn("Delete block", function () { if (confirm("Delete block " + b.name + "?")) { pushHistory(); DOC.blocks.splice(DOC.blocks.indexOf(b), 1); sel = []; render(); } }, "danger"));
}
function inspItem(it) {
  ihead("item", it.t);
  var tabs = document.createElement("div"); tabs.className = "facet-tabs";
  tabs.appendChild(tbtn("Primary", facetTab === "primary", function () { facetTab = "primary"; render(); }));
  tabs.appendChild(tbtn("Shift" + (it.sh ? " ●" : ""), facetTab === "shift", function () { facetTab = "shift"; render(); }));
  insp.appendChild(tabs);

  if (facetTab === "primary") {
    itemFields(it, it, true);
    conditionalEditor(it);
    insp.appendChild(mbtn("+ Add control here", function () {
      pushHistory();
      var own = ownerOf(it);
      if (!own) return;
      var nc = { k: "knob", t: "NEW", key: "", x: it.x + (it.w || 160) + 10, y: it.y, w: 160, h: 194, lw: 60 };
      own.items.push(nc); sel = [nc]; render();
    }));
    insp.appendChild(mbtn("Convert to display-only", function () { pushHistory(); toDisplay(it); render(); }));
    insp.appendChild(mbtn("Delete", function () { pushHistory(); removeItem(it); sel = []; render(); }, "danger"));
  } else {
    if (!it.sh) { insp.appendChild(mbtn("+ Add shift function", function () { pushHistory(); it.sh = { k: it.k, t: it.t, key: it.key, lw: it.lw || 60 }; render(); })); insp.appendChild(hint("Shift facet: what this control does when SHIFT is held.")); }
    else { itemFields(it.sh, it, false); insp.appendChild(mbtn("Remove shift function", function () { pushHistory(); delete it.sh; facetTab = "primary"; render(); }, "danger")); }
  }
}
/* Conditional / lookup-table editor for an item. Pick a driver param key; the
   editor lays out one row per option of that input's set, and each row assigns
   what this control becomes (target key, and optional label/kind overrides) at
   that value. The Preview bar picks the live driver value so the canvas shows
   the active variant. Fully additive: no `when` means an ordinary control. */
function conditionalEditor(it) {
  var box = document.createElement("div"); box.className = "cond-box"; insp.appendChild(box);
  var h = document.createElement("div"); h.className = "cond-hdr"; h.appendChild(sp("", "Conditional — control changes with an input")); box.appendChild(h);

  var dwrap = document.createElement("label"); dwrap.className = "ifield full";
  dwrap.appendChild(sp("", "Driven by input param"));
  var d = document.createElement("input"); d.type = "text"; d.setAttribute("list", "paramKeys"); d.className = "cond-driver";
  d.value = it.when || ""; d.placeholder = "e.g. osc1.wave  (blank = none)";
  d.oninput = function () { pushCoalesced(); it.when = d.value.trim(); if (!it.when) delete it.variants; else if (!it.variants) it.variants = []; render(); var a = insp.querySelector(".cond-driver"); if (a) { a.focus(); var L = a.value.length; try { a.setSelectionRange(L, L); } catch (e) {} } };
  dwrap.appendChild(d); box.appendChild(dwrap);
  if (!it.when) return;

  var opts = optionsForKey(it.when);
  if (opts && opts.length && (!it.variants || !it.variants.length)) {
    box.appendChild(mbtn("Build table from " + optionSetForKey(it.when) + " (" + opts.length + " values)", function () {
      pushHistory(); it.variants = opts.map(function (o) { return { eq: o, key: it.key, t: it.t, k: it.k }; }); render();
    }));
  }
  if (it.variants && it.variants.length) {
    var th = document.createElement("div"); th.className = "cond-th"; th.appendChild(sp("cond-eq", "When")); th.appendChild(sp("", "Target key · label · kind")); box.appendChild(th);
  }
  (it.variants || []).forEach(function (v, vi) {
    var row = document.createElement("div"); row.className = "cond-row";
    var live = previewValues[it.when] === v.eq;
    if (opts && opts.length) row.appendChild(sp("cond-eq" + (live ? " live" : ""), v.eq));
    else { var eqi = document.createElement("input"); eqi.className = "cond-eqin" + (live ? " live" : ""); eqi.value = v.eq || ""; eqi.placeholder = "value"; eqi.oninput = function () { pushCoalesced(); v.eq = eqi.value; buildPreviewBar(); }; row.appendChild(eqi); }
    var stack = document.createElement("div"); stack.className = "cond-stack";
    var ki = document.createElement("input"); ki.type = "text"; ki.setAttribute("list", "paramKeys"); ki.value = v.key || ""; ki.placeholder = "target param key"; ki.dataset.vk = vi;
    ki.oninput = function () { pushCoalesced(); v.key = ki.value; if (CAT.labels[v.key]) v.t = CAT.labels[v.key]; render(); var a = insp.querySelectorAll(".cond-row")[vi]; if (a) { var f = a.querySelector('input[data-vk="' + vi + '"]'); if (f) f.focus(); } };
    stack.appendChild(ki);
    var meta = document.createElement("div"); meta.className = "cond-meta";
    var li = document.createElement("input"); li.className = "cond-lab"; li.value = v.t || ""; li.placeholder = "label"; li.oninput = function () { pushCoalesced(); v.t = li.value; render(); }; meta.appendChild(li);
    var ks = document.createElement("select"); ["", "knob", "fader", "combo", "toggle", "stepgrid"].forEach(function (kk) { var o = document.createElement("option"); o.value = kk; o.textContent = kk || "(kind)"; ks.appendChild(o); }); ks.value = v.k || ""; ks.onchange = function () { pushHistory(); if (ks.value) v.k = ks.value; else delete v.k; render(); }; meta.appendChild(ks);
    stack.appendChild(meta); row.appendChild(stack);
    var del = document.createElement("button"); del.type = "button"; del.className = "cond-del"; del.textContent = "×"; del.onclick = function () { pushHistory(); it.variants.splice(vi, 1); render(); }; row.appendChild(del);
    box.appendChild(row);
  });
  box.appendChild(mbtn("+ Add mapping row", function () { pushHistory(); if (!it.variants) it.variants = []; it.variants.push({ eq: "", key: it.key, t: it.t }); render(); }));
}

function inspDisplay(it) {
  ihead("display", it.t || "(display)");
  var g = ig();
  tfield(g, it, "t", "Label", true);
  selfield(g, it, "role", "Role", ["patch", "value", "scope", "custom"]);
  gnum(g, "X", it.x, function (v) { it.x = v; }); gnum(g, "Y", it.y, function (v) { it.y = v; });
  gnum(g, "W", it.w, function (v) { it.w = v; }); gnum(g, "H", it.h, function (v) { it.h = v; });
  insp.appendChild(mbtn("Convert back to control", function () { pushHistory(); it.k = "knob"; delete it.role; render(); }));
  insp.appendChild(mbtn("Delete", function () { pushHistory(); removeItem(it); sel = []; render(); }, "danger"));
}
function itemFields(obj, ctx, withGeom) {
  var g = ig();
  tfield(g, obj, "t", "Label", true);
  kfield(g, obj, "key", "Param key");
  selfield(g, obj, "k", "Kind", ["knob", "fader", "combo", "toggle", "stepgrid"]);
  numf(g, obj, "lw", "Legend w");
  if (withGeom) {
    gnum(g, "X", ctx.x, function (v) { ctx.x = v; }); gnum(g, "Y", ctx.y, function (v) { ctx.y = v; });
    gnum(g, "W", ctx.w, function (v) { ctx.w = v; }); gnum(g, "H", ctx.h, function (v) { ctx.h = v; });
    /* Control size: what the exporter emits for the element itself, separate
       from the cell box above. Defaults to the cell until set; the green handle
       on the selected control resizes it visually. */
    gnum(g, "Ctrl W", ctx.cw !== undefined ? ctx.cw : ctx.w, function (v) { ctx.cw = v; });
    gnum(g, "Ctrl H", ctx.ch !== undefined ? ctx.ch : ctx.h, function (v) { ctx.ch = v; });
    /* Lock aspect ratio: resizing the control keeps its proportions. Always on
       for knobs (they must stay round); opt-in for other kinds. */
    if ((obj.k || "knob") === "knob")
      insp.appendChild(hint("Aspect locked (knob stays round)."));
    else
      cfield("Lock aspect ratio when resizing", !!ctx.lockAR, function (on) { ctx.lockAR = on; });
    selfield(g, obj, "labelPos", "Label", ["above", "below", "none"]);
  }
  optsField(obj);
}
function inspMulti() {
  ihead("multi", sel.length + " items");
  insp.appendChild(hint("Move with drag or arrows; Delete removes all. Alignment below."));
  var g = document.createElement("div"); g.className = "row-btns"; g.style.flexWrap = "wrap";
  [["Left", "al"], ["Right", "ar"], ["Top", "at"], ["Bottom", "ab"], ["Same W", "aw"], ["Same H", "ah"]].forEach(function (a) {
    g.appendChild(mbtn(a[0], function () { alignSel(a[1]); }));
  });
  insp.appendChild(g);
}
function inspHeader(tag) {
  if (tag === "BAND") {
    ihead("header", "Band (" + view + ")");
    imgPick("Band for " + view, HDR.band[view], ["band_main", "band_reverb", "band_step", "band_arpeggiator", "band_setup"], function (n) { pushHistory(); HDR.band[view] = n; render(); });
    imgPick("Base background", HDR.bg, ["bg_base"], function (n) { pushHistory(); HDR.bg = n; render(); });
    insp.appendChild(hint("Edits the skin; use Export → header patch."));
  } else {
    var i = +tag.slice(3), t = HDR.texts[i];
    ihead("header text", t.id);
    var g = ig();
    tfield(g, t, "t", "Text", true); numf(g, t, "x", "X"); numf(g, t, "y", "Y"); numf(g, t, "w", "W"); numf(g, t, "size", "Size"); tfield(g, t, "color", "Colour", false); selfield(g, t, "align", "Align", ["left", "center", "right"]);
    insp.appendChild(mbtn("+ Add text", function () { pushHistory(); HDR.texts.push({ id: "txt" + HDR.texts.length, t: "TEXT", x: 544, y: 260, w: 400, size: 30, color: "#ffa83a", align: "left" }); sel = ["TXT" + (HDR.texts.length - 1)]; render(); }));
    insp.appendChild(mbtn("Delete text", function () { pushHistory(); HDR.texts.splice(i, 1); sel = ["BAND"]; render(); }, "danger"));
  }
}

function toDisplay(it) { it.k = "display"; it.role = it.role || "value"; delete it.sh; }
function removeItem(it) { var own = ownerOf(it); if (!own) return; var k = own.items.indexOf(it); if (k >= 0) own.items.splice(k, 1); }
function uniq(base) { var n = base, i = 1; while (blockByName(n)) n = base + " " + (++i); return n; }

/* alignment across selection */
function alignSel(kind) {
  var items = sel.filter(function (s) { return typeof s !== "string" && s.x !== undefined; });
  if (items.length < 2) return;
  pushHistory();
  if (kind === "al") { var v = Math.min.apply(null, items.map(function (i) { return i.x; })); items.forEach(function (i) { i.x = v; }); }
  if (kind === "ar") { var v2 = Math.max.apply(null, items.map(function (i) { return i.x + i.w; })); items.forEach(function (i) { i.x = v2 - i.w; }); }
  if (kind === "at") { var v3 = Math.min.apply(null, items.map(function (i) { return i.y; })); items.forEach(function (i) { i.y = v3; }); }
  if (kind === "ab") { var v4 = Math.max.apply(null, items.map(function (i) { return i.y + i.h; })); items.forEach(function (i) { i.y = v4 - i.h; }); }
  if (kind === "aw") { var w = items[0].w; items.forEach(function (i) { i.w = w; }); }
  if (kind === "ah") { var h = items[0].h; items.forEach(function (i) { i.h = h; }); }
  render();
}

/* field helpers */
function tfield(p, o, n, l, full) { var w = document.createElement("label"); w.className = "ifield" + (full ? " full" : ""); w.appendChild(sp("", l)); var inp = document.createElement("input"); inp.type = "text"; inp.dataset.f = n; inp.value = o[n] !== undefined ? o[n] : ""; inp.oninput = function () { pushCoalesced(); o[n] = inp.value; render(); kf(n); }; w.appendChild(inp); p.appendChild(w); }
function numf(p, o, n, l) { var w = document.createElement("label"); w.className = "ifield"; w.appendChild(sp("", l)); var inp = document.createElement("input"); inp.type = "number"; inp.step = "any"; inp.dataset.f = n; inp.value = o[n] !== undefined ? o[n] : ""; inp.oninput = function () { pushCoalesced(); var v = parseFloat(inp.value); if (!isNaN(v)) o[n] = v; else delete o[n]; render(); kf(n); }; w.appendChild(inp); p.appendChild(w); }
function gnum(p, l, val, apply) { var w = document.createElement("label"); w.className = "ifield"; w.appendChild(sp("", l)); var inp = document.createElement("input"); inp.type = "number"; inp.step = "any"; inp.dataset.gf = l; inp.value = Math.round(val * 100) / 100; inp.oninput = function () { pushCoalesced(); var v = parseFloat(inp.value); if (!isNaN(v)) { apply(v); render(); kfg(l); } }; w.appendChild(inp); p.appendChild(w); }
function kfield(p, o, n, l) { var w = document.createElement("label"); w.className = "ifield full"; w.appendChild(sp("", l + (CAT.keys.length ? "  (search)" : ""))); var inp = document.createElement("input"); inp.type = "text"; inp.dataset.f = n; inp.setAttribute("list", "paramKeys"); inp.value = o[n] !== undefined ? o[n] : ""; inp.oninput = function () { pushCoalesced(); o[n] = inp.value; if (CAT.labels[inp.value] && (!o.t || o.t === "NEW")) o.t = CAT.labels[inp.value]; render(); kf(n); }; w.appendChild(inp); p.appendChild(w); }
function selfield(p, o, n, l, opts) { var w = document.createElement("label"); w.className = "ifield"; w.appendChild(sp("", l)); var s = document.createElement("select"); opts.forEach(function (op) { var e = document.createElement("option"); e.value = e.textContent = op; s.appendChild(e); }); s.value = o[n] !== undefined ? o[n] : opts[0]; s.onchange = function () { pushHistory(); o[n] = s.value; render(); }; w.appendChild(s); p.appendChild(w); }
function cfield(l, checked, fn) { var w = document.createElement("label"); w.className = "ifield full check"; var cb = document.createElement("input"); cb.type = "checkbox"; cb.checked = checked; cb.onchange = function () { fn(cb.checked); }; w.appendChild(cb); w.appendChild(sp("", l)); insp.appendChild(w); }
function optsField(o) { var w = document.createElement("label"); w.className = "ifield full"; w.appendChild(sp("", "Options (comma-sep)")); var ta = document.createElement("textarea"); ta.className = "opts-ta"; ta.dataset.f = "opts"; ta.rows = 2; ta.value = o.opts ? o.opts.join(", ") : ""; ta.oninput = function () { pushCoalesced(); o.opts = ta.value.split(",").map(function (s) { return s.trim(); }).filter(Boolean); grow(ta); render(); kf("opts"); }; w.appendChild(ta); var so = optionsForKey(o.key); if (so && so.length) w.appendChild(mbtn("Fill from " + optionSetForKey(o.key) + " (" + so.length + ")", function () { pushHistory(); o.opts = so.slice(); render(); })); insp.appendChild(w); setTimeout(function () { grow(ta); }, 0); }
function grow(ta) { ta.style.height = "auto"; ta.style.height = Math.max(ta.scrollHeight, 30) + "px"; }
function kf(n) { var el = insp.querySelector('[data-f="' + n + '"]'); if (el) { var p = el.selectionStart; el.focus(); try { el.setSelectionRange(p, p); } catch (e) {} } }
function kfg(l) { var el = insp.querySelector('[data-gf="' + CSS.escape(l) + '"]'); if (el) el.focus(); }
function mbtn(l, fn, c) { var b = document.createElement("button"); b.type = "button"; b.className = "mini-btn" + (c ? " " + c : ""); b.textContent = l; b.onclick = fn; return b; }
function tbtn(l, on, fn) { var b = document.createElement("button"); b.type = "button"; b.className = "facet-tab" + (on ? " on" : ""); b.textContent = l; b.onclick = fn; return b; }
function hint(t) { var d = document.createElement("div"); d.className = "hint"; d.textContent = t; return d; }
function imgPick(label, cur, choices, onPick) { var w = document.createElement("div"); w.className = "imgpick"; w.appendChild(sp("il", label)); choices.forEach(function (n) { var img = window.SKIN && window.SKIN[n]; var th = document.createElement("div"); th.className = "imgthumb" + (n === cur ? " on" : ""); th.style.backgroundImage = img ? "url(" + img + ")" : ""; th.title = n; th.onclick = function () { onPick(n); }; var c = document.createElement("div"); c.className = "imgcap"; c.textContent = n.replace("band_", "").replace("bg_", ""); th.appendChild(c); w.appendChild(th); }); insp.appendChild(w); }

/* =========================================================================
   COVERAGE
   ========================================================================= */
var covOpen = { unused: true, used: false, problems: true };
function computeCoverage() {
  var usage = {}; function add(key, bn, lab, f) { if (!key) return; (usage[key] = usage[key] || []).push({ block: bn, ctrl: lab, facet: f }); }
  /* A stepgrid stands in for its per-step params (seq.step_1..N per lane), so
     mark those covered - otherwise all 16xN show as unused. Prefix + step count
     come from the item or its factory map entry. */
  function addGrid(it, bn, lab) {
    var m = (window.PARAMMAP || {})[it.key] || {};
    var pre = it.stepprefix || m.stepprefix;   // "seq.step_,seq.aux_step_"
    if (!pre) { add(it.key, bn, lab, "primary"); return; }
    var steps = parseInt(it.steps || m.steps || "16", 10) || 16;
    pre.split(",").forEach(function (p) {
      p = p.trim(); if (!p) return;
      for (var s = 1; s <= steps; s++) add(p + s, bn, lab, "grid");
    });
  }
  DOC.blocks.forEach(function (b) { b.items.forEach(function (it) { if (isDisplay(it)) return; if ((it.k || (window.PARAMMAP && window.PARAMMAP[it.key] && window.PARAMMAP[it.key].kind)) === "stepgrid") { addGrid(it, b.name, it.t); return; } add(it.key, b.name, it.t, "primary"); if (it.sh) add(it.sh.key, b.name, it.t, "shift"); (it.variants || []).forEach(function (v) { add(v.key, b.name, it.t, "variant:" + v.eq); }); }); });
  var ck = CAT.keys || [], cs = {}; ck.forEach(function (k) { cs[k] = 1; });
  var used = Object.keys(usage).sort(), unused = ck.filter(function (k) { return !usage[k]; }).sort(), unknown = used.filter(function (k) { return !cs[k]; });
  var dupes = used.filter(function (k) { var c = {}; usage[k].forEach(function (u) { c[u.block + "/" + u.ctrl] = 1; }); return Object.keys(c).length > 1; });
  return { usage: usage, used: used, unused: unused, unknown: unknown, dupes: dupes, total: ck.length };
}
function buildCoverage() {
  var host = document.getElementById("coverage"); if (!host) return; var cov = computeCoverage(); host.innerHTML = "";
  var s = document.createElement("div"); s.className = "cov-sum"; s.appendChild(pill(cov.used.length + "/" + cov.total, "assigned", "ok")); s.appendChild(pill(String(cov.unused.length), "unused", cov.unused.length ? "warn" : "ok")); var pr = cov.unknown.length + cov.dupes.length; s.appendChild(pill(String(pr), "problems", pr ? "err" : "ok")); host.appendChild(s);
  csec(host, "unused", "Unused (" + cov.unused.length + ")", function (bd) { if (!cov.unused.length) { bd.appendChild(hint("All params assigned.")); return; } cov.unused.forEach(function (k) { var r = document.createElement("div"); r.className = "cov-row"; r.appendChild(sp("cov-key", k)); if (CAT.labels[k]) r.appendChild(sp("cov-lab", CAT.labels[k])); bd.appendChild(r); }); });
  csec(host, "used", "Where used (" + cov.used.length + ")", function (bd) { cov.used.forEach(function (k) { var h = document.createElement("div"); h.className = "cov-row head"; h.appendChild(sp("cov-key", k)); h.appendChild(sp("cov-count", String(cov.usage[k].length))); bd.appendChild(h); cov.usage[k].forEach(function (u) { var r = document.createElement("div"); r.className = "cov-use"; r.appendChild(sp("cov-facet " + (u.facet === "shift" ? "s" : "p"), u.facet === "shift" ? "S" : "P")); r.appendChild(sp("", u.block + " · " + u.ctrl)); r.onclick = function () { jumpTo(u.block, u.ctrl); }; bd.appendChild(r); }); }); });
  csec(host, "problems", "Problems (" + pr + ")", function (bd) { if (!pr) { bd.appendChild(hint("No issues.")); return; } if (cov.unknown.length) { bd.appendChild(csub("Unknown keys")); cov.unknown.forEach(function (k) { var r = document.createElement("div"); r.className = "cov-row err"; r.appendChild(sp("cov-key", k)); r.onclick = function () { var u = cov.usage[k][0]; jumpTo(u.block, u.ctrl); }; bd.appendChild(r); }); } if (cov.dupes.length) { bd.appendChild(csub("Duplicates")); cov.dupes.forEach(function (k) { var r = document.createElement("div"); r.className = "cov-row warn"; r.appendChild(sp("cov-key", k)); r.appendChild(sp("cov-count", String(cov.usage[k].length))); bd.appendChild(r); }); } });
  function csec(host, id, title, fill) { var w = document.createElement("div"); w.className = "cov-sec"; var h = document.createElement("div"); h.className = "cov-h" + (covOpen[id] ? " open" : ""); h.textContent = (covOpen[id] ? "▾ " : "▸ ") + title; h.onclick = function () { covOpen[id] = !covOpen[id]; buildCoverage(); }; w.appendChild(h); if (covOpen[id]) { var bd = document.createElement("div"); bd.className = "cov-body"; fill(bd); w.appendChild(bd); } host.appendChild(w); }
  function csub(t) { var d = document.createElement("div"); d.className = "cov-subhead"; d.textContent = t; return d; }
}
function pill(n, l, tone) { var d = document.createElement("div"); d.className = "cov-pill " + tone; d.appendChild(sp("n", n)); d.appendChild(sp("l", l)); return d; }
function jumpTo(bn, ctrl) { var b = blockByName(bn); if (!b) return; for (var i = 0; i < b.items.length; i++) if (b.items[i].t === ctrl) { inside = b; sel = [b.items[i]]; render(); return; } sel = [b]; render(); }

/* =========================================================================
   EXPORT + TOOLBAR + BOOT
   ========================================================================= */
/* Preview bar: for every driver key used by a conditional item, a dropdown of
   that input's option values so the canvas shows the matching variant live. */
function driverKeysInUse() { var s = {}; DOC.blocks.forEach(function (b) { b.items.forEach(function (it) { if (it.when) s[it.when] = 1; }); }); return Object.keys(s); }
function buildPreviewBar() {
  var bar = document.getElementById("preview"); if (!bar) return;
  bar.innerHTML = ""; bar.appendChild(sp("clabel", "Preview"));
  var drivers = driverKeysInUse();
  if (!drivers.length) { bar.style.display = "none"; return; }
  bar.style.display = "";
  drivers.forEach(function (key) {
    var opts = optionsForKey(key) || [];
    var s = document.createElement("select"); s.title = key;
    var none = document.createElement("option"); none.value = ""; none.textContent = (CAT.labels[key] || key) + ": —"; s.appendChild(none);
    opts.forEach(function (o) { var op = document.createElement("option"); op.value = op.textContent = o; s.appendChild(op); });
    s.value = previewValues[key] || "";
    s.onchange = function () { if (s.value) previewValues[key] = s.value; else delete previewValues[key]; render(); };
    bar.appendChild(s);
  });
}

function dl(name, text, mime) { var b = new Blob([text], { type: mime || "text/plain" }); var a = document.createElement("a"); a.href = URL.createObjectURL(b); a.download = name; a.click(); }
function exportLayout() { dl("layout.json", JSON.stringify(DOC, null, 1), "application/json"); }

/* Export a runtime jt8000.rml the plugin can load from its skin disk-root (no
   recompile). Mirrors the shipped RML grammar exactly: one control element per
   item (<knob>/<combo>/<toggle>/<fader>) carrying param/key/default/options,
   plus a positioned <div class="legend"> label, all in dp with the standard
   view-class set. param ids come from the bundled PARAMMAP (not derivable from
   keys); unknown keys fall back to key-with-underscores and are listed in a
   trailing comment so nothing binds silently wrong. */
var VIEWCLASS = "v-main v-shift v-reverb v-seq v-arp v-setup";
/* A layer block only shows on its own page; main blocks show on all. Without
   this, Setup/Seq/Arp/Reverb all render at once and overlap. */
var BLOCK_VC = { "Reverb": "v-reverb", "Step Sequencer": "v-seq", "Arpeggiator": "v-arp", "Setup": "v-setup" };
function blockViewClass(b) { return BLOCK_VC[b.name] || VIEWCLASS; }

/* Constant header block, copied VERBATIM from the factory jt8000.rml. These are
   the real functional buttons: transparent hit-areas the plugin's C++ wires by
   class (actionbtn/layerbtn/shiftbtn + data-action/data-layer), the per-view
   lit overlays (layerlit), and the 8 voice dots. The labels themselves are
   painted by the background art underneath - these divs are just the click
   targets and state, so they must be emitted exactly or the tabs do nothing. */
var HEADER_HITAREAS = [
  '\t<!-- Header: transparent hit areas over the painted buttons -->',
  '\t<div class="actionbtn" data-action="sendall" style="left:1580dp; top:66dp; width:254dp; height:74dp;"/>',
  '\t<div class="actionbtn" data-action="loadsyx" style="left:1846dp; top:66dp; width:254dp; height:74dp;"/>',
  '\t<div class="actionbtn" data-action="save" style="left:2112dp; top:66dp; width:254dp; height:74dp;"/>',
  '\t<div class="actionbtn" data-action="load" style="left:2378dp; top:66dp; width:254dp; height:74dp;"/>',
  '\t<div class="actionbtn" data-action="init" style="left:2644dp; top:66dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerlit v-main" style="left:1580dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerbtn" data-layer="main" style="left:1580dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerlit v-reverb" style="left:1846dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerbtn" data-layer="reverb" style="left:1846dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerlit v-seq" style="left:2112dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerbtn" data-layer="seq" style="left:2112dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerlit v-arp" style="left:2378dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerbtn" data-layer="arp" style="left:2378dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerlit v-setup" style="left:2644dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="layerbtn" data-layer="setup" style="left:2644dp; top:154dp; width:254dp; height:74dp;"/>',
  '\t<div class="voice" id="voice0" style="left:1224dp; top:106dp; width:22dp; height:22dp;"/>',
  '\t<div class="voice" id="voice1" style="left:1258dp; top:106dp; width:22dp; height:22dp;"/>',
  '\t<div class="voice" id="voice2" style="left:1292dp; top:106dp; width:22dp; height:22dp;"/>',
  '\t<div class="voice" id="voice3" style="left:1326dp; top:106dp; width:22dp; height:22dp;"/>',
  '\t<div class="voice" id="voice4" style="left:1360dp; top:106dp; width:22dp; height:22dp;"/>',
  '\t<div class="voice" id="voice5" style="left:1394dp; top:106dp; width:22dp; height:22dp;"/>',
  '\t<div class="voice" id="voice6" style="left:1428dp; top:106dp; width:22dp; height:22dp;"/>',
  '\t<div class="voice" id="voice7" style="left:1462dp; top:106dp; width:22dp; height:22dp;"/>',
  '\t<div class="shiftbtn" style="left:2928dp; top:66dp; width:300dp; height:162dp;"/>'
];
var PMAP = window.PARAMMAP || {};
function esc(s) { return String(s == null ? "" : s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"); }
function paramFor(key) { return (PMAP[key] && PMAP[key].param) || (key ? key.replace(/\./g, "_") : ""); }
function defFor(it) { return (PMAP[it.key] && PMAP[it.key].default !== undefined) ? PMAP[it.key].default : "0.0"; }

function exportRml() {
  var unknown = [];
  var skipped = [];
  var lines = [];
  lines.push('<rml>');
  lines.push('<head>');
  lines.push('\t<title>JT-8000</title>');
  lines.push('\t<link type="text/rcss" href="jt8000.rcss"/>');
  lines.push('</head>');
  lines.push('<body class="main">');
  lines.push('');
  lines.push('\t<div class="band"/>');
  lines.push('');

  /* header text overlays (lcd1/lcd2 + any extra) */
  HDR.texts.forEach(function (t) {
    lines.push('\t<div class="' + (t.id === "lcd1" || t.id === "lcd2" ? t.id : "lcd2") + '" id="' + esc(t.id) + '" style="left:' + Math.round(t.x) + 'dp; top:' + Math.round(t.y) + 'dp; width:' + Math.round(t.w) + 'dp; text-align:' + (t.align || "left") + ';">' + esc(t.t) + '</div>');
  });

  /* Emit the functional header hit-areas verbatim (actionbtn / layerbtn /
     shiftbtn + layerlit + voice dots). The plugin wires these by class, so
     without them the view tabs and action buttons do nothing on a custom skin. */
  HEADER_HITAREAS.forEach(function (l) { lines.push(l); });
  lines.push('');
  /* controls + legends, block by block */
  DOC.blocks.forEach(function (b) {
    lines.push('\t<!-- ' + esc(b.name) + ' -->');
    /* section title at the block's top */
    /* Section title uses the block's own view class so layer-block titles only
       show in their view (otherwise Setup/Seq/Arp/Reverb titles pile up). */
    var blockVC = blockViewClass(b);
    lines.push('\t<div class="sectitle ' + blockVC + '" style="left:' + Math.round(b.x) + 'dp; top:' + Math.round(b.y + 5) + 'dp; width:' + Math.round(b.w) + 'dp; text-align:center;">' + esc(b.name.toUpperCase()) + '</div>');
    b.items.forEach(function (it) {
      if (isDisplay(it)) return;   // displays are not interactive controls
      var map = PMAP[it.key];
      var kind = (map && map.kind) ? map.kind : (it.k || "knob");
      /* View class: the factory's per-control value for known keys (so layer
         controls stay on their own page); the block's class for new controls. */
      var vc = (map && map.vc) ? map.vc : blockVC;
      /* Placement comes from the SHARED controlBox so the export matches the
         editor preview exactly. cb gives absolute control + label rects. */
      var cb = controlBox(it);
      var left = cb.cx, top = cb.cy, w = cb.cw, h = cb.ch;

      /* Legend per the same box: above / below / none, aligned per kind. */
      if (it.t && kind !== "stepgrid" && cb.labelPos !== "none") {
        /* Shift facet: the primary legend must HIDE in shift (so it drops
           v-shift), and a red "shiftalt v-shift" legend appears at the same
           spot showing the shift label. This is the factory's exact scheme -
           the control stays put, only the label swaps under SHIFT. */
        var primVC = it.sh ? vc.replace(/\bv-shift\b/, "").replace(/\s+/g, " ").trim() : vc;
        lines.push('\t<div class="legend ' + primVC + '" style="left:' + cb.lx + 'dp; top:' + cb.ly + 'dp; width:' + cb.lw + 'dp; text-align:' + cb.lalign + ';">' + esc(it.t) + '</div>');
        if (it.sh && it.sh.t) {
          lines.push('\t<div class="legend shiftalt v-shift" style="left:' + cb.lx + 'dp; top:' + cb.ly + 'dp; width:' + cb.lw + 'dp; text-align:' + cb.lalign + ';">' + esc(it.sh.t) + '</div>');
        }
      }

      if (!it.key) { skipped.push(b.name + " / " + (it.t || "(unnamed)") + " [no key]"); return; }
      if (!map) unknown.push(b.name + " / " + it.t + "  key=" + it.key + " (no matching param - guessed \"" + paramFor(it.key) + "\", may not bind)");

      var style = 'style="left:' + left + 'dp; top:' + top + 'dp; width:' + w + 'dp; height:' + h + 'dp;"';

      if (kind === "stepgrid") {
        var sg = map || {};
        var sgAttrs = 'class="stepgrid ' + vc + '" param="" key="' + esc(it.key) + '"'
          + ' lanes="' + esc(sg.lanes || it.lanes || "1") + '" steps="' + esc(sg.steps || it.steps || "16") + '"'
          + (sg.stepprefix ? ' stepprefix="' + esc(sg.stepprefix) + '"' : "")
          + (sg.select ? ' select="' + esc(sg.select) + '"' : "")
          + (sg.value ? ' value="' + esc(sg.value) + '"' : "");
        lines.push('\t<stepgrid ' + sgAttrs + ' ' + style + '/>');
        return;
      }

      var attrs = 'class="' + kind + ' ' + vc + '" param="' + esc(paramFor(it.key)) + '" key="' + esc(it.key) + '" default="' + esc(defFor(it)) + '"';
      if (kind === "combo") {
        /* Prefer the factory's full option list (the editor's opts can be a
           truncated copy); fall back to the item's own opts for new controls. */
        var opts = (map && map.options) ? map.options : ((it.opts && it.opts.length) ? it.opts.join(", ") : (optionsForKey(it.key) || []).join(", "));
        attrs += ' options="' + esc(opts) + '" index="0"';
      }

      /* Reproduce any inner markup the factory control carries (e.g. the fader
         thumb) so sliders render their handle. Self-close when there is none. */
      if (map && map.inner) {
        lines.push('\t<' + kind + ' ' + attrs + ' ' + style + '>');
        lines.push('\t\t' + map.inner);
        lines.push('\t</' + kind + '>');
      } else {
        lines.push('\t<' + kind + ' ' + attrs + ' ' + style + '/>');
      }
    });
    lines.push('');
  });

  if (unknown.length || skipped.length) {
    lines.push('\t<!-- EXPORT NOTES -->');
    unknown.forEach(function (u) { lines.push('\t<!--   guessed param: ' + u + ' -->'); });
    skipped.forEach(function (u) { lines.push('\t<!--   skipped (not a bindable control): ' + u + ' -->'); });
  }
  lines.push('</body>');
  lines.push('</rml>');
  dl("jt8000.rml", lines.join("\r\n"), "text/xml");
  var msg = "Exported jt8000.rml.";
  if (unknown.length) msg += " " + unknown.length + " guessed param(s).";
  if (skipped.length) msg += " " + skipped.length + " skipped (see comments).";
  flash(msg);
}

/* Import a saved layout.json. Uses a file picker + FileReader so it works from
   file:// (no fetch/CORS). Validates the shape before replacing, banks an undo
   entry first so a mis-load is one Ctrl+Z away, and preserves the current header
   layer unless the imported file carries its own. */
function importLayoutFile(file) {
  var rd = new FileReader();
  rd.onload = function () {
    var data;
    try { data = JSON.parse(rd.result); }
    catch (e) { alert("Could not parse that file as JSON:\n" + e.message); return; }
    if (!data || !Array.isArray(data.blocks)) { alert("That doesn't look like a layout.json (no blocks array)."); return; }
    pushHistory();                       // so the import can be undone
    DOC = normalize(data);               // auto-flatten any grouped file
    if (!DOC.canvas) DOC.canvas = { w: 3840, h: 2152, margin: 40 };
    if (!DOC.grid) DOC.grid = { colw: 170, row: 46, sub: 4 };
    if (data.header) HEADER = data.header;
    LAYER_NAMES = DOC.blocks.filter(function (b) { return b.layer; }).map(function (b) { return b.name; });
    sel = []; inside = null; previewValues = {};
    render();
    flash("Loaded layout: " + DOC.blocks.length + " blocks.");
  };
  rd.readAsText(file);
}

/* Import a params catalogue. Accepts either a raw params.json ({keys,labels,
   options}) or a params.js that assigns window.PARAMS - we handle both by
   trying JSON first, then a loose extraction of the assigned object. */
function importParamsFile(file) {
  var rd = new FileReader();
  rd.onload = function () {
    var txt = rd.result, obj = null;
    try { obj = JSON.parse(txt); } catch (e) {
      var m = txt.match(/window\.PARAMS\s*=\s*(\{[\s\S]*?\});/);
      if (m) { try { obj = JSON.parse(m[1]); } catch (e2) {} }
    }
    if (!obj || !Array.isArray(obj.keys)) { alert("That doesn't look like a params file (need a keys array)."); return; }
    CAT = { keys: obj.keys, labels: obj.labels || {}, options: obj.options || {} };
    buildDatalist();
    render();
    flash("Loaded catalogue: " + CAT.keys.length + " keys, " + Object.keys(CAT.options).length + " option sets.");
  };
  rd.readAsText(file);
}

/* Transient status message in the footer. */
function flash(msg) { var el = document.getElementById("out"); if (el) { el.textContent = msg; } }

function exportHeader() {
  var rml = "<!-- header text overlays -->\n";
  HDR.texts.forEach(function (t) { rml += '<div class="' + t.id + '" id="' + t.id + '" style="left:' + t.x + 'dp; top:' + t.y + 'dp; width:' + t.w + 'dp; text-align:' + t.align + ';">' + (t.t || "") + "</div>\n"; });
  var rcss = "/* header bands + background */\n";
  var m = { MAIN: "main", REVERB: "reverb", SEQ: "seq", ARP: "arp", SETUP: "setup" };
  Object.keys(HDR.band).forEach(function (v) { rcss += "body." + m[v] + " .band { decorator: image(" + HDR.band[v] + ".png); }\n"; });
  rcss += "body { decorator: image(" + HDR.bg + ".png); }\n";
  dl("header_patch.txt", rml + "\n" + rcss, "text/plain");
}
function buildDatalist() { var dl2 = document.getElementById("paramKeys"); if (!dl2) return; dl2.innerHTML = ""; CAT.keys.forEach(function (k) { var o = document.createElement("option"); o.value = k; if (CAT.labels[k]) o.label = CAT.labels[k]; dl2.appendChild(o); }); }

document.getElementById("zin").onclick = function () { scale = Math.min(1.2, scale * 1.2); render(); };
document.getElementById("zout").onclick = function () { scale = Math.max(0.08, scale / 1.2); render(); };
document.getElementById("fit").onclick = function () { scale = Math.min((wrap.clientWidth - 20) / DOC.canvas.w, (wrap.clientHeight - 20) / DOC.canvas.h); render(); };
document.getElementById("view").onclick = function () { view = VIEWS[(VIEWS.indexOf(view) + 1) % VIEWS.length]; this.textContent = "View: " + view; sel = []; inside = null; render(); };
document.getElementById("shiftToggle").onclick = function () { shiftPreview = !shiftPreview; this.classList.toggle("on", shiftPreview); this.textContent = shiftPreview ? "Shift: ON" : "Shift: OFF"; render(); };
document.getElementById("up").onclick = function () { inside = null; sel = []; render(); };
document.getElementById("undo").onclick = undo;
document.getElementById("redo").onclick = redo;
document.getElementById("expLayout").onclick = exportLayout;
document.getElementById("expRml").onclick = exportRml;
document.getElementById("expHeader").onclick = exportHeader;
/* Import: buttons trigger hidden file inputs; inputs reset value so the same
   file can be chosen twice in a row. */
var impLayoutInput = document.getElementById("impLayoutFile");
var impParamsInput = document.getElementById("impParamsFile");
document.getElementById("impLayout").onclick = function () { impLayoutInput.value = ""; impLayoutInput.click(); };
document.getElementById("impParams").onclick = function () { impParamsInput.value = ""; impParamsInput.click(); };
impLayoutInput.onchange = function () { if (impLayoutInput.files[0]) importLayoutFile(impLayoutInput.files[0]); };
impParamsInput.onchange = function () { if (impParamsInput.files[0]) importParamsFile(impParamsInput.files[0]); };
document.getElementById("reset").onclick = function () { if (!confirm("Discard all changes?")) return; pushHistory(); DOC = normalize(JSON.parse(ORIG)); sel = []; inside = null; render(); };

buildDatalist(); histBtns();
document.getElementById("fit").click();
