'use strict';
/*
 * potluck-out: writes msg.payload (a bool or a number) to a resource through the hub (POST /api/write), and passes
 * the message on with msg.potluck = {result, ok, status, path, value} -- the hub's answer, to show whoever asked.
 * The resource is the node's configured path, else msg.topic.
 */

module.exports = function (RED) {
  function PotluckOutNode(config) {
    RED.nodes.createNode(this, config);
    const node = this;
    const hub = RED.nodes.getNode(config.hub);
    const fixedPath = (config.path || '').trim();
    const numberType = config.numberType || '';

    if (!hub || !hub.client) {
      node.status({ fill: 'red', shape: 'ring', text: hub ? hub.configError || 'hub not set up' : 'no hub chosen' });
      return;
    }
    const c = hub.client;
    hub.register(node);
    node.status({});

    node.on('input', (msg, send, done) => {
      send = send || function () { node.send.apply(node, arguments); };
      const path = fixedPath || msg.topic;
      const opts = {};
      if (numberType && numberType !== 'hub') opts.numberType = numberType;
      node.status({ fill: 'blue', shape: 'ring', text: 'writing' });
      c.write(path, msg.payload, opts)
        .then((r) => {
          msg.potluck = Object.assign({}, msg.potluck, {
            kind: 'write', result: r.result, ok: r.ok, status: r.status, path: r.path, value: r.value,
          });
          node.status(r.ok
            ? { fill: 'green', shape: 'dot', text: r.result }
            : { fill: 'red', shape: 'ring', text: String(r.result).slice(0, 60) });
          send(msg);
          if (done) done();
        })
        .catch((e) => {
          msg.potluck = Object.assign({}, msg.potluck, {
            kind: 'write', result: `the hub did not answer: ${e.message}`, ok: false, status: 0, path,
            value: msg.payload,
          });
          node.status({ fill: 'red', shape: 'ring', text: 'hub unreachable' });
          send(msg);
          if (done) done();
        });
    });

    node.on('close', (done) => {
      Promise.resolve(hub.deregister(node)).catch(() => {}).then(() => done());
    });
  }

  RED.nodes.registerType('potluck-out', PotluckOutNode);
};
