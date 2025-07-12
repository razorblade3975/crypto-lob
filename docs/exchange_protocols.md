## Binance

* **WebSocket URL**: Binance’s market data WebSocket base URL is `wss://stream.binance.com:9443`. Streams can be accessed directly via endpoints like `/ws/<streamName>` for a single stream or via `/stream?streams=<stream1>/<stream2>...` for combined streams. For example, a raw stream endpoint could be `wss://stream.binance.com:9443/ws/btcusdt@depth` for order book updates.
* **Subscription Format**: Binance Spot uses the endpoint path to select streams (no explicit JSON subscribe message needed when connecting to a specific stream). Alternatively, combined streams allow multiple symbols by specifying them in the URL. If using the combined stream endpoint, each event comes wrapped in a JSON with a `"stream"` name and `"data"` payload. Binance also supports a live subscription command: e.g., sending a JSON `{"method": "SUBSCRIBE", "params": ["btcusdt@depth"], "id": 1}` if connected to a base URL (this method is documented but most commonly the stream is specified in the URL).
* **Updates vs Snapshots**: Binance provides both incremental order book updates and periodic snapshots of top levels. **Diff. Depth Stream** (`<symbol>@depth`) is an incremental feed that pushes depth changes (price/quantity updates) continuously (1000ms by default, or 100ms if specified). Each diff update message has an event type `"depthUpdate"` and includes the range of update IDs covered (fields `U` and `u`) along with lists of changed bids and asks. Separately, **Partial Book Depth Streams** (`<symbol>@depth<levels>`) push full snapshots of the top *N* levels at a fixed interval (1s by default, or 100ms). Valid values of `<levels>` are 5, 10, or 20 for partial depth streams, which deliver a fresh top-<em>N</em> snapshot on each update (with a `lastUpdateId` to synchronize with diff streams). In practice, a common approach for full order books is to fetch an initial REST snapshot (e.g. 5000 levels) and then apply the continuous WebSocket diff updates.
* **Message Format**: WebSocket depth messages are JSON. For diff updates, the payload includes: `"e": "depthUpdate"` (event type), `"E"` (event time), `"s"` (symbol), `"U"` (first update ID in this batch), `"u"` (last update ID in this batch), and arrays of `"b"` (bids) and `"a"` (asks) changes. Each entry in `"b"` or `"a"` is `[price, quantity]`. A quantity of `"0"` indicates that level should be removed from the order book. Partial depth snapshots (e.g. `@depth20`) are JSON with `"lastUpdateId"` and full `"bids"`/`"asks"` arrays for the top levels. Binance’s diff stream updates are incremental – they only contain levels that changed, not the entire book. Consumers must maintain state and apply these changes in order, discarding any update event that precedes the last applied update ID.
* **Depth Configuration**: Binance allows subscription to fixed depth snapshots of **5, 10, or 20** levels via the partial book streams. The diff stream (`@depth`) itself isn’t limited by level count – it reports all changes in the order book (up to the maximum depth of 5000 if the client maintains that many). The REST API snapshot can provide up to 5,000 levels, which the order book engine can use for full depth initialization. The top-*N* of interest (e.g. 5, 20, 100) can then be maintained by applying the diff updates to that snapshot. For use cases only requiring top 5/10/20, the partial streams at 100ms can be used directly.
* **Authentication**: **No auth needed** for Binance public market data feeds. The WebSocket endpoints for market streams are public and do not require an API key or token. (Authentication is only required for user-specific data like private orders or user account streams.) A single connection can subscribe to multiple symbols’ streams (up to 200 streams per connection). Rate limits do apply (e.g. max 10 incoming messages per second on a connection).
* **Notable Quirks**: Binance’s WebSocket server sends periodic **ping frames** (for Spot, approximately every 3 minutes) that must be responded to with a **pong** to avoid disconnection. (According to Binance documentation, Spot expects a pong within 1 minute of a ping. Binance futures have a slightly different heartbeat interval.) Another quirk is that Binance’s stream names are case-sensitive and use **lowercase** symbol letters in the URL. Also, when combining multiple streams, Binance wraps each message with the stream name as noted above. Finally, **order book synchronization**: Binance explicitly documents a procedure to manage the order book locally – fetch REST snapshot, then **buffer** incoming WS messages and apply those with IDs > snapshot’s lastUpdateId, discard earlier ones. If any gap in update IDs is detected, the client should re-sync from REST. Properly handling the sequence IDs (especially on reconnects) is crucial for Binance’s incremental feed.

## KuCoin

* **WebSocket URL**: KuCoin’s public market data feed requires a two-step process. The client must first call a REST **token request** to get WebSocket connection details. This returns one or more server URLs (e.g. `wss://ws-feed.kucoin.com/endpoint`) along with a token. The client chooses a server URL and appends the token as a query parameter (e.g. `?token=<token>`). The token grants access to public channels and is valid for 24 hours, after which a new one must be obtained. KuCoin provides separate tokens/endpoints for Spot vs Futures APIs, but the concept is similar. No API key is required for public data (use the **public** token endpoint).
* **Subscription Format**: Once connected, clients **subscribe** by sending a JSON message. The format includes an `id` (client request ID), `type`: `"subscribe"`, and a `topic` string. For order books, KuCoin offers multiple topics: the full order book feed is `"/market/level2:{symbol}"`, and they also offer pre-aggregated depth feeds like `"/market/level2Depth5:{symbol}"` and `"/market/level2Depth50:{symbol}"` for top N levels. For example, to subscribe to the full L2 for BTC-USDT, one would send: `json\n{ "id": <nonce>, "type": "subscribe", "topic": "/market/level2:BTC-USDT", "response": true }\n`
  . KuCoin allows batching multiple symbols in one topic (comma-separated in the topic string) up to 100 symbols per L2 subscription. A successful subscription will be acknowledged by a message of type `"ack"` (if `response: true` was requested).
* **Updates vs Snapshots**: **KuCoin’s Level2 feed is incremental**. After subscribing to `market/level2`, KuCoin will start pushing **delta updates** (they call them `l2update` messages) containing the changes to the order book. Importantly, KuCoin does **not** initially send a full snapshot over WebSocket for the level2 feed. Instead, the client must **get a snapshot via REST** and then apply the incoming WebSocket changes. KuCoin’s docs describe the “calibration” process: upon subscribing, start caching incoming delta messages, then call the REST API (`GET /api/v1/market/orderbook/level2`) to retrieve a full snapshot, and finally merge the cached deltas. Once synchronized, apply subsequent `l2update` messages in order. Each delta message includes a `sequenceStart` and `sequenceEnd` to help identify continuity. In contrast, KuCoin’s dedicated depth channels (Level2-5 and Level2-50) push **full snapshots** of the top 5 or 50 levels continuously, rather than diffs. For example, the `level2Depth5` channel sends an updated 5-level snapshot every 100ms. These are useful if only top-of-book or shallow depth is needed. In summary: *Full order book:* incremental updates + manual snapshot; *Top-N:* frequent snapshots of those N levels.
* **Message Schema**: KuCoin WebSocket messages have a outer structure with fields like `type`, `topic`, `subject`, and `data`. For the full L2 updates, the message looks like: `{"type":"message","topic":"/market/level2:BTC-USDT","subject":"trade.l2update","data":{...}}`. The `data` contains `changes` and associated metadata. For example,

  ```json
  "data": {
      "changes": {
          "asks": [ ["18906", "0.00331", "14103845"], ["18907.3", "0.58751503", "14103844"] ],
          "bids": [ ["18891.9", "0.15688", "14103847"] ]
      },
      "sequenceStart": 14103844,
      "sequenceEnd": 14103847,
      "symbol": "BTC-USDT",
      "time": 1663747970273
  }
  ```

. Here each entry in `changes` is `[price, size, sequence]`. The sequence number represents the orderbook change ID for that price level’s update. For the **top-5/top-50 snapshot channels**, the message schema is slightly different: the `subject` may be `"level2"` and the `data` contains full `asks` and `bids` arrays without individual sequence numbers (since it’s a snapshot). For example, a `level2Depth5` update looks like:

