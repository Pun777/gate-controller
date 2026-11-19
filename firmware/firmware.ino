//Code พี่ฮาร์ต
/******
State Machine ของ RELAY
| UP | DOWN | การทำงาน |
| 0  | 0    | ไม่ได้ระบุ  |
| 0  | 1    | ไม้กั้นปิด  |
| 1  | 0    | ไม้กั้นเปิด  |
| 1  | 1    | ไม่ได้ระบุ  |
******/

// new version ล่าสุด
// ✅ [OTA] = ส่วนที่เพิ่มสำหรับอัปเดตโปรแกรมผ่าน LAN (บอร์ดดาวน์โหลดเวอร์ชันใหม่จากเว็บเอง)
#include <WiFi.h>
#include <ETH.h>
#include <WebServer.h>
#include <Preferences.h> // ✅ ใช้บันทึกค่าการตั้งค่า IP
#include <NetworkClientSecure.h> // ✅ [OTA] HTTPS
#include <HTTPClient.h>          // ✅ [OTA]
#include <HTTPUpdate.h>          // ✅ [OTA]
#include "ca_roots.h"            // ✅ [OTA] ใบรับรองสำหรับตรวจว่าดาวน์โหลดจากเว็บจริง
#include "esp_ota_ops.h"         // ✅ [กันพัง] ย้อนกลับเวอร์ชันเดิมอัตโนมัติ

// ✅ [OTA] tools/build.py สร้าง version.h ให้ตอน build (เลขเวอร์ชัน + ที่อยู่เว็บอัปเดต)
// ถ้าคอมไพล์จาก Arduino IDE ตรงๆ จะไม่มีไฟล์นี้ บอร์ดจะทำงานปกติแต่ปุ่มอัปเดตจะปิดไว้
#if __has_include("version.h")
#include "version.h"
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef FW_UPDATE_BASE
#define FW_UPDATE_BASE ""
#endif

// กำหนดขาเชื่อมต่อของ LAN8720
#define ETH_POWER 16
#define ETH_MDC 23
#define ETH_MDIO 18
#define ETH_ADDR 1
#define ETH_TYPE ETH_PHY_LAN8720
#define ETH_CLKMODE ETH_CLOCK_GPIO17_OUT

// กำหนดขาเชื่อมต่อของ RELAY
#define RELAY_UP 15 // GPIO 15
#define RELAY_DOWN 4 // GPIO 4

bool eth_connected = false;
WebServer server(80);
Preferences preferences; // ✅ ใช้สำหรับเก็บค่าการตั้งค่า

String ipMode = "DHCP";
IPAddress staticIP, gateway, subnet, dnsServer; // ✅ [OTA] เพิ่ม DNS ให้โหมด Static หาเว็บอัปเดตเจอ

unsigned long previousMillis = 0; // เอาไว้ใช้ใน void loop()
const long interval = 5000; // เอาไว้ใช้ใน void loop()

// ✅ [แก้ไข] ตัวแปรสำหรับ Non-blocking Gate Timer
bool gateOpen = false;
unsigned long gateOpenTime = 0;
const long gateTimeout = 10000; // 10 วินาที

// ✅ [OTA] สถานะการอัปเดต
bool updatePending = false;
unsigned long updateRequestedAt = 0;
String updateTarget = "";
String lastUpdateError = "";

// ✅ [กันพัง] หลังอัปเดตผ่าน LAN เวอร์ชันใหม่ต้อง "พิสูจน์ตัวเอง" ก่อน:
// ต่อ LAN ได้และทำงานต่อเนื่อง 30 วินาที จึงยืนยันให้ใช้ถาวร
// ถ้าค้าง/รีบูตวน/ต่อ LAN ไม่ได้ใน 3 นาที บอร์ดจะกลับไปใช้เวอร์ชันเดิมเอง
bool otaPendingVerify = false;
const unsigned long OTA_CONFIRM_AFTER_MS = 30000;
const unsigned long OTA_ROLLBACK_AFTER_MS = 180000;
String lastResetReason = "";

