/*
 * The pad page: a 6-button pad drawn for a thumb on each side.  It sends what
 * is held (AG_SP_* bits, hex) whenever that changes and every 300 ms while
 * anything is held, one request at a time; the module lets go after a second
 * of silence.  Each request also carries every state the buttons went through
 * since the one before (&e=, up to eight), and the module plays them in order
 * before it settles on m.  What is held alone cannot carry a tap that began
 * and ended while a request was out: 12 of 20 quick taps never went out
 * against a module that answers in 100 ms (4 Nov 2026).  Nor can a request a
 * state (two a tap fall behind), a press kept for the next request (taps in a
 * row run together), or the presses alone (17 of 20: a tap begun before the
 * last one's release went out had no release before it).  The line at the top is the module's answer: what it holds, and
 * how many writes and reads the CYD has made of it - the proof the bus is
 * alive, on the phone itself.  MENU is not a button of the pad: it holds
 * Start and Mode together, which is what SEGAPAD.SYS opens a menu on.
 */
#ifndef SEGAPAD_PAGE_H
#define SEGAPAD_PAGE_H

static const char k_page[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1,user-scalable=no'>"
"<title>SEGAPAD</title><style>"
"html,body{margin:0;height:100%;background:#111;color:#ddd;font:14px sans-serif;"
"touch-action:none;user-select:none;-webkit-user-select:none}"
"#st{position:fixed;top:4px;left:0;right:0;text-align:center;color:#8a8}"
".b{position:absolute;display:flex;align-items:center;justify-content:center;"
"border-radius:50%;background:#333;color:#eee;font-weight:bold;font-size:18px}"
".b.on{background:#c33}.s{border-radius:12px;font-size:13px}"
"#kbtog{position:fixed;top:2px;right:4px;z-index:5;padding:6px 10px;background:#335;"
"color:#eee;border-radius:8px;font-weight:bold}"
"#kb{display:none;position:fixed;top:34px;left:0;right:0;bottom:0;padding:8px;"
"touch-action:auto;user-select:text;-webkit-user-select:text}"
"#ki{width:100%;font-size:20px;padding:10px;box-sizing:border-box}"
".k{display:inline-block;margin:4px;padding:12px 14px;background:#333;border-radius:8px;font-size:16px}"
"#kst{margin-top:8px;color:#8a8}"
"</style></head><body><div id=st>...</div><div id=pad></div>"
"<div id=kbtog>KEYS</div><div id=kb><input id=ki autocomplete=off autocapitalize=off "
"autocorrect=off spellcheck=false placeholder='type here'><div id=kk></div><div id=kst></div></div>"
"<script>"
"var B=[['U',1,20,30],['D',2,20,70],['L',4,9,50],['R',8,31,50],"
"['A',16,64,72],['B',32,77,62],['C',64,90,52],"
"['X',256,64,40],['Y',512,77,30],['Z',1024,90,20],"
"['START',128,46,82,1],['MODE',2048,46,18,1],['MENU',2176,20,10,1]];"
"var pad=document.getElementById('pad'),st=document.getElementById('st'),"
"held=0,sent=-1,busy=0,touch={},last=0,evs=[],prev=0;"
"B.forEach(function(b){var e=document.createElement('div');e.className='b'+(b[4]?' s':'');"
"e.textContent=b[0];e.dataset.m=b[1];var w=b[4]?22:15;"
"e.style.cssText='left:'+(b[2]-w/2)+'vw;top:'+(b[3]-7)+'vh;width:'+w+'vw;height:'+(b[4]?8:14)+'vh';"
"pad.appendChild(e)});"
"function at(x,y){var e=document.elementFromPoint(x,y);return e&&e.dataset.m?+e.dataset.m:0}"
"function redo(){held=0;for(var k in touch)held|=touch[k];"
"[].forEach.call(pad.children,function(e){e.classList.toggle('on',(held&e.dataset.m)!=0)});"
"if(held!=prev)evs.push(held);prev=held;send()}"
"function hx(x){return x.toString(16)}"
"function send(){if(busy)return;var e=evs.splice(0,8),m=held;"
"if(!e.length&&m==sent&&(m==0||Date.now()-last<300))return;"
"busy=1;last=Date.now();"
"fetch('/p?m='+hx(m)+(e.length?'&e='+e.map(hx).join(','):'')).then(function(r){return r.text()}).then(function(t){"
"var p=t.split(' ');st.textContent='held '+p[0]+'  CYD wrote '+p[1]+', read '+p[2]+', lost '+p[3];"
"sent=m}).catch(function(){st.textContent='no answer';sent=-1;evs=e.concat(evs)})"
".then(function(){busy=0;send()})}"
"function ev(e){if(kbOn||e.target.id=='kbtog')return;e.preventDefault();for(var i=0;i<e.changedTouches.length;i++){var t=e.changedTouches[i];"
"if(e.type=='touchend'||e.type=='touchcancel')delete touch[t.identifier];"
"else touch[t.identifier]=at(t.clientX,t.clientY)}redo()}"
"['touchstart','touchmove','touchend','touchcancel'].forEach(function(n){"
"document.addEventListener(n,ev,{passive:false})});"
"document.addEventListener('mousedown',function(e){if(kbOn)return;touch.m=at(e.clientX,e.clientY);redo()});"
"document.addEventListener('mouseup',function(){delete touch.m;redo()});"
"setInterval(send,100);send();"
/* The keyboard: what is typed in the field and the keys under it go to /k as
 * CardKB codes (apps/common/cardkb), a batch per request, one request at a
 * time.  The field keeps one space in it so a Backspace on an empty line is
 * still an input event a phone's keyboard sends. */
"var kbOn=0,kq=[],kbusy=0,kb=document.getElementById('kb'),ki=document.getElementById('ki'),"
"kst=document.getElementById('kst'),kk=document.getElementById('kk');"
"document.getElementById('kbtog').onclick=function(){kbOn=!kbOn;kb.style.display=kbOn?'block':'none';"
"pad.style.display=kbOn?'none':'block';this.textContent=kbOn?'PAD':'KEYS';if(kbOn){kreset();ki.focus()}};"
"[['Esc',27],['Tab',9],['←',180],['↑',181],['↓',182],['→',183],"
"['Enter',13],['⌫',8]].forEach(function(b){var e=document.createElement('span');e.className='k';"
"e.textContent=b[0];e.onclick=function(){kput(b[1]);ki.focus()};kk.appendChild(e)});"
"function kreset(){ki.value=' ';try{ki.setSelectionRange(1,1)}catch(x){}}"
"ki.addEventListener('input',function(){var v=ki.value,bad=0;if(v.length==0)kput(8);"
"else for(var i=1;i<v.length;i++){var c=v.charCodeAt(i);if(c>=32&&c<127)kput(c);else bad=1}"
"if(bad)kst.textContent='only Latin letters reach the board';kreset()});"
"ki.addEventListener('keydown',function(e){var a={Enter:13,Escape:27,Tab:9,ArrowLeft:180,"
"ArrowUp:181,ArrowDown:182,ArrowRight:183}[e.key];if(a){kput(a);e.preventDefault()}});"
"function kput(c){kq.push(c);ksend()}"
"function ksend(){if(kbusy)return;var e=kq.splice(0,24);kbusy=1;"
"fetch('/k'+(e.length?'?c='+e.map(hx).join(','):'')).then(function(r){return r.text()}).then(function(t){"
"var p=t.split(' '),n=+p[4];if(n<e.length)kq=e.slice(n).concat(kq);"
"kst.textContent='waiting '+(+p[1]+kq.length)+', the board took '+p[2]}).catch(function(){"
"kq=e.concat(kq);kst.textContent='no answer'}).then(function(){kbusy=0;"
"if(kq.length)setTimeout(ksend,kq.length>24?100:0)})}"
"setInterval(function(){if(kbOn)ksend()},700);"
"</script></body></html>";

#endif
