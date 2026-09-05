/*
 * ArgonOS - the page KVM.AXE serves to the phone.
 *
 * A page rather than an application, and that is a deliberate choice: nothing
 * to install, nothing to sign, nothing to update, and it works from an iPhone,
 * an Android and a laptop the same afternoon.  It also happens to be the only
 * client that already knows how to decode a PNG, which is why the board can get
 * away with never touching a pixel.
 *
 * It is one string in flash and it is served as one write.  Kept in its own
 * header so that kvm.c reads as a program rather than as a wall of markup, and
 * written with single quotes throughout so that nothing here needs escaping to
 * be a C string.
 *
 * The page has two halves, and which one is in front depends on whether there
 * is a picture:
 *
 *   no agent running over there  a trackpad, because there is nothing to point
 *                                at.  Relative mouse, taps and two-finger
 *                                scroll - and the keyboard, which is the half
 *                                that works in a BIOS.
 *   an agent running             the screen itself.  One finger puts the
 *                                pointer where it touches, which is what the
 *                                absolute HID interface exists for; two fingers
 *                                pan and pinch, because a phone showing a
 *                                1920-pixel desktop needs to get close enough
 *                                to hit a menu.
 *
 * Two things about the keyboard are worth knowing before reading the script.
 *
 * On a laptop browser, keydown and keyup carry `code`, which is a *position* on
 * the keyboard - KeyA is the key where A is on a US layout, whatever that
 * machine is set to - and a position is exactly what a HID report carries.  So
 * a laptop drives the far machine key for key, modifiers and all.
 *
 * On a phone there are no key positions.  A mobile keyboard reports what it
 * inserted, not what was pressed, and every browser lies about `code` in a
 * different way.  So the phone gets the text box instead: what it inserts is
 * sent as text and typed out over there character by character, and the keys
 * that have no character - Escape, Tab, the arrows, Ctrl+Alt+Del - are buttons.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_KVM_PAGE_H
#define ARGON_KVM_PAGE_H

static const char k_page[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,"
    "maximum-scale=1,user-scalable=no'>"
    "<title>ArgonOS KVM</title><style>"
    "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}"
    "body{margin:0;background:#111;color:#ddd;font:14px system-ui,sans-serif;"
    "height:100vh;display:flex;flex-direction:column;overflow:hidden;"
    "touch-action:none;user-select:none}"
    "#bar{padding:7px 10px;border-bottom:1px solid #333;display:flex;gap:8px;"
    "align-items:center;font-size:12px}"
    "#dot{width:9px;height:9px;border-radius:50%;background:#a33;flex:none}"
    "#dot.on{background:#3a7}"
    "#cnt{margin-left:auto;color:#777}"
    "#pad{flex:1;margin:8px;border:1px dashed #333;border-radius:8px;"
    "display:flex;align-items:center;justify-content:center;color:#555;"
    "text-align:center;padding:0 20px}"
    "#view{flex:1;position:relative;overflow:hidden;display:none;"
    "background:#000}"
    "#wrap{position:absolute;left:0;top:0;transform-origin:0 0}"
    "#scr{display:block;image-rendering:pixelated}"
    ".row{display:flex;gap:6px;padding:0 8px 8px}"
    "button{flex:1;padding:10px 4px;background:#1c1c1c;color:#ddd;"
    "border:1px solid #333;border-radius:6px;font:13px system-ui;min-height:44px}"
    "button:active,button.on{background:#3a7;color:#000}"
    "#tx{flex:3;padding:10px;background:#1c1c1c;color:#ddd;border:1px solid #333;"
    "border-radius:6px;font:14px system-ui;min-width:0}"
    "</style></head><body>"
    "<div id='bar'><span id='dot'></span><span id='st'>connecting</span>"
    "<span id='cnt'></span></div>"
    "<div id='pad'>trackpad: drag to move, tap to click, two fingers to scroll"
    "<br><br>no picture: start the agent on that computer"
    "</div>"
    "<div id='view'><div id='wrap'><canvas id='scr'></canvas></div></div>"
    "<div class='row'><button id='bL'>Left</button><button id='bM'>Middle"
    "</button><button id='bR'>Right</button><button id='bD'>Drag</button>"
    "<button id='bF'>Fit</button></div>"
    "<div class='row'><button data-k='Escape'>Esc</button>"
    "<button data-k='Tab'>Tab</button><button data-m='1'>Ctrl</button>"
    "<button data-m='2'>Shift</button><button data-m='4'>Alt</button>"
    "<button data-m='8'>Win</button></div>"
    "<div class='row'><button data-k='ArrowLeft'>&larr;</button>"
    "<button data-k='ArrowDown'>&darr;</button>"
    "<button data-k='ArrowUp'>&uarr;</button>"
    "<button data-k='ArrowRight'>&rarr;</button>"
    "<button data-k='Enter'>Enter</button>"
    "<button data-k='Backspace'>&#9003;</button>"
    "<button id='cad'>C-A-Del</button></div>"
    "<div class='row'><input id='tx' autocomplete='off' autocapitalize='off'"
    " autocorrect='off' spellcheck='false' placeholder='type here'>"
    "<button id='clr'>Clear</button></div>"
    "<script>"
    /* The map from a browser key position to a HID usage code.  Built rather
     * than written out: the letters and the digits and the function keys are
     * each a run, and a run written out by hand is a run with a typo in it. */
    "var K={},i,A='ABCDEFGHIJKLMNOPQRSTUVWXYZ';"
    "for(i=0;i<26;i++)K['Key'+A[i]]=4+i;"
    "for(i=1;i<=9;i++)K['Digit'+i]=0x1E+i-1;"
    "for(i=1;i<=12;i++)K['F'+i]=0x3A+i-1;"
    "var M0={Digit0:0x27,Enter:0x28,Escape:0x29,Backspace:0x2A,Tab:0x2B,"
    "Space:0x2C,Minus:0x2D,Equal:0x2E,BracketLeft:0x2F,BracketRight:0x30,"
    "Backslash:0x31,Semicolon:0x33,Quote:0x34,Backquote:0x35,Comma:0x36,"
    "Period:0x37,Slash:0x38,CapsLock:0x39,PrintScreen:0x46,ScrollLock:0x47,"
    "Pause:0x48,Insert:0x49,Home:0x4A,PageUp:0x4B,Delete:0x4C,End:0x4D,"
    "PageDown:0x4E,ArrowRight:0x4F,ArrowLeft:0x50,ArrowDown:0x51,ArrowUp:0x52,"
    "NumLock:0x53,ContextMenu:0x65};"
    "for(var k in M0)K[k]=M0[k];"

    "var W=null,lock=0,btn=0,drag=0;"
    "var D=document,g=function(x){return D.getElementById(x)};"
    "var dot=g('dot'),st=g('st'),cnt=g('cnt'),pad=g('pad'),tx=g('tx'),"
    "view=g('view'),wrap=g('wrap'),scr=g('scr');"
    "var ctx=null,SW=0,SH=0,zoom=1,ox=0,oy=0,q=Promise.resolve();"
    "function S(s){if(W&&W.readyState==1)W.send(s)}"
    /* A binary message to the agent on the far machine, in the same frame
     * format it sends its tiles in.  Only two ever go this way: give me a
     * whole screen, and here is some text for the clipboard. */
    "function B(type,bytes){"
    "if(!W||W.readyState!=1)return;"
    "var n=bytes?bytes.length:0,b=new Uint8Array(8+n);"
    "b[0]=65;b[1]=71;b[2]=type;b[3]=0;"
    "b[4]=n&255;b[5]=(n>>8)&255;b[6]=(n>>16)&255;b[7]=(n>>24)&255;"
    "if(n)b.set(bytes,8);W.send(b.buffer)}"

    /* One socket, reopened for as long as the page is up.  A phone that
     * locks its screen drops it, and coming back to a dead page would be
     * indistinguishable from a dead board. */
    "function open_(){"
    "W=new WebSocket((location.protocol=='https:'?'wss://':'ws://')"
    "+location.host+'/ws');"
    "W.binaryType='arraybuffer';"
    "W.onopen=function(){dot.className='on';st.textContent='connected';"
    "B(0x11,null)};"
    "W.onclose=function(){dot.className='';st.textContent='reconnecting';"
    "W=null;setTimeout(open_,1000)};"
    "W.onmessage=function(e){"
    "if(typeof e.data=='string')return text(e.data);"
    "pic(new Uint8Array(e.data))}}"
    /* S <hid state> <reports> <refused> <leds> <pipe state> <tiles> <resyncs>
     * - the fields in the order push_status writes them, and the reason this
     * is spelled out is that reading them one apart is exactly the mistake
     * that made a healthy link look like eighteen hundred resyncs. */
    "function text(s){var a=s.split(' ');if(a[0]!='S')return;"
    "st.textContent=(['off','waiting for a PC','ready'][+a[1]]||'?')"
    "+(+a[5]==2?' + screen':'');"
    "cnt.textContent=a[2]+' sent, '+a[3]+' lost'"
    "+(+a[6]?', '+a[6]+' tiles':'')+(+a[7]?', '+a[7]+' resync':'')}"

    /* A message from the far machine.  Two matter: how big its screen is, and
     * a rectangle of it as a PNG.  The board forwarded these without looking
     * inside, which is why it needs no decoder and no frame buffer. */
    "function pic(b){"
    "if(b.length<8||b[0]!=65||b[1]!=71)return;"
    "var t=b[2],n=b[4]|(b[5]<<8)|(b[6]<<16)|(b[7]<<24),p=b.subarray(8,8+n);"
    "if(t==1){"
    "SW=p[2]|(p[3]<<8);SH=p[4]|(p[5]<<8);"
    "if(!SW||!SH)return;"
    "scr.width=SW;scr.height=SH;ctx=scr.getContext('2d');"
    "pad.style.display='none';view.style.display='block';fit()}"
    "else if(t==2&&ctx){"
    "var x=p[0]|(p[1]<<8),y=p[2]|(p[3]<<8);"
    "var im=new Blob([p.subarray(8)],{type:'image/png'});"
    /* Chained rather than fired off in parallel: decoding is asynchronous and
     * two updates to the same rectangle would otherwise be free to land in the
     * wrong order, which shows as a patch of screen stuck one frame behind. */
    "q=q.then(function(){return createImageBitmap(im)})"
    ".then(function(bm){ctx.drawImage(bm,x,y);bm.close&&bm.close()})"
    ".catch(function(){})}}"

    /* The picture on a phone: fit it, then let two fingers get closer. */
    "function place(){wrap.style.transform="
    "'translate('+ox+'px,'+oy+'px) scale('+zoom+')'}"
    "function fit(){if(!SW)return;"
    "var r=view.getBoundingClientRect();"
    "zoom=Math.min(r.width/SW,r.height/SH);"
    "ox=(r.width-SW*zoom)/2;oy=(r.height-SH*zoom)/2;place()}"
    "g('bF').onclick=function(){fit();B(0x11,null)};"
    "addEventListener('resize',fit);"

    /* Where on the far machine's screen a finger is, as the absolute pointer
     * wants it: nought to 32767 across, whatever that screen turns out to be. */
    "function abs(e){var r=scr.getBoundingClientRect();"
    "var x=(e.clientX-r.left)/r.width,y=(e.clientY-r.top)/r.height;"
    "x=x<0?0:x>1?1:x;y=y<0?0:y>1?1:y;"
    "return [Math.round(x*32767),Math.round(y*32767)]}"
    "function aSend(e,b){var a=abs(e);S('A '+a[0]+' '+a[1]+' 0 '+b)}"

    /* The pointer.  Movement is accumulated and sent once a frame: a finger
     * produces events faster than a full-speed interrupt endpoint can carry
     * reports, and sending each one would build a queue that arrives after
     * the finger has stopped. */
    "var pts={},np=0,ax=0,ay=0,aw=0,pend=0,t0=0,moved=0,SP=1.6;"
    "var last=null,pinch=0,pd0=0,pz0=1,pc0=null;"
    "function flush(){pend=0;"
    "if(last){var a=abs(last);S('A '+a[0]+' '+a[1]+' 0 '+btn);last=null;return}"
    "var x=Math.round(ax),y=Math.round(ay),w=Math.round(aw);"
    "if(!x&&!y&&!w)return;ax-=x;ay-=y;aw-=w;"
    "S('M '+x+' '+y+' '+w+' '+btn)}"
    "function qf(){if(!pend){pend=1;requestAnimationFrame(flush)}}"
    "function click(b){S('M 0 0 0 '+(btn|b));"
    "setTimeout(function(){S('M 0 0 0 '+btn)},30)}"
    "function aclick(e,b){aSend(e,btn|b);"
    "setTimeout(function(){aSend(e,btn)},30)}"

    "function keys(o){var r=[],k;for(k in o)r.push(o[k]);return r}"
    "function dist(a,b){var dx=a.x-b.x,dy=a.y-b.y;"
    "return Math.sqrt(dx*dx+dy*dy)}"

    "function down(e){"
    "e.target.setPointerCapture&&e.target.setPointerCapture(e.pointerId);"
    "pts[e.pointerId]={x:e.clientX,y:e.clientY};np++;"
    "if(np==1){t0=Date.now();moved=0}"
    "if(np==2&&ctx){var v=keys(pts);pinch=1;pd0=dist(v[0],v[1])||1;pz0=zoom;"
    "pc0={x:(v[0].x+v[1].x)/2,y:(v[0].y+v[1].y)/2,ox:ox,oy:oy}}"
    "e.preventDefault()}"

    "function move(e){var p=pts[e.pointerId];if(!p)return;"
    "var dx=e.clientX-p.x,dy=e.clientY-p.y;"
    "p.x=e.clientX;p.y=e.clientY;moved+=Math.abs(dx)+Math.abs(dy);"
    "if(pinch){var v=keys(pts);if(v.length>=2){"
    "var d=dist(v[0],v[1])||1,c={x:(v[0].x+v[1].x)/2,y:(v[0].y+v[1].y)/2};"
    "zoom=pz0*d/pd0;if(zoom<0.05)zoom=0.05;if(zoom>8)zoom=8;"
    "ox=pc0.ox+(c.x-pc0.x)+(pc0.x-pc0.ox)*(1-zoom/pz0);"
    "oy=pc0.oy+(c.y-pc0.y)+(pc0.y-pc0.oy)*(1-zoom/pz0);place()}"
    "e.preventDefault();return}"
    "if(ctx){last=e;qf()}"
    "else if(np>=2){aw-=dy/28;qf()}"
    "else{ax+=dx*SP;ay+=dy*SP;qf()}"
    "e.preventDefault()}"

    "function up(e){if(!pts[e.pointerId])return;delete pts[e.pointerId];"
    "var was=np;np--;if(np<0)np=0;if(np<2)pinch=0;"
    "if(moved<12&&Date.now()-t0<250){"
    "if(ctx&&was<2)aclick(e,1);else if(!ctx)click(was>=2?2:1)}"
    "e.preventDefault()}"

    "function bind(el){el.addEventListener('pointerdown',down);"
    "el.addEventListener('pointermove',move);"
    "el.addEventListener('pointerup',up);"
    "el.addEventListener('pointercancel',up)}"
    "bind(pad);bind(view);"

    "g('bL').onclick=function(){click(1)};"
    "g('bR').onclick=function(){click(2)};"
    "g('bM').onclick=function(){click(4)};"
    /* Drag holds the left button down, because a touchscreen cannot: there is
     * no way to keep a finger on a button and move another one on a phone. */
    "g('bD').onclick=function(){drag=!drag;this.className=drag?'on':'';"
    "btn=drag?1:0;S('M 0 0 0 '+btn)};"

    /* Sticky modifiers: tapped on, folded into the next key, then off.  A
     * phone has no way to hold one down while pressing another. */
    "function sticky(b,bit){b.onclick=function(){lock^=bit;"
    "b.className=(lock&bit)?'on':''}}"
    "var bs=D.querySelectorAll('button[data-m]');"
    "for(i=0;i<bs.length;i++)sticky(bs[i],+bs[i].getAttribute('data-m'));"
    /* The release names the key.  'K u 0' is a modifier changing with no key
     * behind it, which is a different thing and leaves the key held. */
    "function tap(u,m){S('K d '+u+' '+m);S('K u '+u+' 0');"
    "lock=0;for(var j=0;j<bs.length;j++)bs[j].className=''}"
    "var ks=D.querySelectorAll('button[data-k]');"
    "for(i=0;i<ks.length;i++)(function(b){b.onclick=function(){"
    "tap(K[b.getAttribute('data-k')]||0,lock)}})(ks[i]);"
    /* The one chord the far machine always listens to, and the reason a wire
     * beats any program running over there. */
    "g('cad').onclick=function(){S('K d '+K.Delete+' 5');"
    "S('K u '+K.Delete+' 0')};"

    /* A real keyboard, when there is one.  `code` is a position and a position
     * is what the report carries, so this path needs no character map at all. */
    "function mods(e){return (e.ctrlKey?1:0)|(e.shiftKey?2:0)|(e.altKey?4:0)"
    "|(e.metaKey?8:0)}"
    "function phys(e,d){if(e.target==tx)return;var u=K[e.code];"
    "if(u===undefined&&!e.code.match(/^(Shift|Control|Alt|Meta|OS)/))return;"
    "S('K '+(d?'d':'u')+' '+(u||0)+' '+(mods(e)|lock));e.preventDefault()}"
    "D.addEventListener('keydown',function(e){phys(e,1)});"
    "D.addEventListener('keyup',function(e){phys(e,0)});"

    /* The phone's keyboard.  What it gives us is the text that appeared, not
     * the keys that were pressed, so that is what gets sent - as it is typed,
     * so the box behaves like a keyboard rather than like a form.  A deletion
     * is the one edit that carries no text, and it becomes a backspace.
     *
     * The box keeps what was typed rather than clearing itself, because on a
     * phone clearing an input mid-word is what stops the keyboard's own
     * word-by-word editing from working.  Clear is a button.
     */
    "tx.addEventListener('beforeinput',function(e){"
    "var t=e.inputType;"
    "if(t=='deleteContentBackward'){S('K d '+K.Backspace+' 0');"
    "S('K u '+K.Backspace+' 0')}"
    "else if(t=='insertLineBreak'||t=='insertParagraph'){"
    "S('K d '+K.Enter+' 0');S('K u '+K.Enter+' 0')}"
    "else if(e.data)S('T '+e.data);"
    "else if(e.dataTransfer)S('T '+e.dataTransfer.getData('text/plain'))});"
    "tx.addEventListener('keydown',function(e){if(e.key=='Enter'){"
    "S('K d '+K.Enter+' 0');S('K u '+K.Enter+' 0');e.preventDefault()}});"
    "g('clr').onclick=function(){tx.value='';tx.focus()};"
    "open_();"
    "</script></body></html>";

#endif /* ARGON_KVM_PAGE_H */
