#include "streamClass.h"

static HTTPClient* _http = NULL;
static String _url;
static size_t _remainingBytes = 0;
static size_t _startrange;
static String _user;
static String _pwd;
static bool _bufferFilled = false;
static uint8_t _volume = VS1053_INITIALVOLUME;
static int32_t _metaint = 0;
static int32_t _blockPos = 0; /* position within music data block */
static bool _chunkedResponse = false;
static size_t _bytesLeftInChunk = 0;
static bool _dataSeen = false;

const size_t VS1053_PACKETSIZE = 32;
static uint8_t buff[VS1053_PACKETSIZE];

static enum mimetype_t {
    MP3,
    OGG,
    WAV,
    AAC,
    AACP,
    UNKNOWN
} _currentMimetype = UNKNOWN;

static const String mimestr[] = {"MP3", "OGG", "WAV", "AAC", "AAC+", "UNKNOWN"};

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
    _http->addHeader("Icy-MetaData", VS1053_ICY_METADATA ? "1" : "0");

    if (_startrange)
        _http->addHeader("Range", " bytes=" + String(_startrange) + "-");

    if (_user || _pwd)
        _http->setAuthorization(_user.c_str(), _pwd.c_str());

    //prepare for response headers
    const char* CONTENT_TYPE = "Content-Type";
    const char* ICY_NAME = "icy-name";
    const char* ICY_METAINT = "icy-metaint";
    const char* ENCODING = "Transfer-Encoding";

    const char* header[] = {CONTENT_TYPE, ICY_NAME, ICY_METAINT, ENCODING};
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

                else if (_http->header(CONTENT_TYPE).equals("audio/aac"))
                    _currentMimetype = AAC;

                else if (_http->header(CONTENT_TYPE).equals("audio/aacp"))
                    _currentMimetype = AACP;

                else {
                    ESP_LOGE(TAG, "closing - unsupported mimetype %s", _http->header(CONTENT_TYPE).c_str());
                    stopSong();
                    return false;
                }

                ESP_LOGD(TAG, "codec %s", currentCodec());

                if (audio_showstation && !_http->header(ICY_NAME).equals(""))
                    audio_showstation(_http->header(ICY_NAME).c_str());

                _remainingBytes = _http->getSize();  // -1 when Server sends no Content-Length header
                _chunkedResponse = _http->header(ENCODING).equals("chunked") ? true : false;
                _metaint = _http->header(ICY_METAINT).toInt();
                ESP_LOGD(TAG, "metadata interval is %i", _metaint);
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

void streamClass::_nextChunk(WiFiClient* const stream) {
    stream->readStringUntil('\n');
    while (!stream->available()) delay(1);
    _bytesLeftInChunk = strtol(stream->readStringUntil('\n').c_str(), NULL, 16);
    ESP_LOGD(TAG, "_nextChunk was called - chunk size: %i", _bytesLeftInChunk);
}

static void _parseMetaData(const String& data) {
    ESP_LOGD(TAG, "metadata: %s", data.c_str());
    if (audio_showstreamtitle && data.startsWith("StreamTitle")) {
        int32_t pos = data.indexOf("'");
        const int32_t pos2 = data.indexOf("';");
        if (pos != -1 && pos2 != -1) {
            pos++;
            String streamtitle;
            while (pos < pos2) {
                streamtitle.concat(data.charAt(pos));
                pos++;
            }
            if (!streamtitle.equals("")) audio_showstreamtitle(streamtitle.c_str());
        }
    }
}

void streamClass::_handleMetaData(WiFiClient* const stream) {
    size_t remainingBytes = _metaint - _blockPos;
    uint32_t c = 0;
    while (remainingBytes) {
        if (!_bytesLeftInChunk) _nextChunk(stream);
        buff[c] = stream->read();
        remainingBytes--;
        _bytesLeftInChunk--;
        c++;
    }
    if (c) _vs1053->playChunk(buff, c);

    if (!_bytesLeftInChunk) _nextChunk(stream);

    int32_t metaLength = stream->read() * 16;
    _bytesLeftInChunk--;

    if (metaLength) {
        String data;
        while (metaLength) {
            if (!_bytesLeftInChunk) _nextChunk(stream);
            data.concat((char)stream->read());
            _bytesLeftInChunk--;
            metaLength--;
        }
        if (!data.equals("")) _parseMetaData(data);
    }
    _blockPos = 0;
}

