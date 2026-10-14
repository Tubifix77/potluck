'use strict';
/*
 * The potluck-hub config node: one connection to a Potluck hub, shared by every potluck-in and potluck-out node
 * that names it. It owns the stream, the fallback polling and the state report; the logic is in ../lib/client.js.
 */

const { PotluckClient, pair } = require('../lib/client');
const { discover } = require('../lib/mdns');

module.exports = function (RED) {
  function PotluckHubNode(config) {
    RED.nodes.createNode(this, config);
    const node = this;
    node.url = (config.url || 'http://127.0.0.1:8470/').trim();
    node.clientName = (config.clientName || 'Node-RED').trim();
    node.fingerprint = (config.fingerprint || '').trim();
    node.includeUnexposed = !!config.includeUnexposed;
    node.numberType = config.numberType || 'auto';
    node.users = new Set();
    node.client = null;
    node.configError = null;
    try {
      node.client = new PotluckClient({
        url: node.url,
        name: node.clientName,
        fingerprint: node.fingerprint,
        token: (node.credentials && node.credentials.token) || '',
        includeUnexposed: node.includeUnexposed,
        numberType: node.numberType,
      });
      node.client.on('problem', (p) => node.debug(`potluck: ${p.message}`));
      node.client.on('report', (r) => {
        if (!r.accepted) node.debug(`potluck: state report not accepted (${r.status} ${r.result})`);
      });
    } catch (e) {
      node.configError = e.message;
      node.error(`potluck-hub: ${e.message}`);
    }

    /** A potluck-in or potluck-out node starts using the hub; the first one opens the stream. */
    node.register = (user) => {
      if (!node.client) return;
      node.users.add(user);
      if (node.users.size === 1) node.client.start();
    };

    node.deregister = (user) => {
      node.users.delete(user);
      if (node.users.size === 0 && node.client && node.client.running) {
        return node.client.stop('no node uses it any more');
      }
      return Promise.resolve();
    };

    node.on('close', (removed, done) => {
      node.users.clear();
      const p = node.client ? node.client.stop(removed ? 'removed from the flow' : 'Node-RED redeployed or stopped') : null;
      Promise.resolve(p).catch(() => {}).then(() => done());
    });
  }

  RED.nodes.registerType('potluck-hub', PotluckHubNode, {
    credentials: { token: { type: 'password' } },
  });

  // ------------------------------------------------------------ the editor's buttons: find a hub, pair with it

  const pairings = new Map(); // key -> {state, token?, error?, right, at}
  let pairSeq = 0;
  const perm = RED.auth.needsPermission('potluck-hub.write');

  RED.httpAdmin.post('/potluck-hub/discover', perm, (req, res) => {
    discover({ timeoutMs: 3000 })
      .then((hubs) => res.json({ hubs }))
      .catch((e) => res.status(500).json({ error: e.message }));
  });

  RED.httpAdmin.post('/potluck-hub/pair', perm, (req, res) => {
    const b = req.body || {};
    const key = `p${Date.now().toString(36)}${(pairSeq += 1)}`;
    const entry = { state: 'starting', at: Date.now() };
    pairings.set(key, entry);
    pair({
      url: b.url,
      fingerprint: b.fingerprint,
      name: b.name || 'Node-RED',
      right: b.right === 'write' ? 'write' : 'look',
      onState: (s) => Object.assign(entry, { state: 'pending', expires_in_s: s.expires_in_s }),
    })
      .then((r) => Object.assign(entry, { state: 'approved', token: r.token, right: r.right }))
      .catch((e) => Object.assign(entry, { state: 'failed', error: e.message }));
    res.json({ key });
  });

  RED.httpAdmin.get('/potluck-hub/pair/:key', perm, (req, res) => {
    const entry = pairings.get(req.params.key);
    if (!entry) return res.status(404).json({ state: 'unknown' });
    res.json(entry);
    // The token goes to the editor once; it is kept as the config node's credential when the flow is deployed.
    if (entry.state === 'approved' || entry.state === 'failed') pairings.delete(req.params.key);
    for (const [k, e] of pairings) if (Date.now() - e.at > 10 * 60 * 1000) pairings.delete(k);
    return undefined;
  });
};
