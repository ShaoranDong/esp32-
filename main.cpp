#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <driver/i2s.h>
#include <WiFiClientSecure.h>
#include <SoftwareSerial.h>

// ===================== TW‑TTS 引脚配置 =====================
#define TTS_SOFT_RX 26
#define TTS_SOFT_TX 27
HardwareSerial softTTS(2);

// ===================== WiFi配置 =====================
const char* WIFI_SSID = ""; //你的WIFI名称
const char* WIFI_PASSWORD = ""; //你的WIFI密码

// 百度语音识别
const char* BAIDU_API_KEY = "";
const char* BAIDU_SECRET_KEY = "";
const char* BAIDU_TOKEN_URL = "";
const char* BAIDU_ASR_HOST = "";
const int BAIDU_ASR_PORT = 443;
const char* BAIDU_APP_ID = "";

// DeepSeek官方API
const char* DEEPSEEK_API_KEY = "";
const char* DEEPSEEK_URL = "";
const char* DEEPSEEK_MODEL = "";

// 引脚定义
#define LED_BUILTIN     2
#define AC_CONTROL_PIN  4
#define ASRPRO_RX       16
#define ASRPRO_TX       17
#define I2S_WS          15
#define I2S_SCK         14
#define I2S_SD          32

// 音频参数
#define SAMPLE_RATE        16000
#define BITS_PER_SAMPLE    16
#define CHANNELS           1
#define AUDIO_CHUNK_MS     40
#define AUDIO_CHUNK_SAMPLES 640
#define AUDIO_CHUNK_SIZE    (AUDIO_CHUNK_SAMPLES * sizeof(int16_t))
#define I2S_PORT            I2S_NUM_0
#define MIC_GAIN            3.0f
#define TTS_WAIT_TIMEOUT_MS 4000UL   // TTS最大等待超时4秒兜底

// ===================== 全局对象 =====================
HardwareSerial SerialAsrPro(1);
static WiFiClientSecure* asrClient = nullptr;
static String pendingAiQuery = "";
static bool isAiProcessing = false;
static String lastRecognizedText = "";

// ========= TW‑TTS 通过串口TX获取忙闲状态（替代SYN6288 BUSY引脚） =========
static bool g_ttsIsBusy = false;
static unsigned long g_lastQueryTtsMs = 0;
static unsigned long g_ttsWaitStartMs = 0;
const uint8_t ttsQueryStateFrame[] = {0xFD, 0x00, 0x01, 0x21};

enum WorkState {
    ST_IDLE,
    ST_AI_REPLY,
    ST_WAIT_TTS_DONE
};
static WorkState g_workState = ST_IDLE;
static unsigned long g_waitTtsStartMs = 0;

// LLM后台FreeRTOS任务
static TaskHandle_t llmTaskHandle = nullptr;
static String llmTaskQuery = "";
static bool llmTaskWorkFlag = false;
void llmBackgroundTask(void *pvParameters);

// ===================== TW‑TTS 驱动函数 =====================
void initTTS() {
  softTTS.begin(9600, SERIAL_8N1, TTS_SOFT_RX, TTS_SOFT_TX);
  delay(600);
  Serial.println("[TW‑TTS] 初始化完成");
}

void queryTtsState(void)
{
  softTTS.write(ttsQueryStateFrame, sizeof(ttsQueryStateFrame));
}

void pollTtsStatus()
{
  while(softTTS.available()>0)
  {
    uint8_t b = softTTS.read();
    Serial.printf("[TTS‑RECV] 0x%02X\n",b);
    if(b == 0x4E)
    {
      g_ttsIsBusy = true;
      Serial.println("👉TTS状态：正在播报");
    }
    else if(b == 0x4F)
    {
      g_ttsIsBusy = false;
      Serial.println("👉TTS状态：播报完毕，空闲");
    }
  }
  unsigned long now = millis();
  if(now - g_lastQueryTtsMs > 500)
  {
    queryTtsState();
    g_lastQueryTtsMs = now;
  }
}

