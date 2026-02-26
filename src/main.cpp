#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <coap-simple.h>
#include "BatteryProtocol.h"

// --- 網路設定 ---
const char* ssid     = "test_AP";
const char* password = "passwordis123";
const char* server_ip = "192.168.4.1"; 
const int server_port = 5683;


BatteryPacket data;
uint16_t msgId = 0; // 給自製 CoAP 函數使用的訊息編號

// --- 狀態控制變數 ---
bool piAlive = false;        // Pi 是否正在運作 (Heartbeat)
bool isApproved = false;     // 是否已獲得網頁端授權
unsigned long lastHeartbeat = 0;
unsigned long lastActionTime = 0;

WiFiUDP udp;
Coap coap(udp);

// --- 自定義的安全 CoAP 發送函數 (處理二進位數據) ---
void myCoapPut(IPAddress ip, int port, const char* path, uint8_t* payload, int payloadLen) {
    uint8_t packet[64];
    int idx = 0;

    // 1. [Header] Ver(1), Type(NON), TKL(0) -> 0x40
    packet[idx++] = 0x40; 

    // 2. [Code] PUT 方法 -> 0x03
    packet[idx++] = 0x03;

    // 3. [Message ID] 2 Bytes (防重複)
    msgId++;
    packet[idx++] = (msgId >> 8) & 0xFF;
    packet[idx++] = msgId & 0xFF;

    // 4. [Options] 指定路徑 (Uri-Path)
    int pathLen = strlen(path);
    if (pathLen > 0 && pathLen <= 12) { 
        packet[idx++] = 0xB0 | pathLen; 
        memcpy(&packet[idx], path, pathLen);
        idx += pathLen;
    }

    // 5. [Payload Marker] 分隔線 -> 0xFF
    packet[idx++] = 0xFF;

    // 6. [Payload] 真實數據
    memcpy(&packet[idx], payload, payloadLen);
    idx += payloadLen;

    // 7. 發送 UDP
    udp.beginPacket(ip, port);
    udp.write(packet, idx);
    udp.endPacket();
}

// --- 回呼函式：處理 Pi 的文字回應 (Heartbeat 與 授權) ---
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

    // 註冊 CoAP 回呼並啟動
    coap.response(callback_response);
    coap.start();
}

void loop() {
    coap.loop();
    unsigned long now = millis();

    // --- 安全機制 A：檢查 Pi 是否停止運作 ---
    if (piAlive && (now - lastHeartbeat > 40000)) {
        piAlive = false;
        isApproved = false;
        Serial.println("!!! 警報：Pi 已失聯或停止服務，暫停所有動作 !!!");
    }

    // --- 主邏輯控制 (每 5 秒執行一次) ---
    if (now - lastActionTime > 5000) {
        IPAddress target_ip;
        target_ip.fromString(server_ip);

        // 1. 無論如何，先透過標準函式庫確認 Pi 的生存狀態
        coap.get(target_ip, server_port, "heartbeat");

        if (piAlive) {
            if (!isApproved) {
                // 2. 尚未授權，發送連線請求 (使用標準函式庫發送純文字)
                coap.put(target_ip, server_port, "connect", "REQ");
            } else {
                // 3. 已授權，準備 DBC 格式的電池數據
                data.voltage = random(37000, 42000); // 模擬 37.000V ~ 42.000V (mV)
                data.current = random(-2000, 5000);  // 模擬 -2.000A ~ 5.000A (mA)
                data.status = 1;                     // 狀態正常
                data.temp = random(25, 45);          // 模擬 25C ~ 45C
                data.timestamp = millis() / 1000;    // 簡單的開機時間戳

                Serial.println("傳送 DBC 格式電池數據...");
                
                // 4. 使用自製的二進位發送函式傳送數據
                myCoapPut(target_ip, server_port, "battery", (uint8_t*)&data, sizeof(data));
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