#include <FFat.h>
#include <AsyncTCP.h>                                   /* https://github.com/me-no-dev/AsyncTCP */
#include <ESPAsyncWebServer.h>                          /* https://github.com/me-no-dev/ESPAsyncWebServer */
#include <VS1053.h>                                     /* https://github.com/baldram/ESP_VS1053_Library */
#include <ESP32_VS1053_Stream.h>                        /* https://github.com/CelliesProjects/eStreamPlayer32_VS1053 */

#include "percentEncode.h"
#include "system_setup.h"
#include "playList.h"
#include "index_htm_gz.h"
#include "icons.h"

const char* VERSION_STRING {
    "eStreamPlayer32 for VS1053 v0.0.1"
};

bool inputReceived = false;
bool endCurrentSong = false;

enum {
    PAUSED,
    PLAYING,
    PLAYLISTEND,
} playerStatus{PLAYLISTEND};

#define     NOTHING_PLAYING_VAL   -1
const char* NOTHING_PLAYING_STR   {
    "Nothing playing"
};

/* websocket message headers */
const char* VOLUME_HEADER {
    "volume\n"
};

const char* CURRENT_HEADER{"currentPLitem\n"};
const char* MESSAGE_HEADER{"message\n"};

int currentItem {NOTHING_PLAYING_VAL};

playList_t playList;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

ESP32_VS1053_Stream audio;

struct {
    bool waiting{false};
    String url;
    uint32_t clientId;
} newUrl;

inline __attribute__((always_inline))
void updateHighlightedItemOnClients() {
    ws.textAll(CURRENT_HEADER + String(currentItem));
}

const String urlEncode(const String& s) {
    //https://en.wikipedia.org/wiki/Percent-encoding
    String encodedstr{""};
    for (int i = 0; i < s.length(); i++) {
        switch (s.charAt(i)) {
            case ' ' : encodedstr.concat("%20");
                break;
            case '!' : encodedstr.concat("%21");
                break;
            case '&' : encodedstr.concat("%26");
                break;
            case  39 : encodedstr.concat("%27"); //39 == single quote '
                break;
            default : encodedstr.concat(s.charAt(i));
        }
    }
    ESP_LOGD(TAG, "encoded url: %s", encodedstr.c_str());
    return encodedstr;
}

void playListHasEnded() {
    currentItem = NOTHING_PLAYING_VAL;
    playerStatus = PLAYLISTEND;
    audio_showstation(NOTHING_PLAYING_STR);
    audio_showstreamtitle(VERSION_STRING);
    updateHighlightedItemOnClients();
    ESP_LOGD(TAG, "End of playlist.");
}

void updateFavoritesOnClients() {
    String s;
    ws.textAll(favoritesToString(s));
    ESP_LOGD(TAG, "Favorites and clients are updated.");
}

static char showstation[200]; // These are kept global to update new clients in loop()
void audio_showstation(const char *info) {
    if (!strcmp(info, "")) return;
    playListItem item;
    playList.get(currentItem, item);
    snprintf(showstation, sizeof(showstation), "showstation\n%s\n%s", info, typeStr[item.type]);
    ESP_LOGD(TAG, "showstation: %s", showstation);
    ws.textAll(showstation);
}

static char streamtitle[256]; // These are kept global to update new clients in loop()
void audio_showstreamtitle(const char *info) {
    snprintf(streamtitle, sizeof(streamtitle), "streamtitle\n%s", percentEncode(info).c_str());
    ESP_LOGD(TAG, "streamtitle: %s", streamtitle);
    ws.printfAll(streamtitle);
}

void audio_id3data(const char *info) {
    ESP_LOGI(TAG, "id3data: %s", info);
    ws.printfAll("id3data\n%s", info);
}

// https://sookocheff.com/post/networking/how-do-websockets-work/
// https://noio-ws.readthedocs.io/en/latest/overview_of_websockets.html

