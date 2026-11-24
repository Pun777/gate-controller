// gc_lan_update.cpp - added to every build by the Gate Controller compile server.
// The sketch (firmware.ino) is not changed. This file runs in its own task and adds:
//   * Update over LAN: a small page on port GC_UPDATE_PORT (81). Writing needs the
//     update token that the IDE passes to the page.
//   * Automatic rollback: after an update the new version must get a LAN link + IP
//     and answer on port 80 (the gate page) within 3 minutes. Otherwise the board
//     restarts into the previous version by itself.
//   * Serial command "GC SETIP ..." that the install wizard uses to store the IP
//     in the same Preferences keys the sketch reads.
//   * One "[GC] ver=... mac=..." line at boot so the IDE knows what is installed.
// The compile server skips this file when the sketch defines initVariant() or
// verifyRollbackLater() itself, or contains the word GC_NO_LAN_UPDATE.
#include <Arduino.h>
#include <ETH.h>
#include <WebServer.h>
#include <NetworkClient.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_mac.h>
#include "version.h"

#ifndef GC_UPDATE_PORT
#define GC_UPDATE_PORT 81
#endif
#ifndef GC_OTA_TOKEN
#define GC_OTA_TOKEN ""
#endif

static const uint32_t GC_STABLE_MS = 20000;   // link + IP this long before checking port 80
static const uint32_t GC_GIVE_UP_MS = 180000; // not confirmed by then -> back to the previous version

static WebServer *gcWeb = nullptr;
static bool gcPendingAtBoot = false;
static bool gcConfirmed = false;
static bool gcUpAuth = false;
static String gcUpErr;
static String gcLine;

// Keep the new version "on probation" until gcTask confirms it.
extern "C" bool verifyRollbackLater() { return true; }

static bool gcIsPending() {
  esp_ota_img_states_t st;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
         st == ESP_OTA_IMG_PENDING_VERIFY;
}

static String gcMac() {
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_ETH);
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

static bool gcOnline() { return ETH.linkUp() && ETH.localIP()[0] != 0; }

static const char *gcResetText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT: return "reset-pin";
    default: return "other";
  }
}

static void gcPrintInfo() {
  Serial.printf("[GC] ver=%s mac=%s lan-update=%d%s\n", FW_VERSION, gcMac().c_str(), GC_UPDATE_PORT,
                gcPendingAtBoot && !gcConfirmed ? " testing" : "");
}

/* ---------- the gate page must answer: proves loop() and the sketch's web server run ---------- */
static bool gcGatePageAnswers() {
  NetworkClient c;
  if (!c.connect(ETH.localIP(), 80, 3000)) return false;
  c.print("GET / HTTP/1.0\r\nHost: gc-selftest\r\n\r\n");
  const uint32_t t = millis();
  while (millis() - t < 6000) {
    if (c.available()) {
      String first = c.readStringUntil('\n');
      c.stop();
      return first.startsWith("HTTP/1.");
    }
    delay(20);
  }
  c.stop();
  return false;
}

/* ---------- serial: GC SETIP STATIC <ip> <gateway> <subnet> | GC SETIP DHCP | GC INFO ---------- */
static void gcCommand(String cmd) {
  cmd.trim();
  if (cmd == "INFO") { gcPrintInfo(); return; }
  if (!cmd.startsWith("SETIP")) { Serial.println("[GC] ERR unknown command"); return; }
  String a[4];
  int n = 0;
  int i = 5;
  while (n < 4) {
    while (i < (int)cmd.length() && cmd[i] == ' ') i++;
    if (i >= (int)cmd.length()) break;
    int j = cmd.indexOf(' ', i);
    if (j < 0) j = cmd.length();
    a[n++] = cmd.substring(i, j);
    i = j;
  }
  a[0].toUpperCase();
  const bool isStatic = a[0] == "STATIC";
  if (!isStatic && a[0] != "DHCP") { Serial.println("[GC] SETIP ERR mode"); return; }
  IPAddress ip, gw, mask;
  if (isStatic && (n < 4 || !ip.fromString(a[1]) || !gw.fromString(a[2]) || !mask.fromString(a[3]))) {
    Serial.println("[GC] SETIP ERR address");
    return;
  }
  Preferences p;
  if (!p.begin(GC_PREF_NS, false)) { Serial.println("[GC] SETIP ERR storage"); return; }
  bool ok = p.putString(GC_KEY_MODE, isStatic ? GC_VAL_STATIC : GC_VAL_DHCP) > 0;
  if (isStatic) {
    ok = ok && p.putString(GC_KEY_IP, ip.toString()) > 0;
    ok = ok && p.putString(GC_KEY_GW, gw.toString()) > 0;
    ok = ok && p.putString(GC_KEY_MASK, mask.toString()) > 0;
  }
  p.end();
  if (!ok) { Serial.println("[GC] SETIP ERR storage"); return; }
  if (isStatic) Serial.printf("[GC] SETIP OK %s %s %s\n", ip.toString().c_str(), gw.toString().c_str(), mask.toString().c_str());
  else Serial.println("[GC] SETIP OK DHCP");
  Serial.flush();
  delay(300);
  ESP.restart();
}