````json
{"type":"message","topic":"/spotMarket/level2Depth5:BTC-USDT","subject":"level2","data":{
    "asks": [ ["9989","8"], ["9990","32"], ... ], 
    "bids": [ ["9988","56"], ["9987","15"], ... ], 
    "timestamp": 1586948108193
}}
``` which is the top 5 asks and bids at that moment:contentReference[oaicite:33]{index=33}:contentReference[oaicite:34]{index=34}.  KuCoin uses **absolute quantities** in updates (not deltas), and a size of `"0"` indicates removal of that price level:contentReference[oaicite:35]{index=35}. The `sequenceStart`/`sequenceEnd` in L2 updates are crucial for reassembly – the snapshot’s last ID should fall between one of these ranges to begin applying updates:contentReference[oaicite:36]{index=36}.  
- **Depth Options**: KuCoin supports **configurable depth** in two ways: (1) Subscribe to the special Depth5 or Depth50 channels for exactly top-5 or top-50 snapshots at high frequency:contentReference[oaicite:37]{index=37}. (These are handy for top-of-book or small depth monitoring; the engine can be set to use whichever matches the N needed, e.g. use `level2Depth50` for top-50.) (2) For arbitrary depths like top-20 or top-100, the general approach is to use the full Level2 incremental feed and maintain the order book internally, then simply read off the top N levels from the in-memory book. (KuCoin’s REST API also has endpoints to fetch snapshots of 20 or 100 levels:contentReference[oaicite:38]{index=38}, but via WebSocket the fixed channels are 5 and 50.) In practice, to get, say, top-100, one would subscribe to `market/level2` and build the order book (perhaps limiting the local structure to 100 levels per side for performance) using the diffs. KuCoin’s incremental feed provides **full-depth changes** (all levels) so it can support any N.  
- **Authentication**: **Public order book channels do not require API key auth**, but they *do* require the session token obtained as described. The token handshake is effectively an authentication for the WebSocket, but no API secret signature is needed for public data:contentReference[oaicite:39]{index=39}. The token will expire after 24h, meaning the client should reconnect (obtaining a new token) at least once a day to maintain the feed:contentReference[oaicite:40]{index=40}. KuCoin’s private channels (not relevant for order books) would require a different token (or adding signature) but public depth data is accessible with the public token.  
- **Notable Quirks**: KuCoin imposes a **ping/pong** requirement to keep connections alive. The WebSocket server will include a `pingInterval` (e.g. 18000ms) in the token response, indicating that the client should send a ping (typically as text `"ping"`) every 18 seconds:contentReference[oaicite:41]{index=41}:contentReference[oaicite:42]{index=42}. If the server doesn’t receive a ping within the timeout (e.g. 10 seconds past interval), it will disconnect. So a heartbeat timer on the client is necessary. Another quirk is the need to handle **token expiry** – the client must reconnect using a new token before the 24h is up. Also, KuCoin’s update messages use sequence numbers per price level which are not continuous integers (they represent order change identifiers). The continuity of the feed is determined by the `sequenceStart`/`sequenceEnd` ranges, not by those per-price sequences:contentReference[oaicite:43]{index=43}:contentReference[oaicite:44]{index=44}. To detect any missed messages, one should watch the sequence ranges and ensure no gap (i.e. each new `sequenceStart` is <= last `sequenceEnd+1`). If a gap is suspected or a reconnect happens, re-sync from REST. Overall, KuCoin’s feed requires a bit more initial setup (token + snapshot merging) but provides high-performance incremental data once connected.

## OKX (OKEx)  
- **WebSocket URL**: OKX (formerly OKEx) uses a versioned WebSocket API. The base endpoint for public market data (V5 API) is `wss://ws.okx.com:8443/ws/v5/public`:contentReference[oaicite:45]{index=45}. (There are separate paths for private data and a “business” feed, but order book data is on the public channel.) Different instrument types are all accessible through this same base by specifying parameters in subscription. For example, one would connect to the above URL and then subscribe to the order book channel for a given market.  
- **Subscription Format**: OKX expects a JSON message with `"op": "subscribe"` and an `"args"` array. Each element of `"args"` is an object specifying a `channel` and relevant `instId` (instrument ID). For order books, the primary channels are named `"books"`, `"books5"`, `"books50-l2-tbt"`, or `"books-l2-tbt"`, depending on the depth and update mode:contentReference[oaicite:46]{index=46}:contentReference[oaicite:47]{index=47}. For example, subscribing to 400-level depth for BTC-USDT (with 100ms updates) one could send:  
```json
{ "op": "subscribe", "args": [ { "channel": "books", "instId": "BTC-USDT" } ] }
````

To subscribe to a 5-level order book snapshot stream:

```json
{ "op": "subscribe", "args": [ { "channel": "books5", "instId": "BTC-USDT" } ] }
```

Multiple subscriptions can be included in one request (as separate objects in the `args` array), and the server will return a subscription acknowledgement for each. OKX’s API also supports *login* (`"op": "login"`) which is **required for certain channels** (see below), but not needed for the standard public depth channels.

* **Updates vs Snapshots**: OKX provides both **snapshot** and **incremental** order book feeds, with different channels for each. The `books5` channel publishes a full **snapshot of top 5 levels** at a regular interval (every 100ms). In general, any channel named `books5` (or the legacy `bbo-tbt` for top1) is a snapshot push – every update from that channel is the state of those top levels at that moment (no need to apply diffs). For deeper books, OKX uses **incremental updates**: the `books` channel (400-level depth) sends an initial snapshot followed by incremental deltas aggregated in 100ms batches. Similarly, `books50-l2-tbt` (50-level “tick-by-tick”) and `books-l2-tbt` (full depth tick-by-tick) provide an initial snapshot of the order book (top 50 or top 400 levels respectively) and then stream every change as a real-time update (in \~10ms intervals). These “L2-TBT” feeds are essentially high-frequency diff streams. The **initial snapshot** from an incremental channel is indicated by a message (often with an `"action":"snapshot"` or similar flag; older docs used a separate event for snapshot) containing the full depth data. Subsequent messages carry only changes (often flagged as `"action":"update"`). OKX’s documentation notes that if no change occurs in the book during a given interval, **no update message is sent** for that interval; and if using a snapshot channel, it will not send redundant snapshots when there’s no change. (In practice, for heartbeat, if there’s a prolonged lull in updates, the system may send either a repeat snapshot or a special “noop” update to signal the connection is alive.) Consumers must be prepared to handle the snapshot on subscription and then continuously apply deltas. If a new snapshot message ever arrives on an incremental channel (which could happen if the server resyncs or the user re-subscribed), the local order book should be reset to that snapshot (conceptually similar in Bybit’s note, and applies to OKX if it reissues snapshots).
* **Message Schema**: An OKX order book update message typically looks like:

  ```json
  {
    "arg": { "channel": "books", "instId": "BTC-USDT" },
    "data": [ 
       {
         "asks": [ ["50000","1.234","0","5"], ... ],
         "bids": [ ["49900","0.876","0","3"], ... ],
         "ts": "1697045812345",
         "u": 88912345,
         "checksum": 123456789
       }
    ]
  }
  ```

  The exact fields can vary by channel. In general: `"bids"` and `"asks"` are arrays of \[price, size, *others*]. For OKX, each level entry includes price, size, and sometimes additional fields (e.g. number of orders at that price). For instance, the array `["411.8", "10", "0", "4"]` in OKX’s docs means price 411.8, size 10, \~0 (deprecated liquidity flag), 4 orders at that level. The `"ts"` is the timestamp of the update, and `"u"` is an update ID or sequence number. OKX formerly included a CRC32 `"checksum"` of the top levels to let clients verify their local book’s top 25 matches the feed; however, as of 2025 OKX announced removal of checksum from new feeds. Regardless, the engine can operate without it by relying on sequence continuity. The `"arg"` echo in the message lets you know which subscription the data pertains to (useful when multiple subscriptions share one connection). Notably, **the high-frequency channels (`books50-l2-tbt` and `books-l2-tbt`) require login and have strict sequencing**. They deliver updates with an `action` field (“snapshot” vs “update”) and expect the client to maintain an extremely timely order book. Those feeds update every 10ms, so they are intended for VIP customers who need ultra-low latency. Standard `books` (100ms) and `books5` (100ms snapshots) are accessible to all without auth.
* **Depth Configuration**: OKX’s API offers **fixed depth tiers** rather than arbitrary N. For linear/inverse futures, supported depths are **1, 5, 50, 400** levels. For example, you can subscribe to top-5 or top-50 or full (400) via the corresponding channel. For spot markets, the pattern is similar: spot supports 1, 5, 50, 200 levels in different channels (spot max is 200 levels per the documentation; futures extended to 400 or 500 in some cases). In practice, if you need top-20 or top-100, you would likely subscribe to the next higher tier (50 or 400) and then simply use the top N from it. E.g. to maintain top-20, one could use the `books50-l2-tbt` feed (if available) or subscribe to `books` (400-level, 100ms updates) and filter the local view to 20 levels. OKX does *not* have a specific “depth20” channel. Instead, the exchange between tiers and frequency is: **books5** (5 @ 100ms), **books50-l2-tbt** (50 @ 10ms), **books** (400 @ 100ms), and **books-l2-tbt** (400 @ 10ms, full tick-by-tick). The order book engine should be designed to handle whichever channel best fits the needed depth/frequency combination. For example, for a full-depth order book, one might use the 100ms `books` feed (since the 400-level covers essentially full depth for most pairs), unless ultra-low latency is required (then the authenticated 10ms feed).
* **Authentication**: **Not required for most order book feeds.** All standard public depth channels (including `books` and `books5`) can be used without logging in. However, as noted, the **tick-by-tick channels** are restricted: `books50-l2-tbt` (50-level at 10ms) and `books-l2-tbt` (full depth at 10ms) require the client to login with API key and also the account’s API permission level must meet a VIP threshold (VIP 4+ for 50-level, VIP 5 for full depth). This is a unique case among exchanges – essentially a higher tier data feed. If our engine is to support those, it would need to handle the OKX authentication flow (which involves an `op: login` message with API key, passphrase, and signed timestamp). For normal top-N (5) or 100ms feeds, no auth or token is needed – just connect and subscribe.
* **Notable Quirks**: OKX’s feed has a few nuances. First, the exchange uses **“incremental update IDs”** (the `"u"` field) and might also include `"U"` (previous ID) in messages for continuity (similar to Binance). The client should ensure no gaps in these IDs when applying updates. (If a gap is detected or messages are missed, a resync via REST or requesting a new snapshot via their REST API may be necessary. OKX has an SBE-based REST endpoint for an initial snapshot if needed.) Second, **rate limits**: OKX limits the number of subscriptions and messages; it allows at most 3 login attempts/sec and 300 subscriptions within 60 seconds, etc. In practice, one should bundle subscriptions and avoid frequent reconnects. Third, as a heartbeat mechanism, OKX will send a **channel connection count** update on new subscriptions and may send periodic “*event*”: `"subscribe"` confirmations and empty data messages if no market data change occurred. The engine should ignore non-data events accordingly. Finally, OKX uses **separate endpoints per product type** (spot vs futures vs options), so if our engine trades multiple categories, it might need separate connections. However, with the unified V5 API, this is simplified to different channels on the same base URL (just ensure the instrument ID and channel are correct for the type). Overall, OKX’s order book feeds are rich: they offer high depth and high frequency options, but they demand careful handling of sequence and the potential need for API key if using the fastest streams.

## Bitget

* **WebSocket URL**: Bitget provides a unified WebSocket API (v2) for market data. The main public endpoint is `wss://ws.bitget.com/v2/ws/public`. This single URL covers various market segments (spot, futures, etc.) by specifying parameters in subscriptions. There is a parallel private endpoint (`/private`) for trading, but for order books we stay on the public one. Bitget’s documentation notes a limit of 100 connections per IP and 1000 subscriptions per connection, so a single connection can handle all needed symbols with proper multiplexing.
* **Subscription Format**: The client sends a JSON message with `"op": "subscribe"` and an `"args"` array of subscription objects. Bitget’s subscription objects include an `instType` (market type), a `channel`, and an `instId` (instrument ID). For example, to subscribe to the top-5 depth for BTCUSDT Perpetuals, one would send:

  ```json
  {
    "op": "subscribe",
    "args": [{
        "instType": "USDT-FUTURES",
        "channel": "books5",
        "instId": "BTCUSDT"
    }]
  }
  ```

