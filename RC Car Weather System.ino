#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ============================================================ //
// ESP32 AI RC CAR + KMA REALTIME WEATHER (ULTRA HD DESIGN)
// ============================================================

// Wi-Fi 설정
const char* ssid = "FirstClass2.4G";
const char* password = "12345678";

// 고정 IP 설정
IPAddress local_IP( 10, 114, 189, 128 );
IPAddress gateway( 10, 114, 184, 1 );
IPAddress subnet( 255, 255, 252, 0 );
IPAddress primaryDNS( 10, 114, 184, 1 );
IPAddress secondaryDNS( 8, 8, 8, 8 );

// 웹서버 및 웹소켓
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// 기상청 API 인증키 및 지역 설정 (수원 기준 NX:60, NY:121)
const char* WEATHER_SERVICE_KEY = "vH8etxAo3TN1%2BsKnyTaZNA6Lc0UWrolJHBBVMkmplEP%2Fj962eZtKUWnesmD9LZjYGePE3A%2FdcY%2BKDB9R2cb2Jw%3D%3D";
const int WEATHER_NX = 60;
const int WEATHER_NY = 121;

const unsigned long WEATHER_INTERVAL = 60000UL; // 60초마다 갱신
unsigned long lastWeatherUpdate = 0;

// 날씨 데이터 변수
float weatherTemperature = 0.0;
float weatherHumidity = 0.0;
float weatherRain = 0.0;
float weatherWind = 0.0;
bool weatherValid = false;
String weatherBaseDate = "";
String weatherBaseTime = "";
String weatherError = "데이터 대기 중...";

// 모터 핀 설정 (로보1472)
const int PWMA = 19;
const int PWMB = 18;
const int AIN1 = 33;
const int AIN2 = 32;
const int BIN1 = 25;
const int BIN2 = 26;
const int STBY = 5;

// NeoPixel 설정
const int LED_PIN = 27;
const int LED_COUNT = 2;
Adafruit_NeoPixel strip( LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800 );

// 속도 설정
int moveSpeed = 150;
int turnSpeed = 100;
const int MIN_SPEED = 0;
const int MAX_SPEED = 255;

// 안전 장치
const unsigned long COMMAND_TIMEOUT = 1200UL;
unsigned long lastCommandTime = 0;

// 상태 변수
char currentCommand = 'x';
bool ledManualMode = false;

// 함수 선언
void motor( int leftSpeed, int rightSpeed, int a1, int a2, int b1, int b2 );
void forwardCar();
void backwardCar();
void leftCar();
void rightCar();
void stopCar();
void handleCommand( char cmd );
void setLED( uint8_t r, uint8_t g, uint8_t b );
void broadcastStatus();
String makeStatusJSON();
void updateWeather();
String makeWeatherJSON();
void handleRoot( AsyncWebServerRequest* request );
void handleReset( AsyncWebServerRequest* request );
void sendCorsResponse( AsyncWebServerRequest* request, int code, const String& contentType, const String& content );

// 모터 제어
void motor( int leftSpeed, int rightSpeed, int a1, int a2, int b1, int b2 ) {
    leftSpeed = constrain( leftSpeed, MIN_SPEED, MAX_SPEED );
    rightSpeed = constrain( rightSpeed, MIN_SPEED, MAX_SPEED );
    digitalWrite( AIN1, a1 );
    digitalWrite( AIN2, a2 );
    digitalWrite( BIN1, b1 );
    digitalWrite( BIN2, b2 );
    analogWrite( PWMA, leftSpeed );
    analogWrite( PWMB, rightSpeed );
}

void forwardCar() {
    motor( moveSpeed, moveSpeed, LOW, HIGH, HIGH, LOW );
    setLED( 0, 255, 60 );
    Serial.println( "[CAR] FORWARD" );
}

void backwardCar() {
    motor( moveSpeed, moveSpeed, HIGH, LOW, LOW, HIGH );
    setLED( 255, 30, 30 );
    Serial.println( "[CAR] BACKWARD" );
}

void leftCar() {
    motor( turnSpeed, turnSpeed, LOW, HIGH, LOW, HIGH );
    setLED( 255, 180, 0 );
    Serial.println( "[CAR] LEFT" );
}

