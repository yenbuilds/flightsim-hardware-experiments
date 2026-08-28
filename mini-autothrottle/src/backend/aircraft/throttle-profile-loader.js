// throttle-profile-loader.js
// Loads, resolves, and manages throttle configuration profiles.
//
// Parallel to aircraft-profile-loader.js for throttle/autothrottle configs.
// Supports the same features: inheritance, namespace, auto-detection.
//
// Features:
// - Load profiles from built-in directory and user directory
// - Resolve inheritance (extends field)
// - Auto-detect profile from aircraft profile ID
// - Namespace support (official, local, community)
// - Computed functions derived from JSON config
//
// Usage:
//   const loader = require('./throttle-profile-loader');
//   const profile = loader.getActiveProfile();           // Cached singleton
//   const profile = loader.loadProfile('boeing-737');    // By ID
//   const profile = loader.detectFromAircraft('pmdg-737'); // From aircraft profile
//   const profiles = loader.listProfiles();              // All available
//
// Environment:
//   THROTTLE_PROFILE - Override profile ID (e.g., 'boeing-737', 'local/my-throttle')

const fs = require('fs');
const path = require('path');
const os = require('os');
const Debug = require('../core/debug');
const config = require('../core/config');

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

const BUILTIN_DIR = path.join(__dirname, 'throttle-profiles');
const USER_DIR = path.join(os.homedir(), '.msfs-telemetry', 'throttle-profiles');
const GENERIC_ID = 'generic';
const DISABLED_PROFILE_IDS = new Set(['airbus-base', 'airbus-a320']);

// Cache for loaded profiles
const profileCache = new Map();
let activeProfile = null;
let activeProfileId = null;

// -----------------------------------------------------------------------------
// Profile Loading (same pattern as aircraft-profile-loader)
// -----------------------------------------------------------------------------

/**
 * Read and parse a JSON profile file.
 * @param {string} filePath - Absolute path to profile JSON
 * @returns {object|null} - Parsed profile or null on error
 */
function readProfileFile(filePath) {
  try {
    const content = fs.readFileSync(filePath, 'utf8');
    return JSON.parse(content);
  } catch (err) {
    Debug.log('throttle-loader', `Failed to read profile: ${filePath}`, { error: err.message });
    return null;
  }
}

/**
 * Resolve a profile ID to its file path.
 * @param {string} id - Profile ID or namespaced ID
 * @returns {{filePath: string, namespace: string, id: string}|null}
 */
function resolveProfilePath(id) {
  if (!id || typeof id !== 'string') return null;

  const normalized = id.toLowerCase().trim();

  // Parse namespace/id format
  let namespace = null;
  let profileId = normalized;

  if (normalized.includes('/')) {
    const parts = normalized.split('/');
    if (parts.length === 2 && ['official', 'local', 'community'].includes(parts[0])) {
      namespace = parts[0];
      profileId = parts[1];
    }
  }

  if (DISABLED_PROFILE_IDS.has(profileId)) {
    Debug.log('throttle-loader', `Profile is disabled: ${profileId}`);
    return null;
  }

  // Search order depends on namespace
  const searchPaths = [];

  if (namespace === 'official') {
    searchPaths.push({ dir: BUILTIN_DIR, ns: 'official' });
  } else if (namespace === 'local') {
    searchPaths.push({ dir: USER_DIR, ns: 'local' });
  } else if (namespace === 'community') {
    Debug.log('throttle-loader', 'Community profiles not yet implemented');
    return null;
  } else {
    // No namespace: search user first, then built-in
    searchPaths.push({ dir: USER_DIR, ns: 'local' });
    searchPaths.push({ dir: BUILTIN_DIR, ns: 'official' });
  }

  for (const { dir, ns } of searchPaths) {
    const filePath = path.join(dir, `${profileId}.json`);
    if (fs.existsSync(filePath)) {
      return { filePath, namespace: ns, id: profileId };
    }
  }

  return null;
}

/**
 * Deep merge two objects, with source overriding target.
 */
function deepMerge(target, source) {
  const result = { ...target };

  for (const key of Object.keys(source)) {
    if (source[key] === null || source[key] === undefined) {
      continue;
    }

    if (
      typeof source[key] === 'object' &&
      !Array.isArray(source[key]) &&
      source[key] !== null &&
      typeof target[key] === 'object' &&
      !Array.isArray(target[key]) &&
      target[key] !== null
    ) {
      result[key] = deepMerge(target[key], source[key]);
    } else {
      result[key] = source[key];
    }
  }

  return result;
}

