#ifndef TANRADIO_UI_CONTROLS_H
#define TANRADIO_UI_CONTROLS_H

#include "radio/radio.h"

typedef enum {
    UI_STATION_DISABLED,
    UI_STATION_PLAY,
    UI_STATION_MUTE,
    UI_STATION_UNMUTE,
} ui_station_action_t;

/* The legend and activation path share exactly the same state decision. */
ui_station_action_t ui_station_action(const radio_station_t *selected,
                                     const radio_playback_status_t *playback);
const char *ui_station_action_label(ui_station_action_t action);
bool ui_activate_station(const radio_station_t *selected, bool toggle_mute);

typedef struct {
    size_t selection; /* SIZE_MAX denotes the temporary preview row. */
    bool has_preview;
    radio_station_t preview;
} ui_station_list_t;

void ui_station_list_refresh(ui_station_list_t *list, const radio_station_t *current);
void ui_station_list_focus(ui_station_list_t *list, const radio_station_t *current);
bool ui_station_list_selected(const ui_station_list_t *list, radio_station_t *station);
void ui_station_list_move(ui_station_list_t *list, int direction);

#endif
