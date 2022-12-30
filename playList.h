#ifndef __PLAYLIST_H
#define __PLAYLIST_H

#include <Arduino.h>
#include <vector>
#include "presets.h"

#define PLAYLIST_MAX_ITEMS 100
#define PLAYLIST_MAX_URL_LENGTH 255
#define PLAYLIST_STOPPED -1 /* do not change */

enum streamType { HTTP_FILE,
                  HTTP_STREAM,
                  HTTP_FAVORITE,
                  HTTP_PRESET };
static const char* typeStr[] = { "FILE", "STREAM", "FAVO", "PRESET" };

struct playListItem {
    streamType type;
    String name;
    String url;
    uint32_t index;
};

class playList_t {

  public:
    playList_t() {
        log_d("allocating %i items", PLAYLIST_MAX_ITEMS);
        list.reserve(PLAYLIST_MAX_ITEMS);
    }
    ~playList_t() {
        list.clear();
    }
    int size() {
        return list.size();
    }

    void get(const uint32_t index, playListItem& item) {
        item = (index < list.size()) ? list[index] : (playListItem){};
    }

    String url(const uint32_t index) {
        return (index < list.size()) ? ((list[index].type == HTTP_PRESET) ? preset[list[index].index].url : list[index].url) : "";
    }

    String name(const uint32_t index) {
        return (index < list.size()) ? ((list[index].type == HTTP_PRESET) ? preset[list[index].index].name : list[index].name) : "";
    }

    void add(const playListItem& item) {
        if (list.size() < PLAYLIST_MAX_ITEMS) {
            list.push_back(item);
        }
    }
    void remove(const uint32_t index) {
        if (list.size() > index) {
            list.erase(list.begin() + index);
        }
    }
    void clear() {
        if (list.size()) {
            list.clear();
            _currentItem = PLAYLIST_STOPPED;
        }
    }
    String& toString(String& s);

    int8_t currentItem() {
        return _currentItem;
    }

    void setCurrentItem(int8_t index) {
        _currentItem = index;
    }

  private:
    std::vector<playListItem> list;
    int8_t _currentItem{ PLAYLIST_STOPPED };
};

#endif