/**
 * Resolve profile inheritance chain.
 */
function resolveInheritance(profile, visited = new Set()) {
  if (!profile.extends) {
    return profile;
  }

  const parentId = profile.extends;

  if (visited.has(parentId)) {
    Debug.log('throttle-loader', `Inheritance cycle detected: ${parentId}`);
    return profile;
  }
  visited.add(profile.id);

  const parentResolved = resolveProfilePath(parentId);
  if (!parentResolved) {
    Debug.log('throttle-loader', `Parent profile not found: ${parentId}`);
    return profile;
  }

  const parentProfile = readProfileFile(parentResolved.filePath);
  if (!parentProfile) {
    return profile;
  }

  const resolvedParent = resolveInheritance(parentProfile, visited);
  const merged = deepMerge(resolvedParent, profile);

  // Preserve child's identity
  merged.id = profile.id;
  merged.name = profile.name;
  merged.namespace = profile.namespace;
  merged.extends = profile.extends;

  return merged;
}

// -----------------------------------------------------------------------------
// Computed Functions (derived from JSON config)
// -----------------------------------------------------------------------------

function normalizePercent(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return null;
  return Math.max(0, Math.min(100, numeric));
}

/**
 * Get physical constants, applying config.js overrides.
 * @param {object} profile - Loaded profile
 * @returns {object} - Physical constants with env overrides
 */
function getPhysical(profile) {
  return {
    potMin: config.autothrottle?.potMin ?? profile.physical?.potMin ?? 10,
    potMax: config.autothrottle?.potMax ?? profile.physical?.potMax ?? 1013,
    rawMax: config.autothrottle?.rawMax ?? profile.physical?.rawMax ?? 16383,
  };
}

/**
 * Create mapSimToPhysical function from profile config.
 * @param {object} profile - Loaded profile
 * @returns {function} - Mapping function
 */
function createMapSimToPhysical(profile) {
  const physical = getPhysical(profile);
  const behavior = profile.behavior || {};
  const detents = profile.detents || [];

  if (behavior.mappingMode === 'detent') {
    // Detent-based mapping for a manual-gate profile.
    return function mapSimToPhysical(simPct) {
      const pct = normalizePercent(simPct);
      if (pct === null) return null;

      // Find matching detent
      for (const detent of detents) {
        const [min, max] = detent.simPctRange || [0, 0];
        if (pct >= min && pct <= max) {
          const counts = physical.potMin +
            Math.round((physical.potMax - physical.potMin) * detent.fraction);
          return {
            counts: behavior.servoEnabled ? counts : null,
            detent: detent.id,
            detentCounts: counts,
            label: detent.label,
            gateActive: detent.isDetentGate || false,
          };
        }
      }

      // Fallback to CLB or linear
      const clbDetent = detents.find(d => d.id === 'clb');
      if (clbDetent) {
        const counts = physical.potMin +
          Math.round((physical.potMax - physical.potMin) * clbDetent.fraction);
        return {
          counts: null,
          detent: clbDetent.id,
          detentCounts: counts,
          label: clbDetent.label,
          gateActive: true,
        };
      }

      // Pure fallback - linear
      const counts = physical.potMin +
        Math.round((physical.potMax - physical.potMin) * (pct / 100));
      return { counts, detent: null, label: null, gateActive: false };
    };
  }

  // Linear mapping (Boeing style)
  return function mapSimToPhysical(simPct) {
    const pct = normalizePercent(simPct);
    if (pct === null) return null;

    // Find active floor detent
    const activeFloor = detents.find(d => d.isActiveFloor);
    const floorFrac = activeFloor ? activeFloor.fraction : 0;
    const floorCounts = physical.potMin +
      Math.round((physical.potMax - physical.potMin) * floorFrac);

    // Linear mapping from 0-100% to [floor..max]
    const span = physical.potMax - floorCounts;
    const counts = Math.round(floorCounts + (pct / 100) * span);

    return {
      counts: Math.max(physical.potMin, Math.min(physical.potMax, counts)),
      detent: null,
    };
  };
}