. Here `books5` is the channel for 5-level depth, and `instType` designates the product (they use values like `SP` for spot, `USDT-FUTURES` for USDT-margined futures, etc., depending on API version). Multiple channels can be combined in one subscribe message by adding more elements to the `args` array. The server will reply with a confirmation message (`{"event":"subscribe", "arg":{...}}`) echoing the subscribed channel details. Unsubscription is similar with `"op": "unsubscribe"` and the same args structure.

* **Updates vs Snapshots**: Bitget offers distinct channels for **incremental vs snapshot** depth data:

  * The `**books**` channel is the full order book (all levels) feed. On subscribing to `books`, the **first message** is a `"snapshot"` of the entire order book. Thereafter, Bitget will send **incremental** updates (messages labeled `"update"`) containing only changed levels. This behaves like the classic diff feed: initial snapshot then deltas.
  * The `**books1**`, `**books5**`, and `**books15**` channels provide **fixed-level snapshots** for top 1, 5, or 15 levels respectively. Every update on those channels is essentially a snapshot of that many levels (Bitget calls it pushing “snapshot each time” for those). For example, `books5` will send the top-5 bids and asks whenever any of those change, at a default frequency of 150ms. There is no diff to apply; you simply replace the previous top-5 with the new one. The `books1` (top of book) updates even faster, default 100ms interval.
    In summary, if one needs full depth, use `books` and handle diffs; if one only needs top-N, use the appropriate booksN channel and get periodic snapshots. Bitget’s default update frequency for depth channels is **150ms** (for books, books5, books15) and 100ms for books1, which is relatively slower than some exchanges’ 100ms or 20ms options but still quite frequent. The engine being designed can subscribe to whichever channel suits the needed depth: e.g., for top-5 mode, the `books5` feed might suffice, whereas for full depth reconstruction, the `books` diff feed is intended.
* **Message Schema**: All Bitget market data messages have a common structure with an `"action"` field indicating the type of update and an `"arg"` object echoing the subscription info. For order books, `"action"` will be either `"snapshot"` or `"update"`. The `data` field is an array (often of one object) containing the order book entries. For example, a snapshot message for books5 looks like:

  ```json
  {
    "action": "snapshot",
    "arg": { "instType": "USDT-FUTURES", "channel": "books5", "instId": "BTCUSDT" },
    "data": [ {
         "asks": [ ["27000.5","8.760"], ["27001.0","0.400"], ... ],
         "bids": [ ["27000.0","2.710"], ["26999.5","1.460"], ... ],
         "ts": "1695716059516",
         "seq": 123,
         "checksum": 0
    } ]
  }
  ```

. Here `"seq"` is a sequential number for the order book update, and `"ts"` is the server timestamp. In incremental updates (`"action": "update"` on the `books` channel), the format is similar, but the `asks` and `bids` arrays will list only the levels that changed (with their new quantities). If a quantity is `"0"`, it means remove that level. Bitget also includes a **CRC32 checksum** in each message (`"checksum"` field) which is computed from the top 25 levels of the book. The engine can (optionally) use this to verify that its local book’s top-of-book matches the exchange’s (the checksum is calculated in the same manner as OKX/Binance: by concatenating top bids/asks prices and sizes). A mismatch would indicate a drop or out-of-sync issue, in which case a fresh snapshot may be needed. The presence of `"seq"` in messages also allows detection of missed updates (it increments with each update). For the partial channels (books5, etc.), every message is a snapshot so one can simply replace the previous data; for the full `books` channel, the engine must apply updates in order of seq. The Bitget docs note that the `"seq"` (sequence number) should be used to confirm no packets were lost or out-of-order.

* **Depth Configuration**: Bitget’s predefined channels cover **1, 5, or 15 levels** out-of-the-box. Those are fixed. If an application requires top-20 or top-100 on Bitget, there isn’t a direct channel for those; instead one would use the full `books` channel and then locally filter to the top N levels needed. The full `books` feed provides “all levels” (the documentation implies 400 levels similar to others, though in practice it will include all orders on the book). The engine can maintain, say, top-100 by simply not storing beyond 100 levels when applying diffs. It’s worth noting that Bitget’s REST API offers snapshots of certain depths (e.g., an endpoint for 20 or 100 levels) but via WebSocket the choices are 15 or full. If the requirement is exactly top-20, one could either take the 15-level feed (if approximate is acceptable) or handle full depth and trim to 20. Given our engine’s flexibility, supporting the full feed then outputting configurable top-N (5/20/100) is a straightforward approach.
* **Authentication**: **Not required for public market channels.** The `instType` and `channel` in the subscribe request tell the server which data to stream; no API keys or token needed for order book data. However, Bitget does have a **ping/pong** expectation: the client must send a `"ping"` message every 30 seconds to keep the connection alive. The server will reply with `"pong"`. If the client fails to ping within 2 minutes, the server disconnects. Additionally, Bitget will force-disconnect any WebSocket that has been open for 24 hours, so the engine should handle reconnections gracefully (and re-subscribe as needed). These are standard keepalive precautions in Bitget’s API.
* **Notable Quirks**: Bitget includes a **sequence number and checksum** in order book messages, which is a helpful feature. The engine should verify the continuity of `seq`: if a gap is detected (non-consecutive seq on `books` channel updates), it may indicate packet loss and the safe move is to resync (fetch a fresh snapshot via REST and then resume diffs). The checksum can also be computed on the maintained top 25 levels and compared to the `"checksum"` field as an integrity check. Another quirk is Bitget’s handling of **frequency**: the default 150ms might be reduced under high load, but generally it’s stable. Bitget’s API documentation explicitly instructs clients to send a ping every 30s (instead of relying on server pings), which is slightly different from some others that send ping frames from server side – so our engine’s connector for Bitget should implement that timer. Finally, the subscription format requires knowing the `instType` (market type) – our configuration needs to map symbols to their market type (e.g., `BTCUSDT` might exist in Spot, UM futures, etc., so we’d specify accordingly). If the wrong type is used, the subscription will fail or return no data. This is a Bitget-specific detail due to a unified endpoint for multiple markets.

