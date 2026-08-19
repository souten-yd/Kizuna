#include "PackWebServer.hpp"

#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

#include "AppConfig.hpp"
#include "display/DisplayTask.hpp"
#include "storage/ConfigStore.hpp"

namespace {

PackWebServer* g_instance = nullptr;

const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5Companion packs</title><style>
body{font-family:system-ui,sans-serif;background:#12161a;color:#e8eaed;margin:0;padding:20px;max-width:640px}
h1{font-size:19px;color:#ff8a3d;margin:0 0 2px}p.sub{color:#8b959d;margin:0 0 20px;font-size:13px}
.card{background:#1b2127;border:1px solid #2c343b;border-radius:10px;padding:14px;margin:0 0 14px}
.row{display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid #252c33}
.row:last-child{border:0}.name{flex:1;font-size:15px}.tag{font-size:11px;color:#12161a;background:#7ec96a;padding:2px 7px;border-radius:20px}
button{padding:7px 13px;border:0;border-radius:7px;background:#2c343b;color:#e8eaed;font-size:13px;cursor:pointer}
button.primary{background:#ff8a3d;color:#12161a;font-weight:600}button.danger{background:#5a2630;color:#ffb3b3}
input[type=file]{width:100%;margin:8px 0;color:#8b959d;font-size:13px}
#log{white-space:pre-wrap;font-family:ui-monospace,monospace;font-size:12px;color:#8b959d;margin-top:10px}
progress{width:100%;height:6px}
</style>
<h1>M5Companion</h1><p class="sub">character packs on the SD card</p>
<div class="card" id="packs">loading...</div>
<div class="card">
  <b>Upload a pack</b>
  <p class="sub" style="margin:6px 0">Pick the files from one pack directory, including manifest.json.
  They are written to /companion/packs/&lt;name&gt;/.</p>
  <input id="name" placeholder="pack name, e.g. claudecode" style="width:100%;padding:8px;border-radius:7px;border:1px solid #2c343b;background:#12161a;color:#e8eaed">
  <input type="file" id="files" multiple webkitdirectory>
  <button class="primary" onclick="upload()">Upload</button>
  <progress id="bar" value="0" max="100" hidden></progress>
  <div id="log"></div>
</div>
<script>
const log=(m)=>{document.getElementById('log').textContent+=m+"\n"};
async function refresh(){
  const r=await fetch('/api/packs'); const j=await r.json();
  const el=document.getElementById('packs');
  if(!j.packs.length){el.innerHTML='<b>No packs found.</b>';return}
  el.innerHTML='<b>Installed</b>'+j.packs.map(p=>
    `<div class="row"><span class="name">${p}</span>`+
    (p===j.active?'<span class="tag">active</span>':
     `<button onclick="select('${p}')">Use</button>`)+
    `<button class="danger" onclick="del('${p}')">Delete</button></div>`).join('');
}
async function select(n){await fetch('/api/select?pack='+encodeURIComponent(n),{method:'POST'});refresh()}
async function del(n){if(!confirm('Delete pack '+n+'?'))return;
  await fetch('/api/delete?pack='+encodeURIComponent(n),{method:'POST'});refresh()}
async function upload(){
  const name=document.getElementById('name').value.trim();
  const files=document.getElementById('files').files;
  if(!name){alert('Give the pack a name');return}
  if(!files.length){alert('Pick the pack files');return}
  const bar=document.getElementById('bar');bar.hidden=false;bar.max=files.length;
  for(let i=0;i<files.length;i++){
    const f=files[i];
    const rel=(f.webkitRelativePath||f.name).split('/').slice(1).join('/')||f.name;
    const path='/companion/packs/'+name+'/'+rel;
    const body=new FormData();body.append('file',f,f.name);
    const r=await fetch('/api/upload?path='+encodeURIComponent(path),{method:'POST',body});
    log((r.ok?'ok   ':'FAIL ')+path);
    bar.value=i+1;
  }
  log('done');refresh();
}
refresh();
</script>)HTML";

String packsRoot() { return String(appcfg::kAssetRoot) + "/packs"; }

String basename(const String& path) {
    const int slash = path.lastIndexOf('/');
    return slash < 0 ? path : path.substring(slash + 1);
}

}  // namespace

void PackWebServer::begin(DisplayTask* display, ConfigStore* store, DeviceConfig* config,
                          uint16_t port) {
    if (server_) return;
    display_ = display;
    store_ = store;
    config_ = config;
    g_instance = this;

    server_ = new WebServer(port);
    routes();
    server_->begin();
    log_i("pack web server on http://%s/", WiFi.localIP().toString().c_str());
}

void PackWebServer::routes() {
    server_->on("/", HTTP_GET, [] { g_instance->handleIndex(); });
    server_->on("/api/packs", HTTP_GET, [] { g_instance->handlePacks(); });
    server_->on("/api/upload", HTTP_POST,
                [] { g_instance->handleUpload(); },
                [] { g_instance->handleUploadData(); });
    server_->on("/api/select", HTTP_POST, [] { g_instance->handleSelect(); });
    server_->on("/api/delete", HTTP_POST, [] { g_instance->handleDelete(); });
    server_->onNotFound([] { g_instance->server_->send(404, "text/plain", "not found"); });
}

void PackWebServer::loop() {
    if (server_) server_->handleClient();
}

void PackWebServer::handleIndex() {
    server_->send_P(200, "text/html; charset=utf-8", kIndexHtml);
}

void PackWebServer::handlePacks() {
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

void PackWebServer::handleUploadData() {
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

void PackWebServer::handleUpload() {
    server_->send(uploadOk_ ? 200 : 500, "text/plain", uploadOk_ ? "ok" : "failed");
}

void PackWebServer::handleSelect() {
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

bool PackWebServer::removeTree(const String& path) {
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

void PackWebServer::handleDelete() {
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
