/**
 * @file ferraris_web_pages.h
 * @brief Eingebettete Webseiten: Dashboard (/), WLAN-Einrichtung (/wifi), Einstellungen (/config).
 *
 * Reines HTML/CSS/JS ohne externe Ressourcen (der AP hat kein Internet).
 * Daten kommen per fetch von /api/status, /api/history, /api/scan, /api/wifi,
 * /api/config, /api/restart, /api/ota.
 * Untrusted Strings (SSIDs aus dem Scan) werden nur per textContent gesetzt.
 */

#ifndef FERRARIS_WEB_PAGES_H
#define FERRARIS_WEB_PAGES_H

// Gemeinsames CSS (Farb-Tokens hell/dunkel, Karten, Formular).
#define FERRARIS_WEB_CSS R"CSS(
:root{--bg:#f4f4f2;--card:#fcfcfb;--fg:#0b0b0b;--fg2:#52514e;--mut:#8a8984;--line:#e4e3df;
--series:#2a78d6;--band:#cde2fb;--good:#0ca30c;--warn:#b97700;--crit:#d03b30;--btn:#2a78d6;--btnfg:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#111110;--card:#1a1a19;--fg:#f0efec;--fg2:#c3c2b7;--mut:#8f8e87;
--line:#2c2c2a;--series:#3987e5;--band:#184f95;--good:#2fbf2f;--warn:#fab219;--crit:#ff6b60;--btn:#3987e5}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:860px;margin:0 auto}
header{display:flex;flex-wrap:wrap;align-items:center;gap:8px 12px;margin-bottom:14px}
header h1{font-size:19px;margin:0;flex:1 1 auto}
a{color:var(--series)}
.chip{display:inline-flex;align-items:center;gap:6px;padding:3px 10px;border-radius:999px;background:var(--card);
border:1px solid var(--line);font-size:13px;color:var(--fg2);white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mut)}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;margin-bottom:12px}
.card h2{font-size:14px;font-weight:600;margin:0 0 10px;color:var(--fg2)}
.l{font-size:12px;color:var(--fg2)}
.num{font-variant-numeric:tabular-nums}
.muted{color:var(--mut);font-size:13px}
button{font:inherit;border:0;border-radius:8px;padding:9px 16px;background:var(--btn);color:var(--btnfg);cursor:pointer}
button.sec{background:transparent;color:var(--series);border:1px solid var(--line)}
button:disabled{opacity:.5;cursor:default}
input{font:inherit;width:100%;padding:9px 10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg)}
label{display:block;margin:10px 0 4px;font-size:13px;color:var(--fg2)}
.ok{color:var(--good)}.warn{color:var(--warn)}.crit{color:var(--crit)}
)CSS"

static const char FERRARIS_PAGE_INDEX[] = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ferraris-Zaehler</title>
<style>)HTML" FERRARIS_WEB_CSS R"HTML(
.hero{display:flex;flex-wrap:wrap;align-items:flex-end;gap:4px 16px}
.hero .v{font-size:44px;font-weight:600;line-height:1.1}
.hero .u{font-size:20px;color:var(--fg2);margin-left:4px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:12px}
.tiles .card{margin:0}
.tiles .v{font-size:22px;font-weight:600}
#chart{position:relative}
#chart svg{display:block;width:100%;height:200px;touch-action:none}
#tip{position:absolute;pointer-events:none;background:var(--fg);color:var(--bg);font-size:12px;padding:4px 8px;
border-radius:6px;white-space:nowrap;display:none;transform:translate(-50%,-110%)}
.sensor{display:grid;grid-template-columns:52px 1fr auto;gap:4px 12px;align-items:center;padding:8px 0;
border-top:1px solid var(--line)}
.sensor:first-of-type{border-top:0}
.sensor .n{font-weight:600}
.track{position:relative;height:14px;border-radius:4px;background:var(--bg);border:1px solid var(--line)}
.track .band{position:absolute;top:2px;bottom:2px;border-radius:3px;background:var(--band)}
.track .raw{position:absolute;top:-3px;bottom:-3px;width:3px;margin-left:-1.5px;border-radius:2px;background:var(--fg)}
.track .thr{position:absolute;top:-3px;bottom:-3px;border-left:1px dashed var(--fg2)}
.sensor .vals{grid-column:2/4;font-size:12px;color:var(--fg2)}
.st{font-size:12px;white-space:nowrap}
.kv{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px 16px}
.kv .v{font-weight:600}
.alert{border-color:var(--warn)}
footer{margin-top:4px}
</style></head><body><main>
<header><h1>Ferraris-Zaehler</h1>
<span class="chip" id="net"><span class="dot" id="netdot"></span><span id="nettxt">...</span></span>
<a class="chip" href="/wifi">WLAN einrichten</a><a class="chip" href="/config">Einstellungen</a></header>

<div class="card alert" id="safe" hidden><h2 class="warn">Abgesicherter Modus</h2>
<div>Der Zaehler ist mehrmals hintereinander abgestuerzt und laeuft deshalb mit Werkseinstellungen.
<a href="/config">Gespeicherte Einstellungen pruefen</a> und danach neu starten.</div></div>

<div class="card"><div class="l">Momentanleistung</div>
<div class="hero"><div><span class="v num" id="p">-</span><span class="u">W</span></div>
<div class="muted" id="pdir"></div></div></div>

<div class="tiles">
<div class="card"><div class="l">Bezug</div><div class="v num" id="imp">-</div><div class="muted">kWh</div></div>
<div class="card"><div class="l">Einspeisung</div><div class="v num" id="exp">-</div><div class="muted">kWh</div></div>
<div class="card"><div class="l">Netto</div><div class="v num" id="netkwh">-</div><div class="muted">kWh</div></div>
<div class="card"><div class="l">Umdrehungen</div><div class="v num" id="revs">-</div><div class="muted" id="kinfo"></div></div>
</div>

<div class="card"><h2>Leistung, letzte 60 Minuten (W)</h2>
<div id="chart" aria-label="Verlauf der Leistung"><svg id="svg"></svg><div id="tip"></div></div></div>

<div class="card"><h2>Lichtschranken</h2><div id="sensors"></div>
<p class="muted" style="margin:8px 0 0">Balken: Bereich zwischen Hintergrund (min) und Marke (max), Strich = aktueller Rohwert,
gestrichelt = Schaltschwellen. * = Zaehlpaar.</p></div>

<div class="card"><h2>System</h2><div class="kv" id="sys"></div></div>

<footer class="muted" id="foot"></footer>
</main><script>
const $=id=>document.getElementById(id);
const fmt=(v,d)=>v.toLocaleString('de-DE',{minimumFractionDigits:d,maximumFractionDigits:d});
let hist=[],step=10;