// บอกแกน Arduino ว่าเราจะยืนยันเวอร์ชันใหม่เองทีหลัง (ไม่ต้องยืนยันทันทีตอนบูต)
bool verifyRollbackLater() { return true; }

String resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "เปิดไฟ";
    case ESP_RST_SW:       return "สั่งรีสตาร์ท";
    case ESP_RST_PANIC:    return "โปรแกรมค้าง (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "โปรแกรมค้าง (watchdog)";
    case ESP_RST_BROWNOUT: return "ไฟตก (brownout)";
    case ESP_RST_EXT:      return "กดรีเซ็ต";
    default:               return "อื่นๆ";
  }
}

// ✅ ฟังก์ชันควบคุม GPIO
void setOpenGate() {
    digitalWrite(RELAY_UP, HIGH);
    digitalWrite(RELAY_DOWN, LOW);
}

void setCloseGate() {
    digitalWrite(RELAY_UP, LOW);
    digitalWrite(RELAY_DOWN, HIGH);
}

// ✅ [แก้ไข] ฟังก์ชัน API request — ลบ delay() ออก ใช้ millis() แทน
void handleGPIO() {
    Serial.println("📌 คำขอ: " + server.uri());
    if (server.uri() == "/on") {
        setOpenGate();
        gateOpen = true;
        gateOpenTime = millis(); // ✅ Reset timer ทุกครั้งที่รับ /on
        Serial.println("📌 เปิดไม้กั้น | Timer reset");
    } else if (server.uri() == "/off") {
        setCloseGate();
        gateOpen = false;
        Serial.println("📌 ปิดไม้กั้น (Manual)");
    } else {
        server.send(404, "text/plain", "Not Found");
        return;
    }
    server.send(200);
}

// ✅ [OTA] หน้าข้อความสั้นๆ (ใช้ตอนบันทึก IP และตอนอัปเดต)
void sendMessagePage(const String& title, const String& body, const String& script) {
  String page = "<html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>body{font-family:Arial,sans-serif;background:#d35400;margin:0;padding:20px;display:flex;justify-content:center;align-items:center;min-height:100vh;box-sizing:border-box}"
                ".container{background:#edbb99;padding:30px;border-radius:10px;box-shadow:0 5px 15px rgba(0,0,0,.2);max-width:600px;width:100%;text-align:center}"
                "a{color:#005f73;font-weight:bold}</style></head><body><div class='container'>"
                "<h2 id='t'>" + title + "</h2><p id='b'>" + body + "</p>"
                "<p><a href='/'>กลับหน้าหลัก</a></p></div>" + script + "</body></html>";
  server.send(200, "text/html; charset=utf-8", page);
}

void sendMessagePage(const String& title, const String& body) {
  sendMessagePage(title, body, String());
}

