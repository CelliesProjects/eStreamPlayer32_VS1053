#include "playList.h"

String& playList_t::toString(String& s) {
    s = "playlist\n";
    if (list.size()) {
        for (const auto& item : list) {
            switch (item.type) {

                case HTTP_FILE:
                    s.concat(item.url.substring(item.url.lastIndexOf("/") + 1) + "\n" + typeStr[item.type] + "\n");
                    break;

                case HTTP_PRESET:
                    s.concat(preset[item.index].name + "\n" + typeStr[item.type] + "\n");
                    break;

                case HTTP_FOUND:
                case HTTP_FAVORITE:
                    s.concat(item.name + "\n" + typeStr[item.type] + "\n");
                    break;

                default:
                    log_e("ERROR! Playlist 'item.type' has no handler!");
                    break;
            }
        }
    }
    return s;
}