static void sendTTSFrame(const uint8_t* utf8_data, uint16_t text_byte_len)
{
  uint8_t buf[256];
  int idx = 0;
  buf[idx++] = 0xFD;
  uint16_t total_len = 1 + 1 + text_byte_len;
  buf[idx++] = highByte(total_len);
  buf[idx++] = lowByte(total_len);
  buf[idx++] = 0x01;   //合成播报命令
  buf[idx++] = 0x04;   //UTF‑8编码
  for(uint16_t i=0; i<text_byte_len; i++){
    buf[idx++] = utf8_data[i];
  }
  softTTS.write(buf, idx);
  Serial.printf("[TW‑TTS]发送 %d字节，文本字节:%d\n", idx, text_byte_len);
}

void speakText(const String& text)
{
  if(text.length() == 0) return;
  String workText = "[s4][v7]" + text;
  if(workText.length() > 1200){
    workText = workText.substring(0,1200);
  }
  Serial.print("[TW‑TTS]播报内容：");
  Serial.println(workText.c_str());
  const uint8_t* raw_utf8 = (const uint8_t*)workText.c_str();
  uint16_t len = workText.length();
  sendTTSFrame(raw_utf8, len);
  g_ttsIsBusy = true;
  g_ttsWaitStartMs = millis(); //开启超时计时
}

void stopSpeaking()
{
  uint8_t stopBuf[] = {0xFD, 0x00, 0x01, 0x02};
  softTTS.write(stopBuf, sizeof(stopBuf));
  Serial.println("[TW‑TTS]停止播报");
  g_ttsIsBusy = false;
}

// ===================== WiFi Manager =====================
bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    Serial.print("Connecting to WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected");
        return true;
    }
    Serial.println("\nWiFi failed");
    return false;
}
bool isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

// ===================== Audio Recorder (INMP441) =====================
static bool micInitialized = false;
void initMicrophone() {
    if (micInitialized) return;
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false
    };
    esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.print("I2S install failed: ");
        Serial.println(err);
        return;
    }
    i2s_pin_config_t pins = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };
    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.print("I2S pin set failed: ");
        Serial.println(err);
        return;
    }
    i2s_set_sample_rates(I2S_PORT, SAMPLE_RATE);
    i2s_start(I2S_PORT);
    micInitialized = true;
    Serial.println("Microphone initialized successfully");
}
size_t recordAudio(uint8_t* buffer, size_t maxLen) {
    if (!micInitialized) {
        initMicrophone();
        delay(10);
    }
    size_t outputSamples = maxLen / sizeof(int16_t);
    if (outputSamples == 0) {
        memset(buffer, 0, maxLen);
        return maxLen;
    }
    size_t bytesToRead = outputSamples * 2 * sizeof(int32_t);
    int32_t* rawBuf = (int32_t*)malloc(bytesToRead);
    if (rawBuf == nullptr) {
        memset(buffer, 0, maxLen);
        return maxLen;
    }
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, rawBuf, bytesToRead, &bytesRead, portMAX_DELAY);
    size_t framesRead = bytesRead / (2 * sizeof(int32_t));
    int16_t* outBuf = (int16_t*)buffer;
    size_t outIdx = 0;
    for (size_t i = 0; i < framesRead && outIdx < outputSamples; i++) {
        int32_t sample = (int32_t)(rawBuf[i * 2] >> 16) * MIC_GAIN;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        outBuf[outIdx++] = (int16_t)sample;
    }
    free(rawBuf);
    if (outIdx < outputSamples) {
        memset((uint8_t*)buffer + outIdx * sizeof(int16_t),
               0, (outputSamples - outIdx) * sizeof(int16_t));
    }
    return outputSamples * sizeof(int16_t);
}
void deinitMicrophone() {
    if (micInitialized) {
        i2s_driver_uninstall(I2S_PORT);
        micInitialized = false;
    }
}

// ===================== 百度 ASR (WebSocket + 实时识别) =====================
typedef void (*ASRCallback)(const String& text, bool isFinal);
static bool isRecognizing = false;
static bool wsHandshakeDone = false;
static ASRCallback asrCallback = nullptr;
static String currentResult = "";
static DynamicJsonDocument* wsResponseDoc = nullptr;
void stopStreamingASR();