// ✅ ฟังก์ชันบันทึกค่าการตั้งค่า IP
void handleSetIP() {
  if (server.hasArg("mode")) {
    ipMode = server.arg("mode") == "Static" ? "Static" : "DHCP";
  }
  // ✅ รับเฉพาะค่าที่เป็น IP ถูกต้อง พิมพ์ผิดจะใช้ค่าเดิม
  IPAddress tmp;
  if (server.hasArg("ip") && tmp.fromString(server.arg("ip"))) staticIP = tmp;
  if (server.hasArg("gateway") && tmp.fromString(server.arg("gateway"))) gateway = tmp;
  if (server.hasArg("subnet") && tmp.fromString(server.arg("subnet"))) subnet = tmp;
  if (server.hasArg("dns") && tmp.fromString(server.arg("dns"))) dnsServer = tmp;

// ✅ บันทึกค่าลง Preferences
  preferences.begin("eth-config", false);
  preferences.putString("ipMode", ipMode);
  preferences.putString("staticIP", staticIP.toString());
  preferences.putString("gateway", gateway.toString());
  preferences.putString("subnet", subnet.toString());
  preferences.putString("dns", dnsServer.toString());
  preferences.end();

  Serial.println("📌 บันทึกค่าใหม่สำเร็จ! รีสตาร์ท...");

  // ✅ ตอบเบราว์เซอร์ก่อนรีสตาร์ท ไม่ให้หน้าเว็บค้าง
  String next = ipMode == "Static"
      ? "เปิดหน้านี้ใหม่ที่ <a href='http://" + staticIP.toString() + "/'>http://" + staticIP.toString() + "/</a>"
      : "บอร์ดจะได้ IP ใหม่จาก DHCP ดูได้จากเราเตอร์หรือ Serial Monitor";
  sendMessagePage("บันทึกแล้ว กำลังรีสตาร์ท", next);

  delay(2000);
  ESP.restart(); // ✅ รีสตาร์ทเพื่อใช้ค่าใหม่
}

// ✅ [OTA] อ่านเลขเวอร์ชันล่าสุดจากเว็บ
String fetchLatestVersion(String& err) {
  NetworkClientSecure client;
  client.setCACert(ROOT_CAS);
  HTTPClient http;
  http.setTimeout(8000);
  String latest = "";
  if (!http.begin(client, String(FW_UPDATE_BASE) + "esp32.version")) {
    err = "เชื่อมต่อเว็บอัปเดตไม่ได้";
    return latest;
  }
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    latest = http.getString();
    latest.trim();
  } else {
    err = "เว็บอัปเดตตอบกลับ " + String(code) + " (" + http.errorToString(code) + ")";
  }
  http.end();
  return latest;
}

// ✅ [OTA] กดปุ่ม "อัปเดตโปรแกรม" บนหน้าเว็บของบอร์ด
void handleUpdate() {
  if (String(FW_UPDATE_BASE).length() == 0) {
    sendMessagePage("ยังเปิดใช้การอัปเดตไม่ได้", "โปรแกรมนี้ไม่ได้ build ผ่านระบบอัปเดต ต้องลงผ่านสาย USB จากหน้าติดตั้งก่อน 1 ครั้ง");
    return;
  }
  if (!eth_connected) {
    sendMessagePage("ยังไม่ได้ต่อ LAN", "เสียบสาย LAN แล้วลองใหม่");
    return;
  }
  String err;
  String latest = fetchLatestVersion(err);
  if (latest.length() == 0) {
    lastUpdateError = err;
    sendMessagePage("ตรวจเวอร์ชันใหม่ไม่สำเร็จ",
                    err + "<br><br>ตรวจว่าวง LAN ออกอินเทอร์เน็ตได้ และถ้าใช้ Static IP ให้ตั้ง DNS (เช่น 8.8.8.8)");
    return;
  }
  if (latest == FW_VERSION) {
    sendMessagePage("เป็นเวอร์ชันล่าสุดแล้ว", "เวอร์ชัน " + latest);
    return;
  }

  updateTarget = latest;
  lastUpdateError = "";
  updatePending = true;          // ✅ เริ่มอัปเดตใน loop() หลังตอบหน้าเว็บแล้ว
  updateRequestedAt = millis();

  // หน้านี้ถามสถานะทุก 3 วินาที จนบอร์ดรีสตาร์ทกลับมาด้วยเวอร์ชันใหม่
  String script =
    "<script>const target='" + latest + "',t0=Date.now();let down=false;"
    "const T=document.getElementById('t'),B=document.getElementById('b');"
    "async function poll(){let s='';try{s=(await (await fetch('/update-status',{cache:'no-store'})).text()).trim();}catch(e){down=true;}"
    "if(s==='version:'+target){T.textContent='อัปเดตเสร็จแล้ว';B.textContent='บอร์ดทำงานด้วยเวอร์ชัน '+target+' และยืนยันใช้ถาวรแล้ว';return;}"
    "if(s==='testing:'+target){T.textContent='ติดตั้งแล้ว กำลังทดสอบ';B.textContent='บอร์ดกำลังเช็กว่าเวอร์ชันใหม่ใช้ LAN ได้ปกติ (ราว 30 วินาที) ถ้าไม่ผ่านจะกลับไปใช้เวอร์ชันเดิมเอง';}"
    "else if(s.startsWith('failed:')){T.textContent='อัปเดตไม่สำเร็จ';B.textContent=s.slice(7)+' — บอร์ดยังใช้โปรแกรมเดิมอยู่ ลองกดอัปเดตใหม่ได้';return;}"
    "else if(down&&(s.startsWith('version:')||s.startsWith('testing:'))){T.textContent='เวอร์ชันใหม่ใช้งานไม่ผ่าน';B.textContent='บอร์ดกลับไปใช้เวอร์ชันเดิม ('+s.split(':')[1]+') อย่างปลอดภัยแล้ว ไม้กั้นใช้งานได้ตามปกติ';return;}"
    "if(Date.now()-t0>360000){T.textContent='ใช้เวลานานผิดปกติ';B.textContent='ลองเปิดหน้าหลักใหม่ ถ้าบอร์ดไม่ตอบ ให้ถอดไฟแล้วเสียบใหม่';return;}"
    "setTimeout(poll,3000);}setTimeout(poll,6000);</script>";
  sendMessagePage("กำลังอัปเดตเป็นเวอร์ชัน " + latest,
                  "ไม้กั้นจะปิดและสั่งเปิดไม่ได้ระหว่างอัปเดต ใช้เวลาประมาณ 1 นาที ห้ามถอดไฟ", script);
}

