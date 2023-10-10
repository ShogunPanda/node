'use strict';
const common = require('../common');
const http = require('http');
const MakeDuplexPair = require('../common/duplexpair');

// Test that setting the `maxHeaderSize` option works on a per-stream-basis.

// Test 1: The server sends an invalid header.
{
  const { clientSide, serverSide } = MakeDuplexPair();

  const req = http.request({
    createConnection: common.mustCall(() => clientSide),
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
  const server = http.createServer(common.mustNotCall());

  server.on('clientError', common.mustCall());

  const { clientSide, serverSide } = MakeDuplexPair();
  serverSide.server = server;
  server.emit('connection', serverSide);

  clientSide.write('GET / HTTP/1.1\r\n' +
                   'Host: example.com\r\n' +
                   'Hello: foo\x08foo\r\n' +
                   '\r\n\r\n');
}
