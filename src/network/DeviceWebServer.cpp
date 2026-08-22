#include "DeviceWebServer.hpp"

#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "AppConfig.hpp"
#include "diag/BootLog.hpp"
#include "diag/LogRing.hpp"
#include "display/DisplayTask.hpp"
#include "diag/Recovery.hpp"
#include "network/OtaService.hpp"
#include "storage/FirmwareStore.hpp"
#include "storage/ConfigStore.hpp"

namespace {

DeviceWebServer* g_instance = nullptr;

const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5Companion</title><style>
body{font-family:system-ui,sans-serif;background:#12161a;color:#e8eaed;margin:0;padding:18px;max-width:760px}
h1{font-size:19px;color:#ff8a3d;margin:0 0 2px}p.sub{color:#8b959d;margin:0 0 16px;font-size:13px}
.tabs{display:flex;gap:6px;margin:0 0 14px;flex-wrap:wrap}
.tabs button{background:#1b2127;border:1px solid #2c343b}
.tabs button.on{background:#ff8a3d;color:#12161a;font-weight:600;border-color:#ff8a3d}
.card{background:#1b2127;border:1px solid #2c343b;border-radius:10px;padding:14px;margin:0 0 14px}
.row{display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid #252c33}
.row:last-child{border:0}.name{flex:1;font-size:15px}
.tag{font-size:11px;color:#12161a;background:#7ec96a;padding:2px 7px;border-radius:20px}
button{padding:7px 13px;border:0;border-radius:7px;background:#2c343b;color:#e8eaed;font-size:13px;cursor:pointer}
button.primary{background:#ff8a3d;color:#12161a;font-weight:600}
button.danger{background:#5a2630;color:#ffb3b3}
input[type=file]{width:100%;margin:8px 0;color:#8b959d;font-size:13px}
input[type=text]{width:100%;padding:8px;border-radius:7px;border:1px solid #2c343b;background:#12161a;color:#e8eaed;box-sizing:border-box}
pre{white-space:pre-wrap;word-break:break-all;font-family:ui-monospace,monospace;font-size:12px;color:#c6cdd3;margin:0;max-height:60vh;overflow:auto}
#log{white-space:pre-wrap;font-family:ui-monospace,monospace;font-size:12px;color:#8b959d;margin-top:10px}
progress{width:100%;height:6px}
table{width:100%;border-collapse:collapse;font-size:13px}
td,th{text-align:left;padding:5px 6px;border-bottom:1px solid #252c33}
th{color:#8b959d;font-weight:500}
.k{color:#8b959d}.bad{color:#ff7b7b}.good{color:#7ec96a}
#banner{border-color:#7a4a1a;background:#241a10}
#banner b{color:#ff8a3d}
</style>
<h1>M5Companion</h1><p class="sub" id="sub">connecting...</p>
<div class="card" id="banner" hidden></div>
<div class="tabs">
  <button data-t="status" class="on">Status</button>
  <button data-t="log">Log</button>
  <button data-t="boots">Boots</button>
  <button data-t="fw">Firmware</button>
  <button data-t="packs">Packs</button>
</div>

<div class="pane" id="p-status"><div class="card"><table id="status"><tr><td>loading...</td></tr></table></div></div>

<div class="pane" id="p-log" hidden><div class="card">
  <div class="row"><span class="name"><b>Firmware log</b></span>
    <label class="k"><input type="checkbox" id="follow" checked> follow</label>
    <button onclick="LOG.seq=0;LOG.text='';pollLog()">reload all</button></div>
  <pre id="logtext">...</pre></div>
  <div class="card"><b>Before the last restart</b>
  <p class="sub" style="margin:6px 0">Kept in RTC memory, which a crash or a watchdog leaves alone.
  Empty means the power actually went away.</p>
  <pre id="prevtail">...</pre></div></div>

<div class="pane" id="p-boots" hidden><div class="card">
  <b>Boot history</b>
  <p class="sub" style="margin:6px 0">Written to flash once a minute, so it survives losing power.
  A boot with no <i>clean</i> mark ended without the firmware asking.</p>
  <table id="boots"><tr><td>loading...</td></tr></table></div></div>

<div class="pane" id="p-fw" hidden>
  <div class="card"><b>Pull from a URL</b>
  <p class="sub" style="margin:6px 0">Serve the build directory
  (<code>python3 -m http.server 8000</code>) and give the device the address of
  <code>firmware.bin</code>. The device fetches it itself, so nothing needs to reach the device.</p>
  <input type="text" id="url" placeholder="http://192.168.68.200:8000/firmware.bin">
  <p><button class="primary" onclick="pull()">Fetch and install</button></p>
  <div id="pullstate" class="sub"></div></div>
  <div class="card"><b>Upload a .bin</b>
  <p class="sub" style="margin:6px 0">The same image PlatformIO writes over USB:
  <code>.pio/build/m5go/firmware.bin</code>.</p>
  <input type="file" id="bin" accept=".bin">
  <p><button class="primary" onclick="upfw()">Install and restart</button></p>
  <progress id="fwbar" value="0" max="100" hidden></progress>
  <div id="fwstate" class="sub"></div></div>
  <div class="card"><b>Backups on the card</b>
  <p class="sub" style="margin:6px 0">Written automatically the first time each build
  proves itself - a minute of healthy running. "Known good" means it ran, not that
  someone labelled it.</p>
  <table id="backups"><tr><td>loading...</td></tr></table>
  <p><button onclick="backupNow()">Back up the running firmware now</button></p>
  <div id="bkstate" class="sub"></div></div>

  <div class="card"><b>Recovery</b>
  <table id="recovery"><tr><td>loading...</td></tr></table>
  <p><button onclick="enterRecovery()">Hand over to the recovery application</button>
     <button onclick="bootNormal()">Boot normally</button></p>
  <p class="sub" style="margin:6px 0">The recovery application lives in the factory
  partition, which OTA cannot write. It reads a backup off the card, puts it back and
  restarts - no Wi-Fi, no audio, no pack.</p>
  <div id="rcstate" class="sub"></div></div>

  <div class="card"><b>Restart</b>
  <p class="sub" style="margin:6px 0">Recorded as deliberate, so it is not read back later as a power fault.</p>
  <button class="danger" onclick="reboot()">Reboot</button></div>
</div>

<div class="pane" id="p-packs" hidden>
  <div class="card" id="packs">loading...</div>
  <div class="card">
    <b>Upload a pack</b>
    <p class="sub" style="margin:6px 0">Pick the files from one pack directory, including manifest.json.
    They are written to /companion/packs/&lt;name&gt;/.</p>
    <input type="text" id="name" placeholder="pack name, e.g. kizuna">
    <input type="file" id="files" multiple webkitdirectory>
    <button class="primary" onclick="upload()">Upload</button>
    <progress id="bar" value="0" max="100" hidden></progress>
    <div id="log"></div>
  </div>
</div>

<script>
const $=(i)=>document.getElementById(i);
let TAB='status';
document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{
  TAB=b.dataset.t;
  document.querySelectorAll('.tabs button').forEach(x=>x.classList.toggle('on',x===b));
  document.querySelectorAll('.pane').forEach(p=>p.hidden=p.id!=='p-'+TAB);
  if(TAB==='boots')pollBoots(); if(TAB==='packs')refresh(); if(TAB==='log')pollLog();
  if(TAB==='fw')pollFirmware();
});

const ms=(v)=>{const s=Math.floor(v/1000);return s<60?s+'s':s<3600?Math.floor(s/60)+'m '+(s%60)+'s':
  Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m'};
const kb=(v)=>Math.round(v/1024)+' kB';

async function pollStatus(){
  try{
    const j=await(await fetch('/api/status')).json();
    $('sub').textContent=j.name+'  -  '+j.ip+'  -  fw '+j.fw+'  -  up '+ms(j.uptime_ms);
    const rows=[
      ['state',j.state],['expression',j.expression],
      ['wifi',(j.wifi?'<span class=good>'+j.ssid+' '+j.rssi+' dBm</span>':'<span class=bad>down</span>')],
      ['server',(j.server?'<span class=good>connected</span>':'<span class=bad>offline</span>')],
      ['battery',j.battery+'%'+(j.charging?' <span class=good>(charging)</span>':' <span class=bad>(on battery)</span>')],
      ['boot','#'+j.boot+', reason '+j.reason],
      ['heap','free '+kb(j.heap)+', low water '+kb(j.min_heap)],
      ['uplink',j.uplink_chunks+' sent, '+j.uplink_failures+' failed'],
      ['audio drops','speaker '+j.spk_dropped+', mic '+j.mic_dropped],
      ['fps',j.fps],['volume',j.volume+(j.muted?' (muted)':'')],['brightness',j.brightness],
      ['boost held',j.boost_held?'<span class=good>yes</span>':'<span class=bad>no</span>'],
      ['ota',j.ota?'<span class=good>listening</span>':'<span class=bad>off</span>'],
      ['slot',j.slot+(j.on_trial?' <span class=bad>(on probation)</span>':'')],
      ['backup',j.backed_up?'<span class=good>on the card</span>':'not yet this boot'],
    ];
    $('status').innerHTML=rows.map(r=>'<tr><th>'+r[0]+'</th><td>'+r[1]+'</td></tr>').join('');
    banner(j);
  }catch(e){$('sub').textContent='device unreachable';}
}

const LOG={seq:0,text:''};
async function pollLog(){
  try{
    const j=await(await fetch('/api/log?since='+LOG.seq)).json();
    if(j.from>LOG.seq&&LOG.seq)LOG.text+='\n--- '+(j.from-LOG.seq)+' bytes scrolled out ---\n';
    LOG.text+=j.text; LOG.seq=j.upto;
    if(LOG.text.length>200000)LOG.text=LOG.text.slice(-100000);
    $('logtext').textContent=LOG.text||'(empty)';
    $('prevtail').textContent=j.previous||'(nothing survived the restart)';
    if($('follow').checked)$('logtext').scrollTop=$('logtext').scrollHeight;
  }catch(e){}
}

async function pollBoots(){
  const j=await(await fetch('/api/boot')).json();
  let h='<tr><th>#</th><th>started as</th><th>ran for</th><th>battery</th><th>low heap</th><th>ended</th></tr>';
  for(const b of j.history.slice().reverse()){
    h+='<tr><td>'+b.boot+'</td><td>'+b.reason+'</td><td>'+ms(b.uptime_ms)+'</td><td>'+
      b.battery+'%'+(b.charging?' chg':'')+'</td><td>'+kb(b.min_heap)+'</td><td>'+
      (b.clean?'<span class=good>on purpose</span>':'<span class=bad>without warning</span>')+'</td></tr>';
  }
  $('boots').innerHTML=h;
}

function banner(j){
  const b=document.getElementById('banner');
  let m='';
  if(j.safe_mode)m='<b>Safe mode.</b> '+j.boot_streak+' boots in a row did not stay up, so the '+
    'character pack and the audio were not started. Restore a backup below, or install a '+
    'build, then press <i>Boot normally</i>.';
  else if(j.on_trial)m='<b>This firmware is on probation.</b> It was just installed and has '+
    'not proved itself yet. If the device restarts before it does, the bootloader puts the '+
    'previous image back.';
  b.hidden=!m; b.innerHTML=m;
}

async function pollFirmware(){
  const j=await(await fetch('/api/firmware')).json();
  const st=j.store, rc=j.recovery;
  let h='<tr><th>file</th><th>size</th><th></th><th></th></tr>';
  if(!st.files.length)h+='<tr><td colspan=4 class=k>nothing on the card yet</td></tr>';
  for(const f of st.files){
    h+='<tr><td>'+f.file+'</td><td>'+Math.round(f.size/1024)+' kB</td><td>'+
      (f.known_good?'<span class=good>known good</span>':'')+'</td>'+
      '<td><button onclick="restore(\''+f.file+'\')">Restore</button></td></tr>';
  }
  document.getElementById('backups').innerHTML=h;
  const rows=[
    ['running slot',rc.running],
    ['other slot',rc.other+(rc.other_bootable?' <span class=good>holds an image</span>':
      ' <span class=bad>empty</span>')],
    ['recovery app',rc.factory?'<span class=good>installed</span>':
      '<span class=bad>not installed</span>'],
    ['this image',rc.on_trial?'<span class=bad>on probation</span>':
      (rc.confirmed?'<span class=good>confirmed</span>':'not confirmed yet')],
    ['failed boots',rc.boot_streak],
  ];
  document.getElementById('recovery').innerHTML=
    rows.map(r=>'<tr><th>'+r[0]+'</th><td>'+r[1]+'</td></tr>').join('');
}

async function backupNow(){
  document.getElementById('bkstate').textContent='copying 1.4 MB of flash to the card...';
  const r=await fetch('/api/firmware/backup',{method:'POST'});
  document.getElementById('bkstate').textContent=r.ok?'done':'failed: '+await r.text();
  pollFirmware();
}
async function restore(f){
  if(!confirm('Install '+f+' and restart into it?'))return;
  document.getElementById('bkstate').textContent='installing '+f+'...';
  try{
    const r=await fetch('/api/firmware/restore?file='+encodeURIComponent(f),{method:'POST'});
    document.getElementById('bkstate').textContent=r.ok?'installed - restarting':
      'failed: '+await r.text();
  }catch(e){document.getElementById('bkstate').textContent='installed - restarting';}
}
async function enterRecovery(){
  if(!confirm('Restart into the recovery application?'))return;
  try{
    const r=await fetch('/api/recovery/enter',{method:'POST'});
    document.getElementById('rcstate').textContent=r.ok?'restarting into recovery':
      'failed: '+await r.text();
  }catch(e){document.getElementById('rcstate').textContent='restarting into recovery';}
}
async function bootNormal(){
  try{await fetch('/api/recovery/normal',{method:'POST'})}catch(e){}
  document.getElementById('rcstate').textContent='clearing the counter and restarting';
}

async function pull(){
  const u=$('url').value.trim(); if(!u)return;
  $('pullstate').textContent='fetching - the device draws its own progress bar...';
  try{
    const r=await fetch('/api/ota/pull?url='+encodeURIComponent(u),{method:'POST'});
    $('pullstate').textContent=r.ok?'installed - restarting':'failed: '+await r.text();
  }catch(e){$('pullstate').textContent='installed - restarting (the device dropped the connection)';}
}

function upfw(){
  const f=$('bin').files[0]; if(!f){alert('Pick a .bin');return}
  const x=new XMLHttpRequest(); const bar=$('fwbar'); bar.hidden=false;
  x.upload.onprogress=(e)=>{bar.value=e.loaded*100/e.total};
  x.onload=()=>{$('fwstate').textContent=x.status===200?'installed - restarting':'failed: '+x.responseText};
  x.onerror=()=>{$('fwstate').textContent='connection lost during upload'};
  x.open('POST','/api/ota'); const b=new FormData(); b.append('firmware',f,f.name); x.send(b);
  $('fwstate').textContent='uploading '+f.size+' bytes...';
}

async function reboot(){if(!confirm('Restart the device?'))return;
  try{await fetch('/api/reboot',{method:'POST'})}catch(e){}
  $('sub').textContent='restarting...';}

const log=(m)=>{$('log').textContent+=m+"\n"};
async function refresh(){
  const j=await(await fetch('/api/packs')).json();
  const el=$('packs');
  if(!j.packs.length){el.innerHTML='<b>No packs found.</b>';return}
  el.innerHTML='<b>Installed</b>'+j.packs.map(p=>
    '<div class="row"><span class="name">'+p+'</span>'+
    (p===j.active?'<span class="tag">active</span>':
     '<button onclick="select(\''+p+'\')">Use</button>')+
    '<button class="danger" onclick="del(\''+p+'\')">Delete</button></div>').join('');
}
async function select(n){await fetch('/api/select?pack='+encodeURIComponent(n),{method:'POST'});refresh()}
async function del(n){if(!confirm('Delete pack '+n+'?'))return;
  await fetch('/api/delete?pack='+encodeURIComponent(n),{method:'POST'});refresh()}
async function upload(){
  const name=$('name').value.trim(), files=$('files').files;
  if(!name){alert('Give the pack a name');return}
  if(!files.length){alert('Pick the pack files');return}
  const bar=$('bar');bar.hidden=false;bar.max=files.length;
  for(let i=0;i<files.length;i++){
    const f=files[i];
    const rel=(f.webkitRelativePath||f.name).split('/').slice(1).join('/')||f.name;
    const path='/companion/packs/'+name+'/'+rel;
    const body=new FormData();body.append('file',f,f.name);
    const r=await fetch('/api/upload?path='+encodeURIComponent(path),{method:'POST',body});
    log((r.ok?'ok   ':'FAIL ')+path); bar.value=i+1;
  }
  log('done');refresh();
}

pollStatus(); pollLog();
setInterval(()=>{pollStatus(); if(TAB==='log')pollLog()},2000);
</script>
)HTML";

String packsRoot() { return String(appcfg::kAssetRoot) + "/packs"; }

String basename(const String& path) {
    const int slash = path.lastIndexOf('/');
    return slash < 0 ? path : path.substring(slash + 1);
}

// The log goes out inside a JSON string, so it has to survive its own
// newlines and quotes - and the odd control byte, since the ring is cut at a
// byte boundary and does not care what it lands in the middle of.
String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 16);
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<uint8_t>(c) < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

void DeviceWebServer::begin(DisplayTask* display, ConfigStore* store, DeviceConfig* config,
                            OtaService* ota, const char* password, uint16_t port) {
    if (server_) return;
    display_ = display;
    store_ = store;
    config_ = config;
    ota_ = ota;
    password_ = password ? password : "";
    g_instance = this;

    server_ = new WebServer(port);
    routes();
    server_->begin();
    log_i("device web server on http://%s/", WiFi.localIP().toString().c_str());
}

void DeviceWebServer::routes() {
    server_->on("/", HTTP_GET, [] { g_instance->handleIndex(); });
    server_->on("/api/packs", HTTP_GET, [] { g_instance->handlePacks(); });
    server_->on("/api/upload", HTTP_POST,
                [] { g_instance->handleUpload(); },
                [] { g_instance->handleUploadData(); });
    server_->on("/api/select", HTTP_POST, [] { g_instance->handleSelect(); });
    server_->on("/api/delete", HTTP_POST, [] { g_instance->handleDelete(); });
    server_->on("/api/status", HTTP_GET, [] { g_instance->handleStatus(); });
    server_->on("/api/log", HTTP_GET, [] { g_instance->handleLog(); });
    server_->on("/api/boot", HTTP_GET, [] { g_instance->handleBoot(); });
    server_->on("/api/ota", HTTP_POST,
                [] { g_instance->handleOta(); },
                [] { g_instance->handleOtaData(); });
    server_->on("/api/ota/pull", HTTP_POST, [] { g_instance->handleOtaPull(); });
    server_->on("/api/reboot", HTTP_POST, [] { g_instance->handleReboot(); });
    server_->on("/api/firmware", HTTP_GET, [] { g_instance->handleFirmware(); });
    server_->on("/api/firmware/backup", HTTP_POST,
                [] { g_instance->handleFirmwareBackup(); });
    server_->on("/api/firmware/restore", HTTP_POST,
                [] { g_instance->handleFirmwareRestore(); });
    server_->on("/api/recovery/normal", HTTP_POST,
                [] { g_instance->handleRecoveryNormal(); });
    server_->on("/api/recovery/enter", HTTP_POST,
                [] { g_instance->handleRecoveryEnter(); });
    server_->on("/api/power/test", HTTP_POST, [] { g_instance->handlePowerTest(); });
    server_->onNotFound([] { g_instance->server_->send(404, "text/plain", "not found"); });
}

bool DeviceWebServer::authorised() {
    if (password_.isEmpty()) return true;
    // Basic auth over plain HTTP on a home LAN: worth having against a
    // housemate and a browser bookmark, worth nothing against anyone on the
    // wire. The username is fixed because a second thing to get wrong is not
    // more security, it is more support.
    if (server_->authenticate("m5", password_.c_str())) return true;
    server_->requestAuthentication();
    return false;
}

void DeviceWebServer::scheduleReboot(uint32_t inMs) {
    rebootAtMs_ = millis() + inMs;
    if (!rebootAtMs_) rebootAtMs_ = 1;
}

void DeviceWebServer::loop() {
    if (!server_) return;
    server_->handleClient();
    // Deferred so the browser has its reply before the socket goes away under
    // it. An update that reports "connection lost" reads as a failure even
    // when it worked.
    if (rebootAtMs_ && static_cast<int32_t>(millis() - rebootAtMs_) >= 0) {
        server_->close();
        delay(50);
        ESP.restart();
    }
}

void DeviceWebServer::handleIndex() {
    server_->send_P(200, "text/html; charset=utf-8", kIndexHtml);
}

void DeviceWebServer::handleStatus() {
    String out;
    out.reserve(900);
    if (statusFn_) {
        statusFn_(out, statusCtx_);
    } else {
        out = "{}";
    }
    server_->send(200, "application/json", out);
}

void DeviceWebServer::handleLog() {
    const uint32_t since =
        server_->hasArg("since") ? strtoul(server_->arg("since").c_str(), nullptr, 10) : 0;
    uint32_t from = since, upto = since;
    const String text = appdiag::LogRing::since(since, from, upto);

    String out = "{\"from\":";
    out += from;
    out += ",\"upto\":";
    out += upto;
    out += ",\"text\":\"";
    out += jsonEscape(text);
    out += "\",\"previous\":\"";
    out += jsonEscape(String(appdiag::LogRing::previousBootTail()));
    out += "\"}";
    server_->send(200, "application/json", out);
}

void DeviceWebServer::handleBoot() {
    server_->send(200, "application/json", appdiag::BootLog::asJson());
}

void DeviceWebServer::handleOtaData() {
    // The data handler runs before the request handler, so this is where the
    // check has to be: by the time handleOta() could refuse, the image would
    // already be in flash.
    if (!ota_ || !authorised()) return;
    HTTPUpload& up = server_->upload();

    if (up.status == UPLOAD_FILE_START) {
        otaOk_ = false;
        otaError_ = "";
        busy_ = true;
        // totalSize is what the browser declared, and it is often zero; the
        // OTA layer treats that as "however much the partition holds".
        if (!ota_->pushBegin(up.totalSize)) {
            otaError_ = ota_->lastError();
            busy_ = false;
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (otaError_.isEmpty() && !ota_->pushWrite(up.buf, up.currentSize)) {
            otaError_ = ota_->lastError();
        }
        // A megabyte arrives inside this one request, and the loop task is
        // watched now.
        esp_task_wdt_reset();
    } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        const bool aborted = up.status == UPLOAD_FILE_ABORTED || !otaError_.isEmpty();
        const String err = ota_->pushEnd(aborted);
        if (!aborted && err.isEmpty()) {
            otaOk_ = true;
        } else if (otaError_.isEmpty()) {
            otaError_ = err;
        }
        busy_ = false;
    }
}

void DeviceWebServer::handleOta() {
    if (otaOk_) {
        server_->send(200, "text/plain", "ok");
        scheduleReboot(400);
    } else {
        server_->send(500, "text/plain", otaError_.isEmpty() ? String("failed") : otaError_);
    }
}

void DeviceWebServer::handleOtaPull() {
    if (!authorised()) return;
    if (!ota_) {
        server_->send(503, "text/plain", "ota unavailable");
        return;
    }
    const String url = server_->arg("url");
    if (url.isEmpty()) {
        server_->send(400, "text/plain", "missing url");
        return;
    }
    // pull() does not return when it works: the device restarts inside it, and
    // the browser sees the socket close. Reaching the next line is a failure.
    const String err = ota_->pull(url);
    server_->send(500, "text/plain", err.isEmpty() ? String("failed") : err);
}

void DeviceWebServer::handleReboot() {
    if (!authorised()) return;
    appdiag::BootLog::noteCleanShutdown("web reboot");
    server_->send(200, "text/plain", "ok");
    scheduleReboot(300);
}

void DeviceWebServer::handlePowerTest() {
    if (!authorised()) return;
    if (!powerTestFn_) {
        server_->send(503, "text/plain", "not available");
        return;
    }
    uint32_t seconds = server_->hasArg("seconds")
                           ? strtoul(server_->arg("seconds").c_str(), nullptr, 10)
                           : 6;
    // Answered before it starts, because it holds the loop for half a minute
    // and the whole point is to read the log while it runs.
    server_->send(200, "text/plain", "started - watch the log");
    busy_ = true;
    powerTestFn_(seconds, powerTestCtx_);
    busy_ = false;
}

void DeviceWebServer::handleFirmware() {
    String out = "{\"store\":";
    out += FirmwareStore::listJson();
    out += ",\"recovery\":";
    appdiag::Recovery::appendJson(out);
    out += '}';
    server_->send(200, "application/json", out);
}

void DeviceWebServer::handleFirmwareBackup() {
    if (!authorised()) return;
    busy_ = true;
    const bool ok = FirmwareStore::backupRunning(display_);
    busy_ = false;
    server_->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "backup failed");
}

void DeviceWebServer::handleFirmwareRestore() {
    if (!authorised()) return;
    String file = server_->arg("file");
    if (file.isEmpty()) file = FirmwareStore::knownGood();
    if (file.isEmpty()) {
        server_->send(400, "text/plain", "no backup named, and none marked known-good");
        return;
    }
    busy_ = true;
    String error;
    const bool ok = FirmwareStore::restore(file, display_, error);
    busy_ = false;
    if (!ok) {
        server_->send(500, "text/plain", error);
        return;
    }
    appdiag::BootLog::noteCleanShutdown("restore");
    server_->send(200, "text/plain", "ok");
    scheduleReboot(500);
}

void DeviceWebServer::handleRecoveryNormal() {
    if (!authorised()) return;
    appdiag::Recovery::clearBootLoop();
    appdiag::BootLog::noteCleanShutdown("leave safe mode");
    server_->send(200, "text/plain", "ok");
    scheduleReboot(400);
}

void DeviceWebServer::handleRecoveryEnter() {
    if (!authorised()) return;
    if (!appdiag::Recovery::factoryPresent()) {
        server_->send(409, "text/plain",
                      "no recovery application installed (pio run -e recovery -t upload)");
        return;
    }
    appdiag::BootLog::noteCleanShutdown("enter recovery");
    server_->send(200, "text/plain", "ok");
    // Through the same deferred path as a reboot, so the browser is answered
    // before the device goes away; loop() calls ESP.restart() and the boot
    // partition has already been pointed at factory by then.
    appdiag::Recovery::selectFactory();
    scheduleReboot(500);
}

void DeviceWebServer::handlePacks() {
    if (!display_ || !display_->pause()) {
        server_->send(503, "application/json", "{\"error\":\"display busy\"}");
        return;
    }
    String out = "{\"active\":\"" + config_->packName + "\",\"packs\":[";
    File dir = SD.open(packsRoot().c_str());
    bool first = true;
    if (dir && dir.isDirectory()) {
        for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
            if (entry.isDirectory()) {
                if (!first) out += ',';
                out += '"' + basename(String(entry.name())) + '"';
                first = false;
            }
            entry.close();
        }
        dir.close();
    }
    out += "]}";
    display_->resume();
    server_->send(200, "application/json", out);
}

void DeviceWebServer::handleUploadData() {
    if (!authorised()) return;
    HTTPUpload& up = server_->upload();

    if (up.status == UPLOAD_FILE_START) {
        uploadPath_ = server_->hasArg("path") ? server_->arg("path") : String();
        uploadOk_ = false;
        if (uploadPath_.isEmpty() || !uploadPath_.startsWith(appcfg::kAssetRoot)) {
            // Refuse to write outside the asset tree; a browser upload is not
            // a reason to let anything address the whole card.
            log_w("upload rejected: %s", uploadPath_.c_str());
            return;
        }
        if (!display_ || !display_->pause()) return;
        busy_ = true;

        // Create the parent directories the browser implied.
        for (int i = 1; i < static_cast<int>(uploadPath_.length()); ++i) {
            if (uploadPath_[i] != '/') continue;
            const String parent = uploadPath_.substring(0, i);
            if (!SD.exists(parent.c_str())) SD.mkdir(parent.c_str());
        }
        SD.remove(uploadPath_.c_str());
        upload_ = SD.open(uploadPath_.c_str(), FILE_WRITE);
        uploadOk_ = static_cast<bool>(upload_);

    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (upload_ && upload_.write(up.buf, up.currentSize) != up.currentSize) {
            uploadOk_ = false;
        }

    } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        if (upload_) upload_.close();
        if (up.status == UPLOAD_FILE_ABORTED) {
            SD.remove(uploadPath_.c_str());
            uploadOk_ = false;
        }
        busy_ = false;
        if (display_) display_->resume();
    }
}

void DeviceWebServer::handleUpload() {
    server_->send(uploadOk_ ? 200 : 500, "text/plain", uploadOk_ ? "ok" : "failed");
}

void DeviceWebServer::handleSelect() {
    if (!authorised()) return;
    const String pack = server_->arg("pack");
    if (pack.isEmpty()) {
        server_->send(400, "text/plain", "missing pack");
        return;
    }
    config_->packName = pack;
    if (store_) store_->save(*config_);
    if (display_) {
        display_->setPackName(pack.c_str());
        DisplayTask::CommandMsg msg;
        msg.cmd = DisplayTask::Command::ReloadPack;
        display_->post(msg);
    }
    server_->send(200, "text/plain", "ok");
}

bool DeviceWebServer::removeTree(const String& path) {
    File dir = SD.open(path.c_str());
    if (!dir) return false;
    if (!dir.isDirectory()) {
        dir.close();
        return SD.remove(path.c_str());
    }
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        const String child = path + "/" + basename(String(entry.name()));
        const bool isDir = entry.isDirectory();
        entry.close();
        if (isDir) {
            removeTree(child);
        } else {
            SD.remove(child.c_str());
        }
    }
    dir.close();
    return SD.rmdir(path.c_str());
}

void DeviceWebServer::handleDelete() {
    if (!authorised()) return;
    const String pack = server_->arg("pack");
    if (pack.isEmpty() || pack.indexOf('/') >= 0 || pack == "..") {
        server_->send(400, "text/plain", "bad pack name");
        return;
    }
    if (pack == config_->packName) {
        server_->send(409, "text/plain", "cannot delete the active pack");
        return;
    }
    if (!display_ || !display_->pause()) {
        server_->send(503, "text/plain", "display busy");
        return;
    }
    const bool ok = removeTree(packsRoot() + "/" + pack);
    display_->resume();
    server_->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "failed");
}
