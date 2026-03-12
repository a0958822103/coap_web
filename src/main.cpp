#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <coap-simple.h>
#include "BatteryProtocol.h"

// --- 定義內建 LED 腳位 (大多數 ESP32 是 GPIO 2) ---
#ifndef LED_BUILTIN
#define LED_BUILTIN 2 
#endif

// --- 網路設定 ---

const char* ssid     = "test_AP";
const char* password = "passwordis123";
const char* server_ip = "192.168.4.1"; 

// const char* ssid     = "UWLab_2.4GHz";
// const char* password = "nkust123";

// const char* server_ip = "192.168.1.124"; 
const int server_port = 5683;

BatteryPacket data;
uint16_t msgId = 0; // 給自製 CoAP 函數使用的訊息編號

// --- 狀態控制變數 ---
bool piAlive = false;        // Pi 是否正在運作 (Heartbeat)
bool isApproved = false;     // 是否已獲得網頁端授權
bool isHalted = false;     // 是否進入暫停模式 (Halted)，由 Pi 控制
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

// --- 回呼函式：處理網頁端傳來的充電開關指令 (/led) ---
void callback_led(CoapPacket &packet, IPAddress ip, int port) {
    // 提取 Payload (會收到 "1" 或 "0")
    char p[packet.payloadlen + 1];
    memcpy(p, packet.payload, packet.payloadlen);
    p[packet.payloadlen] = '\0';
    
    String cmd = String(p);
    if (cmd == "1") {
        digitalWrite(LED_BUILTIN, HIGH);
        data.status = 1; // 更新要回傳給網頁的數據狀態
        Serial.println(">>> 收到指令：開啟充電 (LED 亮)");
    } else if (cmd == "0") {
        digitalWrite(LED_BUILTIN, LOW);
        data.status = 0; // 更新要回傳給網頁的數據狀態
        Serial.println(">>> 收到指令：關閉充電 (LED 滅)");
    }
    
    // 回傳 ACK 讓 Python 後端知道指令執行成功
    coap.sendResponse(ip, port, packet.messageid, "OK");
}

// --- 回呼函式：處理 Pi 的文字回應 (Heartbeat 與 授權) ---
void callback_response(CoapPacket &packet, IPAddress ip, int port) {
    String res = "";
    for (int i = 0; i < packet.payloadlen; i++) {
        res += (char)packet.payload[i];
    }

    if (res == "ALIVE") {
        lastHeartbeat = millis();
        if (!piAlive) Serial.println(">>> 偵測到 Pi 存活訊號");
        piAlive = true;
    } 
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

void callback_control(CoapPacket &packet, IPAddress ip, int port) {
    char p[packet.payloadlen + 1];
    memcpy(p, packet.payload, packet.payloadlen);
    p[packet.payloadlen] = '\0';
    String cmd = String(p);
    
    if (cmd == "stop") {
        isHalted = true;
        isApproved = false; // 撤銷授權狀態
        digitalWrite(LED_BUILTIN, LOW); // 安全起見，斷開時關閉 LED/充電
        data.status = 0;
        Serial.println("!!! 收到 Pi 指令：進入安全暫停模式 (Halted) !!!");
    } else if (cmd == "start") {
        isHalted = false;
        // 喚醒後，我們讓它直接恢復授權，或者它會在下一個 loop 重新發送 connect 請求
        Serial.println(">>> 收到 Pi 指令：解除暫停，準備恢復連線");
    }
    
    coap.sendResponse(ip, port, packet.messageid, "OK");
}

void setup() {
    Serial.begin(9600);
    delay(1000);

    // --- 新增：初始化 LED 腳位與預設狀態 ---
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW); // 預設為關閉
    data.status = 0;                // 預設傳輸狀態為 0 (異常/停止)

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi 已連線！");

    // 註冊 CoAP 回呼並啟動
    coap.server(callback_led, "led"); // 監聽 /led 資源
    coap.server(callback_control, "control"); // 監聽 /control 指令
    coap.response(callback_response);
    coap.start();
}

void loop() {
    coap.loop();
    unsigned long now = millis();

    // --- 安全機制 A：檢查 Pi 是否停止運作 ---
    if (piAlive && (now - lastHeartbeat > 10000)) {
        piAlive = false;
        isApproved = false;

        isHalted = false;               // 解除暫停/Halted 狀態
        digitalWrite(LED_BUILTIN, LOW); // 確保斷線時充電開關處於安全關閉狀態
        data.status = 0;                // 狀態歸零
        Serial.println("!!! 警報：Pi 已失聯或停止服務，暫停所有動作 !!!");
    }

    // --- 主邏輯控制 (每 5 秒執行一次) ---
    if (now - lastActionTime > 5000) {
        IPAddress target_ip;
        target_ip.fromString(server_ip);

        coap.get(target_ip, server_port, "heartbeat");

        if (piAlive) {
            if (isHalted) {
                Serial.println("設備處於暫停模式，等待 Pi 喚醒...");
            }
            else if (!isApproved) {
                coap.put(target_ip, server_port, "connect", "REQ");
            } else {
                data.voltage = random(37000, 42000); 
                data.current = random(-2000, 5000);  
                                
                data.temp = random(25, 45);          
                data.timestamp = millis() / 1000;    

                Serial.println("傳送 DBC 格式電池數據...");
                myCoapPut(target_ip, server_port, "battery", (uint8_t*)&data, sizeof(data));
            }
        } else {
            Serial.println("正在尋找 Pi 的生存訊號...");
        }

        lastActionTime = now;
    }
}

void gracefulDisconnect() {
    IPAddress target_ip;
    target_ip.fromString(server_ip);
    coap.put(target_ip, server_port, "disconnect", "BYE");
    isApproved = false;
    Serial.println("已發送正常斷開請求");
}