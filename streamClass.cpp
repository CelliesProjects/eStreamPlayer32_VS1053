#include "streamClass.h"

static HTTPClient* _http = NULL;
static String _url;
static size_t _remainingBytes = 0;
static size_t _startrange;
static String _user;
static String _pwd;
static bool _bufferFilled = false;
static uint8_t _volume = VS1053_INITIALVOLUME;

static enum mimetype_t {
    MP3,
    OGG,
    WAV,
    AAC,
    UNKNOWN
} _currentMimetype = UNKNOWN;

static const String mimestr[] = {"MP3", "OGG", "WAV", "AAC", "UNKNOWN"};

inline __attribute__((always_inline))
static bool networkIsActive() {
    for (int i = TCPIP_ADAPTER_IF_STA; i < TCPIP_ADAPTER_IF_MAX; i++)
        if (tcpip_adapter_is_netif_up((tcpip_adapter_if_t)i)) return true;
    return false;
}

streamClass::streamClass() {}

streamClass::~streamClass() {
    stopSong();
    if (_vs1053) {
        delete _vs1053;
        _vs1053 = NULL;
    }
}

bool streamClass::startDecoder(const uint8_t CS, const uint8_t DCS, const uint8_t DREQ) {
    if (_vs1053) {
        ESP_LOGE(TAG, "vs1053 is already initialized");
        return false;
    }
    _vs1053 = new VS1053(CS, DCS, DREQ);
    if (!_vs1053) {
        ESP_LOGE(TAG, "could not initialize vs1053");
        return false;
    }
    _vs1053->begin();
    _loadUserCode();
    _vs1053->switchToMp3Mode();
    setVolume(_volume);
    return true;
}

bool streamClass::connecttohost(const String& url) {

    if (!_vs1053) {
        ESP_LOGE(TAG, "vs1053 is not initialized");
        return false;
    }

    if (!url.startsWith("http")) {
        ESP_LOGE(TAG, "url should start with http or https");
        return false;
    }

    if (_http) {
        ESP_LOGE(TAG, "client already running!");
        return false;
    }

    if (!networkIsActive()) {
        ESP_LOGE(TAG, "no active network adapter");
        return false;
    }

    _http = new HTTPClient;

    if (!_http) {
        ESP_LOGE(TAG, "client could not be created");
        return false;
    }

    {
        String escapedUrl = url;
        escapedUrl.replace(" ", "%20");
        ESP_LOGD(TAG, "connecting to %s", url.c_str());
        if (!_http->begin(escapedUrl)) {
            ESP_LOGE(TAG, "could not connect to %s", url.c_str());
            stopSong();
            return false;
        }
    }

    // add request headers
    _http->addHeader("Icy-MetaData", "0"); /* set to 0 to prevent glitches */

    if (_startrange)
        _http->addHeader("Range", " bytes=" + String(_startrange) + "-");

    if (_user || _pwd)
        _http->setAuthorization(_user.c_str(), _pwd.c_str());

    //prepare for response headers
    const char* CONTENT_TYPE = "Content-Type";
    const char* ICY_NAME = "icy-name";

    const char* header[] = {CONTENT_TYPE, ICY_NAME};
    _http->collectHeaders(header, sizeof(header) / sizeof(char*));

    _http->setConnectTimeout(url.startsWith("https") ? CONNECT_TIMEOUT_MS_SSL : CONNECT_TIMEOUT_MS);
    _http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    const int _httpCode = _http->GET();

    switch (_httpCode) {
        case 206 : ESP_LOGD(TAG, "server can resume");
        case 200 :
            {
                ESP_LOGD(TAG, "connected to %s", url.c_str());

                /* check if we opened a playlist and try to parse it */
                if (_http->header(CONTENT_TYPE).startsWith("audio/x-scpls") ||
                        _http->header(CONTENT_TYPE).equals("audio/x-mpegurl") ||
                        _http->header(CONTENT_TYPE).equals("application/x-mpegurl") ||
                        _http->header(CONTENT_TYPE).equals("application/pls+xml") ||
                        _http->header(CONTENT_TYPE).equals("application/vnd.apple.mpegurl")) {
                    ESP_LOGW(TAG, "url is a playlist");

                    const String payload = _http->getString();

                    ESP_LOGD(TAG, "payload: %s", payload.c_str());

                    auto index = payload.indexOf("http");
                    if (-1 == index) {
                        ESP_LOGW(TAG, "no url found in file");
                        stopSong();
                        return false;
                    }

                    String newUrl;
                    while (payload.charAt(index) != '\n' && index < payload.length()) {
                        newUrl.concat(payload.charAt(index));
                        index++;
                    }
                    newUrl.trim();

                    ESP_LOGW(TAG, "file parsed - reconnecting to: %s", newUrl.c_str());

                    stopSong();
                    return connecttohost(newUrl);
                }

                else if (_http->header(CONTENT_TYPE).equals("audio/mpeg"))
                    _currentMimetype = MP3;

                //else if (_http->header(CONTENT_TYPE).equals("audio/ogg"))
                //    _currentMimetype = OGG;

                else if (_http->header(CONTENT_TYPE).equals("audio/wav"))
                    _currentMimetype = WAV;

                else if (_http->header(CONTENT_TYPE).startsWith("audio/aac"))
                    _currentMimetype = AAC;

                else {
                    ESP_LOGE(TAG, "closing - unsupported mimetype %s", _http->header(CONTENT_TYPE).c_str());
                    stopSong();
                    return false;
                }

                if (audio_showstation && !_http->header(ICY_NAME).equals(""))
                    audio_showstation(_http->header(ICY_NAME).c_str());

                _remainingBytes = _http->getSize();  // -1 when Server sends no Content-Length header
                _vs1053->startSong();
                _url = url;
                _user.clear();
                _pwd.clear();
                _startrange = 0;
                return true;
            }
        default :
            {
                ESP_LOGE(TAG, "error %i", _httpCode);
                stopSong();
                return false;
            }
    }
}

