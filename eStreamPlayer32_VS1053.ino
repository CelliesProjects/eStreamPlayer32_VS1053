#include <Arduino.h>
#include <FFat.h>
#include <WiFi.h>
#include <AsyncTCP.h>          /* https://github.com/me-no-dev/AsyncTCP */
#include <ESPAsyncWebServer.h> /* https://github.com/me-no-dev/ESPAsyncWebServer */
#include <ESP32_VS1053_Stream.h>

#include "playList.h"
#include "index_htm_gz.h"
#include "icons.h"
#include "system_setup.h"

static const char* VERSION_STRING = "eStreamPlayer32 for VS1053 v2.0.0";

struct playerMessage {
    enum playerAction { SET_VOLUME,
                        CONNECTTOHOST,
                        STOPSONG,
                        SETTONE };
    playerAction action;
    char url[PLAYLIST_MAX_URL_LENGTH];
    size_t value = 0;
};

static QueueHandle_t playerQueue = NULL;
static playList_t playList;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

static const char* FAVORITES_FOLDER = "/"; /* if this is a folder use a closing slash */
static const char* VOLUME_HEADER = "volume";
static const char* MESSAGE_HEADER = "message";
static const char* CURRENT_HEADER = "currentPLitem";

static auto _playerVolume = VS1053_INITIALVOLUME;
static bool _paused = false;

constexpr const auto NUMBER_OF_PRESETS = sizeof(preset) / sizeof(source);

//****************************************************************************************
//                                   P L A Y E R _ T A S K                               *
//****************************************************************************************

void playerTask(void* parameter) {
    log_i("Starting VS1053 codec...");

    SPI.begin();

    ESP32_VS1053_Stream audio;

    if (!audio.startDecoder(VS1053_CS, VS1053_DCS, VS1053_DREQ) || !audio.isChipConnected()) {
        log_e("VS1053 board could not init\nSystem halted");
        while (true) delay(100);
    }
    playlistHasEnded();

    log_i("Ready to rock!");
    while (true) {
        playerMessage msg;
        if (xQueueReceive(playerQueue, &msg, pdMS_TO_TICKS(25)) == pdPASS) {
            switch (msg.action) {
                case playerMessage::SET_VOLUME:
                    audio.setVolume(msg.value);
                    break;
                case playerMessage::CONNECTTOHOST:
                    audio.stopSong();
                    _paused = false;
                    ws.textAll("status\nplaying\n");
                    if (!audio.connecttohost(msg.url, LIBRARY_USER, LIBRARY_PWD, msg.value))
                        startNextItem();
                    break;
                case playerMessage::STOPSONG:
                    audio.stopSong();
                    break;
                default: log_e("error: unhandled audio action: %i", msg.action);
            }
        }
        constexpr const auto MAX_UPDATE_FREQ_HZ = 6;
        constexpr const auto UPDATE_INTERVAL_MS = 1000 / MAX_UPDATE_FREQ_HZ;
        static unsigned long previousTime = millis();
        static size_t previousPosition = 0;
        if (ws.count() && audio.size() && millis() - previousTime > UPDATE_INTERVAL_MS && audio.position() != previousPosition) {
            previousTime = millis();
            ws.printfAll("progress\n%i\n%i\n", audio.position(), audio.size());
            previousPosition = audio.position();
        }
        audio.loop();
    }
}

//****************************************************************************************
//                                   H E L P E R - R O U T I N E S                       *
//****************************************************************************************

#define MAX_STATION_NAME_LENGTH 100

void startItem(uint8_t const index, size_t offset = 0) {
    updateCurrentItemOnClients();
    audio_showstreamtitle("");
    playListItem item;
    playList.get(index, item);
    char name[MAX_STATION_NAME_LENGTH];
    switch (item.type) {
        case HTTP_FILE:
            snprintf(name, sizeof(name), "%s", item.url.substring(item.url.lastIndexOf('/') + 1).c_str());
            audio_showstreamtitle(item.url.substring(0, item.url.lastIndexOf('/')).c_str());
            break;
        case HTTP_PRESET:
            snprintf(name, sizeof(name), "%s", preset[item.index].name.c_str());
            break;
        default: snprintf(name, sizeof(name), "%s", item.name.c_str());
    }
    audio_showstation(name);
    playerMessage msg;
    msg.action = playerMessage::CONNECTTOHOST;
    msg.value = offset;
    snprintf(msg.url, sizeof(msg.url), "%s", playList.url(index).c_str());
    xQueueSend(playerQueue, &msg, portMAX_DELAY);
}