// ✅ [OTA] หน้าอัปเดตใช้ถามสถานะ
void handleUpdateStatus() {
  String s;
  if (updatePending) s = "updating";
  else if (lastUpdateError.length()) s = "failed:" + lastUpdateError;
  else if (otaPendingVerify) s = "testing:" + String(FW_VERSION);  // ✅ [กันพัง] ยังอยู่ช่วงทดสอบ
  else s = "version:" + String(FW_VERSION);
  server.send(200, "text/plain; charset=utf-8", s);
}

// ✅ [OTA] ดาวน์โหลดและเขียนโปรแกรมใหม่ ถ้าสำเร็จบอร์ดจะรีสตาร์ทเอง ถ้าล้มเหลวจะใช้โปรแกรมเดิมต่อ
void runUpdate() {
  setCloseGate(); // ✅ ปิดไม้กั้นก่อนอัปเดต
  gateOpen = false;
  Serial.println("📌 เริ่มอัปเดตเป็นเวอร์ชัน " + updateTarget);

  NetworkClientSecure client;
  client.setCACert(ROOT_CAS);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = httpUpdate.update(client, String(FW_UPDATE_BASE) + "esp32.app.bin");

  // มาถึงตรงนี้ได้แปลว่าไม่ได้รีสตาร์ท = อัปเดตไม่สำเร็จ
  updatePending = false;
  if (ret == HTTP_UPDATE_FAILED) {
    lastUpdateError = httpUpdate.getLastErrorString();
  } else {
    lastUpdateError = "ไม่พบไฟล์โปรแกรมใหม่บนเว็บ";
  }
  Serial.println("📌 อัปเดตไม่สำเร็จ: " + lastUpdateError);
}

