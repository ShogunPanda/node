'use strict';
const common = require('../common');
if (!common.hasCrypto) {
  common.skip('missing crypto');
}

const fixtures = require('../common/fixtures');
const https = require('https');
const MakeDuplexPair = require('../common/duplexpair');
const tls = require('tls');
const { finished } = require('stream');

const certFixture = {
  key: fixtures.readKey('agent1-key.pem'),
  cert: fixtures.readKey('agent1-cert.pem'),
  ca: fixtures.readKey('ca1-cert.pem'),
};

// Test 1: The server sends an invalid header.
{
  const { clientSide, serverSide } = MakeDuplexPair();

  const req = https.request({
    rejectUnauthorized: false,
    createConnection: common.mustCall(() => clientSide)
  }, common.mustNotCall());

  req.on('error', common.mustCall());
  req.end();

  serverSide.resume();  // Dump the request
  serverSide.end('HTTP/1.1 200 OK\r\n' +
                 'Host: example.com\r\n' +
                 'Hello: foo\x08foo\r\n' +
                 'Content-Length: 0\r\n' +
                 '\r\n\r\n');
}

// Test 2: The client sends and invalid header
{
  const server = https.createServer(
    { ...certFixture },
    common.mustNotCall());

  server.on('clientError', common.mustCall());

  server.listen(0, common.mustCall(() => {
    const client = tls.connect({
      port: server.address().port,
      rejectUnauthorized: false
    });
    client.write(
      'GET / HTTP/1.1\r\n' +
      'Host: example.com\r\n' +
      'Hello: foo\x08foo\r\n' +
      '\r\n\r\n');
    client.end();

    client.on('data', () => {});
    finished(client, common.mustCall(() => {
      server.close();
    }));
  }));
}
