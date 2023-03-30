void websocketEventHandler(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            {
                log_d("client %i connected on %s", client->id(), server->url());
                {
                    String s;
                    client->text(playList.toString(s));
                    s.clear();
                    favoritesToString(s);
                    client->text(!s.equals("") ? s : "favorites\nThe folder '" + String(FAVORITES_FOLDER) + "' could not be found!\n");
                }
                client->printf("status\n%s\n", _paused ? "paused" : "playing");
                client->printf("%s\n%i\n", CURRENT_HEADER, playList.currentItem());
                client->printf("%s\n%i\n", VOLUME_HEADER, _playerVolume);
                client->text(showstation);
                client->text(streamtitle);
                if (_paused && _currentSize) client->printf("progress\n%i\n%i\n", _currentPosition, _currentSize);
            }
            break;
        case WS_EVT_DISCONNECT:
            log_d("client %i disconnected from %s", client->id(), server->url());
            break;
        case WS_EVT_ERROR:
            log_e("ws error");
            ws.close(client->id());
            break;
        case WS_EVT_PONG:
            log_i("ws pong");
            break;
        case WS_EVT_DATA:
            {
                AwsFrameInfo* info = (AwsFrameInfo*)arg;
                if (info->opcode == WS_TEXT) {
                    if (info->final && info->index == 0 && info->len == len)
                        handleSingleFrame(client, data, len);
                    else
                        handleMultiFrame(client, data, len, info);
                }
                break;
            }
        default: log_i("unhandled ws event!");
    }
    ws.cleanupClients();
    log_d("Heap: %d Free: ", ESP.getHeapSize(), ESP.getFreeHeap());
    log_d("Smallest free stack: %i bytes", uxTaskGetStackHighWaterMark(NULL));
    log_d("Largest free continuous memory block: %i bytes", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

void handleSingleFrame(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
    data[len] = 0;
    char* pch = strtok(reinterpret_cast<char*>(data), "\n");
    if (!pch) return;

    static size_t _pausedPosition = 0;

    if (_paused && !strcmp("unpause", pch)) {
        playerMessage msg;
        msg.action = playerMessage::CONNECTTOHOST;
        msg.value = _pausedPosition;
        snprintf(msg.url, PLAYLIST_MAX_URL_LENGTH, playList.url(playList.currentItem()).c_str());
        xQueueSend(playerQueue, &msg, portMAX_DELAY);
        return;
    }

    if (!_paused && !strcmp("pause", pch)) {
        pch = strtok(NULL, "\n");
        if (!pch) return;
        _paused = true;
        _pausedPosition = atoi(pch);
        ws.textAll("status\npaused\n");
        playerMessage msg;
        msg.action = playerMessage::STOPSONG;
        xQueueSend(playerQueue, &msg, portMAX_DELAY);
    }

    else if (!strcmp("volume", pch)) {
        pch = strtok(NULL, "\n");
        if (!pch) return;
        playerMessage msg;
        msg.action = playerMessage::SET_VOLUME;
        const uint8_t volume = atoi(pch);
        msg.value = volume > VS1053_MAXVOLUME ? VS1053_MAXVOLUME : volume;
        xQueueSend(playerQueue, &msg, portMAX_DELAY);
        _playerVolume = msg.value;
        //TODO: send to all but not this client
        ws.printfAll("%s\n%i\n", VOLUME_HEADER, volume);
    }

    else if (!strcmp("previous", pch)) {
        if (playList.currentItem() > 0) {
            playList.setCurrentItem(playList.currentItem() - 1);
            startItem(playList.currentItem());
        }
    }

    else if (!strcmp("next", pch)) {
        if (playList.currentItem() == PLAYLIST_STOPPED) return;
        if (playList.currentItem() < playList.size() - 1) {
            startNextItem();
        }
    }

    else if (!strcmp("filetoplaylist", pch) || !strcmp("_filetoplaylist", pch)) {
        const bool startnow = (pch[0] == '_');
        const uint32_t previousSize = playList.size();
        pch = strtok(NULL, "\n");
        while (pch) {
            playList.add({ HTTP_FILE, "", pch, 0 });
            pch = strtok(NULL, "\n");
        }
        const uint32_t itemsAdded{ playList.size() - previousSize };
        client->printf("%s\nAdded %i items to playlist", MESSAGE_HEADER, itemsAdded);
        log_d("Added %i library items to playlist", itemsAdded);

        if (!itemsAdded) return;

        if (startnow || playList.currentItem() == PLAYLIST_STOPPED) {
            playList.setCurrentItem(previousSize);
            startItem(playList.currentItem());
        }
        upDatePlaylistOnClients();
    }

    else if (!strcmp("playitem", pch)) {
        pch = strtok(NULL, "\n");
        if (!pch) return;
        const uint8_t index = atoi(pch);
        if (index < playList.size()) {
            playList.setCurrentItem(index);
            startItem(playList.currentItem());
        }
    }

    else if (!strcmp("deleteitem", pch)) {
        pch = strtok(NULL, "\n");
        if (!pch) return;
        const uint8_t index = atoi(pch);
        if (index >= playList.size()) return;

        playList.remove(index);
        // deleted item was before current item
        if (index < playList.currentItem()) {
            playList.setCurrentItem(playList.currentItem() - 1);
            upDatePlaylistOnClients();
        }
        //  deleted item was the current item
        else if (playList.currentItem() == index) {
            // play the next item if there is one
            if (playList.currentItem() < playList.size()) {
                upDatePlaylistOnClients();
                startItem(playList.currentItem());
            } else {
                playlistHasEnded();
                upDatePlaylistOnClients();
                playerMessage msg;
                msg.action = playerMessage::STOPSONG;
                xQueueSend(playerQueue, &msg, portMAX_DELAY);
            }
        }
        // deleted item was after current item
        else {
            upDatePlaylistOnClients();
        }
    }

    else if (!strcmp("clearlist", pch)) {
        if (!playList.size()) return;
        playerMessage msg;
        msg.action = playerMessage::STOPSONG;
        xQueueSend(playerQueue, &msg, portMAX_DELAY);
        playList.clear();
        log_d("Playlist cleared");
        playlistHasEnded();
        upDatePlaylistOnClients();
    }

    else if (!strcmp("presetstation", pch) || !strcmp("_presetstation", pch)) {
        const bool startnow = (pch[0] == '_');
        pch = strtok(NULL, "\n");
        if (!pch) return;
        const uint32_t index = atoi(pch);
        if (index >= NUMBER_OF_PRESETS) return;

        const uint32_t previousSize = playList.size();
        playList.add({ HTTP_PRESET, "", "", index });
        if (playList.size() == previousSize) {
            client->printf("%s\nCould not add '%s' to playlist", MESSAGE_HEADER, preset[index].name.c_str());
            return;
        }

        log_d("Added '%s' to playlist", preset[index].name.c_str());
        client->printf("%s\nAdded '%s' to playlist", MESSAGE_HEADER, preset[index].name.c_str());

        if (startnow || playList.currentItem() == PLAYLIST_STOPPED) {
            playList.setCurrentItem(playList.size() - 1);
            startItem(playList.currentItem());
        }
        upDatePlaylistOnClients();

    }

    else if (!strcmp("jumptopos", pch)) {
        pch = strtok(NULL, "\n");
        if (!pch) return;
        playerMessage msg;
        msg.action = playerMessage::CONNECTTOHOST;
        msg.value = atoi(pch);
        snprintf(msg.url, sizeof(msg.url), playList.url(playList.currentItem()).c_str());
        xQueueSend(playerQueue, &msg, portMAX_DELAY);
    }

    else if (!strcmp("currenttofavorites", pch)) {
        pch = strtok(NULL, "\n");
        if (!pch) return;
        playListItem item;
        playList.get(playList.currentItem(), item);
        if (saveItemToFavorites(client, pch, item)) {
            String s;
            ws.textAll(favoritesToString(s));
        } else
            client->printf("%s\nSaving '%s' failed!", MESSAGE_HEADER, item.url.c_str());
    }

    else if (!strcmp("favoritetoplaylist", pch) || !strcmp("_favoritetoplaylist", pch)) {
        const bool startNow = (pch[0] == '_');
        pch = strtok(NULL, "\n");
        if (!pch) return;
        if (playList.size() == PLAYLIST_MAX_ITEMS) {
            client->printf("%s\nCould not add '%s' to playlist!", MESSAGE_HEADER, pch);
            return;
        }
        handleFavoriteToPlaylist(client, pch, startNow);
        upDatePlaylistOnClients();
    }

    else if (!strcmp("deletefavorite", pch)) {
        pch = strtok(NULL, "\n");
        if (!pch) return;
        char filename[strlen(FAVORITES_FOLDER) + strlen(pch) + 1];
        snprintf(filename, sizeof(filename), "%s%s", FAVORITES_FOLDER, pch);
        if (!FFat.remove(filename)) {
            client->printf("%s\nCould not delete %s", MESSAGE_HEADER, pch);
        } else {
            String s;
            ws.textAll(favoritesToString(s));
        }
    }

    else if (!strcmp("foundlink", pch) || !strcmp("_foundlink", pch)) {
        if (playList.size() == PLAYLIST_MAX_ITEMS) {
            client->printf("%s\nCould not add new url to playlist", MESSAGE_HEADER);
            return;
        }
        const char* url = strtok(NULL, "\n");
        if (!url) return;
        const char* name = strtok(NULL, "\n");
        if (!name) return;

        playList.add({ HTTP_FOUND, name, url, 0 });
        upDatePlaylistOnClients();
        const bool startnow = (pch[0] == '_');
        if (startnow || playList.currentItem() == PLAYLIST_STOPPED) {
            playList.setCurrentItem(playList.size() - 1);
            startItem(playList.currentItem());
        }
    }

    else {
        log_i("unhandled single frame ws event! %s", pch);
    }
}

void handleMultiFrame(AsyncWebSocketClient* client, uint8_t* data, size_t len, AwsFrameInfo* info) {
    static String message;
    auto cnt = 0;
    while (cnt < len)
        message.concat((char)data[cnt++]);

    if ((info->index + len) == info->len && info->final) {
        log_d("Final multi frame message for %i bytes", info->index + len);
        if (message.startsWith("_filetoplaylist") || message.startsWith("filetoplaylist")) {
            const bool startnow = (message[0] == '_');
            const uint32_t previousSize = playList.size();

            auto pos = message.indexOf("\n");
            if (-1 == pos) return;
            pos++;

            while (pos < info->len) {
                char url[PLAYLIST_MAX_URL_LENGTH];
                auto cnt = 0;
                while (message.charAt(pos) != '\n') {
                    url[cnt++] = message.charAt(pos++);
                    if (pos == info->len) return;
                }
                url[cnt] = 0;
                playList.add({ HTTP_FILE, "", url, 0 });
                pos++;
            }

            const uint32_t itemsAdded{ playList.size() - previousSize };
            client->printf("%s\nAdded %i items to playlist", MESSAGE_HEADER, itemsAdded);
            log_d("Added %i items to playlist", itemsAdded);

            if (itemsAdded && (startnow || playList.currentItem() == PLAYLIST_STOPPED)) {
                playList.setCurrentItem(previousSize);
                startItem(playList.currentItem());
            }
            if (itemsAdded) upDatePlaylistOnClients();
        }
        message.clear();
    }
}