function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e;}

function renderSensors(d){
  const box=$('sensors');box.replaceChildren();
  const pct=v=>Math.max(0,Math.min(100,v/4095*100))+'%';
  for(const s of d.sensors){
    const row=el('div','sensor');
    const pair=d.count_pair.includes(s.name);
    row.append(el('div','n',s.name+(pair?' *':'')));
    const tr=el('div','track');
    const band=el('div','band');band.style.left=pct(s.min);band.style.width=`calc(${pct(s.max)} - ${pct(s.min)})`;tr.append(band);
    if(pair)for(const t of [s.low,s.high]){const x=el('div','thr');x.style.left=pct(t);tr.append(x);}
    const r=el('div','raw');r.style.left=pct(s.raw);tr.append(r);
    row.append(tr);
    const cls=s.hw=='OK'?'ok':(s.hw=='UNSICHER'?'warn':'crit');
    row.append(el('div','st '+cls,(s.hw=='OK'?'✓ ':'⚠ ')+s.hw));
    row.append(el('div','vals num',`GPIO${s.gpio} · roh ${s.raw} · min ${s.min} · max ${s.max}`+(pair?` · Schwellen ${s.high}/${s.low}`:'')));
    box.append(row);
  }
}

function renderSystem(d){
  const h=d.health,box=$('sys');box.replaceChildren();
  const kb=v=>fmt(v/1024,0)+' KB';
  const ota={ok:'bestaetigt',pruefung:'neue Firmware wird geprueft',zurueckgenommen:'letztes Update zurueckgenommen'};
  const items=[
    ['Letzter Start',h.reset,['Absturz','Task-Watchdog','Interrupt-Watchdog','Watchdog','Loop-Watchdog','Unterspannung'].includes(h.reset)?'warn':''],
    ['Starts / Abstuerze',`${h.boots} / ${h.crashes}`,h.crashes?'warn':''],
    ['Speicher frei',`${kb(h.heap_free)} (min ${kb(h.heap_min)})`,h.heap_min<20480?'warn':''],
    ['WLAN-Abbrueche',String(h.wifi_reconnects),''],
    ['MQTT',h.mqtt,h.mqtt=='getrennt'?'warn':''],
    ['ADC-Fehler / Timeouts',`${d.adc.errors} / ${d.adc.timeouts}`,''],
    ['Firmware',`${d.fw}, ${ota[h.ota]||h.ota}`,h.ota=='ok'?'':'warn'],
  ];
  if(h.coredump)items.push(['Absturzbericht','vorhanden (siehe Einstellungen)','warn']);
  for(const [l,v,c] of items){const e=el('div');e.append(el('div','l',l),el('div','v num '+c,v));box.append(e);}
  $('safe').hidden=!h.safe_mode;
}

function renderChart(){
  const svg=$('svg'),W=svg.clientWidth||600,H=200,L=48,R=8,T=8,B=22;
  svg.setAttribute('viewBox',`0 0 ${W} ${H}`);
  const n=hist.length,ns='http://www.w3.org/2000/svg';
  const mk=(t,a)=>{const e=document.createElementNS(ns,t);for(const k in a)e.setAttribute(k,a[k]);return e;};
  svg.replaceChildren();
  let lo=Math.min(0,...hist),hi=Math.max(10,...hist);
  const span=hi-lo,mag=Math.pow(10,Math.floor(Math.log10(span/3))),
        tick=[1,2,5,10].map(m=>m*mag).find(t=>span/t<=4);
  lo=Math.floor(lo/tick)*tick;hi=Math.ceil(hi/tick)*tick;
  const y=v=>T+(H-T-B)*(1-(v-lo)/(hi-lo));
  const css=getComputedStyle(document.documentElement),c=v=>css.getPropertyValue(v).trim();
  for(let v=lo;v<=hi+1e-9;v+=tick){
    svg.append(mk('line',{x1:L,x2:W-R,y1:y(v),y2:y(v),stroke:c(v==0?'--fg2':'--line'),'stroke-width':1}));
    const t=mk('text',{x:L-6,y:y(v)+4,'text-anchor':'end','font-size':11,fill:c('--fg2')});t.textContent=fmt(v,0);svg.append(t);
  }
  [['-60 min',L,'start'],['-30 min',(L+W-R)/2,'middle'],['jetzt',W-R,'end']].forEach(([s,px,a])=>{
    const t=mk('text',{x:px,y:H-6,'text-anchor':a,'font-size':11,fill:c('--fg2')});t.textContent=s;svg.append(t);});
  if(n<2){const t=mk('text',{x:(L+W)/2,y:H/2,'text-anchor':'middle','font-size':12,fill:c('--fg2')});
    t.textContent='Verlauf wird gesammelt ...';svg.append(t);return;}
  // Nur die vorhandenen Punkte, rechtsbuendig auf 360 Slots (60 min).
  const slots=360,xs=i=>L+(W-L-R)*((slots-n+i)/(slots-1));
  svg.append(mk('path',{d:hist.map((v,i)=>(i?'L':'M')+xs(i).toFixed(1)+','+y(v).toFixed(1)).join(''),
    fill:'none',stroke:c('--series'),'stroke-width':2,'stroke-linejoin':'round','stroke-linecap':'round'}));
  const cross=mk('line',{y1:T,y2:H-B,stroke:c('--fg2'),'stroke-width':1,visibility:'hidden'});
  const dot=mk('circle',{r:4,fill:c('--series'),stroke:c('--card'),'stroke-width':2,visibility:'hidden'});
  svg.append(cross,dot);
  const tip=$('tip');
  const move=e=>{
    const r=svg.getBoundingClientRect(),px=(e.clientX-r.left)*W/r.width;
    let i=Math.round((px-L)/(W-L-R)*(slots-1))-(slots-n);i=Math.max(0,Math.min(n-1,i));
    const cx=xs(i),cy=y(hist[i]);
    cross.setAttribute('x1',cx);cross.setAttribute('x2',cx);cross.setAttribute('visibility','visible');
    dot.setAttribute('cx',cx);dot.setAttribute('cy',cy);dot.setAttribute('visibility','visible');
    const ago=Math.round((n-1-i)*step/60);
    tip.textContent=(ago?`vor ${ago} min`:'jetzt')+': '+fmt(hist[i],0)+' W';
    tip.style.left=(cx*r.width/W)+'px';tip.style.top=(cy*r.height/H)+'px';tip.style.display='block';
  };
  svg.onpointermove=move;svg.onpointerdown=move;
  svg.onpointerleave=()=>{cross.setAttribute('visibility','hidden');dot.setAttribute('visibility','hidden');tip.style.display='none';};
}

