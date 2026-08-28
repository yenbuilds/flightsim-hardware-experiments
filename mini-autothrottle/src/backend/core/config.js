'use strict';

function numberFromEnv(name, fallback, options = {}) {
  const raw = process.env[name];
  if (raw == null || raw.trim() === '') return fallback;

  const value = Number(raw);
  const min = options.min ?? Number.NEGATIVE_INFINITY;
  const max = options.max ?? Number.POSITIVE_INFINITY;
  if (!Number.isFinite(value) || value < min || value > max) return fallback;
  return options.integer ? Math.round(value) : value;
}

function booleanFromEnv(name, fallback) {
  const value = process.env[name];
  if (value == null) return fallback;
  return ['1', 'true', 'yes', 'on'].includes(value.trim().toLowerCase());
}

let potMin = numberFromEnv('AT_POT_MIN', 10, { min: 0, max: 1022, integer: true });
const requestedPotMax = numberFromEnv('AT_POT_MAX', 1013, { min: 1, max: 1023, integer: true });
let potMax = requestedPotMax;
if (potMax <= potMin) {
  potMin = 10;
  potMax = 1013;
}

// This intentionally keeps serial output disabled unless AT_ENABLE is set.
module.exports = {
  autothrottle: {
    enable: booleanFromEnv('AT_ENABLE', false),
    profile: process.env.THROTTLE_PROFILE || 'boeing-737',
    debug: booleanFromEnv('AT_DEBUG', false),
    debugSpam: booleanFromEnv('AT_DEBUG_SPAM', false),
    debugIntervalMs: numberFromEnv('AT_DEBUG_INTERVAL_MS', 1000, { min: 0 }),
    teensyPort: process.env.AT_TEENSY_PORT || '',
    baud: numberFromEnv('AT_BAUD', 115200, { min: 1, integer: true }),
    sendIntervalMs: numberFromEnv('AT_SEND_INTERVAL_MS', 80, { min: 0 }),
    minDelta: numberFromEnv('AT_MIN_DELTA', 2, { min: 0 }),
    rawMax: numberFromEnv('AT_RAW_MAX', 16383, { min: 1 }),
    potMin,
    potMax,
    detentFrac: numberFromEnv('AT_DETENT_FRAC', 0.45, { min: 0, max: 1 }),
  },
};