## Bybit

* **WebSocket URL**: Bybit’s API v5 provides separate WebSocket endpoints for different product categories. For instance: **Spot** markets use `wss://stream.bybit.com/v5/public/spot`, **USDT perpetual and futures** use `wss://stream.bybit.com/v5/public/linear`, **Inverse contracts** use `.../public/inverse`, and **Options** use `.../public/option`. The engine should select the correct endpoint based on the instrument (Bybit keeps them distinct to optimize data). All these are public streams; there is also a `.../private` for user data (not needed for order books). Each endpoint can handle multiple subscriptions (for multiple symbols) as long as they belong to that category.
* **Subscription Format**: Bybit expects a JSON message with an operation and topics. The format is similar to OKX:

  ```json
  {
    "req_id": "optional_request_id",
    "op": "subscribe",
    "args": [ "orderbook.<depth>.<symbol>" ]
  }
  ```

  For example, to subscribe to the 50-level order book for BTCUSDT one would send `"args": ["orderbook.50.BTCUSDT"]`. Multiple topics can be subscribed in one go by including several strings in the args array (e.g. `["orderbook.50.BTCUSDT", "publicTrade.BTCUSDT"]` to get both order book and trade feed). The `depth` in the topic can be `1`, `50`, `200`, etc., depending on the market (see Depth options below). Bybit will respond with a message acknowledging the subscription (with `"success": true` and `"op": "subscribe"`). After that, the data messages will start streaming. Unsubscription uses `"op": "unsubscribe"` with similar args format. Bybit also uses `req_id` if you want to correlate the response to your request (not mandatory).
* **Updates vs Snapshots**: Bybit’s order book stream distinguishes **snapshot** vs **delta** messages. Upon subscribing, the **first** message for each book stream is a `"snapshot"` – a full dump of the order book for the requested depth. For example, if you subscribed to `orderbook.50.BTCUSDT`, you’ll initially receive 50 levels of bids and asks (aggregated). After that, Bybit will send **“delta”** updates whenever the book changes. A delta message contains only the differences since the last update (i.e., levels to update or remove). The `type` field in the message will be `"delta"` for those. The client must apply these deltas to its local copy of the book. Bybit’s rules for applying deltas: if an incoming entry’s size is 0, remove that price; if an entry doesn’t exist, add it; if it exists, update the size. This is standard procedure. Bybit also notes that if for any reason they send a new snapshot (e.g., service restart or no updates for a while), you should **discard the old book and replace it with the new snapshot**. In practice, for **level 1** (top book) subscriptions, Bybit will push a fresh snapshot every 3 seconds of inactivity as a heartbeat (with the same update ID as last, effectively). But for depth feeds (like 50, 200), you generally won’t get periodic snapshots unless there’s a reconnection – just the initial one. Our engine should be prepared to handle a snapshot type message at any time (rare event) by resetting that market’s order book.
* **Message Schema**: Bybit’s WebSocket data messages have the format: `{"topic": "orderbook.<depth>.<symbol>", "type": "<snapshot|delta>", "ts": <timestamp>, "data": { ... } }`. Inside `data`, for order books, we have: `"s"` (symbol), `"b"` (bids array), `"a"` (asks array), `"u"` (update ID), and `"seq"` (cross-sequence). The bids (`b`) and asks (`a`) arrays are lists of `[price, size]` pairs (both as strings). On a `"snapshot"` message, `b` and `a` contain the full snapshot for that depth. On a `"delta"` message, `b` and `a` contain only the changes (the same format, but typically much fewer entries). The `u` is an incremental update ID (Bybit uses it to ensure you apply deltas in order). The `seq` is a separate sequence number that can be used to synchronize data across different levels or channels; essentially it’s a monotonically increasing number across all order book events, regardless of depth. Bybit suggests using `seq` if you are comparing data between, say, a 50-level and a 200-level feed of the same market – the one with lower seq was generated earlier. For a single feed, the `u` (update ID) is sufficient for ordering. Additionally, `ts` is the server timestamp (ms) and `cts` (if provided) is the matching engine timestamp of that data, which can correlate to trade timestamps. Importantly, Bybit’s delta messages do not include a range like Binance’s U/u; instead, you ensure continuity by checking that each delta’s `u` is exactly one more than the previous delta’s `u` (for that topic). If it’s not, or if any doubt arises, Bybit recommends resubscribing to get a fresh snapshot. The engine should incorporate this logic.
* **Depth Configuration**: Bybit supports various depths depending on market type. For **linear and inverse contracts** (futures/perpetuals), available depths are **1, 50, 200, and 500** levels. For **spot** markets: depths of **1, 50, and 200** are available (500 is not offered for spot). For **options**: depths of **25 and 100** are offered. The subscription topic must include one of these allowed depth values. So, for example, `orderbook.200.BTCUSDT` (spot) or `orderbook.500.BTCUSD` (inverse perpetual) are valid, but `orderbook.20.XYZ` would not be accepted by the server. Our engine can be designed to request the nearest higher tier if an arbitrary N is needed (e.g., if N=100 for spot, subscribe to 200 and just use top 100 out of it). Typically, top-50 is a common choice for many trading systems; Bybit’s 50 comes at a nice 20ms push interval. If full depth is needed, 200 or 500 can be used, but note that the bandwidth and processing load grows with depth. The design can allow a configuration: for each exchange, choose which subscription depth to use based on desired N. The good news is Bybit will always send a snapshot at that depth on subscribe, so initialization is straightforward.
* **Authentication**: **Not required for public order book streams.** Bybit’s public endpoints for order books do not need an API key. The private endpoint is only for user-specific data. One consideration: Bybit enforces a limit on reconnections and subscriptions (to prevent abuse). If the engine subscribes to a large number of symbols, it should batch them in as few calls as possible (Bybit allows multiple topics in one subscribe message). Also, Bybit suggests sending a **ping** every 20 seconds if no message has been received in that time. In practice, Bybit’s server also sends ping frames (the documentation suggests the client maintain ping/pong as well, likely to cover network scenarios). The connection idle timeout is 30 seconds by default (Bybit will disconnect if neither data nor ping for 30s, though the docs mention one can configure `max_active_time` up to 10 minutes on private streams). For our purposes, sending a ping (`{"op":"ping"}` or a websocket ping frame) every 20s is a good practice. Bybit’s subscription acknowledgements and heartbeats can have request IDs to track, but those are optional.
* **Notable Quirks**: One quirk is that Bybit uses **separate connections per market type**, which means if our engine wants to consolidate, say, both spot and futures data, it actually has to open two WebSocket connections (one to the spot URL, one to linear/inverse URL). This is by design at Bybit for performance. Also, Bybit’s order book data is known to be high-frequency – e.g., 20ms for 50-level – so it will produce a lot of messages. Our engine’s parser should be efficient (which is part of the design anyway). Another detail: Bybit occasionally resets the book (sends a new snapshot) in the event of desync or system issues. Their documentation explicitly states that if you receive a `type: snapshot` out of the blue (after the initial), you must discard old data. So the engine must handle that gracefully. Bybit’s messages include **cross sequence (`seq`)** which is not common elsewhere; it can mostly be ignored unless one is aggregating multiple feeds. Finally, Bybit has a known quirk that for **linear/inverse Level 1**, if no changes for 3 seconds, it sends the same last update again as a heartbeat with identical update ID – so the engine should not treat that as a new change (since `u` doesn’t increase). It’s just a heartbeat. Summing up, Bybit’s feed is quite standard: initial snapshot + deltas, with clearly identified message types, and high throughput, so it aligns well with an engine focusing on top-N updates and full-depth maintenance.

## Gate.io