bool websocketHandshake(const String& host, const String& path) {
    if (!asrClient) return false;
    const String key = "dGhlIHNhbXBsZSBub25jZQ==";
    asrClient->println("GET " + path + " HTTP/1.1");
    asrClient->print("Host: ");
    asrClient->println(host);
    asrClient->println("Upgrade: websocket");
    asrClient->println("Connection: Upgrade");
    asrClient->print("Sec-WebSocket-Key: ");
    asrClient->println(key);
    asrClient->println("Sec-WebSocket-Version: 13");
    asrClient->println();
    unsigned long timeout = millis() + 5000;
    while (!asrClient->available() && millis() < timeout) {
        delay(10);
    }
    if (!asrClient->available()) {
        Serial.println("Handshake timeout");
        return false;
    }
    String response = asrClient->readStringUntil('\n');
    Serial.print("Handshake response: ");
    Serial.println(response);
    if (response.indexOf("101") < 0) {
        Serial.println("Handshake rejected");
        return false;
    }
    unsigned long headerTimeout = millis() + 2000;
    while (millis() < headerTimeout) {
        if (asrClient->available()) {
            String line = asrClient->readStringUntil('\n');
            line.trim();
            if (line.length() == 0) {
                break;
            }
        } else {
            delay(10);
        }
    }
    Serial.println("WebSocket handshake successful");
    return true;
}

bool sendWebSocketFrame(const uint8_t* data, size_t len, bool isBinary) {
    if (!asrClient || !asrClient->connected()) {
        return false;
    }
    uint8_t header[14];
    int headerLen = 0;
    header[headerLen++] = 0x80 | (isBinary ? 0x02 : 0x01);
    if (len <= 125) {
        header[headerLen++] = 0x80 | (uint8_t)len;
    } else if (len <= 65535) {
        header[headerLen++] = 0x80 | 126;
        header[headerLen++] = (uint8_t)((len >> 8) & 0xFF);
        header[headerLen++] = (uint8_t)(len & 0xFF);
    } else {
        header[headerLen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) {
            header[headerLen++] = (uint8_t)((len >> (i * 8)) & 0xFF);
        }
    }
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    for (int i = 0; i < 4; i++) {
        header[headerLen++] = mask[i];
    }
    size_t headerWritten = asrClient->write(header, headerLen);
    if (headerWritten != headerLen) {
        return false;
    }
    uint8_t* maskedPayload = (uint8_t*)malloc(len);
    if (!maskedPayload) return false;
    for (size_t i = 0; i < len; i++) {
        maskedPayload[i] = data[i] ^ mask[i % 4];
    }
    size_t wlen = asrClient->write(maskedPayload, len);
    free(maskedPayload);
    return wlen == len;
}

bool sendWebSocketText(const String& text) {
    return sendWebSocketFrame((const uint8_t*)text.c_str(), text.length(), false);
}

String receiveWebSocketFrame() {
    if (!asrClient || asrClient->available() < 2) {
        return "";
    }
    uint8_t header[2];
    if (asrClient->readBytes(header, 2) != 2) {
        return "";
    }
    uint8_t opcode = header[0] & 0x0F;
    bool masked = header[1] & 0x80;
    uint64_t payloadLen = header[1] & 0x7F;
    if (opcode == 0x8) {
        Serial.println("WebSocket CLOSE received");
        return "";
    }
    if (opcode == 0x9 || opcode == 0xA) {
        return "";
    }
    if (payloadLen == 126) {
        uint8_t ext[2];
        if (asrClient->readBytes(ext, 2) != 2) return "";
        payloadLen = ((uint16_t)ext[0] << 8) | ext[1];
    } else if (payloadLen == 127) {
        uint8_t ext[8];
        if (asrClient->readBytes(ext, 8) != 8) return "";
        payloadLen = 0;
        for (int i = 0; i < 8; i++) {
            payloadLen = (payloadLen << 8) | ext[i];
        }
    }
    if (payloadLen > 8192) {
        return "";
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (asrClient->readBytes(mask, 4) != 4) return "";
    }
    unsigned long timeout = millis() + 2000;
    while ((unsigned)asrClient->available() < payloadLen) {
        if (millis() > timeout) {
            return "";
        }
        delay(1);
    }
    uint8_t* payload = (uint8_t*)malloc(payloadLen + 1);
    if (payload == nullptr) {
        return "";
    }
    size_t received = asrClient->readBytes(payload, payloadLen);
    if (received != payloadLen) {
        free(payload);
        return "";
    }
    if (masked) {
        for (size_t i = 0; i < payloadLen; i++) {
            payload[i] ^= mask[i % 4];
        }
    }
    payload[payloadLen] = '\0';
    String result = String((char*)payload);
    free(payload);
    return result;
}

