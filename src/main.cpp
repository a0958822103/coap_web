#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <coap-simple.h>

// --- 網路設定 ---
const char* ssid     = "test_AP";
const char* password = "passwordis123";
const char* server_ip = "192.168.4.1"; 
const int server_port = 5683;

// --- 狀態控制變數 ---
bool piAlive = false;        // Pi 是否正在運作 (Heartbeat)
bool isApproved = false;     // 是否已獲得網頁端授權
unsigned long lastHeartbeat = 0;
unsigned long lastActionTime = 0;

WiFiUDP udp;
Coap coap(udp);

// --- 回呼函式：處理 Pi 的所有回應 ---
void callback_response(CoapPacket &packet, IPAddress ip, int port) {
    // 將 Payload 轉為字串
    String res = "";
    for (int i = 0; i < packet.payloadlen; i++) {
        res += (char)packet.payload[i];
    }

    // 1. 處理生存訊號 (Heartbeat)
    if (res == "ALIVE") {
        lastHeartbeat = millis();
        if (!piAlive) Serial.println(">>> 偵測到 Pi 存活訊號");
        piAlive = true;
    } 
    // 2. 處理授權狀態
    else if (res == "online") {
        if (!isApproved) Serial.println(">>> 網頁管理員已同意連線！開始傳輸。");
        isApproved = true;
    } 
    else if (res == "pending") {
        Serial.println(">>> 等待網頁管理員授權中...");
        isApproved = false;
    }
    else if (res == "denied") {
        Serial.println(">>> 連線請求被拒絕。");
        isApproved = false;
    }
}

void setup() {
    Serial.begin(9600);
    delay(1000);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi 已連線！");

    coap.response(callback_response);
    coap.start();
}

void loop() {
    coap.loop();
    unsigned long now = millis();

    // --- 安全機制 A：檢查 Pi 是否停止運作 ---
    // 如果超過 40 秒沒收到 Heartbeat (Pi 每 30 秒發一次)，視為失聯
    if (piAlive && (now - lastHeartbeat > 40000)) {
        piAlive = false;
        isApproved = false;
        Serial.println("!!! 警報：Pi 已失聯或停止服務，暫停所有動作 !!!");
    }

    // --- 主邏輯控制 (每 5 秒執行一次) ---
    if (now - lastActionTime > 5000) {
        IPAddress target_ip;
        target_ip.fromString(server_ip);

        // 1. 無論如何，先確認 Pi 的生存狀態
        coap.get(target_ip, server_port, "heartbeat");

        if (piAlive) {
            if (!isApproved) {
                // 2. 如果 Pi 活著但還沒授權，發送連線請求 (等待網頁彈窗)
                coap.put(target_ip, server_port, "connect", "REQ");
            } else {
                // 3. 已授權，正常傳送電池數據
                float voltage = random(370, 420) / 100.0;
                String payload = String(voltage) + "V";
                Serial.print("傳送數據: "); Serial.println(payload);
                coap.put(target_ip, server_port, "battery", payload.c_str());
            }
        } else {
            Serial.println("正在尋找 Pi 的生存訊號...");
        }

        lastActionTime = now;
    }
}

// 正常斷開按鈕或邏輯可以呼叫此函式
void gracefulDisconnect() {
    IPAddress target_ip;
    target_ip.fromString(server_ip);
    coap.put(target_ip, server_port, "disconnect", "BYE");
    isApproved = false;
    Serial.println("已發送正常斷開請求");
}