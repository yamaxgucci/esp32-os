"""What the "find boards" button actually does, in a real Chrome.

Run it with the shop up: python tools\\diag-find.py --base http://127.0.0.1:8090

Nobody can click a Bluetooth chooser from a script, but everything *around* the
chooser can be driven: does the page's module load, does the handler run, what
does the browser have to offer, and - with `requestDevice` stubbed to answer
"here is a board" - does the rest of the introduction work end to end (the
shop told, the board handed the shop's address).

    python build\\diag-find.py --base http://127.0.0.1:8090
"""
from __future__ import annotations

import argparse
import sys

FAKE = r"""
// A board as the chooser would hand it over: a name and an id.  There is no
// GATT server behind it, which is fine - the code has to survive that too.
Object.defineProperty(navigator, "bluetooth", {
  configurable: true,
  value: {
    requestDevice: async (opts) => {
      window.__askedFilters = JSON.stringify(opts);
      return { id: "diag-board-1", name: "Argon-generic", gatt: {
        connect: async () => { throw new Error("no gatt in a diagnostic"); },
        disconnect: () => {},
      } };
    },
  },
});
"""

SERIAL = r"""
// A serial port whose console carries the captured beacon, so the cable path is
// exercised without a cable.
// No Bluetooth in this run: the point is to reach the cable path, and a real
// chooser in a headless browser never answers.
Object.defineProperty(navigator, "bluetooth", { configurable: true, value: undefined });
const BEACON = "\x1b[20;1Hargon-store here\x1b[K\x1b[21;1H  id = diag-serial-1" +
  "\x1b[K\x1b[22;1H  chip = esp32s3\x1b[K\x1b[23;1H  abi = 0.71\x1b[K" +
  "\x1b[24;1H  name = generic\x1b[K\x1b[25;1H\x1b[K";
Object.defineProperty(navigator, "serial", {
  configurable: true,
  value: {
    requestPort: async () => {
      let served = false;
      const bytes = new TextEncoder().encode(BEACON);
      const written = [];
      return {
        open: async () => {},
        close: async () => {},
        readable: {
          getReader: () => ({
            read: async () => {
              if (served) return { value: null, done: false };
              served = true;
              return { value: bytes, done: false };
            },
            cancel: async () => {},
            releaseLock: () => {},
          }),
        },
        writable: {
          getWriter: () => ({
            write: async (chunk) => { written.push(new TextDecoder().decode(chunk)); },
            releaseLock: () => {},
          }),
        },
        __written: written,
      };
    },
  },
});
"""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://127.0.0.1:8090")
    ap.add_argument("--email", default="owner@example.invalid")
    ap.add_argument("--password", default="e2e-owner")
    ap.add_argument("--shot", default="build/web-shots/find-diag.png")
    args = ap.parse_args(argv)

    from playwright.sync_api import sync_playwright

    with sync_playwright() as pw:
        browser = pw.chromium.launch(channel="chrome", headless=True)
        try:
            for name, stub in (("no stubs", ""), ("fake bluetooth", FAKE),
                               ("fake serial", SERIAL)):
                ctx = browser.new_context()
                if stub:
                    ctx.add_init_script(stub)
                page = ctx.new_page()
                errors: list[str] = []
                page.on("pageerror", lambda e: errors.append("pageerror: %s" % e))
                page.on("console", lambda m: errors.append("console.%s: %s"
                                                           % (m.type, m.text)))
                page.on("response", lambda r: errors.append(
                    "http %d %s" % (r.status, r.url)) if r.status >= 400 else None)
                # What the page tells the shop, without needing to read the
                # result out of a page that redirects a moment later.
                posts: list[str] = []
                page.on("request", lambda r: posts.append(
                    "%s %s" % (r.method, r.url))
                        if "/api/device/web/found" in r.url else None)
                page.goto(args.base + "/login")
                page.fill("input[name=email]", args.email)
                page.fill("input[name=password]", args.password)
                page.click("button[type=submit]")
                page.wait_for_load_state()
                page.goto(args.base + "/devices")

                has = page.evaluate(
                    "({ble: !!navigator.bluetooth, serial: !!navigator.serial,"
                    " secure: window.isSecureContext})")
                print("== %s" % name)
                print("   browser: %s" % has)
                button = page.query_selector("form[data-find] button")
                print("   find button present: %s" % bool(button))
                if button:
                    button.click()
                    page.wait_for_timeout(7000)
                    status = page.evaluate(
                        "document.querySelector('#find-status')?.textContent")
                    print("   status: %r" % status)
                    shown = page.evaluate("location.pathname + location.search")
                    print("   at: %s" % shown)
                print("   told the shop: %s" % (posts or "nothing"))
                for line in errors[:4]:
                    print("   %s" % line)
                page.screenshot(path=args.shot)
                ctx.close()
        finally:
            browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