* **WebSocket URL**: Gate.io’s API v4 uses different WebSocket base URLs for spot and futures. For **spot**, the base is `wss://api.gateio.ws/ws/v4/` (the exact path may include region or be `wss://api.gateio.ws/ws/v4/<uid>` but the documentation shows this base). For **futures**, the base is `wss://fx-ws.gateio.ws/v4/ws/` (with perhaps a suffix like `/btc` or `/usdt` indicating the futures settlement currency). In practice, Gate provides multiple cluster domains; one should use the official endpoint provided in their docs. The engine might need to treat Gate’s spot and futures as separate connectors under the hood. (Gate’s “Unified” API still keeps them logically separate.) After connecting, Gate uses a channel/ event system for subscriptions.
* **Subscription Format**: Gate.io WebSocket subscriptions are invoked by sending a JSON with fields: `"time"` (a timestamp or nonce), `"channel"` (the channel name), `"event"` (`"subscribe"` or `"unsubscribe"`), and `"payload"` (an array of parameters). For the order book, Gate offers a few channels, but the recommended one is `spot.order_book_update` for spot or `futures.order_book_update` for futures. The `payload` for `order_book_update` includes the specific market and depth/frequency parameters. For example, to subscribe to **BTC\_USDT perpetual** order book with 20 levels at 20ms updates, one might send:

  ```json
  {
    "time": 123456789,
    "channel": "futures.order_book_update",
    "event": "subscribe",
    "payload": ["BTC_USDT", "20ms", "20"]
  }
  ```

  This would request the top-20 levels with 20ms update interval. Similarly, for top-100 levels at 100ms: `["BTC_USDT", "100ms", "100"]`. For spot markets, the channel would be `spot.order_book_update` and payload like `["BTC_USDT", "100ms", "100"]` (spot uses the same format, just different channel name). Gate’s system is flexible: the payload’s last element is the depth, and the middle element is the update interval. Gate supports intervals like `100ms`, `200ms`, `500ms`, `1s`, etc., and depth values like 5, 10, 20, 50, 100 (and even 0 or 400 for full depth). There are **constraints**: ultra-low interval **20ms is only allowed for smaller depths** (specifically 20 levels). For larger depths (50 or 100), the minimum interval is 100ms. Gate’s docs and announcements indicate options such as **50 levels @ 20ms** and **400 levels @ 100ms** for futures. The engine should be prepared to provide the appropriate payload based on desired N and frequency. After subscribing, Gate will send a confirmation (`"result": {"status": "success"}`) for the subscription request.
* **Updates vs Snapshots**: Gate.io’s `order_book_update` channel is designed for **incremental updates** with manual snapshot retrieval. Unlike some feeds, when you subscribe to `...order_book_update`, Gate does **not** immediately push a snapshot of the book. Instead, the client is expected to fetch an initial snapshot via the REST API and then apply the incoming WebSocket updates. The workflow is: *subscribe -> cache updates -> REST snapshot -> apply diff*. Gate explicitly documents this procedure. Each `order_book_update` message contains an incremental update (with a batch of level changes and associated update IDs). The client should accumulate these until it has the REST snapshot. The snapshot endpoint (e.g., GET `/spot/order_book?currency_pair=BTC_USDT&limit=20&with_id=true` for spot, or the futures equivalent) returns the current book and an `id` (order book ID). Using that `id`, the client finds the first cached WS message whose range covers it, and applies all subsequent messages. After that, real-time updates are in sync. The WebSocket messages themselves are labeled with an `"event": "update"` (for incremental update). Gate’s old channel `futures.order_book` provided full snapshots via `"all"` events, but it’s deprecated for real-time use. Instead, `order_book_update` pushes changes at specified intervals. Notably, the `payload` interval dictates how often updates are sent: e.g., with `"100ms"` interval, Gate will send at most one update every 100ms containing all changes in that window. If many changes occur, they may be bundled; if none occur, the channel might send a heartbeat update or simply no message until a change (in practice, Gate might send an empty update message to indicate no change, but typically they include at least a timestamp or something). The updates contain a range of update IDs (`U` and `u`) to indicate continuity. Summarily, Gate’s approach is similar to Binance’s diff stream but requires an explicit REST snapshot step (similar to KuCoin). The engine should incorporate snapshot fetching and merging logic for Gate feeds.
* **Message Schema**: A Gate `order_book_update` message (after subscription) is structured as follows:

  ```json
  {
    "channel": "spot.order_book_update", 
    "event": "update",
    "time": 1615366381,
    "time_ms": 1615366381417,
    "result": {
        "t": 1615366381417,         // timestamp of this update
        "e": "20ms",                // interval (echoes the subscription interval)
        "U": 27123456,             // First update ID in this batch
        "u": 27123460,             // Last update ID in this batch
        "bids": [ ["50000.12", "0.5"], ... ],
        "asks": [ ["50010.55", "1.2"], ... ]
    }
  }
  ```

  This is an illustrative structure (exact field names might differ slightly in actual API, e.g. `result` might directly contain arrays without nested keys named bids/asks, but from the docs: they do use `"bids"` and `"asks"` inside `"result"`). The key parts: **U** and **u** which mark the update ID range covered by this message, and arrays of **bids** and **asks** with their new aggregated volumes. Gate’s update IDs are comparable to Binance’s – the REST snapshot comes with an ID (sometimes called `last_id` or similar), and if that falls between U and u of an update message, that’s where you begin processing. The price levels in `"bids"`/`"asks"` are absolute updates (not deltas), meaning “set this price to this size”. A size of `"0"` means remove that price level. One important field is `"t"` (or `time_ms`): it gives the exact timestamp of the matching engine when the update was generated, which can be used to sequence among multiple messages with the same second. Gate also has a channel `book_ticker` (best bid/ask) if only top1 is needed, and a legacy `order_book` channel which would send a full snapshot as `"all"` event and then `"update"` events for every change (which results in very high traffic; hence not recommended). Our focus, `order_book_update`, is essentially a throttled diff channel. After each update is applied, the engine should update the local order book and, if needed, compute any top-N output. The presence of **U/u** means the engine should verify continuity: ideally, each message’s U should equal last applied u+1. If there’s a gap, it implies a missed message and the engine should resync (fetch a new snapshot). Gate’s messages do not include a separate sequence number beyond U/u (they do have `time_ms` which can serve to order messages if needed).
* **Depth Configuration**: Gate.io is **highly configurable** for depth. Clients can specify virtually any depth up to 400 (for futures) or the full book. Common choices and limits: as of recent updates, Gate supports **20 levels at 20ms** and **100 levels at 100ms** as popular combinations. They even mention “400-level” which presumably corresponds to full depth (they often refer to 400 as max depth) at 100ms. In practice, if an engine wants top-5 or top-10, it could request a smaller depth (e.g., payload \["BTC\_USDT","100ms","10"]). If it wants top-100, use \["...","100ms","100"]. For *full depth*, one could use depth `"0"` or `"400"` depending on the API (some docs show using `"0"` as depth to indicate full book). The design should allow specifying these parameters. Notably, Gate is introducing very fast 20ms feeds for spot as well (starting May 2025, spot `order_book_update` 20ms is available for 20 levels). This is comparable to other exchanges’ fastest feeds. If our engine needs top-20 with ultra-low latency, Gate can provide that. If we needed top-100, we’d go with 100ms updates as that’s the minimum for that depth. So in summary: Gate supports **N=20** at 20ms, **N=50** at 20ms (futures allowed 50\@20ms in one announcement, presumably spot might also allow 50\@20ms), **N=100** at 100ms, and full depth at >=100ms intervals. The engine can choose the appropriate subscription based on a config (for example, if user wants top-20, subscribe with 20\@20ms; if full book, maybe 100ms with depth 0/400).
* **Authentication**: **Public order book channels on Gate do not require auth.** One simply connects to the public WS endpoint and sends subscribe messages. Authentication is only needed for private user data channels. However, because Gate’s API is unified, we must ensure we connect to the correct base URL for the market (spot vs futures), as mentioned. Also, Gate has a **limit on the number of subscriptions per connection** (I believe around 100 subscriptions per connection). If our engine tracks many markets, we might need multiple connections or to use pattern subscriptions if supported. (Gate’s API allows subscription to multiple contracts in one payload array – e.g., payload could be `["BTC_USDT","ETH_USDT","100ms","20"]` maybe grouping multiple symbols, though the documentation examples usually show one symbol at a time. It’s not explicitly clear if multiple symbols can be in one subscribe for order\_book\_update; if not, we open one subscription per symbol). The engine should handle reconnect logic for Gate: if disconnected, we need to re-subscribe and re-fetch snapshot because the state is lost. Gate’s WS may send pings (the docs aren’t very specific in the snippet we have, but generally, either side can ping). As a precaution, the engine can send a ping frame periodically if the library supports it; Gate doesn’t specify a required interval in their docs, but keeping alive every \~30s is a safe strategy.
* **Notable Quirks**: Gate.io’s approach to order books is similar to Binance/KuCoin in requiring a REST snapshot for reliability. This adds complexity – our Gate connector must integrate an HTTP request and ensure it aligns with the WS messages. Another quirk is Gate’s **payload ordering**: in the payload array, the order of parameters matters (they didn’t use key/value JSON for parameters). For `order_book_update`, it’s `[<symbol>, <interval>, <depth>]`. For other channels like trades or tickers, it might just be `[<symbol>]`. The engine’s config for Gate should be aware of how to assemble this payload. Also, Gate’s channel names differ for spot vs futures (e.g., `"spot.order_book_update"` vs `"futures.order_book_update"`), and futures itself is divided by settlement (they have separate channels namespaces like `futures.usdt.order_book_update` possibly for USDT-margined vs `futures.btc` for coin-margined). We should confirm and use correct channel (from the snippet: they used `"channel": "futures.order_book_update"` and probably connected to a wss with `/btc` to indicate the futures type). It might require connecting to different sub-paths for USDT futures vs BTC (Coin) futures. In our design, we might treat each as separate “exchange” entries or handle it with configuration. Finally, Gate recently optimized their feeds to reduce traffic: rather than sending individual order changes, they aggregate within the interval. So one update message can contain multiple price changes. The engine must apply all entries in the `bids`/`asks` arrays for each update. The presence of `U` and `u` means each message covers a range of order book events. The engine should ensure no overlap or gap between consecutive messages. If a gap is detected (i.e., the next message’s `U` is greater than last `u+1`), it means some updates were missed – likely the connection had an issue or messages dropped. In that case, the safest recovery is to refetch the snapshot and resume. Gate’s feed with 20ms for 20 levels is one of the fastest in the industry; if we use it, we must ensure our system can keep up with \~50 updates per second per symbol. If not strictly needed, a 100ms interval might be chosen for more breathing room. The engine should make this configurable. In summary, Gate.io’s order book data is comprehensive and flexible but requires careful syncing. Once synced, applying diffs with sequence IDs is straightforward and similar to Binance/KuCoin logic, which our unified order book engine will handle by design.

