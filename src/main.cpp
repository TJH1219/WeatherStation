#include <Arduino.h>
#include <WiFiNINA.h>
#include <FlashStorage_SAMD.h>
#include <index_html_gz.h>
#include <MKRENV.h>

#ifndef WIFI_SSID_MAXLEN
#define WIFI_SSID_MAXLEN 32
#endif
#ifndef WIFI_PASS_MAXLEN
#define WIFI_PASS_MAXLEN 63
#endif

WiFiServer server(80);

struct IServerMode {
    virtual void onEnter() = 0;
    virtual void onExit();
    virtual void handleClient(WiFiClient &client) = 0;
    virtual const char* name() const = 0;
    virtual ~IServerMode() = default;
};

inline void IServerMode::onExit() {}

struct WifiCreds {
    char ssid[WIFI_SSID_MAXLEN + 1];
    char pass[WIFI_PASS_MAXLEN + 1];
    uint32_t magic;
};

struct EnvData {
    float Temp;
    float Humidity;
    float Pressure;
};

const uint32_t WIFI_CREDS_MAGIC = 0xC0DEC0DE;

FlashStorage(wifiStore, WifiCreds);

static EnvData getEnvData() {
    ENV.begin();
    EnvData d = {};
    d.Temp = ENV.readTemperature();
    d.Humidity = ENV.readHumidity();
    d.Pressure = ENV.readPressure();
    return d;
}

//url helpers
static inline String urlDecode(const String& src) {
    String out; out.reserve(src.length());
    for (size_t i = 0; i < src.length(); ++i) {
        char c = src[i];
        if (c == '+') { out += ' '; }
        else if (c == '%' && i + 2 < src.length()) {
            auto hex = [](char ch)->int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                return -1;
            };
            int hi = hex(src[i+1]), lo = hex(src[i+2]);
            if (hi >= 0 && lo >= 0) { out += char((hi<<4) | lo); i += 2; }
            else { out += c; }
        } else { out += c; }
    }
    return out;
}

static String getQueryParam(const String& query, const String& key) {
    size_t pos = 0;
    while (pos < query.length()) {
        size_t amp = query.indexOf('&', pos);
        if (amp == (size_t)-1) amp = query.length();
        size_t eq = query.indexOf('=', pos);
        if (eq != (size_t)-1 && eq < amp) {
            String k = query.substring(pos, eq);
            String v = query.substring(eq + 1, amp);
            if (k == key) return urlDecode(v);
        }
        pos = amp + 1;
    }
    return String();
}

static void sendGzipHeaders(WiFiClient& c, const char* contentType, size_t len, bool cache = true) {
    c.println("HTTP/1.1 200 OK");
    c.print("Content-Type: "); c.println(contentType);
    c.println("Content-Encoding: gzip");
    c.println("Vary: Accept-Encoding");
    c.print("Content-Length: "); c.println(len);
    c.println(cache ? "Cache-Control: public, max-age=86400" : "Cache-Control: no-store");
    c.println("Connection: close");
    c.println();
}

static void writeChunks(WiFiClient& c, const uint8_t* data, size_t len) {
    const size_t CHUNK = 512;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
        c.write(data + off, n);
        delay(1);
    }
}


struct STAMode;
void switchTo(IServerMode* next);
void saveCredentialsAndSwitch(const String& ssid, const String& pass);

//Struct representing the MKR being in the access mode state
struct apMode: public IServerMode {
    const char* apSsid;
    const char* apPass;
    apMode(const char* ssid, const char* pass) : apSsid(ssid), apPass(pass) {}

    //Configure access point mode
    void onEnter() override{
        WiFi.end();
        delay(200);
        if (WiFi.beginAP(apSsid,apPass) != WL_AP_LISTENING) {
            WiFi.beginAP(apSsid, apPass);
        }
        delay(200);
        server.begin();
        Serial.print("AP is running at http://"); Serial.println(WiFi.localIP());
    }

    void handleClient(WiFiClient &client) override {
        if (!client) return;

        //Wait for 2s for data
        uint32_t t0 = millis();
        while (!client.available() && millis() - t0 < 2000) delay(1);
        if (!client.available()) return;

        String reqLine = client.readStringUntil('\r'); client.read();

        while (client.connected()) {
            String line = client.readStringUntil('\r'); client.read();
            if (line.length() == 0) break;
        }

        if (reqLine.startsWith("GET /save")) {
            int qmark = reqLine.indexOf('?');
            int spaceAfter = reqLine.indexOf(' ', (qmark >= 0) ? qmark : 0);
            String query;
            if (qmark >= 0 && spaceAfter > qmark) {
                query = reqLine.substring(qmark + 1, spaceAfter);

            }
            String ssid = getQueryParam(query, "ssid");
            String pass = getQueryParam(query, "pass");

            if (ssid.length() == 0) {
                String msg = "Missing SSID";
                client.println("HTTP/1.1 400 Bad Request");
                client.println("Content-Type: text/plain; charset=UTF-8");
                client.print("Content-Length: "); client.println(msg.length());
                client.println("Connection: close");
                client.println();
                client.print(msg);
                client.flush();
                delay(1);
                client.stop();
                return;
            }

            // Save to flash
            WifiCreds creds = {};
            strncpy(creds.ssid, ssid.c_str(), WIFI_SSID_MAXLEN);
            strncpy(creds.pass, pass.c_str(), WIFI_PASS_MAXLEN);
            creds.magic = WIFI_CREDS_MAGIC;
            wifiStore.write(creds);

            // Respond
            String msg = "Credentials saved. Switching to STA...";
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: text/plain; charset=UTF-8");
            client.print("Content-Length: "); client.println(msg.length());
            client.println("Connection: close");
            client.println();
            client.print(msg);
            client.flush();
            delay(1);
            client.stop();

            // Switch to STA mode with new credentials
            saveCredentialsAndSwitch(ssid, pass);
            return;
        }

        String html =
           "<!doctype html><html><body>"
           "<h3>MKR Setup AP</h3>"
           "<form method='GET' action='/save'>"
           "<label>SSID <input name='ssid' required></label><br/>"
           "<label>Password <input name='pass' type='password'></label><br/>"
           "<button type='submit'>Connect</button>"
           "</form></body></html>";

        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: text/html; charset=UTF-8");
        client.print("Content-Length: "); client.println(html.length());
        client.println("Connection: close");
        client.println();
        client.print(html);
        client.flush();
        delay(1);
        client.stop();
    }
    const char* name() const override { return "APMode"; }
};