async function tick(){
  try{
    const d=await (await fetch('/api/status',{cache:'no-store'})).json(),m=d.meter;
    $('p').textContent=fmt(Math.abs(m.power_w),0);
    $('pdir').textContent=m.power_w>0?'Bezug':(m.power_w<0?'Einspeisung':'Stillstand / noch keine volle Umdrehung');
    $('imp').textContent=fmt(m.import_kwh,3);$('exp').textContent=fmt(m.export_kwh,3);
    $('netkwh').textContent=fmt(m.net_kwh,3);
    $('revs').textContent=fmt((m.steps_import-m.steps_export)/4,2);$('kinfo').textContent=`k = ${m.k} U/kWh`;
    const w=d.wifi;
    $('netdot').style.background=w.connected?'var(--good)':(w.ap.active?'var(--warn)':'var(--crit)');
    $('nettxt').textContent=w.connected?`${w.ssid} · ${w.rssi} dBm`:(w.ap.active?`Access Point ${w.ap.ssid}`:'kein WLAN');
    renderSensors(d);renderSystem(d);
    const h=Math.floor(d.uptime_s/3600),mi=Math.floor(d.uptime_s/60)%60;
    $('foot').textContent=`${d.host} · Firmware ${d.fw} · Laufzeit ${h} h ${mi} min`+
      ` · ${d.counting?'zaehlt':'kalibriert ...'}${w.connected?' · IP '+w.ip:''}`+
      ` · ADC ${fmt(d.adc.sets_per_s,0)} Messungen/s${d.adc.restarts?' ('+d.adc.restarts+' Neustarts)':''}`;
    $('foot').className=d.adc.sets_per_s?'muted':'muted crit';
  }catch(e){$('nettxt').textContent='keine Verbindung zum Zaehler';$('netdot').style.background='var(--crit)';}
}
async function history(){
  try{const h=await (await fetch('/api/history',{cache:'no-store'})).json();hist=h.w;step=h.interval_s;renderChart();}catch(e){}
}
tick();history();setInterval(tick,2000);setInterval(history,10000);
addEventListener('resize',renderChart);
</script></body></html>)HTML";

static const char FERRARIS_PAGE_WIFI[] = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WLAN einrichten</title>
<style>)HTML" FERRARIS_WEB_CSS R"HTML(
main{max-width:520px}
#list{margin:0;padding:0;list-style:none}
#list li{display:flex;align-items:center;gap:10px;padding:9px 4px;border-top:1px solid var(--line);cursor:pointer}
#list li:first-child{border-top:0}
#list li:hover,#list li:focus{background:var(--bg);outline:none}
#list .s{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:4px;background:var(--line);border-radius:1px}
.bars i.on{background:var(--fg2)}
.row{display:flex;gap:8px;margin-top:14px;flex-wrap:wrap}
.pw{position:relative}.pw button{position:absolute;right:4px;top:4px;padding:5px 10px;font-size:13px}
#msg{margin-top:12px;min-height:1.4em}
</style></head><body><main>
<header><h1>WLAN einrichten</h1><a class="chip" href="/">&larr; Werte</a><a class="chip" href="/config">Einstellungen</a></header>

<div class="card"><h2>Status</h2><div id="state" class="num">...</div></div>

<div class="card"><h2>Netzwerke in der Naehe</h2>
<ul id="list"><li class="muted" style="cursor:default">Suche ...</li></ul>
<div class="row"><button class="sec" id="rescan" type="button">Neu suchen</button></div></div>

<form class="card" id="form" autocomplete="off"><h2>Verbindung</h2>
<label for="ssid">Netzwerkname (SSID)</label><input id="ssid" name="ssid" maxlength="32" required>
<label for="password">Passwort</label>
<div class="pw"><input id="password" name="password" type="password" maxlength="63">
<button class="sec" type="button" id="show">zeigen</button></div>
<p class="muted" style="margin:6px 0 0">Leer lassen = gespeichertes Passwort behalten (gleiche SSID) bzw. offenes Netz.</p>
<div class="row"><button id="save" type="submit">Speichern und verbinden</button></div>
<div id="msg" role="status"></div></form>
</main><script>
const $=id=>document.getElementById(id);
let waiting=false,target='';

function bars(rssi){const b=document.createElement('span');b.className='bars';
  const lvl=rssi>-55?4:rssi>-65?3:rssi>-75?2:1;
  for(let i=1;i<=4;i++){const e=document.createElement('i');e.style.height=(3*i+2)+'px';if(i<=lvl)e.className='on';b.append(e);}
  b.title=rssi+' dBm';return b;}

async function scan(){
  $('rescan').disabled=true;
  for(let i=0;i<20;i++){
    try{
      const r=await (await fetch('/api/scan',{cache:'no-store'})).json();
      if(!r.scanning){
        const ul=$('list');ul.replaceChildren();
        if(!r.networks.length){const li=document.createElement('li');li.className='muted';li.textContent='Keine Netzwerke gefunden';ul.append(li);}
        for(const n of r.networks){
          const li=document.createElement('li');li.tabIndex=0;
          const s=document.createElement('span');s.className='s';s.textContent=n.ssid;
          li.append(bars(n.rssi),s,document.createTextNode(n.secure?'🔒':''));
          const pick=()=>{$('ssid').value=n.ssid;$('password').value='';$('password').focus();};
          li.onclick=pick;li.onkeydown=e=>{if(e.key=='Enter')pick();};
          ul.append(li);
        }
        break;
      }
    }catch(e){}
    await new Promise(r=>setTimeout(r,1000));
  }
  $('rescan').disabled=false;
}

async function status(){
  try{
    const d=await (await fetch('/api/status',{cache:'no-store'})).json(),w=d.wifi;
    const lines=[];
    lines.push(w.connected?`Verbunden mit „${w.ssid}“ (${w.rssi} dBm), IP ${w.ip} – http://${d.host}.local/`
                          :(w.ssid?`Nicht verbunden (konfiguriert: „${w.ssid}“)`:'Kein WLAN konfiguriert'));
    lines.push(w.ap.active?`Access Point „${w.ap.ssid}“ aktiv, ${w.ap.clients} Geraet(e), IP ${w.ap.ip}`:'Access Point aus');
    $('state').replaceChildren(...lines.map(t=>{const p=document.createElement('div');p.textContent=t;return p;}));
    if(waiting&&w.connected&&w.ssid==target){
      waiting=false;
      $('msg').className='ok';
      $('msg').textContent=`Verbunden. Der Zaehler ist jetzt unter http://${w.ip}/ bzw. http://${d.host}.local/ im Heimnetz erreichbar. `+
        (w.ap.active?'Der Access Point schaltet sich in etwa einer Minute ab.':'');
    }
  }catch(e){if(waiting)$('msg').textContent='Verbindung zum Zaehler unterbrochen (Funkkanal wechselt) ...';}
}

$('show').onclick=()=>{const p=$('password');p.type=p.type=='password'?'text':'password';$('show').textContent=p.type=='password'?'zeigen':'verbergen';};
$('rescan').onclick=scan;
$('form').onsubmit=async e=>{
  e.preventDefault();
  $('save').disabled=true;$('msg').className='';$('msg').textContent='Speichere ...';
  try{
    const r=await fetch('/api/wifi',{method:'POST',body:new URLSearchParams(new FormData($('form')))});
    const j=await r.json();
    if(j.ok){target=$('ssid').value;waiting=true;$('msg').textContent='Gespeichert. Verbinde ...';}
    else{$('msg').className='crit';$('msg').textContent=j.error||'Fehler';}
  }catch(err){$('msg').className='crit';$('msg').textContent='Speichern fehlgeschlagen';}
  $('save').disabled=false;
};
status();scan();setInterval(status,2000);
</script></body></html>)HTML";

