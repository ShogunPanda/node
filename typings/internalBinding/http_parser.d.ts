declare namespace InternalHttpParserBinding {
  type Buffer: Uint8Array;
  type Stream: object;

  class HTTPParser {
    static REQUEST: 1;
    static RESPONSE: 2;

    static kOnMessageBegin: 0;
    static kOnHeaders: 1;
    static kOnHeadersComplete: 2;
    static kOnBody: 3;
    static kOnTrailers: 4;
    static kOnTrailersComplete: 5;
    static kOnMessageComplete: 6;
    static kOnExecute: 7;

    static kLenientNone: number;
    static kLenientHeaders: number;
    static kLenientChunkedLength: number;
    static kLenientKeepAlive: number;
    static kLenientAll: number;

    close(): void;
    free(): void;
    execute(buffer: Buffer, skip: number, limit: number): Error | Buffer;
    finish(): Error | Buffer;
    initialize(
      type: number,
      resource: object,
      maxHeaderSize?: number,
      lenient?: number,
      headersTimeout?: number,
    ): void;
    pause(): void;
    resume(): void;
    consume(stream: Stream): void;
    unconsume(): void;
    getCurrentBuffer(): Buffer;
    duration(): number;
    headersCompleted(): boolean;
    trailersCompleted(): boolean
  
  }
}

export interface HttpParserBinding {
  methods: string[];
  HTTPParser: typeof InternalHttpParserBinding.HTTPParser;
}