/**
 * Create rawToPercent function from profile config.
 * @param {object} profile - Loaded profile
 * @returns {function} - Conversion function
 */
function createRawToPercent(profile) {
  const physical = getPhysical(profile);

  return function rawToPercent(raw) {
    const v = Number(raw);
    if (!Number.isFinite(v) || physical.rawMax <= 0) return null;
    return Math.max(0, Math.min(100, (v / physical.rawMax) * 100));
  };
}

/**
 * Create formatCommand function from profile config.
 * @param {object} profile - Loaded profile
 * @returns {function} - Formatting function
 */
function createFormatCommand() {
  return function formatCommand(result) {
    if (!Number.isFinite(result?.counts) || result.counts < 0 || result.counts > 1023) {
      return null;
    }
    return `T${String(Math.round(result.counts)).padStart(4, '0')}`;
  };
}

/**
 * Create findDetentForSimPct function.
 * @param {object} profile - Loaded profile
 * @returns {function}
 */
function createFindDetent(profile) {
  const detents = profile.detents || [];

  return function findDetentForSimPct(simPct) {
    for (const d of detents) {
      const [min, max] = d.simPctRange || [0, 0];
      if (simPct >= min && simPct <= max) {
        return d;
      }
    }
    return null;
  };
}

/**
 * Create getDetentGates function for Teensy configuration.
 * @param {object} profile - Loaded profile
 * @returns {function}
 */
function createGetDetentGates(profile) {
  const physical = getPhysical(profile);
  const detents = profile.detents || [];

  return function getDetentGates() {
    return detents
      .filter(d => d.isDetentGate)
      .map(d => ({
        id: d.id,
        counts: physical.potMin +
          Math.round((physical.potMax - physical.potMin) * d.fraction),
        label: d.label,
      }));
  };
}

/**
 * Map a detent fraction (0-1) to physical position based on travel mode.
 * - full: fraction spans entire pot range
 * - bottom: fraction compressed into lower half (0-0.5 of pot)
 * - half: fraction compressed into upper half (0.5-1.0 of pot)
 * @param {number} fraction - Detent position as 0-1 of logical travel
 * @param {string} travelMode - 'full', 'bottom', or 'half'
 * @returns {number} - Mapped fraction in physical pot coordinates
 */
function mapDetentFraction(fraction, travelMode) {
  const f = Math.max(0, Math.min(1, fraction));
  if (travelMode === 'bottom') return f * 0.5;           // compress into lower half
  if (travelMode === 'half') return 0.5 + f * 0.5;       // compress into upper half
  return f;                                               // full span
}

/**
 * Build a compact detent layout for external devices (e.g., Teensy).
 * Gate positions are mapped based on travelMode so detents land in the active span.
 * @param {object} profile - Resolved profile; defaults to active
 * @returns {{potMin:number,potMax:number,gates:Array<{id:string,counts:number,label?:string}>}|null}
 */
function getDetentLayout(profile = null) {
  const p = profile || getActiveProfile();
  if (!p) return null;
  const physical = getPhysical(p);
  const travelMode = p.behavior?.travelMode || 'bottom';
  const range = physical.potMax - physical.potMin;
  let gates = (p.detents || [])
    .filter(d => d.isDetentGate)
    .map(d => {
      const mappedFrac = mapDetentFraction(d.fraction, travelMode);
      return {
        id: d.id,
        counts: physical.potMin + Math.round(range * mappedFrac),
        label: d.label,
      };
    });

  // Servo-driven Boeing profiles use the active floor as their default tactile
  // gate. Sending it also keeps firmware pot calibration in sync with the host.
  if (gates.length === 0) {
    const activeFloor = (p.detents || []).find(d => d.isActiveFloor);
    if (activeFloor) {
      gates = [{
        id: activeFloor.id,
        counts: physical.potMin + Math.round(range * activeFloor.fraction),
        label: activeFloor.label,
      }];
    }
  }
  return { potMin: physical.potMin, potMax: physical.potMax, gates, travelMode };
}

/**
 * Attach computed functions to a loaded profile.
 * @param {object} profile - Raw profile
 * @returns {object} - Profile with functions
 */
