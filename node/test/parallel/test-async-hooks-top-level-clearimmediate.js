'use strict';

// Regression test for https://github.com/nodejs/node/issues/13262

const common = require('../common');
const assert = require('assert');
const async_hooks = require('async_hooks');

let seenId, seenResource;

async_hooks.createHook({
  init: common.mustCall((id, provider, triggerId, resource) => {
    seenId = id;
    seenResource = resource;
    assert.strictEqual(provider, 'Immediate');
    assert.strictEqual(triggerId, 1);
  }),
  before: common.mustNotCall(),
  after: common.mustNotCall(),
  destroy: common.mustCall((id) => {
    assert.strictEqual(seenId, id);
  })
}).enable();

const immediate = setImmediate(common.mustNotCall());
assert.strictEqual(immediate, seenResource);
clearImmediate(immediate);