static const char FERRARIS_PAGE_CONFIG[] = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Einstellungen</title>
<style>)HTML" FERRARIS_WEB_CSS R"HTML(
main{max-width:620px}
select{font:inherit;width:100%;padding:9px 10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg)}
input[type=checkbox]{width:auto;margin:0 8px 0 0;accent-color:var(--btn)}
.check{display:flex;align-items:center;margin-top:14px;color:var(--fg)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:0 12px}
.card h2{display:flex;align-items:center;gap:8px}
.badge{font-size:11px;font-weight:500;padding:1px 8px;border-radius:999px;border:1px solid var(--line);color:var(--fg2)}
.hint{margin:6px 0 0}
.bad{border-color:var(--crit);outline:1px solid var(--crit)}
.row{display:flex;gap:8px;margin-top:14px;flex-wrap:wrap}
#banner{border-color:var(--warn)}
progress{width:100%;height:10px;margin-top:12px;accent-color:var(--btn)}
input[type=file]{padding:7px}
#otamsg{margin-top:10px;min-height:1.4em}
.btnlink{display:inline-block;padding:9px 16px;border-radius:8px;border:1px solid var(--line);color:var(--series);text-decoration:none}
#msg{margin-top:12px;min-height:1.4em}
table.cal{width:100%;border-collapse:collapse;font-size:13px;margin-top:10px}
table.cal th,table.cal td{text-align:right;padding:5px 6px;border-top:1px solid var(--line);white-space:nowrap}
table.cal th:first-child,table.cal td:first-child{text-align:left}
table.cal th{color:var(--fg2);font-weight:500;border-top:0}
table.cal tr.pick td{font-weight:600}
.calwrap{overflow-x:auto}
.kv{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px 16px}
.kv .v{font-weight:600}
.thrrow{padding:10px 0;border-top:1px solid var(--line)}
.thrrow:first-child{border-top:0}
.thrrow.off{opacity:.6}
.thrrow .grid{margin-bottom:8px}
.thrrow .vals{font-size:12px;color:var(--fg2);margin-top:6px}
.inrow{display:grid;grid-template-columns:52px 1fr;gap:4px 12px;align-items:center;margin-top:8px}
.inrow .n{font-weight:600}
.sensor{display:grid;grid-template-columns:52px 1fr;gap:4px 12px;align-items:center;padding:8px 0;border-top:1px solid var(--line)}
.sensor:first-child{border-top:0}
.sensor .n{font-weight:600}
.track{position:relative;height:14px;border-radius:4px;background:var(--bg);border:1px solid var(--line)}
.track .band{position:absolute;top:2px;bottom:2px;border-radius:3px;background:var(--band)}
.track .raw{position:absolute;top:-3px;bottom:-3px;width:3px;margin-left:-1.5px;border-radius:2px;background:var(--fg)}
.track .thr{position:absolute;top:-3px;bottom:-3px;border-left:1px dashed var(--fg2)}
.sensor .vals{grid-column:2;font-size:12px;color:var(--fg2)}
</style></head><body><main>
<header><h1>Einstellungen</h1><a class="chip" href="/">&larr; Werte</a><a class="chip" href="/wifi">WLAN</a></header>

<div class="card" id="safebox" hidden style="border-color:var(--warn)"><h2 class="warn">Abgesicherter Modus</h2>
<div>Nach mehreren Abstuerzen in Folge laeuft der Zaehler mit Werkseinstellungen. Die gespeicherten Werte stehen
unten im Formular, wirken aber gerade nicht. Werte pruefen (z. B. Eingaenge), speichern und neu starten.</div></div>

<div class="card" id="banner" hidden><h2>Neustart erforderlich</h2>
<div>Einige gespeicherte Aenderungen wirken erst nach einem Neustart.</div>
<p class="muted hint">Der Zaehlerstand wird vorher gesichert und laeuft danach weiter.</p>
<div class="row"><button type="button" id="restart">Jetzt neu starten</button></div></div>

<div class="card" id="calcard"><h2>Automatisch einmessen</h2>
<p class="muted hint" style="margin-top:0">Zeichnet alle aktiven Lichtschranken auf und schlaegt Zaehlpaar und Schwellen vor.
Die Zaehlung laeuft dabei weiter. Die Scheibe muss sich mindestens zweimal drehen - bei wenig Verbrauch entsprechend lange messen
(z. B. 1 Umdrehung pro Minute: 5 min).</p>
<div class="grid">
<div><label for="caldur">Dauer</label><select id="caldur">
<option value="120">2 min</option><option value="300" selected>5 min</option><option value="600">10 min</option>
<option value="1200">20 min</option><option value="1800">30 min</option></select></div>
</div>
<label class="check"><input type="checkbox" id="calrev">Es wird gerade eingespeist (Scheibe dreht rueckwaerts)</label>
<div class="row"><button type="button" id="calstart">Einmessen starten</button>
<button type="button" class="sec" id="calnow" hidden>Jetzt auswerten</button>
<button type="button" class="sec" id="calcancel" hidden>Abbrechen</button></div>
<progress id="calprog" max="1" value="0" hidden></progress>
<div id="calmsg" role="status" style="margin-top:10px"></div>
<div id="calres" hidden><div class="calwrap"><table class="cal"><thead><tr><th>Sensor</th><th>Ruhe bis</th><th>Marke</th>
<th>Ausschlag</th><th>HIGH</th><th>LOW</th><th>Durchgaenge</th></tr></thead><tbody id="calbody"></tbody></table></div>
<p class="muted hint" id="calinfo"></p>
<div class="row"><button type="button" id="calapply">Vorschlag ins Formular uebernehmen</button></div></div></div>