**Sources:** Binance API documentation, KuCoin API documentation, OKX API/third-party docs, Bitget API docs, Bybit API documentation, Gate.io API docs and changelogs. The information reflects the state of these exchanges’ APIs as of mid-2025. Each exchange’s connector in the C++ engine will need to implement these nuances (URL, subscribe format, auth/ping as needed, diff handling) to support robust order book assembly across all six platforms.


**Summary**
 — for the six venues you’re integrating (Binance, KuCoin, OKX, Bitget, Bybit, Gate.io), the public market-data streams are *price-level* (L2) feeds, not individual‐order (L3) feeds.**

| Venue       | Public WebSocket “depth” channel(s)    | Granularity exposed                                          | Notes                                                                             |                                                  |
| ----------- | -------------------------------------- | ------------------------------------------------------------ | --------------------------------------------------------------------------------- | ------------------------------------------------ |
| **Binance** | `@depth`, `@depth<N>`, diff streams    | Aggregated size per *price level* (arrays of `[price, qty]`) | No order-ID data; individual orders never published.                              |                                                  |
| **KuCoin**  | `market/level2*` and `level2Depth5/50` | Aggregated size per level; deltas carry `[price, size]` only | KuCoin calls true order-ID feed “Level 3”, but that is *REST only* and throttled. |                                                  |
| **OKX**     | `books5`, `books`, `books*-l2-tbt`     | Arrays like `["px","sz", …]` per level                       | Even tick-by-tick (TBT) feeds show changes at *level* resolution.                 |                                                  |
| **Bitget**  | `books`, `books5/15/1`                 | Snapshot/delta of `[price, size]` per level                  | Includes `seq`/CRC, but no order IDs.                                             |                                                  |
| **Bybit**   | `orderbook.<depth>.<symbol>`           | Snapshot + deltas with `"b"` / `"a"` arrays `[price, size]`  | Depth choices 1/50/200/500; always aggregated.                                    |                                                  |
| **Gate.io** | \`spot                                 | futures.order\_book\_update\`                                | Incremental `[price, size]` per level with IDs `U/u`                              | Requires REST snapshot merge; still level-based. |

### What “order-based” (L3) would look like

A true L3 feed publishes *every* add / modify / cancel event with an `order_id`, size, side, and price (e.g., Coinbase Exchange’s **“full”** channel). None of the six exchanges above expose that in real-time to the public. Their matching engines aggregate all active orders at a price into a single size number before broadcasting.

### Why this matters for your engine

* **State model** – You only track the *total* size at each price. When a diff message arrives:

  * If `qty == 0` → remove level
  * Else → overwrite size for that price
* **Memory layout** – No per-order linked-list is required; an array/tree keyed by price plus aggregate size is sufficient (and cheaper).
* **Sequencing** – Each venue attaches sequence numbers (`U/u`, `seq`, `u`, etc.). You must ensure no gaps, but you never reconcile order IDs.
* **Top-N output** – Because feeds are already aggregated, computing the configurable top 5 / 20 / 100 levels is just a matter of keeping your internal container trimmed.

### Edge cases

* **Private order flows** – Some venues (e.g. KuCoin Level-3, OKX “order-channel” after login) let **your own orders** stream with IDs, but that’s separate from public depth.
* **Historical L3** – A few exchanges sell full-depth order history files, but they’re offline datasets.

---

**Bottom line:** treat every incoming depth message as *level-based*. Build your engine around price-level deltas + snapshot reconciliation; add optional logic only if you later integrate a true L3 source such as Coinbase’s “full” channel or Bitstamp’s “diff-order-book”.


# Crypto Exchange WebSocket API Code Examples (Binance, OKX, KuCoin, Bybit, Bitget)

In this guide, we present sample WebSocket API usage for **Binance, OKX, KuCoin, Bybit, and Bitget** – covering real-time **spot**, **margin**, and **derivatives** data streams. Each section provides code snippets (in JavaScript for clarity) demonstrating how to connect to public market data feeds (tickers, trades, order books) as well as how to handle private/user data streams (such as order updates and account changes) where applicable. The examples include connecting to the exchange’s WebSocket endpoints, subscribing to channels, and handling incoming messages in real time. Citations to official docs are provided for accuracy and further reference.

## Binance WebSocket API

Binance offers separate WebSocket endpoints for different products: **Spot (including margin)** and **Futures**. The base URL for Spot market streams is `wss://stream.binance.com:9443` (secure WebSocket). Binance Futures (USDⓈ-M perpetual futures) use a different base, e.g. `wss://fstream.binance.com` for USDT-M futures streams. You can connect to a single “raw” stream (e.g. `/ws/btcusdt@trade`) or use combined streams to subscribe to multiple symbols at once. Binance’s WebSocket API supports **public market data** (trades, book depth, tickers, etc.) via these streams, while **user-specific data** (orders, account updates) are accessed via a **User Data Stream** that requires API key authentication (via a **listenKey**).

### Spot Market Data Stream Example

To subscribe to Binance’s spot market data via WebSocket, you can either specify the stream in the URL or connect first and then send a subscription request. Below is a **Node.js example** that opens a WebSocket connection to Binance and then subscribes to two streams: the aggregate trade feed and depth feed for BTC/USDT. We use the Binance Spot base URL and send a JSON message with `method: "SUBSCRIBE"` and the desired stream names, as per Binance’s specification:

```js
const WebSocket = require('ws');
const ws = new WebSocket('wss://stream.binance.com:9443/ws');  // Spot base endpoint

ws.on('open', () => {
  // Subscribe to BTCUSDT aggregate trades and depth updates
  const subscribeMsg = {
    method: "SUBSCRIBE",
    params: ["btcusdt@aggTrade", "btcusdt@depth"],  // streams: aggTrade and depth
    id: 1
  };
  ws.send(JSON.stringify(subscribeMsg));  // send subscription request
});

ws.on('message', (data) => {
  const event = JSON.parse(data);
  console.log(event);
  // The event.data will contain real-time trade or order book info for BTCUSDT
});
```

In this snippet, once the WebSocket is open, we send a subscription message. Binance expects an `id` field and a `params` list of stream names. The server will respond with a confirmation (`result": null` if successful) and then start streaming the requested data continuously. (Note: You could also connect directly to a combined stream URL like `.../stream?streams=btcusdt@aggTrade/btcusdt@depth` to get the same result.)

### Futures Market Data Stream Example

For Binance **USDT-M futures**, the approach is similar but using the futures WebSocket endpoint. The base URL is `wss://fstream.binance.com` (for coin-margined futures it’s `wss://dstream.binance.com`). The subscription message format is the same. For example, to get BTCUSDT perpetual futures trades, you could connect to `wss://fstream.binance.com/ws` and then subscribe to `"btcusdt@trade"` or other futures stream names. The code structure would mirror the spot example above, just with the different URL. For instance:

```js
const wsFut = new WebSocket('wss://fstream.binance.com/ws');  // Futures base endpoint
wsFut.on('open', () => {
  wsFut.send(JSON.stringify({
    method: "SUBSCRIBE",
    params: ["btcusdt@trade", "btcusdt@depth20"],  // e.g. trades and 20-level depth
    id: 1
  }));
});
wsFut.on('message', msg => console.log(JSON.parse(msg)));
```

