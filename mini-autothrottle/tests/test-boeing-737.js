'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const loader = require('../src/backend/aircraft/throttle-profile-loader');
const throttle = require('../src/backend/aircraft/throttle');
const serial = require('../src/teensy/autothrottle-serial');
const gearLeds = require('../src/teensy/gear-led');

let passed = 0;

function test(name, fn) {
  fn();
  passed += 1;
  console.log(`PASS ${name}`);
}

loader.clearCache();

test('detects the PMDG 737 profile', () => {
  const profile = loader.detectFromAircraft('pmdg-737');
  assert.strictEqual(profile.id, 'boeing-737');
  assert.strictEqual(profile.behavior.servoEnabled, true);
  assert.strictEqual(profile.behavior.travelMode, 'half');
});

test('resolves and switches through the 737 alias', () => {
  assert.strictEqual(loader.resolveAlias('737'), 'boeing-737');
  assert.strictEqual(loader.switchProfile('737').id, 'boeing-737');
});

test('maps the Boeing active floor and TOGA to calibrated counts', () => {
  const profile = loader.loadProfile('boeing-737');
  const idle = profile.mapSimToPhysical(0);
  const toga = profile.mapSimToPhysical(100);

  assert.strictEqual(idle.counts, 461);
  assert.strictEqual(toga.counts, 1013);
  assert.strictEqual(profile.formatCommand(idle), 'T0461');
  assert.strictEqual(profile.formatCommand(toga), 'T1013');
});

test('builds a matching firmware calibration layout', () => {
  const profile = loader.loadProfile('boeing-737');
  const layout = loader.getDetentLayout(profile);

  assert.deepStrictEqual(layout, {
    potMin: 10,
    potMax: 1013,
    travelMode: 'half',
    gates: [{ id: 'active_min', counts: 461, label: 'A/T MIN' }],
  });
});

test('creates a usable two-engine throttle snapshot', () => {
  const snapshot = throttle.computeThrottleSnapshot({ thr1: 0, thr2: 16383 }, false);

  assert.strictEqual(snapshot.profileId, 'boeing-737');
  assert.strictEqual(snapshot.servoEnabled, true);
  assert.strictEqual(snapshot.avgPct, 50);
  assert.strictEqual(snapshot.counts, 737);
  assert.strictEqual(throttle.formatCommand(snapshot.counts), 'T0737');
});

test('rejects invalid mapping and command inputs', () => {
  const profile = loader.loadProfile('boeing-737');
  assert.strictEqual(profile.mapSimToPhysical('not-a-number'), null);
  assert.strictEqual(profile.formatCommand({ counts: Number.NaN }), null);
  assert.strictEqual(profile.formatCommand({ counts: 1024 }), null);
  assert.strictEqual(throttle.computeThrottleSnapshot({ thr1: 'bad' }, false), null);
});

test('keeps the firmware and profile on the same active floor', () => {
  const firmware = fs.readFileSync(
    path.join(__dirname, '../src/teensy/autothrottle/autothrottle.ino'),
    'utf8',
  );

  assert.match(firmware, /const float detentFrac\s*=\s*0\.45f/);
  assert.match(firmware, /travelMode\s*=\s*TRAVEL_HALF/);
  assert.match(firmware, /nearestIdx\s*==\s*-1/);
  assert.match(firmware, /strtol\(buf \+ 1/);
  assert.match(firmware, /#define DEBUG_PID\s+false/);
});

test('does not expose the unverified Airbus profiles', () => {
  assert.strictEqual(loader.loadProfile('airbus-a320'), null);
  assert.strictEqual(loader.resolveAlias('a320'), 'a320');
  assert.ok(!loader.listProfiles().some((profile) => profile.id.startsWith('airbus-')));
});

test('keeps gear LED hysteresis stable at its exact thresholds', () => {
  gearLeds.resetGearLedState();
  assert.strictEqual(gearLeds.makeGearLedCommand({ gearNose: 10, gearLeft: 10, gearRight: 10 }), 'UUU');
  assert.strictEqual(gearLeds.makeGearLedCommand({ gearNose: 11, gearLeft: 11, gearRight: 11 }), 'TTT');
  assert.strictEqual(gearLeds.makeGearLedCommand({ gearNose: 90, gearLeft: 90, gearRight: 90 }), 'DDD');
  assert.strictEqual(gearLeds.makeGearLedCommand({ gearNose: 90, gearLeft: 90, gearRight: 90 }), 'DDD');
  assert.strictEqual(gearLeds.makeGearLedCommand({ gearNose: 89, gearLeft: 89, gearRight: 89 }), 'TTT');
});

test('keeps serial output inert until explicitly enabled', () => {
  assert.strictEqual(serial.initTeensySerial(), false);
  assert.strictEqual(serial.sendThrottleCommand({ counts: 737, servoEnabled: true }), false);
  assert.strictEqual(serial.sendThrottleCounts(Number.NaN), false);
  assert.strictEqual(serial.sendGearLeds('BAD'), false);
});

test('parses every active profile JSON file', () => {
  const profileDir = path.join(__dirname, '../src/backend/aircraft/throttle-profiles');
  for (const name of fs.readdirSync(profileDir).filter(file => file.endsWith('.json'))) {
    assert.doesNotThrow(() => JSON.parse(fs.readFileSync(path.join(profileDir, name), 'utf8')), name);
  }
});

console.log(`\n${passed} autothrottle checks passed.`);