void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        client->text(VOLUME_HEADER + String(audio.getVolume()));
        {
            String s;
            client->text(playList.toString(s));
            client->text(favoritesToString(s));
        }
        client->text(CURRENT_HEADER + String(currentItem));
        client->text(showstation);
        if (currentItem != NOTHING_PLAYING_VAL)
            client->text(streamtitle);
        else {
            char buffer[200];
            snprintf(buffer, sizeof(buffer), "streamtitle\n%s", VERSION_STRING);
            client->text(buffer);
        }
        ESP_LOGD(TAG, "ws[%s][%u] connect", server->url(), client->id());
        return;
    } else if (type == WS_EVT_DISCONNECT) {
        ESP_LOGD(TAG, "ws[%s][%u] disconnect: %u", server->url(), client->id());
        return;
    } else if (type == WS_EVT_ERROR) {
        ESP_LOGE(TAG, "ws[%s][%u] error(%u): %s", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
        return;
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo * info = (AwsFrameInfo*)arg;

        // here all data is contained in a single packet
        if (info->final && info->index == 0 && info->len == len) {
            if (info->opcode == WS_TEXT) {
                data[len] = 0;

                ESP_LOGD(TAG, "ws request: %s", reinterpret_cast<char*>(data));

                char *pch = strtok(reinterpret_cast<char*>(data), "\n");
                if (!pch) return;

                if (!strcmp("volume", pch)) {
                    pch = strtok(NULL, "\n");
                    if (pch) {
                        const uint8_t volume = atoi(pch);
                        audio.setVolume(volume > VS1053_MAXVOLUME ? VS1053_MAXVOLUME : volume);
                        ws.textAll(VOLUME_HEADER + String(audio.getVolume()));
                    }
                    return;
                }

                else if (!strcmp("filetoplaylist", pch)  ||
                         !strcmp("_filetoplaylist", pch)) {
                    const bool startnow = (pch[0] == '_');
                    const uint32_t previousSize = playList.size();
                    pch = strtok(NULL, "\n");
                    while (pch) {
                        ESP_LOGD(TAG, "argument: %s", pch);
                        playList.add({HTTP_FILE, "", pch});
                        pch = strtok(NULL, "\n");
                    }
                    const uint32_t itemsAdded{playList.size() - previousSize};
                    client->printf("%sAdded %i items to playlist", MESSAGE_HEADER, itemsAdded);

                    ESP_LOGD(TAG, "Added %i items to playlist", itemsAdded);

                    if (!itemsAdded) return;

                    {
                        String s;
                        ws.textAll(playList.toString(s));
                    }
                    updateHighlightedItemOnClients();
                    playList.isUpdated = false;

                    if (startnow) {
                        currentItem = previousSize - 1;
                        playerStatus = PLAYING;
                        inputReceived = true;
                        return;
                    }
                    // start playing at the correct position if not already playing
                    if (!audio.isRunning() && PAUSED != playerStatus) {
                        currentItem = previousSize - 1;
                        playerStatus = PLAYING;
                        inputReceived = true;
                    }
                    return;
                }

                else if (!strcmp("clearlist", pch)) {
                    if (!playList.size()) return;
                    playList.clear();
                    playListHasEnded();
                    inputReceived = true;
                    endCurrentSong = true;
                    return;
                }

                else if (!strcmp("playitem", pch)) {
                    pch = strtok(NULL, "\n");
                    if (pch) {
                        const uint32_t index = atoi(pch);
                        if (index < playList.size()) {
                            currentItem = index - 1;
                            playerStatus = PLAYING;
                            inputReceived = true;
                        }
                    }
                    return;
                }

                else if (!strcmp("deleteitem", pch)) {
                    if (!playList.size()) return;
                    pch = strtok(NULL, "\n");
                    if (!pch) return;
                    const uint32_t index = atoi(pch);
                    if (index == currentItem) {
                        playList.remove(index);

                        if (playList.isUpdated )
                        {
                            String s;
                            ws.textAll(playList.toString(s));
                        }
                        playList.isUpdated = false;
                        updateHighlightedItemOnClients();

                        endCurrentSong = true;
                        if (!playList.size()) {
                            playListHasEnded();
                            return;
                        }
                        currentItem--;
                        return;
                    }
                    if (index < playList.size()) {
                        playList.remove(index);
                        if (!playList.size()) {
                            playListHasEnded();
                            return;
                        }
                    } else return;

                    if (currentItem != NOTHING_PLAYING_VAL && index < currentItem) {
                        currentItem--;
                    }
                    return;
                }
                /*
                        else if (!strcmp("pause", pch)) {
                          switch (playerStatus) {
                            case PAUSED :{
                              const uint8_t savedVolume = audio.getVolume();
                              audio.setVolume(0);
                              audio.pauseResume();
                              audio.loop();
                              audio.setVolume(savedVolume);
                              playerStatus = PLAYING;
                              //send play icon to clients
                            }
                            break;
                            case PLAYING : {
                              const uint8_t savedVolume = audio.getVolume();
                              audio.setVolume(0);
                              audio.pauseResume();
                              delay(2);
                              audio.setVolume(savedVolume);
                              playerStatus = PAUSED;
                              //send pause icon to clients
                            }
                            break;
                            default : {};
                          }
                        }
                */
                else if (!strcmp("previous", pch)) {
                    if (PLAYLISTEND == playerStatus) return;
                    ESP_LOGD(TAG, "current: %i size: %i", currentItem, playList.size());
                    if (currentItem > 0) {
                        currentItem--;
                        currentItem--;
                        inputReceived = true;
                        return;
                    }
                    else return;
                }

                else if (!strcmp("next", pch)) {
                    if (PLAYLISTEND == playerStatus) return;
                    ESP_LOGD(TAG, "current: %i size: %i", currentItem, playList.size());
                    if (currentItem < playList.size() - 1) {
                        inputReceived = true;
                        return;
                    }
                    else return;
                }

                else if (!strcmp("newurl", pch)) {
                    pch = strtok(NULL, "\n");
                    if (pch) {
                        ESP_LOGD(TAG, "received new url: %s", pch);
                        newUrl.url = pch;
                        newUrl.clientId = client->id();
                        newUrl.waiting = true;
                    }
                    return;
                }

                else if (!strcmp("currenttofavorites", pch)) {
                    pch = strtok(NULL, "\n");
                    if (pch)
                        handleCurrentToFavorites((String)pch, client->id());
                    return;
                }

                else if (!strcmp("favoritetoplaylist", pch) ||
                         !strcmp("_favoritetoplaylist", pch)) {
                    const bool startNow = (pch[0] == '_');
                    pch = strtok(NULL, "\n");
                    if (pch)
                        handleFavoriteToPlaylist((String)pch, startNow);
                    return;
                }

                else if (!strcmp("deletefavorite", pch)) {
                    pch = strtok(NULL, "\n");
                    if (pch) {
                        if (!FFat.remove("/" + (String)pch)) {
                            ws.printf(client->id(), "%sCould not delete %s", MESSAGE_HEADER, pch);
                        } else {
                            ws.printfAll("%sDeleted favorite %s", MESSAGE_HEADER, pch);
                            updateFavoritesOnClients();
                        }
                    }
                    return;
                }

                else if (!strcmp("presetstation", pch) ||
                         !strcmp("_presetstation", pch)) {
                    const bool startnow = (pch[0] == '_');
                    const uint32_t index = atoi(strtok(NULL, "\n"));
                    if (index < sizeof(preset) / sizeof(source)) { // only add really existing presets to the playlist
                        playList.add({HTTP_PRESET, "", "", index});

                        if (playList.isUpdated) {
                            {
                                String s;
                                ws.textAll(playList.toString(s));
                            }
                            updateHighlightedItemOnClients();
                            playList.isUpdated = false;
                        } else
                            return;

                        ESP_LOGD(TAG, "Added '%s' to playlist", preset[index].name.c_str());
                        client->printf("%sAdded '%s' to playlist", MESSAGE_HEADER, preset[index].name.c_str());

                        if (startnow) {
                            currentItem = playList.size() - 2;
                            playerStatus = PLAYING;
                            inputReceived = true;
                            return;
                        }

                        // start playing at the correct position if not already playing
                        if (!audio.isRunning() && PAUSED != playerStatus) {
                            currentItem = playList.size() - 2;
                            playerStatus = PLAYING;
                            inputReceived = true;
                        }
                        return;
                    }
                }
            }
        } else {
            //message is comprised of multiple frames or the frame is split into multiple packets
            static char* buffer = nullptr;
            if (info->index == 0) {
                if (info->num == 0) {
                    ESP_LOGD(TAG, "ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
                }

                ESP_LOGD(TAG, "ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
                //allocate info->len bytes of memory

                if (!buffer) {
                    // we need at least twice the amount of free memory that is requested (buffer + playlist data)
                    if (info->len * 2 > ESP.getFreeHeap()) {
                        client->printf("%sout of memory", MESSAGE_HEADER);
                        client->close();
                        return;
                    }
                    buffer = new char[info->len + 1];
                }
                else {
                    ESP_LOGE(TAG, "request for buffer but transfer already running. dropping client %i multi frame transfer", client->id());
                    client->printf("%sservice currently unavailable", MESSAGE_HEADER);
                    client->close();
                    return;
                }
            }

            ESP_LOGD(TAG, "ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);
            //move the data to the buffer
            memcpy(buffer + info->index, data, len);
            ESP_LOGD(TAG, "Copied %i bytes to buffer at pos %llu", len, info->index);

            if ((info->index + len) == info->len) {
                ESP_LOGD(TAG, "ws[%s][%u] frame[%u] end[%llu]", server->url(), client->id(), info->num, info->len);
                if (info->final) {
                    ESP_LOGD(TAG, "ws[%s][%u] %s-message end", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");

                    //we should have the complete message now stored in buffer
                    buffer[info->len] = 0;
                    ESP_LOGD(TAG, "complete multi frame request: %s", reinterpret_cast<char*>(buffer));

                    char* pch = strtok(buffer, "\n");
                    if (!strcmp("filetoplaylist", pch) ||
                            !strcmp("_filetoplaylist", pch)) {
                        ESP_LOGD(TAG, "multi frame playlist");
                        const bool startnow = (pch[0] == '_');
                        const uint32_t previousSize = playList.size();
                        pch = strtok(NULL, "\n");
                        while (pch) {
                            ESP_LOGD(TAG, "argument: %s", pch);
                            playList.add({HTTP_FILE, "", pch});
                            pch = strtok(NULL, "\n");
                        }
                        delete []buffer;
                        buffer = nullptr;

                        ESP_LOGD(TAG, "Added %i items to playlist", playList.size() - previousSize);

                        client->printf("%sAdded %i items to playlist", MESSAGE_HEADER, playList.size() - previousSize);

                        if (!playList.isUpdated) return;
                        else
                        {
                            String s;
                            ws.textAll(playList.toString(s));
                            updateHighlightedItemOnClients();
                        }
                        playList.isUpdated = false;

                        if (startnow) {
                            currentItem = previousSize - 1;
                            playerStatus = PLAYING;
                            inputReceived = true;
                            return;
                        }
                        // start playing at the correct position if not already playing
                        if (!audio.isRunning() && PAUSED != playerStatus) {
                            currentItem = previousSize - 1;
                            playerStatus = PLAYING;
                            inputReceived = true;
                        }
                    }
                }
            }
        }
    }
}

const char* HEADER_MODIFIED_SINCE = "If-Modified-Since";

static inline __attribute__((always_inline)) bool htmlUnmodified(const AsyncWebServerRequest * request, const char* date) {
    return request->hasHeader(HEADER_MODIFIED_SINCE) && request->header(HEADER_MODIFIED_SINCE).equals(date);
}

void setup() {
    btStop();

    if (SET_STATIC_IP && !WiFi.config(STATIC_IP, GATEWAY, SUBNET, PRIMARY_DNS, SECONDARY_DNS)) {
        ESP_LOGE(TAG, "Setting static IP failed");
    }

    WiFi.begin(SSID, PSK);
    WiFi.setSleep(false);

    SPI.begin();

    audio.startDecoder(VS1053_CS, VS1053_DCS, VS1053_DREQ);
    audio.setVolume(VS1053_INITIALVOLUME);

    if (psramInit()) {
        ESP_LOGI(TAG, "%.2fMB PSRAM free.", ESP.getFreePsram() / (1024.0 * 1024));
    }

    /* check if a ffat partition is defined and halt the system if it is not defined*/
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat")) {
        ESP_LOGE(TAG, "FATAL ERROR! No FFat partition defined. System is halted.\nCheck 'Tools>Partition Scheme' in the Arduino IDE and select a partition table with a FFat partition.");
        while (true) delay(1000); /* system is halted */
    }

    /* partition is defined - try to mount it */
    if (FFat.begin(0, "", 2)) // see: https://github.com/lorol/arduino-esp32fs-plugin#notes-for-fatfs
        ESP_LOGI(TAG, "FFat mounted.");

    /* partition is present, but does not mount so now we just format it */
    else {
        ESP_LOGI(TAG, "Formatting...");
        if (!FFat.format(true, (char*)"ffat") || !FFat.begin(0, "", 2)) {
            ESP_LOGE(TAG, "FFat error while formatting. Halting.");
            while (true) delay(1000); /* system is halted */;
        }
    }

    ESP_LOGI(TAG, "Found %i presets", sizeof(preset) / sizeof(source));

    WiFi.waitForConnectResult();

    if (!WiFi.isConnected()) {
        ESP_LOGE(TAG, "Could not connect to Wifi! System halted! Check 'wifi_setup.h'!");
        while (true) delay(1000); /* system is halted */;
    }

    ESP_LOGI(TAG, "WiFi: %s", WiFi.localIP().toString().c_str());

    /* sync with ntp */
    configTzTime(TIMEZONE, NTP_POOL);

    struct tm timeinfo {
        0
    };

    ESP_LOGI(TAG, "Waiting for NTP sync..");

    while (!getLocalTime(&timeinfo, 0))
        delay(10);

    time_t bootTime;
    time(&bootTime);
    static char modifiedDate[30];
    strftime(modifiedDate, sizeof(modifiedDate), "%a, %d %b %Y %X GMT", gmtime(&bootTime));

    static const char* HTML_MIMETYPE{"text/html"};
    static const char* HEADER_LASTMODIFIED{"Last-Modified"};
    static const char* CONTENT_ENCODING_HEADER{"Content-Encoding"};
    static const char* CONTENT_ENCODING_VALUE{"gzip"};

    ws.onEvent(onEvent);
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, HTML_MIMETYPE, index_htm_gz, index_htm_gz_len);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        response->addHeader(CONTENT_ENCODING_HEADER, CONTENT_ENCODING_VALUE);
        request->send(response);
    });

    server.on("/stations", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncResponseStream *response = request->beginResponseStream(HTML_MIMETYPE);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        for (int i = 0; i < sizeof(preset) / sizeof(source); i++) {
            response->printf("%s\n", preset[i].name.c_str());
        }
        request->send(response);
    });

    server.on("/scripturl", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncResponseStream *response = request->beginResponseStream(HTML_MIMETYPE);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        response->println(SCRIPT_URL);
        if (!LIBRARY_USER.equals("") || !LIBRARY_PWD.equals("")) {
            response->println(LIBRARY_USER);
            response->println(LIBRARY_PWD);
        }
        request->send(response);
    });

    static const char* SVG_MIMETYPE{"image/svg+xml"};

    server.on("/radioicon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, radioicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/playicon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, playicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/libraryicon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, libraryicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/favoriteicon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, favoriteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/streamicon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, pasteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/deleteicon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, deleteicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/addfoldericon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, addfoldericon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/emptyicon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, emptyicon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.on("/starticon.svg", HTTP_GET, [] (AsyncWebServerRequest * request) {
        if (htmlUnmodified(request, modifiedDate)) return request->send(304);
        AsyncWebServerResponse *response = request->beginResponse_P(200, SVG_MIMETYPE, starticon);
        response->addHeader(HEADER_LASTMODIFIED, modifiedDate);
        request->send(response);
    });

    server.onNotFound([](AsyncWebServerRequest * request) {
        ESP_LOGE(TAG, "404 - Not found: 'http://%s%s'", request->host().c_str(), request->url().c_str());
        request->send(404);
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    server.begin();
    ESP_LOGI(TAG, "Ready to rock!");
}

String& favoritesToString(String& s) {
    File root = FFat.open("/");
    s = "";
    if (!root || !root.isDirectory()) {
        ESP_LOGE(TAG, "ERROR - root folder problem");
        return s;
    }
    s = "favorites\n";
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            s.concat(file.name());
            s.concat("\n");
        }
        file = root.openNextFile();
    }
    return s;
}

