/*
 * ArgonOS - the page KVM.AXE serves to the phone.
 *
 * A page rather than an application, and that is a deliberate choice for the
 * first version: nothing to install, nothing to sign, nothing to update, and it
 * works from an iPhone, an Android and a laptop the same afternoon.  A real
 * application buys two things this cannot have - a keyboard that reports key
 * positions on a phone, and somewhere to put a picture that is not a browser -
 * and both of those are M2's problems.
 *
 * It is one string in flash and it is served as one write.  Kept in its own
 * header so that kvm.c reads as a program rather than as a wall of markup, and
 * written with single quotes throughout so that nothing here needs escaping to
 * be a C string.
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
    "display:flex;align-items:center;justify-content:center;color:#555}"
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
    "</div>"
    "<div class='row'><button id='bL'>Left</button><button id='bM'>Middle"
    "</button><button id='bR'>Right</button><button id='bD'>Drag</button></div>"
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
    "var dot=document.getElementById('dot'),st=document.getElementById('st'),"
    "cnt=document.getElementById('cnt'),pad=document.getElementById('pad'),"
    "tx=document.getElementById('tx');"
    "function S(s){if(W&&W.readyState==1)W.send(s)}"
    /* One socket, reopened for as long as the page is up.  A phone that
     * locks its screen drops it, and coming back to a dead page would be
     * indistinguishable from a dead board. */
    "function open_(){"
    "W=new WebSocket((location.protocol=='https:'?'wss://':'ws://')"
    "+location.host+'/ws');"
    "W.onopen=function(){dot.className='on';st.textContent='connected'};"
    "W.onclose=function(){dot.className='';st.textContent='reconnecting';"
    "W=null;setTimeout(open_,1000)};"
    "W.onmessage=function(e){var a=(''+e.data).split(' ');"
    "if(a[0]=='S'){st.textContent=['off','waiting for a PC','ready'][+a[1]]"
    "||'?';cnt.textContent=a[2]+' sent, '+a[3]+' lost'}}}"
    "open_();"

    /* The pointer.  Movement is accumulated and sent once a frame: a finger
     * produces events faster than a full-speed interrupt endpoint can carry
     * reports, and sending each one would build a queue that arrives after
     * the finger has stopped. */
    "var pts={},np=0,ax=0,ay=0,aw=0,pend=0,t0=0,moved=0,SP=1.6;"
    "function flush(){pend=0;"
    "var x=Math.round(ax),y=Math.round(ay),w=Math.round(aw);"
    "if(!x&&!y&&!w)return;ax-=x;ay-=y;aw-=w;"
    "S('M '+x+' '+y+' '+w+' '+btn)}"
    "function q(){if(!pend){pend=1;requestAnimationFrame(flush)}}"
    "function click(b){S('M 0 0 0 '+(btn|b));"
    "setTimeout(function(){S('M 0 0 0 '+btn)},30)}"
    "pad.addEventListener('pointerdown',function(e){"
    "pad.setPointerCapture(e.pointerId);pts[e.pointerId]={x:e.clientX,y:e.clientY};"
    "np++;if(np==1){t0=Date.now();moved=0}e.preventDefault()});"
    "pad.addEventListener('pointermove',function(e){var p=pts[e.pointerId];"
    "if(!p)return;var dx=e.clientX-p.x,dy=e.clientY-p.y;"
    "p.x=e.clientX;p.y=e.clientY;moved+=Math.abs(dx)+Math.abs(dy);"
    "if(np>=2){aw-=dy/28}else{ax+=dx*SP;ay+=dy*SP}q();e.preventDefault()});"
    "function up(e){if(!pts[e.pointerId])return;delete pts[e.pointerId];"
    "var was=np;np--;if(np<0)np=0;"
    "if(moved<12&&Date.now()-t0<250)click(was>=2?2:1);e.preventDefault()}"
    "pad.addEventListener('pointerup',up);"
    "pad.addEventListener('pointercancel',up);"

    "document.getElementById('bL').onclick=function(){click(1)};"
    "document.getElementById('bR').onclick=function(){click(2)};"
    "document.getElementById('bM').onclick=function(){click(4)};"
    /* Drag holds the left button down, because a touchpad cannot: there is no
     * way to keep a finger on a button and move another one on a phone. */
    "document.getElementById('bD').onclick=function(){drag=!drag;"
    "this.className=drag?'on':'';btn=drag?1:0;S('M 0 0 0 '+btn)};"

    /* Sticky modifiers: tapped on, folded into the next key, then off.  A
     * phone has no way to hold one down while pressing another. */
    "function sticky(b,bit){b.onclick=function(){lock^=bit;"
    "b.className=(lock&bit)?'on':''}}"
    "var bs=document.querySelectorAll('button[data-m]');"
    "for(i=0;i<bs.length;i++)sticky(bs[i],+bs[i].getAttribute('data-m'));"
    /* The release names the key.  "K u 0" is a modifier changing with no key
     * behind it, which is a different thing and leaves the key held. */
    "function tap(u,m){S('K d '+u+' '+m);S('K u '+u+' 0');"
    "lock=0;for(var j=0;j<bs.length;j++)bs[j].className=''}"
    "var ks=document.querySelectorAll('button[data-k]');"
    "for(i=0;i<ks.length;i++)(function(b){b.onclick=function(){"
    "tap(K[b.getAttribute('data-k')]||0,lock)}})(ks[i]);"
    /* The one chord the far machine always listens to, and the reason a wire
     * beats any program running over there. */
    "document.getElementById('cad').onclick=function(){"
    "S('K d '+K.Delete+' 5');S('K u '+K.Delete+' 0')};"

    /* A real keyboard, when there is one.  `code` is a position and a position
     * is what the report carries, so this path needs no character map at all. */
    "function mods(e){return (e.ctrlKey?1:0)|(e.shiftKey?2:0)|(e.altKey?4:0)"
    "|(e.metaKey?8:0)}"
    "function phys(e,d){if(e.target==tx)return;var u=K[e.code];"
    "if(u===undefined&&!e.code.match(/^(Shift|Control|Alt|Meta|OS)/))return;"
    "S('K '+(d?'d':'u')+' '+(u||0)+' '+(mods(e)|lock));e.preventDefault()}"
    "document.addEventListener('keydown',function(e){phys(e,1)});"
    "document.addEventListener('keyup',function(e){phys(e,0)});"

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
    "document.getElementById('clr').onclick=function(){tx.value='';tx.focus()};"
    "</script></body></html>";

#endif /* ARGON_KVM_PAGE_H */
