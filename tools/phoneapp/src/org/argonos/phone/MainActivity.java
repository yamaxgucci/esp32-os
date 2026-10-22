/*
 * ArgonOS on a phone - the Android side.
 *
 * All of the screen is the board's own page (apps/phone/web/index.html), shown
 * in a WebView exactly as a browser shows it over Wi-Fi.  What this file adds
 * is the one thing a page cannot do on its own without a secure origin and a
 * browser that allows it: talk Bluetooth.  The page opens a WebSocket; in this
 * app that WebSocket is a shim (assets/shim.js) whose other end is here, and
 * the bytes that cross are the same messages PHONE.SYS sends over TCP.
 *
 * The wire, as apps/phone/phone.c describes it:
 *   service 0xfff0; a write to 0xfff2 is one message, phone to board;
 *   notifications on 0xfff1 are pieces of messages, board to phone: a flag
 *   byte (0x40 first piece, 0x80 last) and the bytes.
 *
 * Built without Gradle by tools/phoneapp/build.py - the SDK's own tools, no
 * library outside android.jar, so nothing here has to be fetched to build.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
package org.argonos.phone;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Base64;
import android.util.Log;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

public class MainActivity extends Activity {
    private static final String TAG = "ArgonPhone";

    /* The app's own pages are served from here, so they are a secure origin
     * (the page's storage works) and not file:// (which WebView treats as
     * every file being its own origin). */
    private static final String HOST = "argon.app";
    private static final String START = "https://" + HOST + "/start.html";

    private static final UUID SVC = uuid16(0xfff0);
    private static final UUID CHR_IN = uuid16(0xfff1);  /* board to phone */
    private static final UUID CHR_OUT = uuid16(0xfff2); /* phone to board */
    private static final UUID CCCD = uuid16(0x2902);

    private static final int FIRST = 0x40;
    private static final int LAST = 0x80;

    private static final int REQ_PERMS = 1;

    private WebView web;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private boolean scanning;
    private boolean scanWanted; /* asked for before the permissions came */

    private BluetoothGatt gatt;
    private String gattAddr;
    private BluetoothGattCharacteristic out;
    private boolean ready;      /* subscribed: the page may be told "open" */
    private int mtu = 23;

    private final ArrayDeque<byte[]> sendq = new ArrayDeque<>();
    private boolean writing;

    private final ByteArrayOutputStream piece = new ByteArrayOutputStream();
    private boolean inMsg;

    private static UUID uuid16(int v) {
        return UUID.fromString(String.format("0000%04x-0000-1000-8000-00805f9b34fb", v));
    }

    /* ------------------------------------------------------------------ */
    /* The WebView                                                         */
    /* ------------------------------------------------------------------ */

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        WebView.setWebContentsDebuggingEnabled(true); /* chrome://inspect */

        BluetoothManager bm = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = (bm != null) ? bm.getAdapter() : null;

        web = new WebView(this);
        WebSettings ws = web.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setMediaPlaybackRequiresUserGesture(false);
        ws.setTextZoom(100); /* the page lays itself out; the phone's font size would break it */
        web.setWebChromeClient(new WebChromeClient());
        web.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView v, WebResourceRequest r) {
                Uri u = r.getUrl();
                if (!HOST.equals(u.getHost())) {
                    return null; /* a board's own page over Wi-Fi: the network's */
                }
                return asset(u.getPath());
            }
        });
        web.addJavascriptInterface(new Bridge(), "ArgonBle");
        setContentView(web);
        web.loadUrl(START);
    }

    private WebResourceResponse asset(String path) {
        if (path == null || path.equals("/") || path.isEmpty()) {
            path = "/start.html";
        }
        String name = path.substring(1);
        String mime = "application/octet-stream";
        if (name.endsWith(".html")) mime = "text/html";
        else if (name.endsWith(".js")) mime = "application/javascript";
        else if (name.endsWith(".css")) mime = "text/css";
        else if (name.endsWith(".png")) mime = "image/png";
        try {
            InputStream in = getAssets().open(name);
            return new WebResourceResponse(mime, "utf-8", in);
        } catch (IOException e) {
            WebResourceResponse r = new WebResourceResponse("text/plain", "utf-8", null);
            r.setStatusCodeAndReasonPhrase(404, "Not Found");
            return r;
        }
    }

    /* Tell the page something.  Always on the UI thread: Bluetooth calls back
     * on a binder thread, and a WebView touched from there throws. */
    private void js(final String code) {
        ui.post(new Runnable() {
            public void run() {
                if (web != null) web.evaluateJavascript(code, null);
            }
        });
    }

    private static String q(String s) {
        if (s == null) return "null";
        StringBuilder b = new StringBuilder("\"");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c < 0x20 || c > 0x7e) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        return b.append('"').toString();
    }

    @Override
    public void onBackPressed() {
        /* Back from a board is back to the list, not out of the app: the list
         * is where the next board is. */
        String url = web.getUrl();
        if (url != null && !url.startsWith(START)) {
            disconnect();
            web.loadUrl(START);
            return;
        }
        super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        stopScan();
        disconnect();
        super.onDestroy();
    }

    /* ------------------------------------------------------------------ */
    /* What the pages can ask for                                          */
    /* ------------------------------------------------------------------ */

    private class Bridge {
        @JavascriptInterface
        public boolean available() {
            return adapter != null;
        }

        @JavascriptInterface
        public void scan() {
            ui.post(new Runnable() { public void run() { startScan(); } });
        }

        @JavascriptInterface
        public void stopScan() {
            ui.post(new Runnable() { public void run() { MainActivity.this.stopScan(); } });
        }

        @JavascriptInterface
        public void connect(final String addr) {
            ui.post(new Runnable() { public void run() { MainActivity.this.connect(addr); } });
        }

        /* The page closed its socket.  The radio link stays: the board keeps
         * it, and the page's next socket is a hello away, not a reconnect. */
        @JavascriptInterface
        public void close() {
        }

        @JavascriptInterface
        public void disconnect() {
            ui.post(new Runnable() { public void run() { MainActivity.this.disconnect(); } });
        }

        @JavascriptInterface
        public void send(String b64) {
            final byte[] msg = Base64.decode(b64, Base64.NO_WRAP);
            ui.post(new Runnable() { public void run() { queue(msg); } });
        }

        @JavascriptInterface
        public void keepScreenOn(final boolean on) {
            ui.post(new Runnable() {
                public void run() {
                    if (on) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                }
            });
        }
    }

    /* ------------------------------------------------------------------ */
    /* Permissions                                                         */
    /* ------------------------------------------------------------------ */

    private String[] neededPerms() {
        List<String> want = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 31) {
            want.add(Manifest.permission.BLUETOOTH_SCAN);
            want.add(Manifest.permission.BLUETOOTH_CONNECT);
        } else {
            want.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        List<String> missing = new ArrayList<>();
        for (String p : want) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        }
        return missing.toArray(new String[0]);
    }

    private boolean havePerms() {
        String[] missing = neededPerms();
        if (missing.length == 0) return true;
        requestPermissions(missing, REQ_PERMS);
        return false;
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        if (code != REQ_PERMS) return;
        if (neededPerms().length != 0) {
            js("window.onScanError && onScanError(" + q("Bluetooth permission was refused") + ")");
            return;
        }
        if (scanWanted) startScan();
    }

    /* ------------------------------------------------------------------ */
    /* Finding a board                                                     */
    /* ------------------------------------------------------------------ */

    private final ScanCallback scanCb = new ScanCallback() {
        @Override
        public void onScanResult(int type, ScanResult r) {
            found(r);
        }

        @Override
        public void onBatchScanResults(List<ScanResult> rs) {
            for (ScanResult r : rs) found(r);
        }

        @Override
        public void onScanFailed(int err) {
            scanning = false;
            js("window.onScanError && onScanError(" + q("scan failed (" + err + ")") + ")");
        }
    };

    private void found(ScanResult r) {
        BluetoothDevice d = r.getDevice();
        ScanRecord rec = r.getScanRecord();
        String name = (rec != null) ? rec.getDeviceName() : null;
        js("window.onBoard && onBoard(" + q(d.getAddress()) + "," + q(name) + "," + r.getRssi() + ")");
    }

    private void startScan() {
        scanWanted = true;
        if (adapter == null) {
            js("window.onScanError && onScanError(" + q("this phone has no Bluetooth") + ")");
            return;
        }
        if (!havePerms()) return;
        if (!adapter.isEnabled()) {
            try {
                startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
            } catch (SecurityException e) {
                /* permissions raced; the page says so below */
            }
            js("window.onScanError && onScanError(" + q("Bluetooth is off - turn it on and search again") + ")");
            return;
        }
        if (scanning) return;
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            js("window.onScanError && onScanError(" + q("Bluetooth is not ready") + ")");
            return;
        }
        List<ScanFilter> filters = new ArrayList<>();
        filters.add(new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SVC)).build());
        ScanSettings set = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        try {
            scanner.startScan(filters, set, scanCb);
            scanning = true;
            js("window.onScanStarted && onScanStarted()");
        } catch (SecurityException e) {
            js("window.onScanError && onScanError(" + q("Bluetooth permission was refused") + ")");
        }
        /* A scan left running drains the battery and slows a connection down;
         * fifteen seconds finds anything that is advertising. */
        ui.postDelayed(new Runnable() { public void run() { stopScan(); } }, 15000);
    }

    private void stopScan() {
        scanWanted = false;
        if (!scanning || scanner == null) return;
        try {
            scanner.stopScan(scanCb);
        } catch (SecurityException | IllegalStateException e) {
            /* the radio went off under it: stopped anyway */
        }
        scanning = false;
        js("window.onScanStopped && onScanStopped()");
    }

    /* ------------------------------------------------------------------ */
    /* The link                                                            */
    /* ------------------------------------------------------------------ */

    private void connect(String addr) {
        stopScan();
        if (gatt != null && addr.equalsIgnoreCase(gattAddr)) {
            if (ready) {
                js("window.__argonLink && __argonLink.open()");
            }
            return; /* already there, or on the way: open() comes when it is */
        }
        disconnect();
        if (adapter == null || !havePerms()) {
            js("window.__argonLink && __argonLink.closed(" + q("no bluetooth") + ")");
            return;
        }
        try {
            BluetoothDevice d = adapter.getRemoteDevice(addr);
            gattAddr = addr;
            gatt = d.connectGatt(this, false, gattCb, BluetoothDevice.TRANSPORT_LE);
        } catch (IllegalArgumentException | SecurityException e) {
            gatt = null;
            js("window.__argonLink && __argonLink.closed(" + q("cannot connect: " + e.getMessage()) + ")");
        }
    }

    private void disconnect() {
        ready = false;
        writing = false;
        sendq.clear();
        inMsg = false;
        piece.reset();
        out = null;
        if (gatt != null) {
            try {
                gatt.disconnect();
                gatt.close();
            } catch (SecurityException e) {
                /* nothing to do about it */
            }
            gatt = null;
        }
        gattAddr = null;
    }

    private void lost(final BluetoothGatt g, final String why) {
        ui.post(new Runnable() {
            public void run() {
                if (g != gatt) return; /* an old link, already replaced */
                disconnect();
                js("window.__argonLink && __argonLink.closed(" + q(why) + ")");
            }
        });
    }

    private final BluetoothGattCallback gattCb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int state) {
            if (state == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                try {
                    g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
                    /* Big pieces: a console row is ~160 bytes, and at the
                     * default 20 it would be nine notifications instead of one. */
                    if (!g.requestMtu(517)) g.discoverServices();
                } catch (SecurityException e) {
                    lost(g, "permission refused");
                }
            } else if (state == BluetoothProfile.STATE_DISCONNECTED) {
                lost(g, status == BluetoothGatt.GATT_SUCCESS ? "" : "link lost (" + status + ")");
            }
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int m, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) mtu = m;
            try {
                g.discoverServices();
            } catch (SecurityException e) {
                lost(g, "permission refused");
            }
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            BluetoothGattService s = g.getService(SVC);
            BluetoothGattCharacteristic in = (s != null) ? s.getCharacteristic(CHR_IN) : null;
            BluetoothGattCharacteristic o = (s != null) ? s.getCharacteristic(CHR_OUT) : null;
            if (in == null || o == null) {
                lost(g, "not an ArgonOS board");
                return;
            }
            out = o;
            try {
                g.setCharacteristicNotification(in, true);
                BluetoothGattDescriptor d = in.getDescriptor(CCCD);
                if (d == null) {
                    lost(g, "the board cannot notify");
                    return;
                }
                d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                g.writeDescriptor(d);
            } catch (SecurityException e) {
                lost(g, "permission refused");
            }
        }

        @Override
        public void onDescriptorWrite(final BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                lost(g, "subscribe failed (" + status + ")");
                return;
            }
            ui.post(new Runnable() {
                public void run() {
                    if (g != gatt) return;
                    ready = true;
                    js("window.__argonLink && __argonLink.open(" + (mtu - 3) + ")");
                    pump();
                }
            });
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            ui.post(new Runnable() {
                public void run() {
                    writing = false;
                    pump();
                }
            });
        }

        /* Android 13 and later call this one... */
        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v) {
            onPiece(v);
        }

        /* ...and older ones this.  Never both: the new one's default is what
         * calls the old, and it is overridden above. */
        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            onPiece(c.getValue());
        }
    };

    /* One notification: a piece of a message.  Whole messages go to the page. */
    private synchronized void onPiece(byte[] v) {
        if (v == null || v.length < 1) return;
        int flags = v[0] & 0xff;
        if ((flags & FIRST) != 0) {
            piece.reset(); /* whatever half came before was given up on */
            inMsg = true;
        }
        if (!inMsg) return; /* the middle of a message we never saw start */
        piece.write(v, 1, v.length - 1);
        if ((flags & LAST) != 0) {
            byte[] msg = piece.toByteArray();
            piece.reset();
            inMsg = false;
            js("window.__argonLink && __argonLink.message(\"" +
               Base64.encodeToString(msg, Base64.NO_WRAP) + "\")");
        }
    }

    /* ---- phone to board: one write per message, one write at a time ---- */

    private void queue(byte[] msg) {
        if (msg.length == 0 || gatt == null || !ready) return;
        int room = Math.max(20, mtu - 3);
        if (msg.length <= room) {
            sendq.add(msg);
        } else if (msg[0] == 'T') {
            /* A long paste: several 'T' messages, cut between characters -
             * never inside a UTF-8 sequence, which the board would drop. */
            int at = 1;
            while (at < msg.length) {
                int end = Math.min(msg.length, at + room - 1);
                while (end < msg.length && end > at + 1 && (msg[end] & 0xc0) == 0x80) end--;
                byte[] part = new byte[1 + end - at];
                part[0] = 'T';
                System.arraycopy(msg, at, part, 1, end - at);
                sendq.add(part);
                at = end;
            }
        } else {
            Log.w(TAG, "dropping a " + msg.length + " byte message: MTU " + mtu);
            return;
        }
        pump();
    }

    @SuppressWarnings("deprecation")
    private void pump() {
        if (writing || gatt == null || out == null || !ready) return;
        byte[] m = sendq.poll();
        if (m == null) return;
        out.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
        out.setValue(m);
        try {
            writing = gatt.writeCharacteristic(out);
        } catch (SecurityException e) {
            writing = false;
        }
        if (!writing) {
            /* The stack is busy with something else; try again shortly. */
            sendq.addFirst(m);
            ui.postDelayed(new Runnable() { public void run() { pump(); } }, 10);
        }
    }
}