static void gcSerialPoll() {
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch < 0 || ch == '\r') continue;
    if (ch != '\n') {
      if (gcLine.length() < 120) gcLine += (char)ch;
      continue;
    }
    String line = gcLine;
    gcLine = "";
    line.trim();
    if (line.startsWith("GC ")) gcCommand(line.substring(3));
  }
}

/* ---------- update page on port 81 ---------- */
static const char GC_PAGE[] = R"GCPAGE(<!doctype html><html lang="th"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>อัปเดตไม้กั้นผ่าน LAN</title>
<style>body{font-family:system-ui,"Segoe UI",sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#f4f5f7;color:#1b1f24}
.c{background:#fff;border-radius:12px;padding:16px 18px;box-shadow:0 1px 4px #0002;margin-bottom:12px}h1{font-size:21px;margin:0 0 10px}
dl{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:0}dt{color:#667}dd{margin:0;font-weight:600;word-break:break-all}
button{font-size:17px;padding:12px 22px;border:0;border-radius:10px;background:#1565c0;color:#fff;cursor:pointer;min-height:48px}
button:disabled{opacity:.45;cursor:not-allowed}.bar{height:14px;background:#e3e6ea;border-radius:7px;overflow:hidden;margin:10px 0}
.bar i{display:block;height:100%;width:0;background:#1565c0;transition:width .2s}#msg{font-size:16px;margin:0}.ok{color:#1b7f3b}.bad{color:#c62828}
input{font-size:16px;padding:9px;width:100%;box-sizing:border-box;margin:6px 0;border:1px solid #bbb;border-radius:8px}summary{cursor:pointer;color:#1565c0}</style></head><body>
<div class="c"><h1>อัปเดตไม้กั้นผ่าน LAN</h1><dl><dt>เวอร์ชันในบอร์ด</dt><dd id="cur">…</dd><dt>เวอร์ชันใหม่</dt><dd id="new">—</dd><dt>MAC</dt><dd id="mac">—</dd></dl></div>
<div class="c"><p id="msg">กำลังโหลด…</p><div class="bar"><i id="bar"></i></div><button id="go" disabled>อัปเดตเลย</button>
<details id="man" style="margin-top:14px"><summary>อัปโหลดไฟล์ .bin เอง</summary><input type="file" id="file" accept=".bin"><input id="tok" placeholder="รหัสอัปเดต (ดูในหน้า IDE)" autocomplete="off"></details></div>
<script>
const $=i=>document.getElementById(i),sleep=m=>new Promise(r=>setTimeout(r,m));
if(location.hash.length>1){try{sessionStorage.setItem("gc",location.hash.slice(1))}catch(e){}history.replaceState(null,"",location.pathname)}
let q;try{q=new URLSearchParams(sessionStorage.getItem("gc")||"")}catch(e){q=new URLSearchParams()}
const src=q.get("s"),tok=q.get("t");let info=null,fw=null;
function msg(t,c){$("msg").textContent=t;$("msg").className=c||""}
function bar(p){$("bar").style.width=Math.round(p)+"%"}
async function getInfo(){const r=await fetch("/info",{cache:"no-store"});return r.json()}
function showInfo(i){$("cur").textContent=i.version+(i.testing?" (กำลังทดสอบ)":"");$("mac").textContent=i.mac}
async function start(){
 try{info=await getInfo();showInfo(info)}catch(e){$("cur").textContent="อ่านไม่ได้"}
 if(tok)$("tok").value=tok;
 if(!src||!tok){msg("เปิดหน้านี้จากปุ่ม “อัปเดตผ่าน LAN” ในหน้า IDE หรือเลือกไฟล์เองด้านล่าง");$("man").open=true;$("go").disabled=false;return}
 try{const r=await fetch(src+"/ota/firmware.json?t="+encodeURIComponent(tok),{cache:"no-store"});if(!r.ok)throw Error(r.status==403?"รหัสอัปเดตไม่ตรงกับเซิร์ฟเวอร์":"HTTP "+r.status);
  fw=await r.json();$("new").textContent=fw.version;
  if(info&&info.version===fw.version){msg("บอร์ดใช้เวอร์ชันนี้อยู่แล้ว กดถ้าต้องการติดตั้งซ้ำ","ok");$("go").textContent="ติดตั้งซ้ำ"}
  else msg("พร้อมอัปเดต ใช้เวลาราว 1 นาที ระหว่างรีสตาร์ทไม้กั้นจะไม่ตอบสักครู่");
  $("go").disabled=false}
 catch(e){msg("โหลดโปรแกรมจากเซิร์ฟเวอร์ IDE ไม่ได้ ("+e.message+") เครื่องนี้ต้องเปิดหน้า IDE ได้ และติดตั้งใบรับรองแล้ว หรือเลือกไฟล์เองด้านล่าง","bad");$("man").open=true;$("go").disabled=false}}
function send(blob,md5,token){return new Promise((ok,bad)=>{const x=new XMLHttpRequest();x.open("POST","/update");
 x.setRequestHeader("X-GC-Token",token);x.setRequestHeader("X-GC-Size",String(blob.size));if(md5)x.setRequestHeader("X-GC-MD5",md5);
 x.upload.onprogress=e=>{if(e.lengthComputable)bar(e.loaded/e.total*100)};
 x.onload=()=>x.status==200?ok():bad(Error(x.status==403?"รหัสอัปเดตไม่ถูกต้อง":(x.responseText||"HTTP "+x.status)));
 x.onerror=()=>bad(Error("การเชื่อมต่อหลุดระหว่างส่ง"));const f=new FormData();f.append("firmware",blob,"app.bin");x.send(f)})}
async function update(){$("go").disabled=true;bar(0);
 try{let blob,md5=null,ver=null;const token=$("tok").value.trim(),file=$("file").files[0];
  if(file&&$("man").open){blob=file}
  else{if(!fw)throw Error("ยังไม่มีไฟล์ เลือกไฟล์ .bin เอง");msg("กำลังดาวน์โหลดจากเซิร์ฟเวอร์…");
   const r=await fetch(src+"/ota/app.bin?t="+encodeURIComponent(tok),{cache:"no-store"});if(!r.ok)throw Error("ดาวน์โหลดไม่ได้ (HTTP "+r.status+")");
   blob=await r.blob();if(fw.size&&blob.size!==fw.size)throw Error("ไฟล์ที่ดาวน์โหลดไม่ครบ");md5=fw.md5;ver=fw.version}
  if(!token)throw Error("ต้องมีรหัสอัปเดต");
  msg("กำลังส่งโปรแกรมเข้าบอร์ด ห้ามถอดไฟหรือสาย LAN");await send(blob,md5,token);bar(100);
  msg("เขียนเสร็จ บอร์ดกำลังรีสตาร์ท…");await waitBoot(ver)}
 catch(e){msg("อัปเดตไม่สำเร็จ: "+e.message+" (โปรแกรมเดิมยังทำงานอยู่)","bad");$("go").disabled=false}}
async function waitBoot(ver){const old=info&&info.version,t0=Date.now();await sleep(5000);
 while(Date.now()-t0<240000){try{const i=await getInfo();showInfo(i);
  if(!i.testing&&(!ver||i.version===ver)){msg("อัปเดตสำเร็จ ✓ บอร์ดตรวจตัวเองผ่านแล้ว ใช้เวอร์ชัน "+i.version,"ok");$("go").disabled=false;return}
  if(!i.testing&&ver&&old&&old!==ver&&i.version===old){msg("เวอร์ชันใหม่ไม่ผ่านการตรวจ บอร์ดกลับไปใช้เวอร์ชันเดิมแล้ว ไม้กั้นใช้งานได้ตามเดิม","bad");$("go").disabled=false;return}
  if(i.testing)msg("บอร์ดเปิดเวอร์ชันใหม่แล้ว กำลังตรวจว่า LAN และหน้าไม้กั้นทำงาน (ไม่เกิน 3 นาที)…")}catch(e){}
  await sleep(2000)}
 msg("บอร์ดยังไม่ตอบ ลองรีเฟรชหน้านี้ ถ้าเวอร์ชันใหม่มีปัญหา บอร์ดจะกลับไปเวอร์ชันเดิมเองภายใน 3 นาที","bad");$("go").disabled=false}
$("go").onclick=update;start();
</script></body></html>)GCPAGE";

static void gcUploadChunk() {
  HTTPUpload &u = gcWeb->upload();
  if (u.status == UPLOAD_FILE_START) {
    gcUpErr = "";
    const String token = gcWeb->header("X-GC-Token");
    gcUpAuth = strlen(GC_OTA_TOKEN) > 0 && token == GC_OTA_TOKEN;
    if (!gcUpAuth) return;
    const size_t size = (size_t)gcWeb->header("X-GC-Size").toInt();
    if (!Update.begin(size > 0 ? size : UPDATE_SIZE_UNKNOWN, U_FLASH)) { gcUpErr = Update.errorString(); return; }
    const String md5 = gcWeb->header("X-GC-MD5");
    if (md5.length() == 32) Update.setMD5(md5.c_str());
    Serial.printf("[GC] update: receiving %u bytes\n", (unsigned)size);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!gcUpAuth || gcUpErr.length()) return;
    if (Update.write(u.buf, u.currentSize) != u.currentSize) gcUpErr = Update.errorString();
  } else if (u.status == UPLOAD_FILE_END) {
    if (!gcUpAuth) return;
    if (gcUpErr.length()) { Update.abort(); return; }
    if (!Update.end(true)) gcUpErr = Update.errorString();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (gcUpAuth) Update.abort();
    gcUpErr = "aborted";
  }
}

static void gcUploadDone() {
  gcWeb->sendHeader("Connection", "close");
  if (!gcUpAuth) { gcWeb->send(403, "text/plain", "wrong token"); return; }
  if (gcUpErr.length() || Update.hasError()) {
    Serial.printf("[GC] update failed: %s\n", gcUpErr.c_str());
    gcWeb->send(500, "text/plain; charset=utf-8", "update failed: " + gcUpErr);
    return;
  }
  gcWeb->send(200, "text/plain", "OK");
  Serial.println("[GC] update written, restarting into the new version");
  Serial.flush();
  delay(800);
  ESP.restart();
}

static void gcStartWeb() {
  gcWeb = new WebServer(GC_UPDATE_PORT);
  const char *keys[] = {"X-GC-Token", "X-GC-Size", "X-GC-MD5"};
  gcWeb->collectHeaders(keys, 3);
  gcWeb->on("/", HTTP_GET, [] { gcWeb->send(200, "text/html; charset=utf-8", GC_PAGE); });
  gcWeb->on("/info", HTTP_GET, [] {
    String j = "{\"version\":\"" FW_VERSION "\",\"testing\":";
    j += (gcPendingAtBoot && !gcConfirmed) ? "true" : "false";
    j += ",\"mac\":\"" + gcMac() + "\",\"ip\":\"" + ETH.localIP().toString() + "\",\"uptime\":" + String(millis() / 1000);
    j += ",\"reset\":\"" + String(gcResetText()) + "\"}";
    gcWeb->sendHeader("Cache-Control", "no-store");
    gcWeb->send(200, "application/json", j);
  });
  gcWeb->on("/update", HTTP_POST, gcUploadDone, gcUploadChunk);
  gcWeb->begin();
}

static void gcTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(1500)); // let setup() start Serial first
  gcPendingAtBoot = gcIsPending();
  gcPrintInfo();
  const uint32_t start = millis();
  uint32_t onlineSince = 0, lastTry = 0;
  for (;;) {
    gcSerialPoll();
    const bool online = gcOnline();
    if (online && !gcWeb) gcStartWeb();
    if (gcWeb) gcWeb->handleClient();

    if (gcPendingAtBoot && !gcConfirmed) {
      if (!online) onlineSince = 0;
      else if (!onlineSince) onlineSince = millis();
      if (onlineSince && millis() - onlineSince > GC_STABLE_MS && millis() - lastTry > 10000) {
        lastTry = millis();
        if (gcGatePageAnswers()) {
          esp_ota_mark_app_valid_cancel_rollback();
          gcConfirmed = true;
          Serial.println("[GC] new version confirmed (LAN + gate page OK)");
        }
      }
      if (!gcConfirmed && millis() - start > GC_GIVE_UP_MS) {
        Serial.println("[GC] new version failed the self-test, going back to the previous version");
        Serial.flush();
        delay(200);
        esp_ota_mark_app_invalid_rollback_and_reboot();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Called by the ESP32 core right before setup().
extern "C" void initVariant() {
  xTaskCreatePinnedToCore(gcTask, "gc-lan", 8192, nullptr, 1, nullptr, ARDUINO_RUNNING_CORE);
}
