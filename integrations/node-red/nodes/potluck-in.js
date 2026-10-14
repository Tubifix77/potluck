'use strict';
/*
 * potluck-in: one message per change in the cell, from the hub's stream.
 *   resource:  msg.topic = the resource's path, msg.payload = its value (null when it has none to show),
 *              msg.potluck = {kind: 'resource', unit, quality, age_ms, owner, board: {node, label, state}, ...}
 *   board:     msg.topic = potluck://<system>/node-<id>, msg.payload = alive | asleep | dead | unknown
 *   event:     msg.topic = potluck://<system>/event/<kind>, msg.payload = the event's detail
 */

module.exports = function (RED) {
  /** "node-7368/*, potluck://lab/act/rf_fusion/out" -> a test on a resource's path and name. */
  function pathFilter(text) {
    const pats = String(text || '').split(/[\n,]+/).map((s) => s.trim()).filter(Boolean);
    if (!pats.length) return () => true;
    const res = pats.map((p) => {
      const esc = p.replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*');
      return new RegExp(`^${esc}$`);
    });
    return (r) => res.some((re) => re.test(r.path) || (r.name && re.test(r.name)));
  }

  function PotluckInNode(config) {
    RED.nodes.createNode(this, config);
    const node = this;
    const hub = RED.nodes.getNode(config.hub);
    const wanted = pathFilter(config.paths);
    const sendBoards = !!config.boards;
    const sendEvents = !!config.events;
    const sendInitial = config.initial !== false;

    if (!hub || !hub.client) {
      node.status({ fill: 'red', shape: 'ring', text: hub ? hub.configError || 'hub not set up' : 'no hub chosen' });
      return;
    }
    const c = hub.client;

    const onResource = (r) => {
      if (!wanted(r)) return;
      if (r.initial && !sendInitial) return;
      const potluck = {
        kind: 'resource',
        name: r.name,
        unit: r.unit,
        quality: r.quality,
        age_ms: r.age_ms,
        owner: r.owner,
        board: r.board,
        latency_class: r.latency_class,
        outside_writable: r.outside_writable,
        safety_relevant: r.safety_relevant,
        cell: r.cell,
        system: r.system,
        at: r.at,
        initial: !!r.initial,
      };
      if (r.removed) potluck.removed = true;
      if (r.hub_unreachable) potluck.hub_unreachable = true;
      if (r.note) potluck.note = r.note;
      // Never a value without its quality: payload is null whenever the hub has none to show.
      node.send({ topic: r.path, payload: r.value, potluck });
    };

    const onBoard = (b) => {
      if (!sendBoards || (b.initial && !sendInitial)) return;
      const system = (c.model && c.model.cell && c.model.cell.system) || '';
      node.send({
        topic: `potluck://${system}/node-${b.node}`,
        payload: b.state,
        potluck: { kind: 'board', node: b.node, label: b.label, state: b.state, previous: b.previous,
          since_at: b.since_at, hub_unreachable: !!b.hub_unreachable, initial: !!b.initial },
      });
    };

    const onEvent = (e) => {
      if (!sendEvents) return;
      const system = (c.model && c.model.cell && c.model.cell.system) || '';
      node.send({
        topic: `potluck://${system}/event/${e.kind}`,
        payload: e.detail,
        potluck: { kind: 'event', event: e.kind, seq: e.seq, at: e.at, board: e.board },
      });
    };

    const onState = (s) => {
      const map = {
        connecting: { fill: 'grey', shape: 'ring', text: 'connecting' },
        streaming: { fill: 'green', shape: 'dot', text: 'connected' },
        polling: { fill: 'yellow', shape: 'dot', text: 'connected (polling, stream down)' },
        unreachable: { fill: 'red', shape: 'ring', text: 'hub unreachable' },
        refused: { fill: 'red', shape: 'dot', text: `refused: ${s.detail}`.slice(0, 60) },
        idle: { fill: 'grey', shape: 'ring', text: 'stopped' },
      };
      node.status(map[s.state] || { fill: 'grey', shape: 'ring', text: s.state });
    };

    c.on('resource', onResource);
    c.on('board', onBoard);
    c.on('event', onEvent);
    c.on('state', onState);
    onState({ state: c.conn, detail: c.connDetail });
    hub.register(node);

    // A node added after the hub already had a model starts from what it knows.
    if (c.model && sendInitial) {
      for (const r of c.resources()) onResource({ ...r, initial: true });
    }

    node.on('close', (done) => {
      c.removeListener('resource', onResource);
      c.removeListener('board', onBoard);
      c.removeListener('event', onEvent);
      c.removeListener('state', onState);
      Promise.resolve(hub.deregister(node)).catch(() => {}).then(() => done());
    });
  }

  RED.nodes.registerType('potluck-in', PotluckInNode);
};