<div class="card" id="cntcard"><h2>Zaehlerstand</h2>
<div class="kv num" id="cntinfo">...</div>
<p class="muted hint" id="cnthint"></p>
<div class="row"><button type="button" class="sec" id="cntreset" hidden>Zaehler zuruecksetzen</button></div>
<div id="cntmsg" role="status" style="margin-top:10px"></div></div>

<form id="form" autocomplete="off" novalidate>
<div class="card"><h2>Zaehler <span class="badge">sofort</span></h2>
<label for="k">Zaehlerkonstante k (Umdrehungen je kWh, steht auf dem Typenschild)</label>
<input id="k" type="number" min="1" max="10000" step="1" inputmode="numeric">
<p class="muted hint">Bisherige Werte werden mit dem neuen k umgerechnet (gezaehlt werden Umdrehungen).</p></div>

<div class="card"><h2>Schaltschwellen je Lichtschranke <span class="badge">sofort</span></h2>
<p class="muted hint" style="margin-top:0">Reihenfolge: <b>Ruhepegel &lt; LOW &lt; HIGH &lt; Marke</b>. Ueber HIGH gilt die Marke als
erkannt, erst unter LOW wieder als vorbei. Liegt LOW unter dem Ruhepegel, bleibt die Schranke haengen und es wird nicht gezaehlt.
In Ruhe muss der Strich (Rohwert) links von beiden Linien stehen, bei der Marke rechts davon. Mit WLAN liegt der Ruhepegel hoeher.</p>
<p class="warn hint" id="atwarn" hidden>Auto-Tune ist aktiv: die Schwellen des Zaehlpaars werden alle paar Minuten automatisch neu gesetzt.</p>
<div id="thr"></div>
<div class="row"><button type="button" class="sec" id="thrall">Werte von Sensor A fuer alle uebernehmen</button></div>
<label class="check"><input type="checkbox" id="envelope_auto">min/max automatisch mitlaufen lassen (sonst fest 0..4095)</label></div>

<div class="card"><h2>Eingaenge <span class="badge">nach Neustart</span></h2>
<label for="sensor_count">Anzahl Lichtschranken</label><select id="sensor_count"></select>
<div id="inputs"></div>
<p class="muted hint">Nur ADC1-Pins (GPIO32..39), jeder Pin nur einmal. GPIO37/38 sind auf den meisten
Boards nicht herausgefuehrt. Mehr Eingaenge teilen sich die Abtastrate (<span id="rate"></span>).</p>
<div class="grid">
<div><label for="count_a">Zaehlpaar Sensor A (Marke kommt bei Bezug zuerst)</label><select id="count_a"></select></div>
<div><label for="count_b">Zaehlpaar Sensor B</label><select id="count_b"></select></div>
</div></div>

<div class="card"><h2>Netzwerk und MQTT <span class="badge">nach Neustart</span></h2>
<label for="hostname">Hostname (http://name.local/)</label>
<input id="hostname" maxlength="32" autocapitalize="off" spellcheck="false">
<label for="mqtt_uri">MQTT-Broker (leer = kein MQTT)</label>
<input id="mqtt_uri" maxlength="127" placeholder="mqtt://broker.local:1883" autocapitalize="off" spellcheck="false">
<div class="grid">
<div><label for="mqtt_user">Benutzer</label><input id="mqtt_user" maxlength="64" autocapitalize="off" spellcheck="false"></div>
<div><label for="mqtt_password">Passwort</label><input id="mqtt_password" type="password" maxlength="64"></div>
</div>
<div class="grid">
<div><label for="mqtt_prefix">Topic-Praefix</label><input id="mqtt_prefix" maxlength="64" autocapitalize="off" spellcheck="false"></div>
<div><label for="mqtt_interval_s">Sendeintervall (s)</label><input id="mqtt_interval_s" type="number" min="1" max="3600" step="1" inputmode="numeric"></div>
</div>
<p class="muted hint" id="topics"></p></div>

<div class="card"><h2>Zugang <span class="badge">nach Neustart</span></h2>
<div class="grid">
<div><label for="ap_password">Neues Passwort</label><input id="ap_password" type="password" maxlength="63" autocomplete="new-password"></div>
<div><label for="ap_password2">Wiederholen</label><input id="ap_password2" type="password" maxlength="63" autocomplete="new-password"></div>
</div>
<p class="muted hint">Gilt fuer den Einrichtungs-Access-Point und die Anmeldung (Benutzer admin). 8..63 Zeichen, leer = unveraendert.
Gut merken: ohne das Passwort hilft nur neu flashen mit Loeschen des Speichers.</p></div>

<div class="row"><button type="submit" id="save">Speichern</button>
<button type="button" class="sec" id="defaults">Standardwerte eintragen</button></div>
<div id="msg" role="status"></div>
</form>

<div class="card" id="ota" style="margin-top:12px"><h2>Firmware-Update</h2>
<div class="num" id="fwinfo">...</div>
<div id="otastate" class="hint"></div>
<label for="fwfile">Neue Firmware (<code>firmware.bin</code> aus <code>.pio/build/esp32/</code>)</label>
<input id="fwfile" type="file" accept=".bin,application/octet-stream">
<progress id="otaprog" max="1" value="0" hidden></progress>
<p class="muted hint">Der Zaehler startet danach neu. Zaehlerstand, Einstellungen und WLAN bleiben.
Schlaegt das Update fehl, laeuft die bisherige Firmware unveraendert weiter.</p>
<div class="row"><button type="button" id="otabtn" disabled>Installieren</button></div>
<div id="otamsg" role="status"></div></div>

<div class="card" id="cdbox" hidden><h2>Absturzbericht</h2>
<div id="cdinfo" class="num"></div>
<p class="muted hint">Auswerten: <code>esp-coredump info_corefile --core-format raw --core ferraris-coredump.bin
.pio/build/esp32/firmware.elf</code> (gleiche Firmware-Version wie beim Absturz).</p>
<div class="row"><a class="btnlink" href="/api/coredump" download>Herunterladen</a>
<button type="button" class="sec" id="cddel">Loeschen</button></div>
<div id="cdmsg" role="status" style="margin-top:10px"></div></div>
</main><script>
const $=id=>document.getElementById(id);
const F=['k','sensor_count','count_a','count_b','hostname','mqtt_uri','mqtt_user','mqtt_prefix','mqtt_interval_s'];
const P=['mqtt_password','ap_password','ap_password2'];
const GPIOS={32:'interne Pull-Widerstaende',33:'interne Pull-Widerstaende',34:'',35:'',36:'VP',37:'meist nicht herausgefuehrt',
  38:'meist nicht herausgefuehrt',39:'VN'};
let cfg=null,last=null,G=[],T=[];

function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e;}
function say(cls,t){$('msg').className=cls;$('msg').textContent=t;}