String getBaiduAccessToken() {
    if (!connectWiFi()) return "";
    HTTPClient http;
    http.begin(BAIDU_TOKEN_URL);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String postBody = "grant_type=client_credentials"
                    "&client_id=" + String(BAIDU_API_KEY) +
                    "&client_secret=" + String(BAIDU_SECRET_KEY);
    int code = http.POST(postBody);
    String token = "";
    if (code > 0) {
        String resp = http.getString();
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, resp);
        if (!error) {
            token = doc["access_token"].as<String>();
            Serial.print("Token obtained: ");
            Serial.println(token);
        }
    } else {
        Serial.print("Token failed, code: ");
        Serial.println(code);
    }
    http.end();
    return token;
}

bool startStreamingASR(ASRCallback callback) {
    if (isRecognizing) return false;
    asrCallback = callback;
    currentResult = "";
    isRecognizing = true;
    wsHandshakeDone = false;
    if(asrClient == nullptr)
    {
        asrClient = new WiFiClientSecure();
        asrClient->setInsecure();
    }
    else
    {
        asrClient->stop();
    }
    String sn = String(millis()) + "-" + String(random(0xffff), HEX);
    String wsPath = "/realtime_asr?sn=" + sn;
    Serial.print("Connecting to: ");
    Serial.print(BAIDU_ASR_HOST);
    Serial.println(wsPath);
    if (!asrClient->connect(BAIDU_ASR_HOST, BAIDU_ASR_PORT)) {
        Serial.println("TCP connection failed");
        isRecognizing = false;
        return false;
    }
    if (!websocketHandshake(BAIDU_ASR_HOST, wsPath)) {
        Serial.println("WebSocket handshake failed");
        asrClient->stop();
        isRecognizing = false;
        return false;
    }
    wsHandshakeDone = true;
    String startCmd = "{\"type\":\"START\",\"data\":{"
                      "\"appid\":" + String(BAIDU_APP_ID) + ","
                      "\"appkey\":\"" + String(BAIDU_API_KEY) + "\","
                      "\"dev_pid\":1537,"
                      "\"format\":\"pcm\","
                      "\"sample\":16000,"
                      "\"channel\":1,"
                      "\"cuid\":\"esp32‑car\"}}";
    if (!sendWebSocketText(startCmd)) {
        Serial.println("START send failed");
        stopStreamingASR();
        return false;
    }
    uint8_t audioBuffer[AUDIO_CHUNK_SIZE];
    size_t bytesRead = recordAudio(audioBuffer, sizeof(audioBuffer));
    if (bytesRead > 0) {
        sendWebSocketFrame(audioBuffer, bytesRead, true);
    } else {
        memset(audioBuffer, 0, AUDIO_CHUNK_SIZE);
        sendWebSocketFrame(audioBuffer, AUDIO_CHUNK_SIZE, true);
    }
    Serial.println("ASR started successfully");
    return true;
}

void stopStreamingASR() {
    if (isRecognizing && wsHandshakeDone && asrClient && asrClient->connected()) {
        sendWebSocketText("{\"type\":\"FINISH\"}");
        delay(100);
    }
    if (asrClient != nullptr) {
        asrClient->stop();
    }
    isRecognizing = false;
    wsHandshakeDone = false;
}