void rightCar() {
    motor( turnSpeed, turnSpeed, HIGH, LOW, HIGH, LOW );
    setLED( 0, 150, 255 );
    Serial.println( "[CAR] RIGHT" );
}

void stopCar() {
    motor( 0, 0, LOW, LOW, LOW, LOW );
    if (!ledManualMode) {
        setLED( 255, 255, 255 );
    }
    currentCommand = 'x';
    Serial.println( "[CAR] STOP" );
}

void setLED( uint8_t r, uint8_t g, uint8_t b ) {
    strip.fill( strip.Color( r, g, b ) );
    strip.show();
}

void handleCommand( char cmd ) {
    cmd = tolower(cmd);
    lastCommandTime = millis();
    currentCommand = cmd;
    switch(cmd) {
        case 'w': forwardCar(); break;
        case 's': backwardCar(); break;
        case 'a': leftCar(); break;
        case 'd': rightCar(); break;
        case 'x': stopCar(); break;
        default:
            Serial.print( "[ERROR] UNKNOWN COMMAND: " );
            Serial.println( cmd );
            stopCar();
            break;
    }
}

// 상태 JSON 생성
String makeStatusJSON() {
    String json = "{";
    json += "\"command\":\"" + String(currentCommand) + "\",";
    json += "\"speed\":" + String(moveSpeed) + ",";
    json += "\"turnSpeed\":" + String(turnSpeed) + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += "}";
    return json;
}

void broadcastStatus() {
    String json = makeStatusJSON();
    ws.textAll(json);
}

// 날씨 JSON 생성
String makeWeatherJSON() {
    JsonDocument doc;
    doc["valid"] = weatherValid;
    doc["temperature"] = weatherTemperature;
    doc["humidity"] = weatherHumidity;
    doc["rain"] = weatherRain;
    doc["wind"] = weatherWind;
    doc["baseDate"] = weatherBaseDate;
    doc["baseTime"] = weatherBaseTime;
    doc["error"] = weatherError;
    
    String output;
    serializeJson(doc, output);
    return output;
}

// 기상청 초단기실황 API 호출 함수
void updateWeather() {
    if (WiFi.status() != WL_CONNECTED) {
        weatherError = "WiFi Disconnected";
        weatherValid = false;
        return;
    }

    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        // NTP 동기화 안되어있을 시 기본 가상 시각 생성
        weatherBaseDate = "20260818";
        weatherBaseTime = "1800";
    } else {
        char dateStr[9];
        char timeStr[5];
        strftime(dateStr, sizeof(dateStr), "%Y%m%d", &timeinfo);
        strftime(timeStr, sizeof(timeStr), "%H30", &timeinfo); // 매시 30분 발표 기준
        weatherBaseDate = String(dateStr);
        weatherBaseTime = String(timeStr);
    }

    String url = "https://apis.data.go.kr/1360000/VilageFcstInfoService_2.0/getUltraSrtNcst?";
    url += "serviceKey=" + String(WEATHER_SERVICE_KEY);
    url += "&pageNo=1&numOfRows=1000&dataType=JSON";
    url += "&base_date=" + weatherBaseDate;
    url += "&base_time=" + weatherBaseTime;
    url += "&nx=" + String(WEATHER_NX);
    url += "&ny=" + String(WEATHER_NY);

    WiFiClientSecure client;
    client.setInsecure(); // 인증서 검증 생략
    HTTPClient http;

    Serial.println("[WEATHER] Requesting KMA API...");
    if (http.begin(client, url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);
            
            if (!error) {
                String resultCode = doc["response"]["header"]["resultCode"];
                if (resultCode == "00") {
                    JsonArray items = doc["response"]["body"]["items"]["item"];
                    for (JsonObject item : items) {
                        String category = item["category"];
                        String obsValue = item["obsrValue"];
                        if (category == "T1H") weatherTemperature = obsValue.toFloat();
                        else if (category == "REH") weatherHumidity = obsValue.toFloat();
                        else if (category == "RN1") weatherRain = obsValue.toFloat();
                        else if (category == "WSD") weatherWind = obsValue.toFloat();
                    }
                    weatherValid = true;
                    weatherError = "";
                    Serial.println("[WEATHER] Updated successfully!");
                } else {
                    weatherValid = false;
                    weatherError = "API Error Code: " + resultCode;
                    Serial.println("[WEATHER] " + weatherError);
                }
            } else {
                weatherValid = false;
                weatherError = "JSON Parse Failed";
                Serial.println("[WEATHER] JSON Parse Error");
            }
        } else {
            weatherValid = false;
            weatherError = "HTTP Error: " + String(httpCode);
            Serial.println("[WEATHER] HTTP Request Failed: " + http.getString());
        }
        http.end();
    } else {
        weatherValid = false;
        weatherError = "HTTPS Connect Failed";
        Serial.println("[WEATHER] HTTPS Connect Failed");
    }
}