function options(sel,n,label){
  const v=sel.value;sel.replaceChildren();
  for(let i=1;i<=n;i++){const o=el('option',null,label(i));o.value=i;sel.append(o);}
  if(v)sel.value=v;
}
function buildInputs(){
  const box=$('inputs');box.replaceChildren();G=[];
  for(let i=1;i<=cfg.max_sensors;i++){
    const row=el('div','inrow'),lab=el('label','n','Q'+i),sel=el('select');
    sel.id='gpio'+i;lab.htmlFor=sel.id;lab.style.margin='0';
    for(const g in GPIOS){const o=el('option',null,'GPIO'+g+(GPIOS[g]?' – '+GPIOS[g]:''));o.value=g;sel.append(o);}
    row.append(lab,sel);box.append(row);G.push(sel);
  }
  // Schwellen je Eingang
  const tb=$('thr');tb.replaceChildren();T=[];
  for(let i=1;i<=cfg.max_sensors;i++){
    const row=el('div','thrrow'),head=el('div','n'),grid=el('div','grid');
    const name=el('b',null,'Q'+i),badge=el('span','badge');badge.style.marginLeft='8px';head.append(name,badge);
    const mk=(kind,min,max)=>{const d=el('div'),l=el('label',null,kind=='high'?'HIGH (Marke erkannt)':'LOW (Marke vorbei)'),
      inp=el('input');inp.id=kind+i;inp.type='number';inp.min=min;inp.max=max;inp.step=1;inp.inputMode='numeric';
      l.htmlFor=inp.id;d.append(l,inp);grid.append(d);return inp;};
    const hi=mk('high',1,4095),lo=mk('low',0,4094);
    const tr=el('div','track'),vals=el('div','vals num');
    row.append(head,grid,tr,vals);tb.append(row);
    T.push({row,badge,hi,lo,tr,vals});
  }
}
function inputs(){
  const n=+$('sensor_count').value,seen={};
  G.forEach((sel,i)=>{sel.parentNode.hidden=i>=n;sel.classList.remove('bad');});
  G.slice(0,n).forEach(sel=>{if(seen[sel.value]){sel.classList.add('bad');seen[sel.value].classList.add('bad');}seen[sel.value]=sel;});
  for(const id of ['count_a','count_b'])options($(id),n,i=>'Q'+i);
  const a=+$('count_a').value,b=+$('count_b').value;
  T.forEach((t,i)=>{
    t.row.hidden=i>=n;
    const role=i+1==a?'Sensor A':(i+1==b?'Sensor B':'nicht im Zaehlpaar');
    t.badge.textContent=role;t.row.classList.toggle('off',i+1!=a&&i+1!=b);
  });
  $('rate').textContent=`${n} Eingaenge: je ca. ${Math.round(50000/n/100)/10} kHz`;
}
function fill(s){
  $('sensor_count').value=s.sensor_count;
  s.gpio.forEach((g,i)=>{if(G[i])G[i].value=g;});
  T.forEach((t,i)=>{t.hi.value=s.high[i];t.lo.value=s.low[i];});
  inputs();
  for(const f of F)$(f).value=s[f];
  $('envelope_auto').checked=s.envelope_auto;
  $('mqtt_password').placeholder=cfg.settings.mqtt_password_set?'gespeichert, leer = behalten':'keins';
  update();
}
function update(){
  inputs();
  const p=$('mqtt_prefix').value.trim()||'?';
  $('topics').textContent=$('mqtt_uri').value.trim()?`Topics: ${p}/state (JSON), ${p}/status (online/offline)`:'MQTT aus';
  renderLive();
}
function renderLive(){
  const pct=v=>Math.max(0,Math.min(100,v/4095*100))+'%';
  T.forEach((t,i)=>{
    if(t.row.hidden)return;
    const h=+t.hi.value,l=+t.lo.value,s=last&&last.sensors.find(x=>x.name=='Q'+(i+1));
    t.tr.replaceChildren();
    if(s){const band=el('div','band');band.style.left=pct(s.min);band.style.width=`calc(${pct(s.max)} - ${pct(s.min)})`;t.tr.append(band);}
    for(const v of [l,h]){const x=el('div','thr');x.style.left=pct(v);t.tr.append(x);}
    if(s){const r=el('div','raw');r.style.left=pct(s.raw);t.tr.append(r);}
    const parts=[];
    if(s)parts.push(`roh ${s.raw} · min ${s.min} · max ${s.max}`);
    parts.push(h>l?`Hysterese ${h-l}`:'HIGH muss groesser als LOW sein');
    t.vals.textContent=parts.join(' · ');t.vals.className='vals num'+(h>l?'':' crit');
  });
}
function banner(on){$('banner').hidden=!on;}
function otaSay(cls,t){$('otamsg').className=cls;$('otamsg').textContent=t;}
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
/** Nach einem Neustart warten, bis der Zaehler wieder antwortet, dann neu laden. */
async function awaitReboot(show){
  const host=cfg&&cfg.settings.hostname;
  show('','Startet neu ...'+(host?` Danach erreichbar unter http://${host}.local/`:''));
  await sleep(4000);
  for(let i=0;i<30;i++){
    try{const d=await (await fetch('/api/status',{cache:'no-store'})).json();if(d.uptime_s<120){location.reload();return true;}}catch(e){}
    await sleep(2000);
  }
  show('warn','Der Zaehler meldet sich noch nicht. Seite spaeter neu laden.');
  return false;
}