void loopASR() {
    if (!isRecognizing) return;
    if (!asrClient || !asrClient->connected()) {
        Serial.println("TCP disconnected!");
        isRecognizing = false;
        wsHandshakeDone = false;
        return;
    }
    while (asrClient->available() >= 2) {
        String response = receiveWebSocketFrame();
        if (response.length() == 0) break;
        if (wsResponseDoc == nullptr) {
            wsResponseDoc = new DynamicJsonDocument(4096);
        }
        wsResponseDoc->clear();
        DeserializationError error = deserializeJson(*wsResponseDoc, response);
        if (error) continue;
        String type = (*wsResponseDoc)["type"].as<String>();
        if (type == "HEARTBEAT") continue;
        if (wsResponseDoc->containsKey("err_no")) {
            int errNo = (*wsResponseDoc)["err_no"].as<int>();
            if (errNo != 0) {
                Serial.print("ASR warning: ");
                Serial.println(errNo);
                continue;
            }
        }
        String text = (*wsResponseDoc)["result"].as<String>();
        if (type == "MID_TEXT" && text.length() > 0) {
            currentResult = text;
            if (asrCallback != nullptr) {
                asrCallback(currentResult, false);
            }
        }
        if (type == "FIN_TEXT") {
            if (text.length() > 0) currentResult = text;
            if (asrCallback != nullptr && currentResult.length() > 0) {
                asrCallback(currentResult, true);
            }
            sendWebSocketText("{\"type\":\"FINISH\"}");
            delay(50);
            stopStreamingASR();
            return;
        }
    }
    if (wsHandshakeDone && isRecognizing) {
        static unsigned long lastSend = 0;
        unsigned long now = millis();
        if (now - lastSend >= 40) {
            uint8_t audioBuffer[AUDIO_CHUNK_SIZE];
            size_t bytesRead = recordAudio(audioBuffer, sizeof(audioBuffer));
            if (bytesRead < AUDIO_CHUNK_SIZE) {
                memset(audioBuffer + bytesRead, 0, AUDIO_CHUNK_SIZE - bytesRead);
                bytesRead = AUDIO_CHUNK_SIZE;
            }
            //TW‑TTS忙，发送静音帧抑制回声，和SYN6288逻辑完全一致
            if(g_ttsIsBusy)
            {
                memset(audioBuffer,0,AUDIO_CHUNK_SIZE);
            }
            if (!sendWebSocketFrame(audioBuffer, bytesRead, true)) {
                stopStreamingASR();
                return;
            }
            lastSend += 40;
        }
    }
}

bool isASRRecognizing() {
    return isRecognizing;
}
void initBaiduASR() {
    if (wsResponseDoc == nullptr) {
        wsResponseDoc = new DynamicJsonDocument(4096);
    }
    Serial.println("Baidu ASR initialized");
}

// ===================== DeepSeek LLM 官方API =====================
typedef void (*StreamCallback)(const String& text, bool isFinal);
static void onStreamChunk(const String& text, bool isFinal);