bool startPlaylistItem(const playListItem& item) {
    audio.stopSong();
    switch (item.type) {
        case HTTP_FILE :
            ESP_LOGD(TAG, "STARTING file: %s", item.url.c_str());
            audio_showstation(item.url.substring(item.url.lastIndexOf("/") + 1).c_str());
            audio_showstreamtitle(item.url.substring(0, item.url.lastIndexOf("/")).c_str());
            if (LIBRARY_USER || LIBRARY_PWD)
                audio.connecttohost(urlEncode(item.url), LIBRARY_USER, LIBRARY_PWD);
            else
                audio.connecttohost(urlEncode(item.url));
            break;
        case HTTP_STREAM :
            ESP_LOGD(TAG, "STARTING stream: %s", item.url.c_str());
            audio_showstation(item.url.substring(item.url.lastIndexOf("/") + 1).c_str());
            audio_showstreamtitle("");
            audio.connecttohost(urlEncode(item.url));
            break;
        case HTTP_PRESET :
            ESP_LOGD(TAG, "STARTING preset: %s -> %s", preset[item.index].name.c_str(), preset[item.index].url.c_str());
            audio_showstreamtitle("");
            audio_showstation(preset[item.index].name.c_str());
            audio.connecttohost(urlEncode(preset[item.index].url));
            break;
        case HTTP_FAVORITE :
            ESP_LOGD(TAG, "STARTING favorite: %s -> %s", item.name.c_str(), item.url.c_str());
            audio_showstation(item.name.c_str());
            audio_showstreamtitle("");
            audio.connecttohost(urlEncode(item.url));
            break;
        default : ESP_LOGE(TAG, "Unhandled item.type.");
    }
    return audio.isRunning();
}