async function load(){
  try{
    const r=await fetch('/api/config',{cache:'no-store'});
    cfg=await r.json();
    options($('sensor_count'),cfg.max_sensors,i=>String(i));
    [...$('sensor_count').options].forEach(o=>{if(+o.value<cfg.min_sensors)o.remove();});
    buildInputs();
    fill(cfg.settings);banner(cfg.restart_pending);$('atwarn').hidden=!cfg.autotune;
    const fw=cfg.firmware;
    $('fwinfo').textContent=`Laeuft: Version ${fw.version} (Bereich ${fw.partition}), Platz fuer Updates ${Math.round(fw.ota_max/1024)} KB`;
    const os=$('otastate');
    if(fw.ota_state=='pruefung'){os.className='warn hint';os.textContent=`Neue Firmware wird geprueft: sie wird bestaetigt, sobald Messung und Netz `+
      `ca. 30 s stabil laufen, sonst in spaetestens ${Math.ceil(fw.ota_left_s/60)} min automatisch zurueckgenommen.`;}
    else if(fw.ota_state=='zurueckgenommen'){os.className='warn hint';os.textContent='Das letzte Update wurde zurueckgenommen, '+
      'weil die neue Firmware abgestuerzt ist oder nicht gesund lief. Es laeuft wieder die vorige Version.';}
    else{os.className='muted hint';os.textContent='Nach einem Update muss sich die neue Firmware erst bewaehren, sonst wird automatisch zurueckgeschaltet.';}
    $('safebox').hidden=!fw.safe_mode;
    const cd=fw.coredump;$('cdbox').hidden=!cd.present;
    if(cd.present)$('cdinfo').textContent=`${Math.round(cd.size/1024)} KB, Task ${cd.task||'?'}, PC ${cd.pc}`+(cd.reason?` – ${cd.reason}`:'');
  }catch(e){say('crit','Einstellungen konnten nicht geladen werden');}
}
const kwh=v=>v.toLocaleString('de-DE',{minimumFractionDigits:3,maximumFractionDigits:3});
function ago(s){return s<90?`${s} s`:(s<5400?`${Math.round(s/60)} min`:`${Math.round(s/3600)} h`);}
function renderCounter(){
  if(!last)return;
  const m=last.meter,box=$('cntinfo');box.replaceChildren();
  const rows=[['Bezug',kwh(m.import_kwh)+' kWh'],['Einspeisung',kwh(m.export_kwh)+' kWh'],['Netto',kwh(m.net_kwh)+' kWh'],
    ['Umdrehungen',((m.steps_import-m.steps_export)/4).toLocaleString('de-DE')],
    ['Gesichert',m.saved_s<0?'noch nicht':`vor ${ago(m.saved_s)}`]];
  for(const [l,v] of rows){const e=el('div');e.append(el('div','l',l),el('div','v',v));box.append(e);}
  $('cnthint').textContent=cfg&&!cfg.counter_persist?'Der Zaehlerstand wird nicht gespeichert und beginnt nach jedem Neustart bei 0.':
    'Gespeichert wird regelmaessig (nur bei Aenderung) und vor jedem Neustart oder Firmware-Update. '+
    'Bei Stromausfall fehlen hoechstens die Umdrehungen seit der letzten Sicherung.';
  $('cntreset').hidden=false;
}
async function live(){
  try{last=await (await fetch('/api/status',{cache:'no-store'})).json();renderLive();renderCounter();}catch(e){}
}
$('cntreset').onclick=async()=>{
  if(!confirm('Zaehlerstand wirklich auf 0 setzen? Bezug, Einspeisung und Umdrehungen gehen verloren.'))return;
  const msg=$('cntmsg');$('cntreset').disabled=true;
  try{
    const j=await (await fetch('/api/counter/reset',{method:'POST',headers:{'X-Ferraris':'1'}})).json();
    msg.className=j.ok?'ok':'crit';msg.textContent=j.ok?'Zaehlerstand auf 0 gesetzt.':(j.error||'Fehler');
    live();
  }catch(e){msg.className='crit';msg.textContent='Zuruecksetzen fehlgeschlagen';}
  $('cntreset').disabled=false;
};