void chatWithDeepSeekStream(const String& userQuestion, StreamCallback callback) {
    Serial.print("👉进入chatWithDeepSeekStream，问题：");
    Serial.println(userQuestion);
    if (!connectWiFi()) {
        isAiProcessing = false;
        if (callback) callback("网络未连接", true);
        return;
    }
    HTTPClient http;
    http.begin(DEEPSEEK_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + DEEPSEEK_API_KEY);
    http.setTimeout(10000);
    StaticJsonDocument<1024> doc;
    doc["model"] = DEEPSEEK_MODEL;
    JsonArray msgs = doc.createNestedArray("messages");
    JsonObject sysMsg = msgs.createNestedObject();
    sysMsg["role"] = "system";
    sysMsg["content"] = "你是车载语音助手，回答简短口语，只用一两句话，每句结尾带上中文句号，不要复杂标点符号。";
    JsonObject msg = msgs.createNestedObject();
    msg["role"] = "user";
    msg["content"] = userQuestion;
    doc["max_tokens"] = 200;
    doc["temperature"] = 0.7;
    doc["stream"] = true;
    String payload;
    serializeJson(doc, payload);
    int code = http.POST(payload);
    Serial.printf("👉POST返回code=%d\n",code);
    if (code <200 || code >=300)
    {
        Serial.println("👉HTTP请求错误！");
        http.end();
        isAiProcessing = false;
        if (callback) callback("AI接口请求异常", true);
        return;
    }
    WiFiClient* stream = http.getStreamPtr();
    String fullReply = "";
    String lastSentText = "";
    while (stream->connected()) {
        if (stream->available()) {
            String line = stream->readStringUntil('\n');
            if (line.startsWith("data: ")) {
                String jsonData = line.substring(6);
                jsonData.trim();
                if (jsonData == "[DONE]") {
                    Serial.println("👉收到[DONE]，LLM流结束");
                    break;
                }
                DynamicJsonDocument chunkDoc(1024);
                DeserializationError error = deserializeJson(chunkDoc, jsonData);
                if (error) {
                    Serial.print("👉JSON解析错误：");
                    Serial.println(error.c_str());
                    continue;
                }
                String delta = chunkDoc["choices"][0]["delta"]["content"].as<String>();
                if (delta.length() > 0) {
                    fullReply += delta;
                    Serial.print("delta:");Serial.println(delta);
                }
            }
        }
        delay(5);
    }
    Serial.print("👉LLM完整返回文本：");
    Serial.println(fullReply);
    http.end();
    if (callback) {
        String remaining = fullReply.substring(lastSentText.length());
        if (remaining.length() > 0) {
            callback(remaining, false);
        }
        if (fullReply.length() > 200) {
            fullReply = fullReply.substring(0, 200) + "...";
        }
        callback(fullReply, true);
    }
}

static void onStreamChunk(const String& text, bool isFinal) {
    static String g_aiFullReply = "";
    if(!isFinal){
        g_aiFullReply += text;
        return;
    }
    isAiProcessing = false;
    Serial.println("✅AI Final: " + g_aiFullReply);

    String buf = g_aiFullReply;
    g_aiFullReply = "";
    buf.trim();

    if(buf.length()>0){
        speakText(buf);
        g_workState = ST_WAIT_TTS_DONE;
        g_waitTtsStartMs = millis();
    }
}