// WebSocket 이벤트 처리
void onWsEvent( AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len ) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf( "[WS] CLIENT #%u CONNECTED\n", client->id() );
        client->text( makeStatusJSON() );
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf( "[WS] CLIENT #%u DISCONNECTED\n", client->id() );
        stopCar();
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if ( info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT ) {
            if ( len > 0 && data != nullptr ) {
                char command = (char)data[0];
                handleCommand( command );
                broadcastStatus();
            }
        }
    }
}

// CORS 응답 헬퍼
void sendCorsResponse( AsyncWebServerRequest* request, int code, const String& contentType, const String& content ) {
    AsyncWebServerResponse* response = request->beginResponse( code, contentType, content );
    response->addHeader( "Access-Control-Allow-Origin", "*" );
    request->send( response );
}

// HTML 메인 대시보드 (초고퀄리티 다크 글래스모피즘 디자인)
void handleRoot( AsyncWebServerRequest* request ) {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ko">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 AI RC CAR & WEATHER CENTER</title>
    <style>
        :root {
            --bg-color: #050811;
            --card-bg: rgba(20, 27, 45, 0.7);
            --border-color: rgba(255, 255, 255, 0.1);
            --primary: #00ff88;
            --accent: #00bfff;
            --text-main: #ffffff;
            --text-sub: #8ea0b5;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: 'Segoe UI', Roboto, sans-serif; }
        body {
            background-color: var(--bg-color);
            background-image: radial-gradient(circle at 50% 10%, #111e38 0%, var(--bg-color) 70%);
            color: var(--text-main);
            min-height: 100vh;
            padding: 20px;
            display: flex;
            justify-content: center;
            align-items: center;
        }
        .main-container {
            width: 100%;
            max-width: 950px;
            display: flex;
            flex-direction: column;
            gap: 20px;
        }
        header {
            text-align: center;
            padding: 20px;
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: 20px;
            backdrop-filter: blur(10px);
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.37);
        }
        h1 { font-size: 28px; letter-spacing: 2px; color: var(--primary); margin-bottom: 5px; }
        .subtitle { font-size: 13px; color: var(--text-sub); }
        
        .grid-layout {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
        }
        @media (max-width: 768px) {
            .grid-layout { grid-template-columns: 1fr; }
        }
        .card {
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: 20px;
            padding: 25px;
            backdrop-filter: blur(10px);
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.37);
        }
        .card h2 { font-size: 18px; margin-bottom: 15px; color: var(--accent); border-bottom: 1px solid var(--border-color); padding-bottom: 8px; display: flex; justify-content: space-between; align-items: center; }
        
        /* 제어 패널 스타일 */
        .controls-grid {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 10px;
            margin-bottom: 20px;
            text-align: center;
        }
        .btn {
            background: rgba(255, 255, 255, 0.05);
            border: 1px solid var(--border-color);
            color: white;
            padding: 15px;
            font-size: 16px;
            font-weight: bold;
            border-radius: 12px;
            cursor: pointer;
            transition: all 0.2s ease;
        }
        .btn:hover { background: var(--primary); color: #000; box-shadow: 0 0 15px var(--primary); }
        .btn:active { transform: scale(0.95); }
        .btn-forward { grid-column: 2; }
        .btn-stop { background: rgba(255, 50, 50, 0.2); border-color: rgba(255, 50, 50, 0.4); color: #ff6b6b; }
        .btn-stop:hover { background: #ff3333; color: white; box-shadow: 0 0 15px #ff3333; }

        /* 날씨 위젯 스타일 */
        .weather-grid {
            display: grid;
            grid-template-columns: repeat(2, 1fr);
            gap: 12px;
        }
        .weather-item {
            background: rgba(0, 0, 0, 0.2);
            padding: 15px;
            border-radius: 12px;
            border: 1px solid var(--border-color);
            text-align: center;
        }
        .weather-item .label { font-size: 12px; color: var(--text-sub); margin-bottom: 5px; }
        .weather-item .value { font-size: 20px; font-weight: bold; color: var(--text-main); }

        /* 상태 및 설정 */
        .status-box {
            display: flex;
            justify-content: space-between;
            margin-bottom: 10px;
            font-size: 14px;
        }
        .status-val { font-weight: bold; color: var(--primary); }
        
        .slider-group {
            margin-top: 15px;
        }
        .slider-group label { display: block; font-size: 13px; color: var(--text-sub); margin-bottom: 5px; }
        input[type=range] {
            width: 100%;
            accent-color: var(--primary);
        }
        .btn-reset {
            width: 100%;
            margin-top: 15px;
            background: rgba(255, 165, 0, 0.2);
            border-color: rgba(255, 165, 0, 0.4);
            color: #ffb703;
        }
        .btn-reset:hover { background: #ffb703; color: #000; box-shadow: 0 0 15px #ffb703; }
    </style>
</head>
<body>
    <div class="main-container">
        <header>
            <h1>ESP32 AI RC CAR</h1>
            <div class="subtitle">Real-time IoT Control & KMA Weather Telemetry Dashboard</div>
        </header>

        <div class="grid-layout">
            <!-- RC CAR 제어 카드 -->
            <div class="card">
                <h2><span>🎮 RC Car Control</span> <span id="conn-status" style="font-size:12px; color:#ff3333;">● DISCONNECTED</span></h2>
                <div class="controls-grid">
                    <div></div>
                    <button class="btn btn-forward" onclick="send('w')">▲<br>FWD</button>
                    <div></div>
                    <button class="btn" onclick="send('a')">◀<br>LEFT</button>
                    <button class="btn btn-stop" onclick="send('x')">■<br>STOP</button>
                    <button class="btn" onclick="send('d')">▶<br>RIGHT</button>
                    <div></div>
                    <button class="btn" onclick="send('s')">▼<br>BWD</button>
                    <div></div>
                </div>
                
                <div class="slider-group">
                    <label>모터 속도 조절 (Speed: <span id="speed-val">150</span>)</label>
                    <input type="range" id="speedRange" min="0" max="255" value="150" oninput="changeSpeed(this.value)">
                </div>
            </div>

            <!-- 기상청 날씨 정보 카드 -->
            <div class="card">
                <h2><span>🌤️ KMA Live Weather</span> <span style="font-size:11px; color:var(--text-sub);" id="weather-time">수원 (NX:60, NY:121)</span></h2>
                <div class="weather-grid" style="margin-bottom: 15px;">
                    <div class="weather-item">
                        <div class="label">기온 (T1H)</div>
                        <div class="value" id="w-temp">--.- °C</div>
                    </div>
                    <div class="weather-item">
                        <div class="label">습도 (REH)</div>
                        <div class="value" id="w-hum">-- %</div>
                    </div>
                    <div class="weather-item">
                        <div class="label">1시간 강수량 (RN1)</div>
                        <div class="value" id="w-rain">-- mm</div>
                    </div>
                    <div class="weather-item">
                        <div class="label">풍속 (WSD)</div>
                        <div class="value" id="w-wind">-- m/s</div>
                    </div>
                </div>
                <div style="font-size: 11px; color: var(--text-sub); text-align: center;" id="w-status-msg">실시간 초단기실황 연동 중...</div>
                
                <button class="btn btn-reset" onclick="resetSystem()">⚡ 시스템 초기화 / 상태 리셋</button>
            </div>
        </div>
    </div>

    <script>
        let ws;
        function initWebSocket() {
            ws = new WebSocket('ws://' + window.location.hostname + '/ws');
            ws.onopen = function() {
                document.getElementById('conn-status').innerText = '● ONLINE';
                document.getElementById('conn-status').style.color = '#00ff88';
            };
            ws.onclose = function() {
                document.getElementById('conn-status').innerText = '● DISCONNECTED';
                document.getElementById('conn-status').style.color = '#ff3333';
                setTimeout(initWebSocket, 2000);
            };
            ws.onmessage = function(event) {
                let data = JSON.parse(event.data);
                document.getElementById('speed-val').innerText = data.speed;
                document.getElementById('speedRange').value = data.speed;
            };
        }

        function send(cmd) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(cmd);
            } else {
                fetch('/' + (cmd === 'w' ? 'forward' : cmd === 's' ? 'backward' : cmd === 'a' ? 'left' : cmd === 'd' ? 'right' : 'stop'));
            }
        }

        function changeSpeed(val) {
            document.getElementById('speed-val').innerText = val;
            fetch('/speed?value=' + val);
        }

        async function loadWeather() {
            try {
                let res = await fetch('/weather');
                let data = await res.json();
                if (data.valid) {
                    document.getElementById('w-temp').innerText = data.temperature.toFixed(1) + " °C";
                    document.getElementById('w-hum').innerText = data.humidity.toFixed(0) + " %";
                    document.getElementById('w-rain').innerText = data.rain.toFixed(1) + " mm";
                    document.getElementById('w-wind').innerText = data.wind.toFixed(1) + " m/s";
                    document.getElementById('w-time').innerText = "발표: " + data.baseDate + " " + data.baseTime;
                    document.getElementById('w-status-msg').innerText = "기상청 초단기실황 연동 완료";
                } else {
                    document.getElementById('w-status-msg').innerText = "데이터 수신 대기 중 (" + data.error + ")";
                }
            } catch (e) {
                console.error(e);
            }
        }

        async function resetSystem() {
            if (!confirm("RC카 시스템 및 상태를 초기화하시겠습니까?")) return;
            try {
                let res = await fetch('/reset');
                let data = await res.json();
                if (data.success) {
                    alert("시스템이 성공적으로 초기화되었습니다.");
                    location.reload();
                }
            } catch (e) {
                console.error(e);
            }
        }

        window.onload = function() {
            initWebSocket();
            loadWeather();
            setInterval(loadWeather, 60000); // 1분마다 날씨 자동 갱신
        };
    </script>
</body>
</html>
)rawliteral";

    sendCorsResponse(request, 200, "text/html", html);
}

// HTTP API 라우트 핸들러
void handleForward( AsyncWebServerRequest* request ) { handleCommand('w'); sendCorsResponse(request, 200, "text/plain", "FORWARD"); }
void handleBackward( AsyncWebServerRequest* request ) { handleCommand('s'); sendCorsResponse(request, 200, "text/plain", "BACKWARD"); }
void handleLeft( AsyncWebServerRequest* request ) { handleCommand('a'); sendCorsResponse(request, 200, "text/plain", "LEFT"); }
void handleRight( AsyncWebServerRequest* request ) { handleCommand('d'); sendCorsResponse(request, 200, "text/plain", "RIGHT"); }
void handleStop( AsyncWebServerRequest* request ) { handleCommand('x'); sendCorsResponse(request, 200, "text/plain", "STOP"); }

void handleLED( AsyncWebServerRequest* request ) {
    if ( !request->hasParam("state") ) {
        sendCorsResponse(request, 400, "text/plain", "Missing state");
        return;
    }
    String state = request->getParam("state")->value();
    if (state == "on") {
        ledManualMode = true;
        setLED(255, 0, 0);
        sendCorsResponse(request, 200, "text/plain", "LED ON");
    } else if (state == "off") {
        ledManualMode = false;
        setLED(0, 0, 0);
        sendCorsResponse(request, 200, "text/plain", "LED OFF");
    } else {
        sendCorsResponse(request, 400, "text/plain", "Invalid state");
    }
}

void handleSpeed( AsyncWebServerRequest* request ) {
    if ( !request->hasParam("value") ) {
        sendCorsResponse(request, 400, "text/plain", "Missing value");
        return;
    }
    moveSpeed = constrain(request->getParam("value")->value().toInt(), MIN_SPEED, MAX_SPEED);
    sendCorsResponse(request, 200, "text/plain", "SPEED=" + String(moveSpeed));
}

void handleStatusRoute( AsyncWebServerRequest* request ) {
    sendCorsResponse(request, 200, "application/json", makeStatusJSON());
}

void handleWeatherRoute( AsyncWebServerRequest* request ) {
    sendCorsResponse(request, 200, "application/json", makeWeatherJSON());
}

void handleReset( AsyncWebServerRequest* request ) {
    stopCar();
    moveSpeed = 150;
    turnSpeed = 100;
    currentCommand = 'x';
    ledManualMode = false;
    lastCommandTime = millis();
    setLED(255, 255, 255);
    broadcastStatus();
    
    // 기상청 날씨 즉시 재조회
    updateWeather();
    
    sendCorsResponse(request, 200, "application/json", "{\"success\":true,\"message\":\"SYSTEM RESET OK\"}");
    Serial.println("[SYSTEM] Reset requested via Web API.");
}

// SETUP
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println( "========================================" );
    Serial.println( " ESP32 AI RC CAR & WEATHER SYSTEM       " );
    Serial.println( "========================================" );

    // 모터 핀 설정
    pinMode(PWMA, OUTPUT);
    pinMode(PWMB, OUTPUT);
    pinMode(AIN1, OUTPUT);
    pinMode(AIN2, OUTPUT);
    pinMode(BIN1, OUTPUT);
    pinMode(BIN2, OUTPUT);
    pinMode(STBY, OUTPUT);
    digitalWrite(STBY, HIGH);

    // NeoPixel 초기화
    strip.begin();
    strip.setBrightness(80);
    strip.show();
    stopCar();

    // Wi-Fi 설정
    WiFi.mode(WIFI_STA);
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
        Serial.println( "[WiFi] Static IP FAILED" );
    } else {
        Serial.println( "[WiFi] Static IP OK" );
    }

    WiFi.begin(ssid, password);
    Serial.print( "[WiFi] Connecting" );
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        retry++;
        if (retry >= 40) {
            Serial.println();
            Serial.println( "[WiFi] TIMEOUT -> Restarting..." );
            ESP.restart();
        }
    }
    Serial.println();
    Serial.println( "[WiFi] CONNECTED!" );
    Serial.print( "[WiFi] IP Address: " );
    Serial.println( WiFi.localIP() );

    // 시간 동기화 (NTP)
    configTime(32400, 0, "pool.ntp.org", "time.nist.gov");

    // 최초 날씨 데이터 가져오기
    updateWeather();

    // WebSocket 설정
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // HTTP 라우트 등록
    server.on("/", HTTP_GET, handleRoot);
    server.on("/forward", HTTP_GET, handleForward);
    server.on("/backward", HTTP_GET, handleBackward);
    server.on("/left", HTTP_GET, handleLeft);
    server.on("/right", HTTP_GET, handleRight);
    server.on("/stop", HTTP_GET, handleStop);
    server.on("/led", HTTP_GET, handleLED);
    server.on("/speed", HTTP_GET, handleSpeed);
    server.on("/status", HTTP_GET, handleStatusRoute);
    server.on("/weather", HTTP_GET, handleWeatherRoute);
    server.on("/reset", HTTP_GET, handleReset);

    server.begin();
    Serial.println( "[HTTP] SERVER STARTED SUCCESSFULLY!" );
    Serial.println( "----------------------------------------" );
    Serial.println( "Access URL: http://10.114.189.128/" );
    Serial.println( "========================================" );

    // 부팅 완료 LED 효과
    setLED(0, 80, 255);
    delay(300);
    setLED(0, 255, 100);
    delay(300);
    stopCar();
}

// LOOP
void loop() {
    ws.cleanupClients();

    // 안전 정지 (일정 시간 명령이 없으면 정지)
    if (currentCommand != 'x' && millis() - lastCommandTime > COMMAND_TIMEOUT) {
        Serial.println( "[SAFETY] COMMAND TIMEOUT -> STOP" );
        stopCar();
    }

    // Wi-Fi 연결 감시
    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck > 5000) {
        lastWiFiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println( "[WiFi] CONNECTION LOST -> Reconnecting..." );
            WiFi.reconnect();
        }
    }

    // 주기적 날씨 데이터 자동 갱신 (60초)
    static unsigned long lastWeatherTick = 0;
    if (millis() - lastWeatherTick > WEATHER_INTERVAL) {
        lastWeatherTick = millis();
        updateWeather();
    }
}
