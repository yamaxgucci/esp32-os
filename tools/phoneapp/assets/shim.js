/*
 * The board's page, with Bluetooth where its WebSocket was.
 *
 * The page (apps/phone/web/index.html, copied in unchanged by build.py) opens
 * `new WebSocket(...)` and speaks PHONE.SYS's messages over it.  Loaded as
 * term.html?ble=<address>&name=<name>, this replaces WebSocket with a socket
 * whose far end is the app's Bluetooth link (MainActivity, "ArgonBle"), so the
 * page itself cannot tell and needs no second copy for the app.
 *
 * What a WebSocket does that has to be imitated:
 *   onopen when the link is ready (subscribed on the board, not just connected);
 *   onmessage with an ArrayBuffer per message;
 *   onclose with a reason - "replaced" and "badpass" mean something to the
 *   page, and over BLE they arrive as the board's 'Q' message.
 */
(function () {
  "use strict";
  const q = new URLSearchParams(location.search);
  const addr = q.get("ble");
  if (!addr || !window.ArgonBle) return;

  /* The page keys its remembered password by this, so each board has its own. */
  window.ARGON_BOARD = q.get("name") || addr;

  const OP_QUIT = 0x51; /* 'Q' */
  let current = null;

  function toB64(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i += 0x8000) {
      s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    }
    return btoa(s);
  }

  function fromB64(b) {
    const s = atob(b);
    const u8 = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) u8[i] = s.charCodeAt(i);
    return u8;
  }

  class BleSocket {
    constructor(url) {
      this.url = url;
      this.readyState = 0; /* CONNECTING */
      this.binaryType = "arraybuffer";
      this.onopen = null;
      this.onmessage = null;
      this.onclose = null;
      this.onerror = null;
      if (current && current !== this) current._end("");
      current = this;
      window.ArgonBle.connect(addr);
    }

    send(data) {
      if (this.readyState !== 1) return;
      let u8;
      if (data instanceof Uint8Array) u8 = data;
      else if (data instanceof ArrayBuffer) u8 = new Uint8Array(data);
      else if (ArrayBuffer.isView(data)) u8 = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      else u8 = new TextEncoder().encode(String(data));
      window.ArgonBle.send(toB64(u8));
    }

    close() {
      /* The page's own close: the radio link stays up (see ArgonBle.close). */
      if (this.readyState >= 2) return;
      window.ArgonBle.close();
      this._end("");
    }

    _open() {
      if (this.readyState !== 0) return;
      this.readyState = 1;
      if (this.onopen) this.onopen({ type: "open" });
    }

    _end(reason) {
      if (this.readyState === 3) return;
      this.readyState = 3;
      if (current === this) current = null;
      const ev = { type: "close", code: 1000, reason: reason, wasClean: true };
      /* Asynchronously, as a browser does: the page may still be inside the
       * call that closed it. */
      setTimeout(() => { if (this.onclose) this.onclose(ev); }, 0);
    }
  }
  BleSocket.CONNECTING = 0;
  BleSocket.OPEN = 1;
  BleSocket.CLOSING = 2;
  BleSocket.CLOSED = 3;

  window.WebSocket = BleSocket;

  /* The app's side calls these. */
  window.__argonLink = {
    open: function () {
      if (current) current._open();
      window.ArgonBle.keepScreenOn(true);
    },
    message: function (b64) {
      const s = current;
      if (!s || s.readyState !== 1) return;
      const u8 = fromB64(b64);
      if (u8.length > 0 && u8[0] === OP_QUIT) {
        s._end(new TextDecoder().decode(u8.subarray(1)));
        return;
      }
      if (s.onmessage) s.onmessage({ data: u8.buffer });
    },
    closed: function (reason) {
      window.ArgonBle.keepScreenOn(false);
      if (current) current._end(reason || "");
    }
  };
})();