function attachFunctions(profile) {
  return {
    ...profile,

    // Legacy compatibility: meta object
    meta: {
      id: profile.id,
      name: profile.name,
      description: profile.description,
    },

    // Physical with env overrides
    physical: getPhysical(profile),

    // Computed functions
    mapSimToPhysical: createMapSimToPhysical(profile),
    rawToPercent: createRawToPercent(profile),
    formatCommand: createFormatCommand(profile),
    findDetentForSimPct: createFindDetent(profile),
    getDetentGates: createGetDetentGates(profile),
  };
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

/**
 * Load a profile by ID.
 * @param {string} id - Profile ID
 * @returns {object|null} - Resolved profile with functions, or null
 */
function loadProfile(id) {
  if (!id) return null;

  const cacheKey = id.toLowerCase().trim();
  if (profileCache.has(cacheKey)) {
    return profileCache.get(cacheKey);
  }

  const resolved = resolveProfilePath(id);
  if (!resolved) {
    Debug.log('throttle-loader', `Profile not found: ${id}`);
    return null;
  }

  const profile = readProfileFile(resolved.filePath);
  if (!profile) {
    return null;
  }

  if (!profile.namespace) {
    profile.namespace = resolved.namespace;
  }

  // Resolve inheritance
  const resolvedProfile = resolveInheritance(profile);

  // Attach computed functions
  const withFunctions = attachFunctions(resolvedProfile);

  // Mark as loaded
  withFunctions._loaded = true;
  withFunctions._source = resolved.filePath;
  withFunctions._qualifiedId = `${resolved.namespace}/${resolved.id}`;

  // Cache
  profileCache.set(cacheKey, withFunctions);
  profileCache.set(withFunctions._qualifiedId, withFunctions);

  Debug.log('throttle-loader', `Loaded profile: ${withFunctions.name}`, {
    id: withFunctions.id,
    namespace: withFunctions.namespace,
    extends: profile.extends || null,
    behavior: withFunctions.behavior?.mappingMode,
  });

  return withFunctions;
}

/**
 * Detect throttle profile from aircraft profile ID.
 * @param {string} aircraftProfileId - Aircraft profile ID
 * @returns {object} - Best matching throttle profile
 */
function detectFromAircraft(aircraftProfileId) {
  if (!aircraftProfileId) {
    return loadProfile(GENERIC_ID);
  }

  // List all profiles and find one that matches this aircraft
  const profiles = listProfiles();

  for (const p of profiles) {
    if (p.abstract) continue;

    const full = loadProfile(p.id);
    if (!full || !full.matching) continue;

    const aircraftMatches = full.matching.aircraftProfiles || [];
    if (aircraftMatches.includes(aircraftProfileId)) {
      Debug.log('throttle-loader', `Auto-detected throttle profile for ${aircraftProfileId}`, {
        throttleProfile: full.id,
      });
      return full;
    }

    // Also check aliases
    const aliases = full.matching.aliases || [];
    if (aliases.some(a => a.toLowerCase() === aircraftProfileId.toLowerCase())) {
      Debug.log('throttle-loader', `Auto-detected throttle profile via alias for ${aircraftProfileId}`, {
        throttleProfile: full.id,
      });
      return full;
    }
  }

  Debug.log('throttle-loader', `No throttle profile matched aircraft ${aircraftProfileId}, using generic`);
  return loadProfile(GENERIC_ID);
}

/**
 * Get the active throttle profile.
 * Loads from THROTTLE_PROFILE env var on first call.
 * @returns {object}
 */
function getActiveProfile() {
  if (activeProfile) {
    return activeProfile;
  }

  const profileId = config.autothrottle?.profile || 'boeing-737';

  // Handle 'auto' or 'profile' mode - detect from active aircraft profile
  if (profileId === 'auto' || profileId === 'profile') {
    try {
      const aircraftLoader = require('./aircraft-profile-loader');
      const aircraftProfile = aircraftLoader.getActiveProfile();
      if (aircraftProfile) {
        activeProfile = detectFromAircraft(aircraftProfile.id);
        activeProfileId = activeProfile.id;
        console.log(`[throttle-loader] Auto-detected: ${activeProfile.name}`);
        return activeProfile;
      }
    } catch (err) {
      Debug.log('throttle-loader', 'Failed to auto-detect from aircraft profile', { error: err.message });
    }
  }

  // Direct profile ID
  activeProfile = loadProfile(resolveAlias(profileId));
  if (!activeProfile) {
    activeProfile = loadProfile(GENERIC_ID);
  }
  activeProfileId = activeProfile?.id || GENERIC_ID;

  console.log(`[throttle-loader] Loaded: ${activeProfile?.name || 'generic'}`);
  return activeProfile;
}

/**
 * Get the active profile ID.
 * @returns {string}
 */
function getActiveProfileId() {
  if (!activeProfileId) {
    getActiveProfile();
  }
  return activeProfileId;
}

/**
 * Switch to a different profile at runtime.
 * @param {string} id - New profile ID
 * @returns {object}
 */
function switchProfile(id) {
  activeProfile = loadProfile(resolveAlias(id));
  if (!activeProfile) {
    activeProfile = loadProfile(GENERIC_ID);
  }
  activeProfileId = activeProfile?.id || GENERIC_ID;
  console.log(`[throttle-loader] Switched to: ${activeProfile?.name}`);
  return activeProfile;
}

/**
 * Set active profile from aircraft profile change.
 * @param {string} aircraftProfileId - Aircraft profile ID
 * @returns {object}
 */
function setFromAircraftProfile(aircraftProfileId) {
  activeProfile = detectFromAircraft(aircraftProfileId);
  activeProfileId = activeProfile?.id || GENERIC_ID;
  console.log(`[throttle-loader] Set from aircraft: ${activeProfile?.name}`);
  return activeProfile;
}

/**
 * List all available profiles.
 * @returns {Array<{id: string, name: string, description: string, abstract: boolean}>}
 */
function listProfiles() {
  const profiles = [];
  const seen = new Set();

  // Search built-in directory
  if (fs.existsSync(BUILTIN_DIR)) {
    for (const file of fs.readdirSync(BUILTIN_DIR)) {
      if (!file.endsWith('.json') || file.startsWith('_') || file.includes('schema')) {
        continue;
      }

      const id = file.replace('.json', '');
      if (DISABLED_PROFILE_IDS.has(id)) continue;
      if (seen.has(id)) continue;
      seen.add(id);

      const profile = readProfileFile(path.join(BUILTIN_DIR, file));
      if (profile) {
        profiles.push({
          id: profile.id || id,
          name: profile.name || id,
          description: profile.description || '',
          abstract: profile.abstract || false,
          namespace: 'official',
        });
      }
    }
  }

  // Search user directory
  if (fs.existsSync(USER_DIR)) {
    for (const file of fs.readdirSync(USER_DIR)) {
      if (!file.endsWith('.json') || file.startsWith('_')) {
        continue;
      }

      const id = file.replace('.json', '');
      if (DISABLED_PROFILE_IDS.has(id)) continue;
      if (seen.has(id)) continue;
      seen.add(id);

      const profile = readProfileFile(path.join(USER_DIR, file));
      if (profile) {
        profiles.push({
          id: profile.id || id,
          name: profile.name || id,
          description: profile.description || '',
          abstract: profile.abstract || false,
          namespace: 'local',
        });
      }
    }
  }

  return profiles;
}

/**
 * Clear all caches.
 */
function clearCache() {
  profileCache.clear();
  activeProfile = null;
  activeProfileId = null;
}

/**
 * Resolve alias to profile ID.
 * @param {string} alias - Alias or ID
 * @returns {string} - Resolved ID
 */
function resolveAlias(alias) {
  if (!alias) return GENERIC_ID;

  const lower = alias.toLowerCase().trim();

  // Check all profiles for matching alias
  const profiles = listProfiles();
  for (const p of profiles) {
    if (p.abstract) continue;

    const full = loadProfile(p.id);
    if (!full || !full.matching) continue;

    const aliases = full.matching.aliases || [];
    if (aliases.some(a => a.toLowerCase() === lower)) {
      return p.id;
    }
  }

  // Not an alias, return as-is
  return alias;
}

module.exports = {
  loadProfile,
  detectFromAircraft,
  getActiveProfile,
  getActiveProfileId,
  switchProfile,
  setFromAircraftProfile,
  listProfiles,
  clearCache,
  resolveAlias,
  resolveProfilePath,
  getDetentLayout,
  BUILTIN_DIR,
  USER_DIR,
  GENERIC_ID,
  DISABLED_PROFILE_IDS,
};