void startNextItem() {
    if (playList.currentItem() < playList.size() - 1) {
        playList.setCurrentItem(playList.currentItem() + 1);
        startItem(playList.currentItem());
    } else {
        playlistHasEnded();
    }
}

void playlistHasEnded() {
    audio_showstation("Nothing playing");
    audio_showstreamtitle(VERSION_STRING);
    playList.setCurrentItem(PLAYLIST_STOPPED);
    updateCurrentItemOnClients();
}

inline __attribute__((always_inline)) void updateCurrentItemOnClients() {
    ws.printfAll("%s\n%i\n", CURRENT_HEADER, playList.currentItem());
}

void upDatePlaylistOnClients() {
    {
        String s;
        ws.textAll(playList.toString(s));
    }
    updateCurrentItemOnClients();
}

bool saveItemToFavorites(AsyncWebSocketClient* client, const char* filename, const playListItem& item) {
    if (!strlen(filename)) {
        log_e("ERROR! no filename");
        return false;
    }
    switch (item.type) {
        case HTTP_FILE:
            log_d("file (wont save)%s", item.url.c_str());
            return false;
        case HTTP_PRESET:
            log_d("preset (wont save) %s %s", preset[item.index].name.c_str(), preset[item.index].url.c_str());
            return false;
        case HTTP_STREAM:
        case HTTP_FAVORITE:
            {
                log_d("saving stream: %s -> %s", filename, item.url.c_str());
                char path[strlen(FAVORITES_FOLDER) + strlen(filename) + 1];
                snprintf(path, sizeof(path), "%s%s", FAVORITES_FOLDER, filename);
                File file = FFat.open(path, FILE_WRITE);
                if (!file) {
                    log_e("failed to open file for writing");
                    return false;
                }
                char url[item.url.length() + 2];
                snprintf(url, sizeof(url), "%s\n", item.url.c_str());
                const auto bytesWritten = file.print(url);
                file.close();
                if (bytesWritten < strlen(url)) {
                    log_e("ERROR! saving '%s' failed - disk full?", filename);
                    client->printf("%s\nCould not save '%s' to favorites!", MESSAGE_HEADER, filename);
                    return false;
                }
                return true;
            }
            break;
        default:
            {
                log_w("Unhandled item.type.");
                return false;
            }
    }
}

void handleFavoriteToPlaylist(AsyncWebSocketClient* client, const char* filename, const bool startNow) {
    if (PLAYLIST_MAX_ITEMS == playList.size()) {
        log_e("ERROR! Could not add %s to playlist", filename);
        client->printf("%s\nCould not add '%s' to playlist", MESSAGE_HEADER, filename);
        return;
    }
    char path[strlen(FAVORITES_FOLDER) + strlen(filename) + 1];
    snprintf(path, sizeof(path), "%s%s", FAVORITES_FOLDER, filename);
    File file = FFat.open(path);
    if (!file) {
        log_e("ERROR! Could not open %s", filename);
        client->printf("%s\nCould not add '%s' to playlist", MESSAGE_HEADER, filename);
        return;
    }
    char url[file.size() + 1];
    auto cnt = 0;
    char ch = (char)file.read();
    while (ch != '\n' && file.available()) {
        url[cnt++] = ch;
        ch = (char)file.read();
    }
    url[cnt] = 0;
    file.close();
    const auto previousSize = playList.size();
    playList.add({ HTTP_FAVORITE, filename, url, 0 });

    log_d("favorite to playlist: %s -> %s", filename, url);
    client->printf("%s\nAdded '%s' to playlist", MESSAGE_HEADER, filename);

    if (startNow || playList.currentItem() == PLAYLIST_STOPPED) {
        playList.setCurrentItem(previousSize);
        startItem(playList.currentItem());
    }
}

