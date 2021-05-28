#ifndef __STREAMCLASS_CLIENT__
#define __STREAMCLASS_CLIENT__

#include <Arduino.h>
#include <HTTPClient.h>
#include <VS1053.h>

#include "vs1053b-patches.plg.h"

#define VS1053_INITIALVOLUME   93
#define VS1053_MAXVOLUME       100
#define CONNECT_TIMEOUT_MS     250
#define CONNECT_TIMEOUT_MS_SSL 2500

extern void audio_showstation(const char*) __attribute__((weak));
extern void audio_eof_stream(const char*) __attribute__((weak));
extern void audio_showstreamtitle(const char*) __attribute__((weak));

class streamClass {
    private:
        VS1053* _vs1053 = NULL;
        void _loadUserCode();
    public:
        streamClass();
        ~streamClass();

        bool startDecoder(const uint8_t CS, const uint8_t DCS, const uint8_t DREQ);

        bool connecttohost(const String& url);
        bool connecttohost(const String& url, const size_t startrange);
        bool connecttohost(const String& url, const String& user, const String& pwd);
        bool connecttohost(const String& url, const String& user, const String& pwd, const size_t startrange);

        void loop();
        bool isRunning();
        void stopSong();
        uint8_t getVolume();
        void setVolume(const uint8_t vol); /* 0-100 but only range 60-100 is used in web interface */
        String currentCodec();
};

#endif
