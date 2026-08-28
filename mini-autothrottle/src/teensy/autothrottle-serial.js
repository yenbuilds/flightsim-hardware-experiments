// ------------------------------------------------------------
// autothrottle-serial.js
// Optional Teensy serial gateway for throttle counts.
// Enabled by env AT_TEENSY_PORT (e.g., COM10) and AT_BAUD (default 115200).
//
// Profile-aware serial gateway for servo-driven Boeing-style T#### commands.
// ------------------------------------------------------------

const config = require('../backend/core/config');
const throttleProfileLoader = require('../backend/aircraft/throttle-profile-loader');
const timeSource = require('../backend/core/time-source');

const SerialPort = (() => {
  try { return require('serialport').SerialPort; } catch (e) { return null; }
})();

let port = null;
let lastSentCounts = null;
let lastSentMs = 0;
const RATE_LIMIT_MS = config.autothrottle.sendIntervalMs;
const MIN_DELTA = config.autothrottle.minDelta;
const DEBUG = config.autothrottle.debug;

// Cache last detent layout command to avoid redundant writes
let lastLayoutCmd = '';
let lastRangeCmd = '';
let lastCmd = '';

// Cache last gear LED command to avoid redundant writes
let lastGearCmd = '';

// Track if we've already logged the "disabled" message to avoid spam
let loggedDisabled = false;

function resetSendState() {
  lastSentCounts = null;
  lastSentMs = 0;
  lastLayoutCmd = '';
  lastRangeCmd = '';
  lastGearCmd = '';
  lastCmd = '';
}

function isValidTarget(counts) {
  return Number.isFinite(counts) && counts >= 0 && counts <= 1023;
}

function initTeensySerial() {
  if (port) return true;

  // Check master enable flag first (disabled by default in packaged mode)
  if (!config.autothrottle.enable) {
    if (!loggedDisabled && DEBUG) {
      console.log('[at-serial] Autothrottle disabled (AT_ENABLE=0 or packaged mode)');
      loggedDisabled = true;
    }
    return false;
  }

  const path = config.autothrottle.teensyPort;
  if (!path) {
    if (DEBUG) console.log('[at-serial] AT_TEENSY_PORT not set; serial disabled');
    return false;
  }
  if (!SerialPort) {
    if (DEBUG) console.log('[at-serial] serialport package not available');
    return false;
  }
  const baud = config.autothrottle.baud;
  try {
    resetSendState();
    const openedPort = new SerialPort({ path, baudRate: baud });
    port = openedPort;
    openedPort.on('open', () => {
      if (DEBUG) console.log(`[at-serial] Opened ${path} @${baud}`);
    });
    // Suppress noisy errors - only log if debug mode is on
    openedPort.on('error', err => {
      if (DEBUG) console.log('[at-serial] Error:', err.message);
    });
    openedPort.on('close', () => {
      if (port === openedPort) {
        port = null;
        resetSendState();
      }
    });
    return true;
  } catch (e) {
    // Suppress noisy "File not found" etc. errors - only log in debug mode
    if (DEBUG) console.log('[at-serial] Failed to open serial:', e.message);
    port = null;
    return false;
  }
}

/**
 * Send detent layout (pot min/max + gate counts) to Teensy once per profile.
 * Command format: D,<potMin>,<potMax>,<gate1>,<gate2>,...
 * No newline management here; caller should not add newline.
 */
function sendDetentLayout(profileOverride = null) {
  if (!port) return false;
  const layout = throttleProfileLoader.getDetentLayout(profileOverride);
  if (!layout || !Array.isArray(layout.gates) || layout.gates.length === 0) return false;
  const gates = layout.gates.map(g => g.counts).join(',');
  const cmd = `D,${layout.potMin},${layout.potMax},${gates}\n`;
  if (cmd === lastLayoutCmd) return true;
  try {
    port.write(cmd);
    lastLayoutCmd = cmd;
    if (DEBUG) console.log(`[at-serial] -> ${cmd.trim()}`);
    return true;
  } catch (e) {
    console.warn('[at-serial] detent layout write failed:', e.message);
    return false;
  }
}