$('form').oninput=e=>{e.target.classList.remove('bad');update();};
$('thrall').onclick=()=>{
  const a=T[+$('count_a').value-1];if(!a)return;
  T.forEach(t=>{t.hi.value=a.hi.value;t.lo.value=a.lo.value;});update();
  say('','Schwellen von Sensor A auf alle Eingaenge uebertragen - noch nicht gespeichert.');
};
$('defaults').onclick=()=>{if(cfg){fill(cfg.defaults);say('','Standardwerte eingetragen (Passwoerter unveraendert) - noch nicht gespeichert.');}};
$('form').onsubmit=async e=>{
  e.preventDefault();
  for(const f of [...F,...P])$(f).classList.remove('bad');
  const n=+$('sensor_count').value,used=G.slice(0,n).map(s=>s.value);
  if(new Set(used).size!=used.length){inputs();say('crit','Jeder GPIO darf nur einmal vorkommen');return;}
  for(const id of ['count_a','count_b'])if(!$(id).value){$(id).classList.add('bad');say('crit','Zaehlpaar unter den aktiven Eingaengen waehlen');return;}
  if($('ap_password').value!=$('ap_password2').value){$('ap_password2').classList.add('bad');say('crit','Passwoerter stimmen nicht ueberein');return;}
  const body=new URLSearchParams();
  for(const f of F)body.set(f,$(f).value.trim());
  G.forEach(sel=>body.set(sel.id,sel.value));
  T.forEach(t=>{body.set(t.hi.id,t.hi.value.trim());body.set(t.lo.id,t.lo.value.trim());});
  body.set('envelope_auto',$('envelope_auto').checked?'1':'0');
  for(const f of P)body.set(f,$(f).value);
  $('save').disabled=true;say('','Speichere ...');
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'X-Ferraris':'1'},body});
    const j=await r.json();
    if(j.ok){
      for(const f of P)$(f).value='';
      await load();
      say('ok',j.restart_pending?'Gespeichert. Zaehler, Schwellen und Huelle gelten sofort, der Rest nach dem Neustart.':'Gespeichert und sofort aktiv.');
    }else{
      const f=j.field&&$(j.field);if(f){f.classList.add('bad');f.focus();}
      say('crit',j.error||'Fehler');
    }
  }catch(err){say('crit','Speichern fehlgeschlagen');}
  $('save').disabled=false;
};
$('restart').onclick=async()=>{
  if(!confirm('Jetzt neu starten? Der Zaehlerstand wird vorher gesichert.'))return;
  $('restart').disabled=true;
  try{await fetch('/api/restart',{method:'POST',headers:{'X-Ferraris':'1'}});}catch(e){}
  if(!await awaitReboot(say))$('restart').disabled=false;
};
$('fwfile').onchange=async()=>{
  const f=$('fwfile').files[0];$('otabtn').disabled=true;otaSay('','');
  if(!f)return;
  const head=new Uint8Array(await f.slice(0,1).arrayBuffer());
  if(head[0]!=0xE9){otaSay('crit','Das ist keine ESP32-App. Bitte firmware.bin verwenden (nicht firmware.factory.bin).');return;}
  if(cfg&&f.size>cfg.firmware.ota_max){otaSay('crit',`Datei zu gross (${Math.round(f.size/1024)} KB, Platz ${Math.round(cfg.firmware.ota_max/1024)} KB).`);return;}
  $('otabtn').disabled=false;otaSay('muted',`${f.name}, ${Math.round(f.size/1024)} KB`);
};
$('otabtn').onclick=()=>{
  const f=$('fwfile').files[0];if(!f)return;
  if(!confirm('Firmware installieren? Der Zaehler startet danach neu, der Zaehlerstand wird vorher gesichert.'))return;
  const fd=new FormData();fd.append('firmware',f,f.name);
  const x=new XMLHttpRequest(),prog=$('otaprog');
  x.open('POST','/api/ota');x.setRequestHeader('X-Ferraris','1');
  x.upload.onprogress=e=>{if(e.lengthComputable){prog.value=e.loaded/e.total;otaSay('',`Uebertrage ... ${Math.round(e.loaded/e.total*100)} %`);}};
  x.upload.onload=()=>otaSay('','Pruefe und installiere ...');
  const done=ok=>{$('otabtn').disabled=ok;$('fwfile').disabled=ok;if(!ok)prog.hidden=true;};
  x.onload=async()=>{
    let j={};try{j=JSON.parse(x.responseText);}catch(e){}
    if(x.status==200&&j.ok){otaSay('ok','Installiert.');await awaitReboot(otaSay);done(false);}
    else{otaSay('crit',j.error||`Fehler (HTTP ${x.status})`);done(false);}
  };
  x.onerror=()=>{otaSay('crit','Verbindung abgebrochen');done(false);};
  prog.value=0;prog.hidden=false;done(true);x.send(fd);
};
// ─── Einmessen ───
let calTimer=null,calLast=null;
const mmss=s=>`${Math.floor(s/60)}:${String(s%60).padStart(2,'0')}`;
function calSay(cls,t){$('calmsg').className=cls;$('calmsg').textContent=t;}
function calRender(c){
  calLast=c;const run=c.state=='running';
  $('calstart').hidden=run;$('calnow').hidden=!run;$('calcancel').hidden=!run;$('caldur').disabled=run;$('calrev').disabled=run;
  $('calprog').hidden=!run;
  if(run){
    $('calprog').value=Math.min(1,c.elapsed_s/c.duration_s);
    calSay('',`Zeichne auf ... ${mmss(c.elapsed_s)} von ${mmss(c.duration_s)}`+(c.bucket_ms?`, Aufloesung ${Math.round(c.bucket_ms)} ms`:''));
  }
  const r=c.result;$('calres').hidden=!(c.state=='done'&&r);
  if(c.state!='done'||!r){if(!run&&c.state=='idle')calSay('','');return;}
  calSay(r.ok?'ok':'crit',r.message);
  const body=$('calbody');body.replaceChildren();
  r.sensors.forEach((s,i)=>{
    const q=i+1,role=q==r.count_a?' (A)':(q==r.count_b?' (B)':'');
    const tr=el('tr',r.ok&&role?'pick':'');
    const cells=['Q'+q+role,s.rest_hi,s.peak,s.gap,s.usable?s.high:'-',s.usable?s.low:'-',s.usable?s.passes:'zu schwach'];
    cells.forEach(v=>tr.append(el('td',null,String(v))));
    if(!s.usable)tr.className='muted';
    body.append(tr);
  });
  const info=[];
  if(r.ok){
    info.push(`Zaehlpaar A = Q${r.count_a}, B = Q${r.count_b} (${r.pairs_valid} von ${r.pairs_checked} Paaren mit sauberer Reihenfolge), ${r.revolutions} Umdrehungen gesehen.`);
    const k=+$('k').value;
    if(r.period_s>0&&k>0)info.push(`Eine Umdrehung dauerte ca. ${fmt1(r.period_s)} s - das entspricht ca. ${Math.round(3.6e6/(k*r.period_s))} W bei k = ${k}.`);
  }
  info.push(`Aufloesung der Aufzeichnung ${Math.round(r.bucket_ms)} ms.`);
  $('calinfo').textContent=info.join(' ');
  $('calapply').hidden=!r.ok;
}
const fmt1=v=>v.toLocaleString('de-DE',{maximumFractionDigits:1});
async function calPoll(){
  try{
    const c=await (await fetch('/api/calib',{cache:'no-store'})).json();
    calRender(c);
    clearTimeout(calTimer);
    if(c.state=='running')calTimer=setTimeout(calPoll,2000);
  }catch(e){calTimer=setTimeout(calPoll,4000);}
}
async function calPost(url,body){
  const r=await fetch(url,{method:'POST',headers:{'X-Ferraris':'1'},body:new URLSearchParams(body||{})});
  const j=await r.json();if(!j.ok)throw new Error(j.error||'Fehler');
}
$('calstart').onclick=async()=>{
  try{await calPost('/api/calib/start',{duration_s:$('caldur').value,reverse:$('calrev').checked?'1':'0'});calPoll();}
  catch(e){calSay('crit',e.message);}
};
$('calnow').onclick=async()=>{try{calSay('','Werte aus ...');await calPost('/api/calib/stop',{mode:'finish'});calPoll();}catch(e){calSay('crit',e.message);}};
$('calcancel').onclick=async()=>{try{await calPost('/api/calib/stop',{mode:'cancel'});calPoll();}catch(e){calSay('crit',e.message);}};
$('calapply').onclick=()=>{
  const r=calLast&&calLast.result;if(!r||!r.ok)return;
  r.sensors.forEach((s,i)=>{if(s.usable&&T[i]){T[i].hi.value=s.high;T[i].lo.value=s.low;}});
  $('count_a').value=r.count_a;$('count_b').value=r.count_b;update();
  say('',`Vorschlag eingetragen (Zaehlpaar Q${r.count_a}/Q${r.count_b}, Schwellen der nutzbaren Sensoren). Jetzt "Speichern" - `+
    'die Schwellen wirken sofort, ein geaendertes Zaehlpaar nach dem Neustart.');
  $('save').scrollIntoView({behavior:'smooth',block:'center'});
};

$('cddel').onclick=async()=>{
  if(!confirm('Absturzbericht loeschen?'))return;
  try{
    const j=await (await fetch('/api/coredump/delete',{method:'POST',headers:{'X-Ferraris':'1'}})).json();
    if(j.ok){$('cdbox').hidden=true;}else{$('cdmsg').className='crit';$('cdmsg').textContent=j.error||'Fehler';}
  }catch(e){$('cdmsg').className='crit';$('cdmsg').textContent='Loeschen fehlgeschlagen';}
};
load().then(calPoll);live();setInterval(live,2000);
</script></body></html>)HTML";

#endif // FERRARIS_WEB_PAGES_H
