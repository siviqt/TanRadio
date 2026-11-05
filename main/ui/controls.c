#include "ui/controls.h"

ui_station_action_t ui_station_action(const radio_station_t *selected,
                                     const radio_playback_status_t *playback)
{
    if (!selected || !selected->url[0]) return UI_STATION_DISABLED;
    if (playback->stopped || !stations_same(selected, &playback->station)) {
        return UI_STATION_PLAY;
    }
    /* Buffering changes output readiness, not the user's mute intention. */
    return playback->muted ? UI_STATION_UNMUTE : UI_STATION_MUTE;
}

const char *ui_station_action_label(ui_station_action_t action)
{
    switch (action) {
        case UI_STATION_MUTE: return "Mute";
        case UI_STATION_UNMUTE: return "Unmute";
        default: return "Play";
    }
}

bool ui_activate_station(const radio_station_t *selected, bool toggle_mute)
{
    radio_playback_status_t playback;
    radio_get_playback_status(&playback);
    ui_station_action_t action = ui_station_action(selected, &playback);
    if (action == UI_STATION_DISABLED) return false;
    if (action == UI_STATION_MUTE) {
        if (!toggle_mute) return true; /* Enter always plays; never mutes. */
        radio_toggle_mute();
        return radio_is_muted();
    }
    if (action == UI_STATION_UNMUTE) {
        radio_toggle_mute();
        return !radio_is_muted();
    }
    if (!radio_play_record(selected)) return false;
    if (radio_is_muted()) radio_toggle_mute();
    return !radio_is_muted();
}

void ui_station_list_refresh(ui_station_list_t *list, const radio_station_t *current)
{
    size_t saved_index = 0, count = stations_count();
    bool saved = stations_find(current, &saved_index);
    list->has_preview = current && current->url[0] && !saved;
    if (list->has_preview) list->preview = *current;
    if (!list->has_preview && list->selection == SIZE_MAX) list->selection = saved ? saved_index : 0;
    if (list->has_preview && !count) list->selection = SIZE_MAX;
    if (list->selection != SIZE_MAX && list->selection >= count) list->selection = 0;
}

void ui_station_list_focus(ui_station_list_t *list, const radio_station_t *current)
{
    ui_station_list_refresh(list, current);
    size_t index;
    if (stations_find(current, &index)) list->selection = index;
    else if (list->has_preview) list->selection = SIZE_MAX;
}

bool ui_station_list_selected(const ui_station_list_t *list, radio_station_t *station)
{
    if (list->selection == SIZE_MAX) {
        if (!list->has_preview) return false;
        *station = list->preview;
        return true;
    }
    const radio_station_t *saved = stations_get(list->selection);
    if (!saved) return false;
    *station = *saved;
    return true;
}

void ui_station_list_move(ui_station_list_t *list, int direction)
{
    size_t saved = stations_count(), count = saved + (list->has_preview ? 1 : 0);
    if (!count) return;
    size_t ordinal = list->selection == SIZE_MAX ? saved : list->selection;
    ordinal = direction < 0 ? (ordinal + count - 1) % count : (ordinal + 1) % count;
    list->selection = ordinal == saved ? SIZE_MAX : ordinal;
}
