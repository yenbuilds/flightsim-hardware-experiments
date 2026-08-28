// ------------------------------------------------------------
// throttle.js
// Profile-aware throttle conversion + mapping for autothrottle integration.
// ------------------------------------------------------------
// Responsibilities:
//  - Load and manage aircraft-specific throttle profiles
//  - Provide snapshot decoding (raw -> percent 0..100)
//  - Delegate percent -> physical mapping to the active profile
//  - Mock support (synthetic ramp) so UI can show data in --mock mode
//
// PROFILES: Aircraft-specific behavior lives in throttle-profiles-json/*.json
//       Set THROTTLE_PROFILE env var to select (e.g., 'boeing-737').
// ------------------------------------------------------------

const config = require('../core/config');
const profileManager = require('./throttle-profile-loader');
const timeSource = require('../core/time-source');

// ---------- State ----------
const DEBUG = config.autothrottle.debug;
const DEBUG_INTERVAL_MS = config.autothrottle.debugIntervalMs;
let lastDebugMs = 0;
const DEBUG_SPAM = config.autothrottle.debugSpam;

// ---------- Profile-aware helpers ----------

/**
 * Get the active throttle profile.
 * @returns {object} Profile module
 */
function getProfile() {
  return profileManager.getActiveProfile();
}

/**
 * Convert raw throttle input to percent using the active profile.
 * Falls back to config-based conversion if profile doesn't provide one.
 *
 * @param {number} raw - Raw throttle value
 * @returns {number|null} - Percent 0-100 or null if invalid
 */
function rawToPercent(raw) {
  const profile = getProfile();
  if (profile && typeof profile.rawToPercent === 'function') {
    return profile.rawToPercent(raw);
  }
  // Fallback to config-based conversion
  const v = Number(raw);
  const rawMax = config.autothrottle.rawMax;
  if (!Number.isFinite(v) || !Number.isFinite(rawMax) || rawMax <= 0) return null;
  return Math.max(0, Math.min(100, (v / rawMax) * 100));
}

/**
 * Map throttle percent to physical potentiometer counts using the active profile.
 *
 * @param {number} percent - Throttle percent 0-100
 * @returns {number|null} - Physical counts or null
 */
function mapPercentToCounts(percent) {
  if (percent == null) return null;

  const profile = getProfile();
  if (profile && typeof profile.mapSimToPhysical === 'function') {
    const result = profile.mapSimToPhysical(percent);
    return result ? result.counts : null;
  }

  // Fallback to config-based mapping
  const pct = Math.max(0, Math.min(100, percent));
  const potMin = config.autothrottle.potMin;
  const potMax = config.autothrottle.potMax;
  const detentFrac = config.autothrottle.detentFrac;
  const detent = potMin + Math.round((potMax - potMin) * detentFrac);
  const span = potMax - detent;
  if (!Number.isFinite(span) || span <= 0) return null;
  return Math.round(detent + (pct / 100) * span);
}

/**
 * Get full mapping result including detent info (profile-aware).
 *
 * @param {number} percent - Throttle percent 0-100
 * @returns {object} - { counts, detent, detentCounts, gateActive, label, ... }
 */
function mapPercentToPhysical(percent) {
  if (percent == null) return null;

  const profile = getProfile();
  if (profile && typeof profile.mapSimToPhysical === 'function') {
    return profile.mapSimToPhysical(percent);
  }

  // Fallback
  return {
    counts: mapPercentToCounts(percent),
    detent: null,
    gateActive: false,
  };
}

/**
 * Format command string for Teensy using active profile.
 *
 * @param {number} counts - Target counts
 * @param {string|null} detent - Detent ID
 * @param {object} [extra] - Extra info (gateActive, detentCounts, etc.)
 * @returns {string} - Command string (without newline)
 */
function formatCommand(counts, detent = null, extra = {}) {
  const profile = getProfile();
  if (profile && typeof profile.formatCommand === 'function') {
    // Profile's formatCommand expects a single result object
    const result = { counts, detent, ...extra };
    return profile.formatCommand(result);
  }
  // Legacy format
  return `T${counts}`;
}

// ---------- Synthetic throttle for mock mode (simple triangle wave 0..100) ----------
function syntheticPercent(timeMs) {
  const period = 8000; // 8s full cycle
  const t = timeMs % period;
  const half = period / 2;
  return t < half ? (t / half) * 100 : (1 - (t - half) / half) * 100;
}