bool saveItemToFavorites(const playListItem& item, const String& filename) {
    switch (item.type) {
        case HTTP_FILE :
            ESP_LOGD(TAG, "file (wont save)%s", item.url.c_str());
            return false;
            break;
        case HTTP_PRESET :
            ESP_LOGD(TAG, "preset (wont save) %s %s", preset[item.index].name.c_str(), preset[item.index].url.c_str());
            return false;
            break;
        case HTTP_STREAM :
        case HTTP_FAVORITE :
            {
                if (filename.equals("")) {
                    ESP_LOGE(TAG, "Could not save current item. No filename given!");
                    return false;
                }
                ESP_LOGD(TAG, "saving stream: %s -> %s", filename.c_str(), item.url.c_str());
                File file = FFat.open("/" + filename, FILE_WRITE);
                if (!file) {
                    ESP_LOGE(TAG, "failed to open file for writing");
                    return false;
                }
                bool result = file.print(item.url.c_str());
                file.close();
                ESP_LOGD(TAG, "%s writing to '%s'", result ? "ok" : "WARNING - failed", filename.c_str());
                return result;
            }
            break;
        default :
            {
                ESP_LOGW(TAG, "Unhandled item.type.");
                return false;
            }
    }
}

void handlePastedUrl() {

    if (playList.size() > PLAYLIST_MAX_ITEMS - 1) {
        char buffer[50];
        snprintf(buffer, sizeof(buffer), "%sPlaylist is full.", MESSAGE_HEADER);
        ws.text(newUrl.clientId, buffer);
        return;
    }

    const playListItem item {HTTP_STREAM, newUrl.url, newUrl.url};
    playList.add(item);

    if (!playList.isUpdated) return;

    ESP_LOGI(TAG, "STARTING new url: %s with %i items in playList", newUrl.url.c_str(), playList.size());

    {
        String buffer = String(MESSAGE_HEADER) + "opening " + newUrl.url;
        ws.text(newUrl.clientId, buffer.c_str());
        ws.textAll(playList.toString(buffer));
    }

    currentItem = playList.size() - 1;
    updateHighlightedItemOnClients();

    if (startPlaylistItem(item)) {
        ESP_LOGD(TAG, "url started successful");
        playerStatus = PLAYING;
    }
    else {
        char buff[100];
        snprintf(buff, sizeof(buff), "%sFailed to play stream", MESSAGE_HEADER);
        ws.text(newUrl.clientId, buff);
        playList.remove(playList.size() - 1);
        playListHasEnded();
        ESP_LOGD(TAG, "url failed to start");
    }
}

