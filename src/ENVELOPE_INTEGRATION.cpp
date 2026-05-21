// ===========================================================================
// ENVELOPE WIDGET INTEGRATION GUIDE
//
// This file documents the targeted edits needed in existing files.
// Apply each change to your deployed source.  No full-file replacements.
// ===========================================================================


// ───────────────────────────────────────────────────────────────────────────
// 1. JT8000_Sections.h — add ENVELOPE to CtrlType, update section defs
// ───────────────────────────────────────────────────────────────────────────

// In the CtrlType enum (around line 64), add ENVELOPE before NONE:
//
//   GRID    = 3,
//   ENVELOPE = 4,   // ← ADD THIS
//   NONE    = 255

// Add shorthand macro (around line 117, after the G() macro):
//
//   #define E(cc, lbl)   { cc, CtrlType::ENVELOPE, lbl }

// Replace Amp Envelope section (lines ~254-274):
//
// { "Amp Envelope", {
//     { "", { E(CC::AMP_ATTACK, "AMP"),
//             EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY }, 1 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
// }, 1 },

// Replace Filter Envelope section (lines ~279-298):
//
// { "Filter Envelope", {
//     { "", { E(CC::FILTER_ENV_ATTACK, "FILTER"),
//             EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY }, 1 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
// }, 1 },

// Replace Pitch Envelope section (lines ~303-324):
//
// { "Pitch Envelope", {
//     { "", { E(CC::PITCH_ENV_ATTACK, "PITCH"),
//             EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY }, 1 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 },
//     { "", {EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY}, 0 }
// }, 1 },


// ───────────────────────────────────────────────────────────────────────────
// 2. HomeScreen.h — add envelope cursor state
// ───────────────────────────────────────────────────────────────────────────

// Add include at top:
//   #include "MiniEnvelope.h"

// In the private section (after _pendingCount), add:
//
//   int8_t  _envCursor;     // sub-point cursor within an envelope widget (0..7)
//   EnvFlavour _envFlavour; // which envelope the cursor is currently on


// ───────────────────────────────────────────────────────────────────────────
// 3. HomeScreen.cpp — add ENVELOPE cases throughout
// ───────────────────────────────────────────────────────────────────────────

// === In _calcExpandedBodyHeight() — add case in the rowH switch: ===
//
//   case CtrlType::ENVELOPE: rowH = max(rowH, (int16_t)MiniEnvLayout::ENV_CELL_H); break;
//
// (There are 3 copies of this switch — the one in _calcExpandedBodyHeight,
//  and two in _calcControlLayoutPositions/_drawSectionBody. Add to all three.)

// === In _drawSectionBody() — width advance switch: ===
//
//   case CtrlType::ENVELOPE: curX += SW - 2 * BODY_PAD_X; break;
//
// (Same as GRID — full width.)

// === In _drawControl() — add new case: ===
//
//   case CtrlType::ENVELOPE: {
//       // Determine flavour from the sentinel CC
//       EnvFlavour flav = EnvFlavour::AMP;
//       if (ctrl.cc == CC::FILTER_ENV_ATTACK) flav = EnvFlavour::FILTER;
//       if (ctrl.cc == CC::PITCH_ENV_ATTACK)  flav = EnvFlavour::PITCH;
//
//       // Selected point: only if cursor is on this control
//       int8_t selPt = -1;
//       if (selected) {
//           selPt = MiniEnvelope::cursorToPoint(flav, _envCursor);
//       }
//
//       // Wrap _getCC into a static-compatible lambda via _instance
//       auto getCCfn = [](uint8_t cc) -> uint8_t {
//           return _instance ? _instance->_getCC(cc) : 0;
//       };
//
//       MiniEnvelope::draw(*_display, screenX, screenY,
//                          SW - 2 * BODY_PAD_X,
//                          flav, getCCfn, selPt);
//       break;
//   }

// === In _adjustValue() — add ENVELOPE case before the default knob/select: ===
//
//   } else if (ctrl.type == CtrlType::ENVELOPE) {
//       // Determine flavour
//       EnvFlavour flav = EnvFlavour::AMP;
//       if (ctrl.cc == CC::FILTER_ENV_ATTACK) flav = EnvFlavour::FILTER;
//       if (ctrl.cc == CC::PITCH_ENV_ATTACK)  flav = EnvFlavour::PITCH;
//
//       // Get the CC for the currently selected sub-point
//       int8_t globalPt = MiniEnvelope::cursorToPoint(flav, _envCursor);
//       uint8_t ptCC = MiniEnvelope::pointToCC(flav, globalPt);
//       if (ptCC != 255) {
//           int16_t newVal = (int16_t)_getCC(ptCC) + delta;
//           _setCC(ptCC, (uint8_t)constrain(newVal, 0, 127));
//       }

// === In onEncoderLeftPress() — handle press on envelope point: ===
//
// When the cursor is on an ENVELOPE control and the user presses the left
// encoder, currently it calls _openEntry().  For envelopes we want to
// cycle the sub-cursor instead of opening numeric entry:
//
//   if (_cursor.controlIdx >= 0) {
//       const ControlDef& ctrl = _controlDef(_cursor.sectionIdx, _cursor.controlIdx);
//       if (ctrl.type == CtrlType::ENVELOPE) {
//           // Determine flavour
//           EnvFlavour flav = EnvFlavour::AMP;
//           if (ctrl.cc == CC::FILTER_ENV_ATTACK) flav = EnvFlavour::FILTER;
//           if (ctrl.cc == CC::PITCH_ENV_ATTACK)  flav = EnvFlavour::PITCH;
//
//           // Cycle to next sub-point
//           _envCursor = (_envCursor + 1) % MiniEnvelope::pointCount(flav);
//           _markControlDirty(_cursor.controlIdx);
//           return;
//       }
//       _openEntry(_cursor.sectionIdx, _cursor.controlIdx);
//   }

// === In onEncoderLeft() — initialise _envCursor when landing on envelope: ===
//
// When _cursor.controlIdx changes, check if the new control is an ENVELOPE.
// If so, reset _envCursor to 0:
//
//   // After: _cursor.controlIdx = newCtrl;
//   if (newCtrl >= 0) {
//       const ControlDef& c = _controlDef(_expandedSection, newCtrl);
//       if (c.type == CtrlType::ENVELOPE) {
//           _envCursor = 0;
//           _envFlavour = EnvFlavour::AMP;
//           if (c.cc == CC::FILTER_ENV_ATTACK) _envFlavour = EnvFlavour::FILTER;
//           if (c.cc == CC::PITCH_ENV_ATTACK)  _envFlavour = EnvFlavour::PITCH;
//       }
//   }

// === In notifyCC() — mark envelope dirty when any of its CCs change: ===
//
// The envelope widget responds to multiple CCs, but occupies one ControlDef slot.
// When a CC arrives that belongs to any envelope, find that section's envelope
// control and mark it dirty.  The simplest approach: in the existing loop that
// checks cc == ctrl.cc, also check against all envelope CC mappings.
// OR: just mark all controls dirty for the expanded section on any CC in the
// envelope CC range (147-155 for curves, plus the ADSR CCs). This is a minor
// over-invalidation but costs nothing since envelope redraw is ~300µs.


// ───────────────────────────────────────────────────────────────────────────
// 4. Constructor initialisation
// ───────────────────────────────────────────────────────────────────────────
//
// In HomeScreen::HomeScreen(), add:
//   _envCursor = 0;
//   _envFlavour = EnvFlavour::AMP;