void streamClass::_handleStream(WiFiClient* const stream) {
    const size_t size = stream->available();

    if (size) {
        if (!_dataSeen) {
            ESP_LOGD(TAG, "first data bytes are seen - %i bytes", size);
            _dataSeen = true;
            _vs1053->startSong();









/*
            while (stream->read() != 0xFF && (stream->peek() >> 4 != 0xF || stream->peek() >> 4 != 0xE)) {
                _remainingBytes -= _remainingBytes > 0 ? 1 : 0;  
                _blockPos++;
            }
            ESP_LOGI(TAG, "skipped %i bytes before mp3 sync frame", _blockPos);

            buff[0] = 0xFF;
            buff[1] = stream->read();
            _remainingBytes -= _remainingBytes > 0 ? 1 : 0;
            _blockPos++;
            _vs1053->playChunk(buff, 2);
*/













        }
        if (_metaint && (_blockPos > (_metaint - VS1053_PACKETSIZE))) {
            const int c = stream->readBytes(buff, _metaint - _blockPos);
            _remainingBytes -= _remainingBytes > 0 ? c : 0; 
            _vs1053->playChunk(buff, c);
            _blockPos = 0;

            int32_t metaLength = stream->read() * 16;
            _remainingBytes -= _remainingBytes > 0 ? 1 : 0;             

            ESP_LOGD(TAG, "meta length = %i", metaLength);

            if (metaLength) {
                //while (stream->available() < metaLength) delay(1);
                String data;
                while (metaLength) {
                    data.concat((char)stream->read());
                    _remainingBytes -= _remainingBytes > 0 ? 1 : 0;
                    metaLength--;
                }
                if (!data.equals("")) _parseMetaData(data);
            }
        }
        else {
            const int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
            _remainingBytes -= _remainingBytes > 0 ? c : 0;
            _vs1053->playChunk(buff, c);
            _blockPos += c;
        }
    }
}

void streamClass::_handleChunkedStream(WiFiClient* const stream) {
    if (!_bytesLeftInChunk) {
        _bytesLeftInChunk = strtol(stream->readStringUntil('\n').c_str(), NULL, 16);
        ESP_LOGD(TAG, "chunk size: %i", _bytesLeftInChunk);

        if (!_dataSeen && _bytesLeftInChunk) {
            switch (_currentMimetype) {
                case MP3 :
                    ESP_LOGD(TAG, "first data chunk is seen - %i bytes", _bytesLeftInChunk);

                    while (_bytesLeftInChunk && stream->read() != 0xFF && (stream->peek() >> 4 != 0xF || stream->peek() >> 4 != 0xE)) {
                        _bytesLeftInChunk--;
                        _blockPos++;
                    }

                    if (_bytesLeftInChunk) {

                        ESP_LOGI(TAG, "skipped %i bytes before mp3 sync frame", _blockPos);

                        buff[0] = 0xFF;
                        buff[1] = stream->read();
                        _bytesLeftInChunk--;
                        _blockPos++;

                        _dataSeen = true;
                        _vs1053->startSong();
                        _vs1053->playChunk(buff, 2);
                    }
                    break;
                default : {
                        _dataSeen = true;
                    }
            }
        }
    }

    while (_bytesLeftInChunk && _vs1053->data_request()) {
        if (_metaint && (_metaint - _blockPos) < VS1053_PACKETSIZE) _handleMetaData(stream);
        const size_t bytes = min(_bytesLeftInChunk, VS1053_PACKETSIZE);
        const int c = stream->readBytes(buff, bytes);
        _vs1053->playChunk(buff, c);
        _bytesLeftInChunk -= c;
        _blockPos += c;
    }

    if (!_bytesLeftInChunk)
        stream->readStringUntil('\n');
}

void streamClass::loop() {
    if (!_http || !_http->connected() || !_vs1053) return;

    WiFiClient* const stream = _http->getStreamPtr();
    if (!stream->available()) return;
 
        {
            const size_t HTTP_BUFFERSIZE = 1024 * 6;  // on stream start - try to wait for this amount of bytes in the buffer
            const auto MAX_RETRIES = 10;              // but just start playing after MAX_RETRIES regardless of stored amount
            static auto count = 0;

            if (!_bufferFilled) ESP_LOGD(TAG, "Pass: %i available: %i", count, stream->available());

            if ((!_bufferFilled && count++ < MAX_RETRIES) && stream->available() < min(HTTP_BUFFERSIZE, _remainingBytes))
                return;

            _bufferFilled = true;
            count = 0;
        }
    
    if (_http->connected() && (_remainingBytes > 0 || _remainingBytes == -1)) {
        while (_vs1053->data_request() && stream->available()) {
            if (_chunkedResponse) _handleChunkedStream(stream);
            else _handleStream(stream);
        }
    }

    if (!_remainingBytes) {
        ESP_LOGD(TAG, "all data read - closing stream");
        const String temp = audio_eof_stream ? _url : "";
        stopSong();
        if (audio_eof_stream) audio_eof_stream(temp.c_str());
    }
}


bool streamClass::isRunning() {
    return _http != NULL;
}

void streamClass::stopSong() {
    if (_http) {
        if (_http->connected()) {
            {
                WiFiClient* const stream = _http->getStreamPtr();
                stream->stop();
                stream->flush();
            }
            _http->end();
            _vs1053->stopSong();
            _dataSeen = false;
            _bytesLeftInChunk = 0;
            _blockPos = 0;
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