void handleFavoriteToPlaylist(const String& filename, const bool startNow) {
    File file = FFat.open("/" + filename);
    String url;
    if (file) {
        while (file.available() && (file.peek() != '\n') && url.length() < 1024) /* only read the first line and limit the size of the resulting string - unknown/leftover files might contain garbage*/
            url += (char)file.read();
        file.close();
    }
    else {
        ESP_LOGE(TAG, "Could not open %s", filename.c_str());
        ws.printfAll("%sCould not add '%s' to playlist", MESSAGE_HEADER, filename.c_str());
        return;
    }
    playList.add({HTTP_FAVORITE, filename, url});

    if (!playList.isUpdated) return;

    {
        String s;
        ws.textAll(playList.toString(s));
    }
    updateHighlightedItemOnClients();
    playList.isUpdated = false;

    ESP_LOGD(TAG, "favorite to playlist: %s -> %s", filename.c_str(), url.c_str());
    ws.printfAll("%sAdded '%s' to playlist", MESSAGE_HEADER, filename.c_str());
    if (startNow) {
        currentItem = playList.size() - 2;
        playerStatus = PLAYING;
        inputReceived = true;
        return;
    }
    if (!audio.isRunning() && PAUSED != playerStatus) {
        currentItem = playList.size() - 2;
        playerStatus = PLAYING;
    }
}

