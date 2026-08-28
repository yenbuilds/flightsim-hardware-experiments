'use strict';

function log(scope, message, details) {
  if (!['1', 'true', 'yes', 'on'].includes((process.env.AT_DEBUG || '').toLowerCase())) {
    return;
  }

  if (details === undefined) {
    console.log(`[${scope}] ${message}`);
    return;
  }

  console.log(`[${scope}] ${message}`, details);
}

module.exports = { log };