const String& favoritesToString(String& s) {
    s = "favorites\n";
    File folder = FFat.open(FAVORITES_FOLDER);
    if (!folder) {
        log_e("ERROR! Could not open favorites folder");
        return s;
    }
    File file = folder.openNextFile();
    while (file) {
        if (!file.isDirectory() && file.size() < PLAYLIST_MAX_URL_LENGTH) {
            s.concat(file.name()[0] == '/' ? &file.name()[1] : file.name()); /* until esp32 core 1.6.0 'file.name()' included the preceding slash */
            s.concat("\n");
        }
        file = folder.openNextFile();
    }
    return s;
}

const String& favoritesToCStruct(String& s) {
    File folder = FFat.open(FAVORITES_FOLDER);
    if (!folder) {
        s = "ERROR! Could not open folder " + String(FAVORITES_FOLDER);
        return s;
    }
    s = "const source preset[] = {\n";
    File file = folder.openNextFile();
    while (file) {
        if (!file.isDirectory() && file.size() < PLAYLIST_MAX_URL_LENGTH) {
            s.concat("    {\"");
            s.concat(file.name()[0] == '/' ? &file.name()[1] : file.name()); /* until esp32 core 1.6.0 'file.name()' included the preceding slash */
            s.concat("\", \"");
            char ch = (char)file.read();
            while (file.available() && ch != '\n') {
                s.concat(ch);
                ch = (char)file.read();
            }
            s.concat("\"},\n");
        }
        file = folder.openNextFile();
    }
    s.concat("};\n");
    return s;
}

//****************************************************************************************
//                                   S E T U P                                           *
//****************************************************************************************

const char* HEADER_MODIFIED_SINCE = "If-Modified-Since";

static inline __attribute__((always_inline)) bool htmlUnmodified(const AsyncWebServerRequest* request, const char* date) {
    return request->hasHeader(HEADER_MODIFIED_SINCE) && request->header(HEADER_MODIFIED_SINCE).equals(date);
}