void handleCurrentToFavorites(const String& filename, const uint32_t clientId) {
    playListItem item;
    playList.get(currentItem, item);

    if (saveItemToFavorites(item, filename)) {
        ws.printfAll("%sAdded '%s' to favorites!", MESSAGE_HEADER, filename.c_str());
        updateFavoritesOnClients();
    }
    else
        ws.printf(clientId, "%sSaving '%s' failed!", MESSAGE_HEADER, filename.c_str());
}

void startCurrentItem() {
    playListItem item;
    playList.get(currentItem, item);

    ESP_LOGI(TAG, "Starting playlist item: %i", currentItem);

    updateHighlightedItemOnClients();

    if (!startPlaylistItem(item))
        ws.printfAll("error - could not start %s", (item.type == HTTP_PRESET) ? preset[item.index].url.c_str() : item.url.c_str());
}

void loop() {
    audio.loop();
    /*
        static int32_t lastFreeRAM = 0;
        if (lastFreeRAM != ESP.getFreeHeap()) {
            lastFreeRAM = ESP.getFreeHeap();
            ESP_LOGI(TAG, "free ram: %i", lastFreeRAM);
        }
    */
    ws.cleanupClients();

    if (endCurrentSong) {
        audio.stopSong();
        endCurrentSong = false;
    }

    if (newUrl.waiting) {
        handlePastedUrl();
        newUrl.waiting = false;
    }

    if ((!audio.isRunning() || inputReceived) && playList.size() && PLAYING == playerStatus) {
        inputReceived = false;
        if (currentItem < playList.size() - 1) {
            currentItem++;
            startCurrentItem();
        }
        else
            playListHasEnded();
    }

    if (playList.isUpdated) {
        {
            String s;
            ws.textAll(playList.toString(s));
        }

        ESP_LOGI(TAG, "Playlist updated. %i items. Free mem: %i", playList.size(), ESP.getFreeHeap());

        updateHighlightedItemOnClients();
        playList.isUpdated = false;
    }
    delay(4);
}