void printMacAddress() {
    uint8_t mac[6]; // ตัวแปรเก็บค่า MAC Address
    ETH.macAddress(mac); // ดึงค่า MAC จาก Ethernet
    Serial.print("📌 Ethernet MAC Address: ");
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ✅ ฟังก์ชัน UI สำหรับหน้าเว็บ
void handleRoot() {
  char macBuffer[18]; // สร้าง Buffer สำหรับเก็บ MAC Address
    uint8_t mac[6];
    ETH.macAddress(mac);
    sprintf(macBuffer, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // ✅ [OTA] ส่วนอัปเดตโปรแกรม
  String updateBlock;
  if (String(FW_UPDATE_BASE).length()) {
    updateBlock = "<form action='/update' method='POST' onsubmit=\"this.querySelector('input').value='กำลังตรวจเวอร์ชัน…'\">"
                  "<input type='submit' value='อัปเดตโปรแกรม' class='save'>"
                  "</form>";
    if (lastUpdateError.length()) {
      updateBlock += "<p style='color:#922b21'><strong>อัปเดตครั้งล่าสุดไม่สำเร็จ:</strong> " + lastUpdateError + "</p>";
    }
  } else {
    updateBlock = "<p><small>อัปเดตผ่าน LAN: ปิดอยู่ (โปรแกรมนี้ไม่ได้ build ผ่านระบบอัปเดต)</small></p>";
  }

  String page = "<html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width, initial-scale=1'>"
              "<style>"
              "body { font-family: Arial, sans-serif; text-align: center; background-color: #d35400; padding: 20px; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }"
              "h2 { color: #333; margin-bottom: 20px; font-size: 38px; }"
              "button { font-size: 18px; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; transition: 0.3s; }"
              "button:hover { opacity: 0.8; }"
              // ✅ [แก้ไข] คอมเมนต์ CSS เดิมปิดไม่ครบ ทำให้ padding/ขอบมนไม่ทำงาน
              ".on,.off { font-size: 20px; padding: 15px 30px; border: none; border-radius: 8px; cursor: pointer; transition: 0.3s; }"
              ".on { background-color: #4CAF50; color: white; }"
              ".on:hover { background-color: #005f73; }"
              ".off { background-color: #f44336; color: white; }"
              ".off:hover { background-color: #005f73; }"
              ".save { background-color: #008CBA; color: white; font-size: 18px; padding: 12px 25px; margin-top: 20px; border: none; border-radius: 5px; cursor: pointer; width: 80%; transition: 0.3s; }"
              ".save:hover { background-color: #005f73; }"
              ".container { background: #edbb99; padding: 30px; border-radius: 10px; box-shadow: 0px 5px 15px rgba(0,0,0,0.2); width: 45%; max-width: 600px; }"
              ".form-group { display: flex; align-items: center; justify-content: space-between; margin: 15px 0; text-align: left; }"
              ".form-group label { font-weight: bold; width: 40%; }"
              ".form-group input { font-size: 16px; padding: 8px; width: 55%; border: 1px solid #ccc; border-radius: 5px; }"
              "select { font-size: 16px; padding: 8px; width: 60%; margin: 10px 0; border-radius: 5px; border: 1px solid #ccc; }"
              "</style>"
              "</head><body>"
              "<div class='container'>"
              "<h2>Bangkok Global Software</h2>"
              "<p><strong>IP Mode:</strong> " + ipMode + "</p>"
              "<p><strong>IP Address:</strong> " + ETH.localIP().toString() + "</p>"
              "<p><strong>MAC Address:</strong> " + String(macBuffer) + "</p>"  // ✅ แสดง MAC Address
              "<p><strong>Firmware:</strong> " + String(FW_VERSION) + String(otaPendingVerify ? " (กำลังทดสอบ)" : "") + "</p>"   // ✅ [OTA] เวอร์ชันโปรแกรม
              "<p><strong>รีสตาร์ทล่าสุดเพราะ:</strong> " + lastResetReason + "</p>"   // ✅ [กันพัง]
              + String(esp_reset_reason() == ESP_RST_BROWNOUT ? "<p style='color:#922b21'><strong>⚠️ ไฟตก:</strong> ใช้แหล่งจ่าย 5V อย่างน้อย 1A</p>" : "") +

              // ✅ ปุ่มควบคุม GATE CONTROLLER
              "<div class='form-group'>"
              "<label>GATE CONTROLLER:</label>"
              "<a href='/on'><button class='on'>ON</button></a> "
              "<a href='/off'><button class='off'>OFF</button></a>"
              "</div>"

              // ✅ ตั้งค่า Static IP
              "<form action='/setip' method='POST'>"
              "<div class='form-group'>"
              "<label>Mode:</label>"
              "<select name='mode'>"
              "<option value='DHCP' " + String(ipMode == "DHCP" ? "selected" : "") + ">DHCP</option>"
              "<option value='Static' " + String(ipMode == "Static" ? "selected" : "") + ">Static</option>"
              "</select>"
              "</div>"

              "<div class='form-group'>"
              "<label>IP Address:</label>"
              "<input type='text' name='ip' value='" + staticIP.toString() + "'>"
              "</div>"

              "<div class='form-group'>"
              "<label>Gateway:</label>"
              "<input type='text' name='gateway' value='" + gateway.toString() + "'>"
              "</div>"

              "<div class='form-group'>"
              "<label>Subnet:</label>"
              "<input type='text' name='subnet' value='" + subnet.toString() + "'>"
              "</div>"

              // ✅ [OTA] DNS สำหรับโหมด Static
              "<div class='form-group'>"
              "<label>DNS:</label>"
              "<input type='text' name='dns' value='" + dnsServer.toString() + "'>"
              "</div>"

              "<input type='submit' value='Save & Reboot' class='save'>"
              "</form>"

              + updateBlock +

              "</div>"
              "</body></html>";
  server.send(200, "text/html; charset=utf-8", page);
}

void setup() {
    Serial.begin(115200);

    // ตั้งค่า MODE ของ GPIO
    pinMode(ETH_POWER, OUTPUT);
    pinMode(RELAY_UP, OUTPUT);
    pinMode(RELAY_DOWN, OUTPUT);

    // ตั้งค่า State เริ่มต้นของทุก GPIO
    digitalWrite(ETH_POWER, HIGH);
    setCloseGate(); // RELAY_UP -> LOW | RELAY_DOWN -> HIGH

    Serial.println("📌 Firmware: " FW_VERSION);

    // ✅ [กันพัง] บอกสาเหตุที่บอร์ดรีสตาร์ทครั้งล่าสุด (เช่น ไฟตก) เพื่อให้รู้ทันว่าไฟเลี้ยงมีปัญหา
    lastResetReason = resetReasonText();
    Serial.println("📌 รีสตาร์ทล่าสุดเพราะ: " + lastResetReason);
    if (esp_reset_reason() == ESP_RST_BROWNOUT) {
        Serial.println("⚠️ ไฟตก! ใช้แหล่งจ่าย 5V ที่จ่ายกระแสได้อย่างน้อย 1A");
    }

    // ✅ [กันพัง] ถ้าเพิ่งอัปเดตผ่าน LAN เวอร์ชันนี้ยังอยู่ในช่วงทดลอง
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t otaState;
    if (esp_ota_get_state_partition(running, &otaState) == ESP_OK && otaState == ESP_OTA_IMG_PENDING_VERIFY) {
        otaPendingVerify = true;
        Serial.println("📌 เวอร์ชันใหม่: กำลังทดสอบ ถ้าไม่ผ่านจะกลับไปใช้เวอร์ชันเดิมเอง");
    }

    // รับเหตุการณ์ของ Ethernet (ใช้ระบบ event ร่วมกับ Wi-Fi)
    WiFi.onEvent([](arduino_event_t *event) {
        if (event->event_id == ARDUINO_EVENT_ETH_GOT_IP) {
            Serial.print("📌 IP ที่ได้รับ: ");
            Serial.println(ETH.localIP());
            eth_connected = true;
            printMacAddress();
        } else if (event->event_id == ARDUINO_EVENT_ETH_DISCONNECTED) {
            eth_connected = false;
        }
    });

    // ✅ โหลดค่าจาก Preferences
    preferences.begin("eth-config", false);
    ipMode = preferences.getString("ipMode", "DHCP");
    staticIP.fromString(preferences.getString("staticIP", "10.99.99.4"));
    gateway.fromString(preferences.getString("gateway", "10.99.99.254"));
    subnet.fromString(preferences.getString("subnet", "255.255.255.0"));
    dnsServer.fromString(preferences.getString("dns", "8.8.8.8")); // ✅ [OTA]
    preferences.end();

    Serial.println("📌 Mode: " + ipMode);
    Serial.println("📌 Static IP: " + staticIP.toString());
    Serial.println("📌 Gateway: " + gateway.toString());
    Serial.println("📌 Subnet: " + subnet.toString());
    Serial.println("📌 DNS: " + dnsServer.toString());

    ETH.begin(ETH_TYPE, ETH_ADDR, ETH_MDC, ETH_MDIO, ETH_POWER, ETH_CLKMODE);

    if (ipMode == "Static") {
        Serial.println("📌 ตั้งค่า Static IP...");
        ETH.config(staticIP, gateway, subnet, dnsServer);
    } else {
        Serial.println("📌 ใช้ DHCP");
    }

    // ✅ ลงทะเบียน API
    server.on("/", handleRoot);
    server.on("/setip", HTTP_POST, handleSetIP);
    server.on("/on", handleGPIO);
    server.on("/off", handleGPIO);
    server.on("/update", HTTP_POST, handleUpdate);          // ✅ [OTA]
    server.on("/update-status", HTTP_GET, handleUpdateStatus); // ✅ [OTA]
    server.on("/version", HTTP_GET, []() { server.send(200, "text/plain", FW_VERSION); }); // ✅ [OTA]

    server.begin();
}

// ✅ [แก้ไข] loop() — ตรวจสอบ gate timer แบบ Non-blocking
void loop() {
    unsigned long currentMillis = millis();

    // ✅ ปิดไม้กั้นอัตโนมัติเมื่อครบเวลา (ไม่บล็อก loop)
    if (gateOpen && (currentMillis - gateOpenTime >= gateTimeout)) {
        setCloseGate();
        gateOpen = false;
        Serial.println("📌 ปิดไม้กั้นอัตโนมัติ (ครบ 10 วินาที)");
    }

    // ✅ [กันพัง] ยืนยันหรือย้อนกลับเวอร์ชันที่เพิ่งอัปเดต
    if (otaPendingVerify) {
        if (eth_connected && currentMillis >= OTA_CONFIRM_AFTER_MS) {
            esp_ota_mark_app_valid_cancel_rollback();
            otaPendingVerify = false;
            Serial.println("✅ เวอร์ชันใหม่ทำงานปกติ ยืนยันใช้ถาวรแล้ว");
        } else if (currentMillis >= OTA_ROLLBACK_AFTER_MS) {
            Serial.println("⚠️ เวอร์ชันใหม่ต่อ LAN ไม่ได้ใน 3 นาที กลับไปใช้เวอร์ชันเดิม");
            setCloseGate();
            delay(200);
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }

    if (eth_connected) {
        // Check if 5 seconds have passed
        if (currentMillis - previousMillis >= interval) {
            previousMillis = currentMillis;  // Update last time message was printed
            Serial.println("นำ IP ไปเปิดใน browser: " + ETH.localIP().toString());
        }
    }
    server.handleClient();  // Keep handling client requests

    // ✅ [OTA] เริ่มอัปเดตหลังส่งหน้า "กำลังอัปเดต" ให้เบราว์เซอร์ไปแล้วครู่หนึ่ง
    if (updatePending && millis() - updateRequestedAt > 1000) {
        runUpdate();
    }
}