This would stream live trades and order book updates for the BTCUSDT perpetual contract. Binance’s futures streams use similar naming conventions (all lowercase symbol + @streamName).

### User Data Stream (Private Account Updates)

Binance’s user-specific events (e.g. order execution reports, balance updates) are delivered via a **User Data Stream**. To use it, you must first obtain a **listenKey** (a temporary token) through the REST API, then connect to `wss://stream.binance.com:9443/ws/<listenKey>`. Once connected, Binance will push events like `executionReport` (order updates), `outboundAccountPosition` (account balance changes), etc. for your account in real-time. Below is an example of connecting to the user stream after obtaining a listenKey (note: in practice you’d use the REST API to get the listenKey, which we assume is stored in `listenKey` variable):

```js
// Assume listenKey is a string obtained via POST /api/v3/userDataStream
const userWs = new WebSocket(`wss://stream.binance.com:9443/ws/${listenKey}`);
userWs.on('message', (data) => {
  const evt = JSON.parse(data);
  console.log(`User Stream Event: ${evt.e}`, evt);
  // e.g. evt.e might be "executionReport" for order updates or "outboundAccountPosition" for balance update
});
```

This will log any personal account events. For example, an `executionReport` event includes details of an order’s status (filled, canceled, etc.), and an `outboundAccountPosition` event shows balance changes (like funds moved from Spot to Margin). **Margin trading** on Binance is integrated into this same user data stream – for instance, if you have margin account updates, they will also appear in these events (e.g. balance updates when transferring between Spot and Margin are indicated by `balanceUpdate` events).

*Note:* Binance has recently introduced a new **WebSocket API** that can directly use API keys to authenticate and subscribe to user data without a listenKey (as of 2025). However, the above listenKey method is still commonly used (the new method uses a different endpoint and request format, and is beyond the scope of this simple example).

## OKX WebSocket API

OKX’s API (version 5) provides unified WebSocket endpoints for all its markets (spot, margin, futures, options). There are separate base URLs for **public** and **private** streams. For example, the public endpoint is `wss://ws.okx.com:8443/ws/v5/public` for market data, and the private endpoint is `wss://ws.okx.com:8443/ws/v5/private` for user data (trading/account updates).

**Public market data** channels do not require authentication – you can subscribe to tickers, trades, order books, etc. by sending a subscribe message after connecting. **Private channels** (like order updates) require a login message with your API key, passphrase, and a signed timestamp for authentication.

### Public Market Data Subscription Example

Here is a **JavaScript example** using OKX’s WebSocket to subscribe to a spot ticker stream. In this example, we subscribe to the BTC-USDT ticker channel (which provides real-time price updates). The code uses the public WebSocket endpoint and sends an `op: "subscribe"` request with the channel name and instrument ID as per OKX’s specification:

```js
const WebSocket = require('ws');
const ws = new WebSocket('wss://ws.okx.com:8443/ws/v5/public');  // OKX public WS endpoint

ws.on('open', () => {
  const subscribeMessage = {
    op: "subscribe",
    args: [
      {
        channel: "tickers",
        instId: "BTC-USDT"  // instrument ID for BTC/USDT spot market
      }
    ]
  };
  ws.send(JSON.stringify(subscribeMessage));
});

ws.on('message', (data) => {
  const json = JSON.parse(data);
  console.log(json);
  // Expected output includes tickers data for BTC-USDT (last price, 24h volume, etc.)
});
```

In this snippet, we connect to the public endpoint and, upon opening, send a message subscribing to the *tickers* channel for `instId: BTC-USDT`. OKX will then stream ticker updates for that trading pair. You can similarly subscribe to other channels by changing the `channel` and `instId` (for example, `channel: "trades"` for trade data, or `channel: "books"` for order book, as documented in OKX API). If multiple subscriptions are needed, you can include multiple objects in the `args` array in one message.

### Private (User Data) Subscription Example

To receive user-specific data such as order execution updates or account balance changes on OKX, you must authenticate with the private WebSocket. The authentication (“login”) message requires your API key, API passphrase, a timestamp, and a signature. The signature is a Base64-encoded HMAC SHA256 of the string: `<timestamp> + 'GET' + '/users/self/verify'` signed with your secret key. (This is per OKX API v5 spec for WebSocket login.)

For example, after computing the signature (not shown here), you would send a message like:

```js
// Pseudocode for login (after connecting to wss://ws.okx.com:8443/ws/v5/private)
const loginMsg = {
  op: "login",
  args: [{
    apiKey: API_KEY,
    passphrase: API_PASSPHRASE,
    timestamp: TIMESTAMP,        // current Unix timestamp (seconds)
    sign: SIGNATURE             // HMAC_SHA256 of "<timestamp>GET/users/self/verify"
  }]
};
wsPrivate.send(JSON.stringify(loginMsg));
```

Upon successful auth, the server responds with an event confirming login (e.g. `{"event":"login","code":"0","msg":""}`). After that, you can send subscribe requests for private channels like order updates. For instance, subscribing to order events might look like:

```js
wsPrivate.send(JSON.stringify({
  op: "subscribe",
  args: [{ channel: "orders", instType: "ANY", instId: "BTC-USDT" }]
}));
```

OKX private channels include order updates, account balance updates, position updates for futures, etc., which will stream in real-time once subscribed. (Refer to OKX API documentation for exact channel names and parameters for private subscriptions.)

**Note:** Ensure to maintain the connection with periodic pings as required by OKX (they typically expect a ping or will send one, and you should respond to avoid timeouts).

## KuCoin WebSocket API

KuCoin provides real-time data via WebSocket for its Spot, Margin, and Futures trading. KuCoin’s WebSocket workflow is a bit different: you must first **obtain a token** (and server endpoint) from KuCoin’s REST API (`/api/v1/bullet-public` for public feeds, or `/api/v1/bullet-private` for private feeds). The REST response will contain a WebSocket URL (endpoint) and a `token`. You then connect to that URL with the token as a query parameter. This token negotiation step is required even for public data. Once connected, you’ll receive a welcome message, after which you can subscribe to topics.

### Connecting and Subscribing to Market Data

After retrieving a public token, you will get an endpoint like `wss://ws-api.kucoin.com/endpoint`. Here’s an example (in JavaScript) of connecting to KuCoin’s public WebSocket and subscribing to a ticker feed for BTC-USDT:

```js
const WebSocket = require('ws');
// Normally, get token via POST https://api.kucoin.com/api/v1/bullet-public
const token = "<YOUR_PUBLIC_TOKEN>";  
const ws = new WebSocket(`wss://ws-api.kucoin.com/endpoint?token=${token}`);

ws.on('open', () => {
  console.log("KuCoin WebSocket connected.");
  // Subscribe to BTC-USDT ticker updates (public channel)
  const subMsg = {
    type: "subscribe",
    topic: "/market/ticker:BTC-USDT",  // topic for ticker of BTC-USDT
    privateChannel: false,
    response: true
  };
  ws.send(JSON.stringify(subMsg));
});

ws.on('message', (data) => {
  const msg = JSON.parse(data);
  console.log(msg);
  // The message will contain topic, subject, and data fields for the ticker.
});
```

In this snippet, after connecting (using the URL with the token), we send a JSON message with `type: "subscribe"` and the topic. KuCoin’s topics are prefixed by product and channel, e.g. `"/market/ticker:BTC-USDT"` subscribes to the ticker for that symbol. The server will start sending messages like `{"topic":"/market/ticker:BTC-USDT","data":{...}}` containing price, volume, etc.

You can subscribe to multiple symbols by comma-separating them in the topic (e.g. `"/market/ticker:BTC-USDT,BTC-USDC"`), or by sending additional subscribe messages. Keep an eye on the `id` or `response` fields if you need to confirm subscription acknowledgement.

### Private Channels (Orders and Accounts)

For private data (such as order events or balance updates), KuCoin requires a similar token process but using the *private* bullet endpoint. You must also include authentication details (API key, passphrase, etc.) when connecting or in the subscription message. KuCoin’s private topics (like `/account/balance` or order updates) require the WebSocket connection to be authenticated. Typically, the token response for private will include an **encrypted** `token` that you use, and you must set `privateChannel: true` in your subscribe message. Due to the complexity, many developers use KuCoin’s provided SDKs or the official guidelines.

For example, after getting a private token and connecting, a subscription to your order updates might look like:

```js
// Assuming wsPrivate is an authenticated WebSocket connection for private channels
wsPrivate.send(JSON.stringify({
  type: "subscribe",
  topic: "/spot/tradeOrders",   // hypothetical topic for your spot trade order updates
  privateChannel: true,
  response: true
}));
```

KuCoin would then push messages whenever your orders are executed or status changes. (Refer to KuCoin’s docs for the exact topic paths; e.g. `/spot/tradeOrders` or futures equivalents, and ensure your subscribe message includes the required signature headers if any. The **KuCoin API docs** outline the needed HMAC signature in the connection URL for private channels.)

*Note:* KuCoin’s WebSocket sends periodic ping frames and expects a pong; you should handle these to keep the connection alive. The welcome message will contain a `pingInterval` value (e.g., 18000 ms) after which you should respond with a pong or send a heartbeat to avoid disconnection.

## Bybit WebSocket API

Bybit’s API (v5) unifies Spot, Derivatives (Inverse and Linear perpetuals/futures), and Options under a common structure. Bybit uses distinct WebSocket endpoints for different categories of public market data, for example:

* **Spot public:** `wss://stream.bybit.com/v5/public/spot`
* **Linear contracts (USDT perpetual/futures):** `wss://stream.bybit.com/v5/public/linear`
* **Inverse contracts:** `wss://stream.bybit.com/v5/public/inverse`
* (Testnet endpoints exist with a similar pattern on `stream-testnet` domains.)

