'use strict';

// Converts 0-100 gear-position percentages into a compact Nose/Left/Right
// command for the Teensy: U = up/off, T = transit/red, D = down/green.

const LOW_THRESHOLD = 0.10;
const HIGH_THRESHOLD = 0.90;

let lastStates = { N: 'U', L: 'U', R: 'U' };

function toFraction(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return 0;
  return Math.max(0, Math.min(1, numeric / 100));
}

function decideState(tag, fraction) {
  const last = lastStates[tag];

  if (last === 'U') {
    return fraction > LOW_THRESHOLD ? 'T' : 'U';
  }

  if (last === 'D') {
    return fraction < HIGH_THRESHOLD ? 'T' : 'D';
  }

  if (fraction <= LOW_THRESHOLD) return 'U';
  if (fraction >= HIGH_THRESHOLD) return 'D';
  return 'T';
}

/**
 * @param {{gearNose?:number, gearLeft?:number, gearRight?:number}} frame
 * @returns {string} Three characters in Nose/Left/Right order, such as UTD.
 */
function makeGearLedCommand(frame = {}) {
  const fractions = {
    N: toFraction(frame.gearNose),
    L: toFraction(frame.gearLeft),
    R: toFraction(frame.gearRight),
  };

  lastStates = {
    N: decideState('N', fractions.N),
    L: decideState('L', fractions.L),
    R: decideState('R', fractions.R),
  };

  return `${lastStates.N}${lastStates.L}${lastStates.R}`;
}

function resetGearLedState() {
  lastStates = { N: 'U', L: 'U', R: 'U' };
}

module.exports = {
  makeGearLedCommand,
  resetGearLedState,
};