void setup() {
    log_i("\n\n\t\t\t\t%s\n", VERSION_STRING);
    log_i("CPU: %iMhz", getCpuFrequencyMhz());
    log_d("Heap: %d", ESP.getHeapSize());
    log_d("Free: %d", ESP.getFreeHeap());
    log_d("PSRAM: %d", ESP.getPsramSize());
    log_d("Free: %d", ESP.getFreePsram());
    log_i("Found %i presets", NUMBER_OF_PRESETS);

    /* check if a ffat partition is defined and halt the system if it is not defined*/
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat")) {
        log_e("FATAL ERROR! No FFat partition defined. System is halted.\nCheck 'Tools>Partition Scheme' in the Arduino IDE and select a partition table with a FFat partition.");
        while (true) delay(1000); /* system is halted */
    }

    /* partition is defined - try to mount it */
    if (FFat.begin(0, "", 2))  // see: https://github.com/lorol/arduino-esp32fs-plugin#notes-for-fatfs
        log_i("FFat mounted");

    /* partition is present, but does not mount so now we just format it */
    else {
        log_i("Formatting FFat...");
        if (!FFat.format(true, (char*)"ffat") || !FFat.begin(0, "", 2)) {
            log_e("FFat error while formatting. Halting.");
            while (true) delay(1000); /* system is halted */
        }
    }

    btStop();

    if (SET_STATIC_IP && !WiFi.config(STATIC_IP, GATEWAY, SUBNET, PRIMARY_DNS, SECONDARY_DNS)) {
        log_e("Setting static IP failed");
    }
    WiFi.begin(SSID, PSK);
    WiFi.setSleep(false);
    log_i("Connecting to %s...", SSID);

    playerQueue = xQueueCreate(5, sizeof(struct playerMessage));

    if (!playerQueue) {
        log_e("Could not create queue. System halted.");
        while (true) delay(100);
    }

    WiFi.waitForConnectResult();

    if (!WiFi.isConnected()) {
        log_e("Could not connect to Wifi! System halted! Check 'system_setup.h'!");
        while (true) delay(1000); /* system is halted */
    }

    log_i("WiFi connected - IP %s", WiFi.localIP().toString().c_str());

    configTzTime(TIMEZONE, NTP_POOL);

    struct tm timeinfo {};

    log_i("Waiting for NTP sync...");

    while (!getLocalTime(&timeinfo, 0))
        delay(10);

    log_i("Synced");

    //****************************************************************************************
    //                                   W E B S E R V E R                                   *
    //****************************************************************************************

    time_t bootTime;
    time(&bootTime);
    static char modifiedDate[30];
    strftime(modifiedDate, sizeof(modifiedDate), "%a, %d %b %Y %X GMT", gmtime(&bootTime));

    static const char* HTML_MIMETYPE{ "text/html" };
    //static const char* HEADER_MODIFIED_SINCE = "If-Modified-Since";
    static const char* HEADER_LASTMODIFIED{ "Last-Modified" };
    static const char* HEADER_CONTENT_ENCODING{ "Content-Encoding" };
    static const char* GZIP_CONTENT_ENCODING{ "gzip" };

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, HTML_MIMETYPE, index_htm_gz, index_htm_gz_len);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        response->addHeader(HEADER_CONTENT_ENCODING, GZIP_CONTENT_ENCODING);
        request->send(response);
    });

    server.on("/scripturl", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncResponseStream* const response = request->beginResponseStream(HTML_MIMETYPE);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        response->println(SCRIPT_URL);
        if (strlen(LIBRARY_USER) || strlen(LIBRARY_PWD)) {
            response->println(LIBRARY_USER);
            response->println(LIBRARY_PWD);
        }
        request->send(response);
    });

    server.on("/stations", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncResponseStream* const response = request->beginResponseStream(HTML_MIMETYPE);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        auto i = 0;
        while (i < NUMBER_OF_PRESETS)
            response->printf("%s\n", preset[i++].name.c_str());
        request->send(response);
    });

    server.on("/favorites", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncResponseStream* const response = request->beginResponseStream("text/plain");
        String s;
        response->print(favoritesToCStruct(s));
        request->send(response);
    });

    static const char* SVG_MIMETYPE{ "image/svg+xml" };

    server.on("/radioicon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, radioicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/playicon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, playicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/libraryicon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, libraryicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/favoriteicon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, favoriteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/streamicon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, pasteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/deleteicon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, deleteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/addfoldericon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, addfoldericon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/emptyicon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, emptyicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/starticon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, starticon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/pauseicon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse* const response = request->beginResponse_P(200, SVG_MIMETYPE, pauseicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.onNotFound([](AsyncWebServerRequest* request) {
        log_e("404 - Not found: 'http://%s%s'", request->host().c_str(), request->url().c_str());
        request->send(404);
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    server.begin();
    log_i("Webserver started");

    ws.onEvent(websocketEventHandler);
    server.addHandler(&ws);

    const BaseType_t result = xTaskCreatePinnedToCore(
        playerTask,            /* Function to implement the task */
        "playerTask",          /* Name of the task */
        8000,                  /* Stack size in BYTES! */
        NULL,                  /* Task input parameter */
        3 | portPRIVILEGE_BIT, /* Priority of the task */
        NULL,                  /* Task handle. */
        1                      /* Core where the task should run */
    );

    if (result != pdPASS) {
        log_e("ERROR! Could not create playerTask. System halted.");
        while (true) delay(100);
    }

    playerMessage msg;
    msg.action = playerMessage::SET_VOLUME;
    msg.value = VS1053_INITIALVOLUME;
    xQueueSend(playerQueue, &msg, portMAX_DELAY);

    vTaskDelete(NULL);  // this deletes both setup() and loop() - see ~/.arduino15/packages/esp32/hardware/esp32/1.0.6/cores/esp32/main.cpp
}

//****************************************************************************************
//                                   L O O P                                             *
//****************************************************************************************

void loop() {
}

//*****************************************************************************************
//                                  E V E N T S                                           *
//*****************************************************************************************

static char showstation[MAX_STATION_NAME_LENGTH];
void audio_showstation(const char* info) {
    playListItem item;
    playList.get(playList.currentItem(), item);
    snprintf(showstation, sizeof(showstation), "showstation\n%s\n%s", info, typeStr[item.type]);
    log_d("%s", showstation);
    ws.textAll(showstation);
}

#define MAX_METADATA_LENGTH 255
static char streamtitle[MAX_METADATA_LENGTH];
void audio_showstreamtitle(const char* info) {
    snprintf(streamtitle, sizeof(streamtitle), "streamtitle\n%s", percentEncode(info).c_str());
    log_d("%s", streamtitle);
    ws.textAll(streamtitle);
}

void audio_eof_stream(const char* info) {
    log_d("%s", info);
    startNextItem();
}