bool streamClass::connecttohost(const String& url, const size_t startrange) {
    _startrange = startrange;
    return connecttohost(url);
}

bool streamClass::connecttohost(const String& url, const String& user, const String& pwd) {
    _user = user;
    _pwd = pwd;
    return connecttohost(url);
}

bool streamClass::connecttohost(const String& url, const String& user, const String& pwd, const size_t startrange) {
    _user = user;
    _pwd = pwd;
    _startrange = startrange;
    return connecttohost(url);
}

void streamClass::loop() {
    if (!_http || !_http->connected() || !_vs1053) return;

    WiFiClient* const stream = _http->getStreamPtr();

    if (!stream->available()) return;

    {
        const size_t HTTP_BUFFERSIZE = 1024 * 6;  /* on stream start - try to wait for this amount of bytes in the buffer */
        const auto MAX_RETRIES = 10;              /* but just start playing after MAX_RETRIES regardless of stored amount*/
        static auto count = 0;

        //if (!_bufferFilled) ESP_LOGI(TAG, "Pass: %i available: %i", count, stream->available());

        if ((!_bufferFilled && count++ < MAX_RETRIES) && stream->available() < std::min(HTTP_BUFFERSIZE, _remainingBytes))
            return;
        _bufferFilled = true;
        count = 0;
    }

    if (_http->connected() && (_remainingBytes > 0 || _remainingBytes == -1)) {

        while (_vs1053->data_request() && stream->available()) {
            const size_t size = stream->available();
            if (size) {
                static uint8_t buff[32];
                const int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
                if (_remainingBytes > 0) _remainingBytes -= c;
                _vs1053->playChunk(buff, c);
            }
        }

        if (!_remainingBytes) {
            ESP_LOGD(TAG, "all data read - closing stream");
            const String temp = audio_eof_stream ? _url : "";
            stopSong();
            if (audio_eof_stream) audio_eof_stream(temp.c_str());
        }
    }
}

bool streamClass::isRunning() {
    return _http != NULL;
}

void streamClass::stopSong() {
    if (_vs1053) _vs1053->stopSong();
    if (_http) {
        if (_http->connected()) {
            {
                WiFiClient* const stream = _http->getStreamPtr();
                stream->stop();
                stream->flush();
            }
            _http->end();
            _bufferFilled = false;
            _currentMimetype = UNKNOWN;
            _url.clear();
            ESP_LOGD(TAG, "closed stream");
        }
        delete _http;
        _http = NULL;
    }
}

uint8_t streamClass::getVolume() {
    return _volume;
}

void streamClass::setVolume(const uint8_t vol) {
    _volume = vol;
    if (_vs1053) _vs1053->setVolume(_volume);
}

String streamClass::currentCodec() {
    return mimestr[_currentMimetype];
}

void streamClass::_loadUserCode(void) {
    int i = 0;
    while (i < sizeof(plugin) / sizeof(plugin[0])) {
        unsigned short addr, n, val;
        addr = plugin[i++];
        n = plugin[i++];
        if (n & 0x8000U) { /* RLE run, replicate n samples */
            n &= 0x7FFF;
            val = plugin[i++];
            while (n--) {
                _vs1053->write_register(addr, val);
            }
        } else {           /* Copy run, copy n samples */
            while (n--) {
                val = plugin[i++];
                _vs1053->write_register(addr, val);
            }
        }
    }
}