struct STAMode : public IServerMode {
    String ssid, pass;
    STAMode(const String& s, const String& p): ssid(s), pass(p) {};

    void onEnter() override {
        WiFi.end();
        delay(200);
        Serial.print("Connecting to SSID: "); Serial.println(ssid);
        WiFi.begin(ssid.c_str(), pass.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
            delay(500); Serial.print(".");
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) {
            server.begin();
            Serial.print("STA IP: "); Serial.println(WiFi.localIP());
        } else {
            Serial.println("STA failed to connect");
        }
    }

    void handleClient(WiFiClient& client) override {
        if (!client) return;

        uint32_t t0 = millis();
        while (!client.available() && millis() - t0 < 2000) delay(1);
        if (!client.available()) return;

        String reqLine = client.readStringUntil('\r'); client.read();
        while (client.connected()) {
            String line = client.readStringUntil('\r'); client.read();
            if (line.length() == 0) break;
        }

        // Parse method and path
        bool isGet = reqLine.startsWith("GET ");
        int pathStart = isGet ? 4 : -1;
        int pathEnd = (pathStart >= 0) ? reqLine.indexOf(' ', pathStart) : -1;
        String path = (pathStart >= 0 && pathEnd > pathStart) ? reqLine.substring(pathStart, pathEnd) : "/";

        // Serve the compressed index.html
        if (isGet && (path == "/" || path == "/index.html")) {
            sendGzipHeaders(client, "text/html; charset=UTF-8", index_html_gz_len, true);
            writeChunks(client, index_html_gz, index_html_gz_len);
            client.flush();
            delay(1);
            client.stop();
            return;
        }
        else if (isGet && (path == "/api/env")) {
            EnvData d = getEnvData();
            String json = "{";
            json += "\"temp\": " + String(d.Temp) + ",";
            json += "\"hum\": " + String(d.Humidity) + ",";
            json += "\"press\": " + String(d.Pressure);
            json += "}";
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: application/json; charset=UTF-8");
            client.println("Cache-Control: no-store, no-cache, must-revalidate");
            client.print("Content-Length: "); client.println(json.length());
            client.println("Connection: close");
            client.println();
            client.print(json); // use print to avoid extra newline in body
            client.flush();
            delay(1);
            client.stop();
            return;

        }

        // ... existing code ...
        String body = "Device online at " + WiFi.localIP().toString() + "\n";
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: text/plain; charset=UTF-8");
        client.print("Content-Length: "); client.println(body.length());
        client.println("Connection: close");
        client.println();
        client.print(body);
        client.flush();
        delay(1);
        client.stop();
    }


    const char* name() const override { return "STAMode"; }
};

apMode apMode{"MKR-Setup", "configure123"};
STAMode staMode{"",""};
IServerMode* currentMode = &apMode;

void switchTo(IServerMode* next) {
    if (currentMode == next) return;
    if (currentMode) currentMode->onExit();
    currentMode = next;
    currentMode->onEnter();
}

void saveCredentialsAndSwitch(const String& ssid, const String& pass) {
    staMode.ssid = ssid;
    staMode.pass = pass;
    switchTo(&staMode);
}

void setup() {
    Serial.begin(9600);
    while (!Serial) {}
    if (WiFi.status() == WL_NO_MODULE) {
        Serial.println("WiFi module not found");
        while (true) delay(1000);
    }

    // Load saved credentials (if any)
    WifiCreds saved = {};
    wifiStore.read(saved);
    if (saved.magic == WIFI_CREDS_MAGIC && saved.ssid[0] != '\0') {
        staMode.ssid = String(saved.ssid);
        staMode.pass = String(saved.pass);
        currentMode = &staMode;
    } else {
        currentMode = &apMode;
    }
    currentMode->onEnter();
}

void loop() {
    WiFiClient client = server.available();
    if (client && currentMode) {
        currentMode->handleClient(client);
    }

    // Fallback to AP if STA drops
    if (currentMode == (IServerMode*)&staMode && WiFi.status() != WL_CONNECTED) {
        Serial.println("Lost STA; switching back to AP");
        switchTo(&apMode);
    }
}