function sendRangeMode(profileOverride = null) {
  if (!port) return false;
  const layout = throttleProfileLoader.getDetentLayout(profileOverride);
  const requestedMode = layout?.travelMode;
  const mode = ['half', 'full', 'bottom'].includes(requestedMode) ? requestedMode : 'half';
  const cmd = `R,${mode}\n`;
  if (cmd === lastRangeCmd) return true;
  try {
    port.write(cmd);
    lastRangeCmd = cmd;
    if (DEBUG) console.log(`[at-serial] -> ${cmd.trim()}`);
    return true;
  } catch (e) {
    console.warn('[at-serial] range mode write failed:', e.message);
    return false;
  }
}

/**
 * Send throttle command to Teensy.
 *
 * Servo-driven profiles send T#### target commands. Manual-detent profiles have
 * their gate layout sent at startup and deliberately receive no target command.
 *
 * @param {object} throttleSnap - Throttle snapshot from throttle.js
 *   { counts, detent, detentLabel, gateActive, servoEnabled, ... }
 */
function sendThrottleCommand(throttleSnap) {
  if (!port || !throttleSnap) return false;

  const { counts, servoEnabled } = throttleSnap;

  // Manual-detent profiles do not receive movement commands.
  if (!servoEnabled || !isValidTarget(counts)) return false;

  const now = timeSource.now();

  if (typeof counts === 'number' && lastSentCounts !== null) {
    const delta = Math.abs(counts - lastSentCounts);
    if (delta < MIN_DELTA && (now - lastSentMs) < (RATE_LIMIT_MS * 2)) {
      return false; // too small change too soon
    }
    if ((now - lastSentMs) < RATE_LIMIT_MS) {
      return false;
    }
  }

  // The supplied firmware accepts only T#### movement targets.
  const cmd = `T${String(Math.round(counts)).padStart(4, '0')}\n`;

  // Avoid redundant writes
  if (cmd === lastCmd && (now - lastSentMs) < (RATE_LIMIT_MS * 4)) {
    return false;
  }

  try {
    port.write(cmd);
    lastSentCounts = counts;
    lastSentMs = now;
    lastCmd = cmd;
    if (DEBUG) console.log(`[at-serial] -> ${cmd.trim()}`);
    return true;
  } catch (e) {
    console.warn('[at-serial] write failed:', e.message);
    return false;
  }
}

/**
 * Legacy function for backward compatibility.
 * Prefer sendThrottleCommand(throttleSnap) for profile-aware behavior.
 *
 * @param {number} counts - Target servo counts
 * @deprecated Use sendThrottleCommand instead
 */
function sendThrottleCounts(counts) {
  if (!port || !isValidTarget(counts)) return false;
  const now = timeSource.now();
  if (lastSentCounts !== null) {
    const delta = Math.abs(counts - lastSentCounts);
    if (delta < MIN_DELTA && (now - lastSentMs) < (RATE_LIMIT_MS * 2)) {
      return false; // too small change too soon
    }
    if ((now - lastSentMs) < RATE_LIMIT_MS) {
      return false; // rate limited
    }
  }
  const cmd = `T${String(Math.round(counts)).padStart(4, '0')}\n`;
  try {
    port.write(cmd);
    lastSentCounts = counts;
    lastSentMs = now;
    lastCmd = cmd;
    if (DEBUG) console.log(`[at-serial] -> ${cmd.trim()}`);
    return true;
  } catch (e) {
    console.warn('[at-serial] write failed:', e.message);
    return false;
  }
}

// Send gear LED states. Expects a 3-character string states in order: Nose, Left, Right.
// Each char should be one of: 'U' (up/off), 'T' (transit/red), 'D' (down/green)
function sendGearLeds(states) {
  if (!port) return false;
  if (typeof states !== 'string' || !/^[UTD]{3}$/.test(states)) return false;
  // De-duplicate
  if (states === lastGearCmd) return false;
  const cmd = `G${states}\n`;
  try {
    port.write(cmd);
    lastGearCmd = states;
    if (DEBUG) console.log(`[at-serial] -> ${cmd.trim()}`);
    return true;
  } catch (e) {
    console.warn('[at-serial] gear write failed:', e.message);
    return false;
  }
}

module.exports = {
  initTeensySerial,
  sendThrottleCommand,
  sendThrottleCounts, // legacy, prefer sendThrottleCommand
  sendGearLeds,
  sendDetentLayout,
  sendRangeMode,
};