void llmBackgroundTask(void *pvParameters) {
    for (;;) {
        if (llmTaskWorkFlag && llmTaskQuery.length() > 0) {
            String q = llmTaskQuery;
            llmTaskQuery = "";
            llmTaskWorkFlag = false;
            Serial.print("后台任务执行LLM请求：");
            Serial.println(q);
            chatWithDeepSeekStream(q, onStreamChunk);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void onASRResult(const String& text, bool isFinal) {
    if (isFinal) {
        if (text.length() > 0 && !text.startsWith("<error>") && !isAiProcessing) {
            lastRecognizedText = text;
            Serial.println("ASR Final: " + text);
            speakText("收到语音指令");
            isAiProcessing = true;
            pendingAiQuery = text;
        } else if (text.startsWith("<error>")) {
            isAiProcessing = false;
            String errMsg = text.substring(7);
            if (errMsg.length() > 0) {
                speakText("识别失败: " + errMsg);
            } else {
                speakText("识别失败，请重试");
            }
        }
    }
}

void initCommandHandler() {
    SerialAsrPro.begin(115200, SERIAL_8N1, ASRPRO_RX, ASRPRO_TX);
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(AC_CONTROL_PIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    digitalWrite(AC_CONTROL_PIN, LOW);
    initBaiduASR();
    initTTS();
    initMicrophone();
}

void processCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;
    Serial.println("CMD: " + cmd);
    if (cmd.equalsIgnoreCase("CMD_LIGHT_ON")) {
        digitalWrite(LED_BUILTIN, HIGH);
        return;
    }
    if (cmd.equalsIgnoreCase("CMD_LIGHT_OFF")) {
        digitalWrite(LED_BUILTIN, LOW);
        return;
    }
    if (cmd.equalsIgnoreCase("CMD_AC_ON")) {
        digitalWrite(AC_CONTROL_PIN, HIGH);
        return;
    }
    if (cmd.equalsIgnoreCase("CMD_AC_OFF")) {
        digitalWrite(AC_CONTROL_PIN, LOW);
        return;
    }
    if (cmd.startsWith("CMD_MUSIC")) {
        return;
    }
    if (cmd.equalsIgnoreCase("CMD_CHAT_START")) {
        if (isAiProcessing || g_workState == ST_WAIT_TTS_DONE) {
            speakText("请稍候");
            return;
        }
        if (!connectWiFi()) {
            speakText("网络未连接");
            return;
        }
        delay(2000);
        speakText("请说话");
        bool started = startStreamingASR(onASRResult);
        if (!started) {
            speakText("语音识别启动失败");
        }
        return;
    }
    if (cmd.equalsIgnoreCase("CMD_AUDIO_STOP") || cmd.equalsIgnoreCase("CMD_ASSISTANT_OFF")) {
        stopSpeaking();
        stopStreamingASR();
        isAiProcessing = false;
        pendingAiQuery = "";
        llmTaskWorkFlag = false;
        llmTaskQuery = "";
        g_workState = ST_IDLE;
        g_waitTtsStartMs = 0;
        return;
    }
}

// ===================== main setup & loop =====================
void setup() {
    Serial.begin(115200);
    connectWiFi();
    initCommandHandler();
    Serial.println("ESP32 车载语音助手｜TW‑TTS 使用TX应答替代BUSY引脚");
    Serial.println("=====TTS上电测试播报=====");
    speakText("上电测试语音播报");
    xTaskCreate(llmBackgroundTask, "llm_task", 12288, NULL, 2, &llmTaskHandle);
}

void loop() {
    pollTtsStatus();   //持续读取TW‑TTS模块TX应答字节，替代SYN6288的BUSY引脚电平读取
    loopASR();

    //投递到FreeRTOS后台LLM任务（保留原来SYN6288架构）
    if (isAiProcessing && pendingAiQuery.length() > 0)
    {
        llmTaskQuery = pendingAiQuery;
        pendingAiQuery = "";
        llmTaskWorkFlag = true;
        Serial.print("将问题投递至后台任务：");
        Serial.println(llmTaskQuery);
    }

    // ========= 状态机：ST_WAIT_TTS_DONE 完全照搬SYN6288逻辑，把GPIO读电平替换为g_ttsIsBusy，增加超时兜底 =========
    if(g_workState == ST_WAIT_TTS_DONE)
    {
        unsigned long now = millis();
        //条件：模块返回空闲 或者 超过4秒超时强制退出（防止收不到0x4F卡死）
        if( g_ttsIsBusy == false || (now - g_waitTtsStartMs) > TTS_WAIT_TIMEOUT_MS )
        {
            if((now - g_waitTtsStartMs) > TTS_WAIT_TIMEOUT_MS ){
                Serial.println("⚠️TW‑TTS状态查询超时保护，强制退出等待，请检查TTS接线！");
            }else{
                Serial.println("✅TW‑TTS播报完毕(收到0x4F空闲)，准备重启ASR");
            }
            g_workState = ST_IDLE;
            g_waitTtsStartMs = 0;
            if(!connectWiFi()){
                speakText("网络异常，无法继续对话");
            }else{
                delay(350);
                bool ok = startStreamingASR(onASRResult);
                if(!ok){
                    speakText("语音识别重启失败");
                }
            }
        }
    }

    while (SerialAsrPro.available()) {
        String cmd = SerialAsrPro.readStringUntil('\n');
        processCommand(cmd);
    }

    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        if (!isWiFiConnected()) {
            connectWiFi();
        }
    }

    if (isASRRecognizing()) {
        static unsigned long lastCheck = 0;
        if (millis() - lastCheck > 5000) {
            lastCheck = millis();
            if (!asrClient || !asrClient->connected()) {
                Serial.println("ASR client disconnected");
                stopStreamingASR();
            }
        }
    }
}
