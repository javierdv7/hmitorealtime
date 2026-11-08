#pragma once
// Page served by the ESP32-C3 itself. Self-contained (no external assets): it must work
// with the CM4 and the internet gone.
static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PLC Edge-01 · Nivel y alarmas</title>
<style>
:root{color-scheme:dark;--bg:#0d0d0d;--panel:#171717;--line:rgba(255,255,255,.10);--text:#ececec;--muted:#a8a8a8;--ok:#4ade80;--bad:#f87171;--warn:#facc15;--info:#00b0c2;--accent:#e16e09;--mono:ui-monospace,Menlo,Consolas,monospace;--radius:9px}
*{box-sizing:border-box}body{display:grid;grid-template-rows:auto minmax(0,1fr);height:100dvh;overflow:hidden;margin:0;background:radial-gradient(ellipse at 50% -30%,#252019 0,transparent 45%),var(--bg);color:var(--text);font:14px/1.5 Geist,Inter,system-ui,-apple-system,sans-serif}
header{padding:10px clamp(16px,3vw,40px);border-bottom:1px solid var(--line);background:rgba(13,13,13,.86);display:flex;gap:8px;align-items:center;flex-wrap:wrap}
h1{display:flex;align-items:center;gap:12px;margin:0 auto 0 0;font-size:14px;font-weight:600;letter-spacing:-.02em}h1:before{content:"ER";display:grid;place-items:center;width:34px;height:34px;border-radius:7px;background:#b95a07;color:#fff;font:700 12px var(--mono);letter-spacing:-.08em}h1 small{display:block;color:var(--muted);font-size:12px;font-weight:400;letter-spacing:0}
.chip{min-height:26px;padding:4px 8px;border-radius:5px;border:1px solid var(--line);background:rgba(255,255,255,.025);color:var(--muted);font:11px var(--mono)}
.chip.ok{color:var(--ok);border-color:rgba(74,222,128,.25);background:rgba(74,222,128,.08)}.chip.bad{color:var(--bad);border-color:rgba(248,113,113,.25);background:rgba(248,113,113,.08)}
main{width:min(100%,1440px);height:100%;min-height:0;margin-inline:auto;display:grid;grid-template-columns:repeat(12,minmax(0,1fr));grid-template-rows:minmax(0,1.25fr) minmax(0,.75fr);gap:8px;padding:12px clamp(16px,3vw,40px) 14px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);padding:14px;min-width:0;min-height:0;overflow:hidden;box-shadow:inset 0 1px rgba(255,255,255,.035),0 10px 30px rgba(0,0,0,.18)}
h2{margin:0;font-size:10px;letter-spacing:.09em;text-transform:uppercase;color:var(--muted);font-weight:600}
.tank{grid-column:span 8;display:flex;flex-direction:column}.tank-head{display:flex;align-items:baseline;justify-content:space-between;gap:12px}.tank-value{font-size:25px;font-weight:600;letter-spacing:-.04em;color:var(--info);font-variant-numeric:tabular-nums}.chart{flex:1;min-height:0;margin-top:8px}#level-chart{display:block;width:100%;height:100%}.chart-note{margin-top:4px;color:var(--muted);font-size:10px}
.alarms{grid-column:span 4;display:flex;flex-direction:column}.alarm-list{list-style:none;padding:0;margin:8px 0 0;min-height:0;overflow:auto;font:10px/1.5 var(--mono)}.alarm-list li{display:flex;gap:8px;padding:7px 0;border-top:1px solid var(--line)}.alarm-list time{color:var(--muted);flex:none}.alarm-list .fire{color:var(--bad)}.alarm-list .clear{color:var(--ok)}
.log{grid-column:1/-1;display:flex;flex-direction:column}#log{font:10px/1.5 var(--mono);flex:1;min-height:0;overflow:auto;white-space:pre-wrap;margin:8px 0 0;color:#c5c5c5}
#log .f{color:var(--bad)}#log .c{color:var(--ok)}#log .b{color:var(--warn)}
.empty{color:var(--muted);font-style:normal}
#lost{display:none;position:fixed;inset:0;align-items:center;justify-content:center;text-align:center;padding:20px;background:rgba(13,13,13,.84);backdrop-filter:blur(5px);color:var(--muted);font:600 20px var(--mono)}body.lost #lost{display:flex}body.lost header,body.lost main{filter:grayscale(1);opacity:.45}
@media(max-width:760px){body{height:auto;min-height:100dvh;overflow:auto;display:block}main{height:auto;grid-template-columns:1fr;grid-template-rows:none;grid-auto-rows:auto;padding-top:14px}.tank,.alarms,.log{grid-column:1}.tank{min-height:320px}.alarms{min-height:180px}.log{height:260px}header h1{flex-basis:100%;margin-bottom:4px}}
</style></head><body>
<header><h1>PLC Edge-01 <small>Panel local · ESP32-C3</small></h1></header>
<main>
<section class="card tank"><div class="tank-head"><h2>Nivel del tanque · simulado</h2><div class="tank-value" id="lvl">—</div></div>
<div class="chart"><canvas id="level-chart" role="img" aria-label="Historial del nivel de tanque simulado durante el último minuto"></canvas></div>
<div class="chart-note">BOOT pulsado: +20 %/s · liberado: −10 %/s · AI-01</div></section>
<section class="card alarms"><h2>Alarmas registradas</h2><ul class="alarm-list" id="alarms"><li class="empty">Sin alarmas registradas</li></ul></section>
<section class="card log"><h2>Log del PLC</h2><pre id="log"></pre></section>
</main>
<div id="lost">PLC SIN ENLACE · comprueba alimentación y Wi-Fi</div>
<script>
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[&<>]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;"}[c]));
let since=0,fails=0,lastSample=0,levelSamples=[],alarmEntries=[];
function drawLevel(){
 const c=$("level-chart"),r=c.getBoundingClientRect();if(!r.width||!r.height)return;
 const dpr=Math.min(window.devicePixelRatio||1,2),ctx=c.getContext("2d");c.width=Math.round(r.width*dpr);c.height=Math.round(r.height*dpr);ctx.scale(dpr,dpr);
 const left=34,right=r.width-8,top=8,bottom=r.height-18,plotH=bottom-top,now=Date.now();ctx.font="10px ui-monospace,monospace";ctx.textBaseline="middle";
  for(const v of [100,50,0]){const y=bottom-v/100*plotH;ctx.strokeStyle="rgba(255,255,255,.10)";ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(right,y);ctx.stroke();ctx.fillStyle="#a8a8a8";ctx.textAlign="right";ctx.fillText(v+"%",left-6,y)}
  ctx.fillStyle="#a8a8a8";ctx.textAlign="left";ctx.fillText("−60 s",left,r.height-7);ctx.textAlign="center";ctx.fillText("−30 s",(left+right)/2,r.height-7);ctx.textAlign="right";ctx.fillText("ahora",right,r.height-7);
 const vis=levelSamples.filter(p=>p.at>=now-60000);if(!vis.length)return;
 const pts=vis.map(p=>[left+Math.max(0,(p.at-(now-60000))/60000)*(right-left),bottom-Math.max(0,Math.min(100,p.value))/100*plotH]);
 ctx.beginPath();pts.forEach(([x,y],i)=>i?ctx.lineTo(x,y):ctx.moveTo(x,y));ctx.strokeStyle="#00b0c2";ctx.lineWidth=2;ctx.stroke();ctx.lineTo(pts.at(-1)[0],bottom);ctx.lineTo(pts[0][0],bottom);ctx.closePath();ctx.fillStyle="rgba(0,176,194,.10)";ctx.fill();
}
function renderAlarms(){
 $("alarms").innerHTML=alarmEntries.length?alarmEntries.map(a=>'<li><time>'+esc(a.time)+" s</time><span class=\""+a.kind+"\">"+esc(a.text)+"</span></li>").join(""):'<li class="empty">Sin alarmas registradas</li>';
}
function render(d){
  if(d.log_seq<since){since=0;$("log").textContent="";alarmEntries=[]}
  $("lvl").textContent=d.level.toFixed(1)+" %";
  if(Date.now()-lastSample>=1000){lastSample=Date.now();levelSamples.push({at:lastSample,value:d.level});levelSamples=levelSamples.slice(-60);drawLevel()}
 if(d.log.length){const L=$("log"),stick=L.scrollTop+L.clientHeight>=L.scrollHeight-20;
   for(const line of d.log){const sp=document.createElement("span");
    sp.className=/FIRED|REJECTED/.test(line)?"f":/CLEARED|ONLINE|CONNECTED|ACCEPTED/.test(line)?"c":/OFFLINE|failed|DOWN/.test(line)?"b":"";
    sp.textContent=line+"\n";L.appendChild(sp);
    if(/RULE (FIRED|CLEARED)/.test(line)){const m=line.match(/^\[\s*([0-9.]+)\]/);alarmEntries.unshift({time:m?m[1]:"—",kind:/FIRED/.test(line)?"fire":"clear",text:line.replace(/^\[\s*[0-9.]+\]\s*/,"")})}}
   while(L.childNodes.length>300)L.removeChild(L.firstChild);if(stick)L.scrollTop=L.scrollHeight}
  alarmEntries=alarmEntries.slice(0,12);renderAlarms();
  since=d.log_seq;
}
async function poll(){
 const ac=new AbortController(),t=setTimeout(()=>ac.abort(),1500);
  try{const r=await fetch("/state?since="+since,{signal:ac.signal,cache:"no-store"});render(await r.json());fails=0;document.body.classList.remove("lost")}
  catch(e){if(++fails>3)document.body.classList.add("lost")}
 finally{clearTimeout(t);setTimeout(poll,200)}
}
window.addEventListener("resize",drawLevel);
poll();
</script></body></html>)HTML";
