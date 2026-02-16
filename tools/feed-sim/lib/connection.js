'use strict';

const net = require('net');

class Connection {
  constructor(host, port, opts = {}) {
    this.host = host;
    this.port = port;
    this.verbose = opts.verbose || false;
    this._socket = null;
    this._messageCount = 0;
    this._connected = false;
  }

  connect() {
    return new Promise((resolve, reject) => {
      this._socket = net.createConnection({ host: this.host, port: this.port }, () => {
        this._connected = true;
        resolve();
      });
      this._socket.on('error', (err) => {
        if (!this._connected) {
          reject(err);
        } else {
          console.error(`[connection] socket error: ${err.message}`);
        }
      });
      this._socket.on('close', () => {
        this._connected = false;
      });
      this._socket.setNoDelay(true);
    });
  }

  send(obj) {
    if (!this._connected) throw new Error('Not connected');
    const line = JSON.stringify(obj) + '\n';
    this._socket.write(line);
    this._messageCount++;
    if (this.verbose) {
      process.stdout.write(`  > ${line}`);
    }
  }

  sendBatch(msgs) {
    if (!this._connected) throw new Error('Not connected');
    let buf = '';
    for (const obj of msgs) {
      buf += JSON.stringify(obj) + '\n';
      this._messageCount++;
    }
    this._socket.write(buf);
    if (this.verbose) {
      process.stdout.write(buf.split('\n').filter(Boolean).map(l => `  > ${l}\n`).join(''));
    }
  }

  get messageCount() {
    return this._messageCount;
  }

  get connected() {
    return this._connected;
  }

  close() {
    return new Promise((resolve) => {
      if (!this._socket) return resolve();
      this._socket.once('close', resolve);
      this._socket.end();
    });
  }
}

module.exports = { Connection };