Public channels (market data) require no auth; private streams (such as user orders) require authentication via an `op: "auth"` message using your API key and a signature.

### Spot Market Data Subscription Example

Below is a Node.js example connecting to Bybit’s spot public stream and subscribing to two topics: the public trades feed and the Level-1 orderbook for BTCUSDT. The subscription format uses an array of topic strings in the `args` field of the request:

```js
const WebSocket = require('ws');
const ws = new WebSocket('wss://stream.bybit.com/v5/public/spot');

ws.on('open', () => {
  // Subscribe to BTCUSDT public trade stream and orderbook (depth 1) stream
  const subscribeReq = {
    op: "subscribe",
    args: [
      "publicTrade.BTCUSDT",    // real-time trades for BTCUSDT
      "orderbook.1.BTCUSDT"     // full order book (level 1) for BTCUSDT
    ]
  };
  ws.send(JSON.stringify(subscribeReq));
});

ws.on('message', (data) => {
  const msg = JSON.parse(data);
  if (msg.topic) {
    console.log(`Bybit message on topic ${msg.topic}:`, msg.data);
  } else {
    console.log(msg);
  }
});
```

This will print updates for every trade (`publicTrade.BTCUSDT`) and order book change (`orderbook.1.BTCUSDT`). Bybit’s topics are strings combining the data type and instrument. For example, `"publicTrade.BTCUSDT"` is the trades stream and `"orderbook.1.BTCUSDT"` is the order book at 1-depth (you can also subscribe to deeper order book snapshots, e.g. `orderbook.50.BTCUSDT` for 50 levels). Ticker data would be under a `tickers` topic (e.g. `"tickers.BTCUSDT"`). The Bybit server will respond with a subscription confirmation (`{"success": true, "op": "subscribe", ...}`), and then start sending messages with `topic` and `data`.

### Private WebSocket (Authentication and User Streams)

For Bybit’s private WebSocket (user data), you must authenticate after connecting to the private endpoint (`wss://stream.bybit.com/v5/private` for unified account, or the appropriate URL for your account type). Authentication is done by sending an `{"op":"auth","args":[apiKey, expires, signature]}` message. The `signature` is a HEX digest (not base64 in this case) of an HMAC SHA256 on a string composed of e.g. `GET/realtime` + expires (for v5 this may differ; check Bybit’s latest docs). Upon successful auth, you will get a response with `op: "auth", success: true`.

After logging in, you can subscribe to private topics. Bybit’s private topics include order events, position updates, wallet balance changes, etc. For example, to subscribe to your account’s order updates on spot, you might send:

```js
wsPrivate.send(JSON.stringify({
  op: "subscribe",
  args: ["order", "stopOrder"]  // subscribe to active order and stop order updates (example)
}));
```

(Actual topic strings for private feeds should be verified from Bybit’s documentation; they often look like `"order"`, `"execution"`, `"position"` for derivatives, etc., without symbol since they apply to your whole account.)

**Bybit Ping/Pong:** Bybit requires ping messages to keep the connection alive. You should send `{"op":"ping"}` every 20 seconds (as recommended); the server will respond with a `pong` message indicating the connection is alive.

## Bitget WebSocket API

Bitget provides WebSocket streams for all its markets: spot, margin, futures (USDT-M, coin-M), etc., using a unified API. The base WebSocket endpoint is `wss://ws.bitget.com/mix/v1/stream` for their unified **Mix** service. Once connected, you can subscribe to public channels (market data) and, if needed, authenticate and subscribe to private channels (user data). Bitget’s subscription model uses an `op` field with values like `"subscribe"`/`"unsubscribe"` and an `args` list containing objects with `instType`, `channel`, and `instId` (instrument) identifiers.

### Example: Login and Subscribe (Spot Ticker)

Below is a **JavaScript example** that connects to Bitget’s WebSocket, performs authentication (for private access), then subscribes to a public ticker channel for BTCUSDT. This example uses Node's crypto library to create the required signature. Bitget’s login requires an HMAC SHA256 signature of the string `<timestamp>GET/user/verify` (note timestamp is in seconds) encoded in Base64. We then send a login message and, upon success, a subscribe message:

```js
const WebSocket = require('ws');
const crypto = require('crypto');

// Your API credentials
const apiKey = 'YOUR_API_KEY';
const secretKey = 'YOUR_API_SECRET';
const passphrase = 'YOUR_PASSPHRASE';

// Prepare authentication params
const timestamp = Math.floor(Date.now() / 1000).toString();  // seconds
const method = 'GET';
const requestPath = '/user/verify';
const prehash = timestamp + method + requestPath;
const signature = crypto.createHmac('sha256', secretKey).update(prehash).digest('base64');

const ws = new WebSocket('wss://ws.bitget.com/mix/v1/stream');

ws.on('open', () => {
  // 1. Send login (auth) request
  const authMsg = {
    op: "login",
    args: [{
      apiKey: apiKey,
      passphrase: passphrase,
      timestamp: timestamp,
      sign: signature
    }]
  };
  ws.send(JSON.stringify(authMsg));

  // 2. Subscribe to BTCUSDT spot ticker after auth (or for public-only, this can be sent immediately)
  const subMsg = {
    op: "subscribe",
    args: [{
      instType: "SPOT",     // instrument type: SPOT market
      channel: "ticker",    // subscribe to ticker channel
      instId: "BTCUSDT"     // instrument ID (trading pair symbol)
    }]
  };
  ws.send(JSON.stringify(subMsg));
});

ws.on('message', (data) => {
  console.log(`Received: ${data}`);
});
```

In this snippet, we first calculate the signature as per Bitget’s requirements (timestamp + "GET" + "/user/verify", HMAC-SHA256, Base64). Then we open the WebSocket to Bitget. On open, we send a `"login"` message with our API credentials. Once logged in (the server will respond with an event `"login"` and code 0 for success), we send a `"subscribe"` request for the **spot ticker** channel of BTCUSDT. The `instType` field specifies the market type (here `"SPOT"`; other values could be `"MARGIN"`, `"USDT-FUTURES"` for futures, etc. as documented), and `channel` is the data type (ticker, trades, candlesticks, etc.). After subscribing, the server will stream ticker updates for BTCUSDT (price, bid/ask, 24h change, etc.) every 100-300ms as per Bitget’s ticker channel spec.

For subscribing to other data, you can adjust `channel` and `instType`. For example, to get futures trades, you might use `instType: "USDT-FUTURES"` and `channel: "trade"` with an appropriate `instId` (contract symbol). The process (login then subscribe) remains the same. If only public data is needed, the `login` step can be skipped – you can connect and subscribe to public channels without authentication.

**Note:** Bitget enforces limits like max 10 messages per second and recommends subscribing to at most 50 channels per connection for stability. Also, Bitget expects heartbeat pings; if you receive a `ping` (often just a `"ping"` string), respond with a `"pong"` to keep the connection alive.

---

**Conclusion:** Each of these exchanges provides robust WebSocket APIs for getting real-time market data and user account updates. The general pattern is: **connect to the appropriate WebSocket endpoint, then send a subscription or authentication message as needed**. Spot vs. derivatives are often separated by endpoint or parameters (as shown for each exchange). By using the above examples as a starting point – and adjusting for the specific channels or symbols you need – you can stream live crypto market data and trading updates from Binance, OKX, KuCoin, Bybit, and Bitget. Always refer to the latest official documentation for each exchange (linked in citations) for detailed parameters, as APIs are updated frequently.

**Sources:**

* Binance WebSocket API – Binance Academy & Official Docs
* OKX WebSocket v5 – Official Guide and API Reference
* KuCoin WebSocket API – Official Documentation & StackOverflow example
* Bybit WebSocket API v5 – Official Docs (GitHub)
* Bitget WebSocket API – Official Docs & Integration Guide