// ---------- Snapshot ----------
/**
 * Provide a snapshot with normalized throttle values.
 * Provider layers should provide normalized throttle inputs
 * in processData.thr1..thr4 before calling this function.
 *
 * @param {object} processData - unified throttle data (contains thr1/thr2/thr3/thr4)
 * @param {boolean} isMock     - if true & no data present, use synthetic values
 * @returns {object|null} - Snapshot with eng*Pct, avgPct, counts, profile info
 */
function computeThrottleSnapshot(processData, isMock) {
  const profile = getProfile();
  const profileId = profile ? profile.meta.id : 'default';

  const raw = [
    processData && processData.thr1,
    processData && processData.thr2,
    processData && processData.thr3,
    processData && processData.thr4,
  ];

  // Mock fallback when we have no usable raw throttle.
  if (raw[0] == null && isMock) {
    const now = timeSource.now();
    const pct = syntheticPercent(now);
    const physical = mapPercentToPhysical(pct);

    const snap = {
      eng1Pct: pct,
      eng2Pct: pct,
      eng3Pct: null,
      eng4Pct: null,
      avgPct: pct,
      counts: physical ? physical.counts : null,
      detent: physical ? physical.detent : null,
      gateActive: physical ? physical.gateActive : false,
      profileId,
      servoEnabled: profile ? profile.behavior.servoEnabled : true,
    };

    if (DEBUG) {
      if (DEBUG_SPAM || now - lastDebugMs > DEBUG_INTERVAL_MS) {
        console.log(
          `[throttle][mock][${profileId}] avgPct=${snap.avgPct.toFixed(1)} counts=${snap.counts} detent=${snap.detent || 'none'}`
        );
        lastDebugMs = now;
      }
    }

    return snap;
  }

  // No usable data.
  if (raw[0] == null) return null;

  const pct = raw.map(rawToPercent);

  const eng1Pct = pct[0];
  const eng2Pct = pct[1];
  const eng3Pct = pct[2];
  const eng4Pct = pct[3];

  if (eng1Pct == null) return null;

  // Preserve legacy serial behavior: use avg of engines 1/2 when both present.
  // Otherwise, average whatever engines we have.
  let avgPct;
  if (eng1Pct != null && eng2Pct != null) {
    avgPct = (eng1Pct + eng2Pct) / 2;
  } else {
    const present = [eng1Pct, eng2Pct, eng3Pct, eng4Pct].filter((v) => typeof v === 'number');
    avgPct = present.length ? present.reduce((a, b) => a + b, 0) / present.length : eng1Pct;
  }

  const physical = mapPercentToPhysical(avgPct);

  const snap = {
    eng1Pct,
    eng2Pct,
    eng3Pct,
    eng4Pct,
    avgPct,
    counts: physical ? physical.counts : null,
    detent: physical ? physical.detent : null,
    detentLabel: physical ? physical.label : null,
    detentCounts: physical ? physical.detentCounts : null,  // For manual-detent profiles
    gateActive: physical ? physical.gateActive : false,
    profileId,
    servoEnabled: profile ? profile.behavior.servoEnabled : true,
  };

  if (DEBUG) {
    const now = timeSource.now();
    if (DEBUG_SPAM || now - lastDebugMs > DEBUG_INTERVAL_MS) {
      const fmtPct = (v) => (typeof v === 'number' ? v.toFixed(1) : 'NA');
      console.log(
        `[throttle][${profileId}] pct1=${fmtPct(eng1Pct)} pct2=${fmtPct(eng2Pct)} avgPct=${avgPct.toFixed(1)} counts=${snap.counts} detent=${snap.detent || 'none'} servo=${snap.servoEnabled}`
      );
      lastDebugMs = now;
    }
  }

  return snap;
}

// ---------- Exports ----------
module.exports = {
  // Core functions
  computeThrottleSnapshot,

  // Profile-aware mapping
  mapPercentToCounts,
  mapPercentToPhysical,
  rawToPercent,
  formatCommand,
  syntheticPercent,

  // Profile management
  getProfile,
  getActiveProfileId: profileManager.getActiveProfileId,
  switchProfile: profileManager.switchProfile,
  listProfiles: profileManager.listProfiles,
};
