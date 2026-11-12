#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stdio.h>

#define SETTINGS_PATH "/ext/apps_data/alarm_clock.bin"
#define SETTINGS_MAGIC 0x414C524D
#define SNOOZE_TICKS 3000 // 5 minutes at 100 ms per loop

typedef struct {
    ViewPort* view;
    void* notification;
    uint8_t hour, minute;
    bool enabled, ringing, exit, dirty;
    uint8_t field;
    uint32_t last_day;
    uint32_t snooze_ticks;
    uint8_t ring_ticks;
} App;

typedef struct {
    uint32_t magic;
    uint8_t hour;
    uint8_t minute;
    uint8_t enabled;
} AlarmSettings;

static const NotificationSequence alarm_sequence = {
    &message_vibro_on,
    &message_note_c5,
    &message_delay_100,
    &message_note_e5,
    &message_delay_100,
    &message_note_g5,
    &message_vibro_off,
    NULL,
};

static const NotificationSequence alarm_stop_sequence = {
    &message_vibro_off,
    &message_sound_off,
    NULL,
};

static void settings_load(App* a) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    AlarmSettings settings = {0};

    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(file, &settings, sizeof(settings)) == sizeof(settings) &&
           settings.magic == SETTINGS_MAGIC &&
           settings.hour < 24 && settings.minute < 60) {
            a->hour = settings.hour;
            a->minute = settings.minute;
            a->enabled = settings.enabled != 0;
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void settings_save(App* a) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    AlarmSettings settings = {
        .magic = SETTINGS_MAGIC,
        .hour = a->hour,
        .minute = a->minute,
        .enabled = a->enabled,
    };

    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, &settings, sizeof(settings));
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    a->dirty = false;
}

static void draw(Canvas* canvas, void* ctx) {
    App* a = ctx;
    char text[32];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 30, 12, "Alarm Clock");

    canvas_set_font(canvas, FontSecondary);
    snprintf(text, sizeof(text), "Alarm: %02u:%02u", a->hour, a->minute);
    canvas_draw_str(canvas, 20, 29, text);
    canvas_draw_str(canvas, 20, 41, a->enabled ? "Enabled (daily)" : "Disabled");

    if(a->ringing) {
        canvas_draw_str(canvas, 2, 54, "Back: dismiss");
        canvas_draw_str(canvas, 2, 63, "Other keys: snooze 5m");
    } else if(a->snooze_ticks) {
        canvas_draw_str(canvas, 2, 54, "Snoozed for 5 minutes");
        canvas_draw_str(canvas, 2, 63, "Back: exit");
    } else {
        canvas_draw_str(canvas, 2, 54, "Left/Right: field");
        canvas_draw_str(canvas, 2, 63, "Up/Down: set  OK: enable");
    }
}

static void input(InputEvent* e, void* ctx) {
    App* a = ctx;
    if(e->type != InputTypePress) return;

    if(a->ringing) {
        a->ringing = false;
        a->ring_ticks = 0;

        if(e->key == InputKeyBack) {
            a->snooze_ticks = 0;
        } else {
            a->snooze_ticks = SNOOZE_TICKS;
        }

        notification_message(a->notification, &alarm_stop_sequence);
    } else if(e->key == InputKeyBack) {
        a->exit = true;
    } else if(e->key == InputKeyLeft || e->key == InputKeyRight) {
        a->field ^= 1;
    } else if(e->key == InputKeyOk) {
        a->enabled = !a->enabled;
        a->dirty = true;
    } else if(e->key == InputKeyUp || e->key == InputKeyDown) {
        int d = e->key == InputKeyUp ? 1 : -1;

        if(a->field == 0) {
            a->hour = (a->hour + 24 + d) % 24;
        } else {
            a->minute = (a->minute + 60 + d) % 60;
        }
        a->dirty = true;
    }

    view_port_update(a->view);
}

int32_t alarm_clock_app(void* p) {
    UNUSED(p);

    App a = {.hour = 7, .minute = 0};
    settings_load(&a);

    a.notification = furi_record_open(RECORD_NOTIFICATION);
    Gui* gui = furi_record_open(RECORD_GUI);

    a.view = view_port_alloc();
    view_port_draw_callback_set(a.view, draw, &a);
    view_port_input_callback_set(a.view, input, &a);
    gui_add_view_port(gui, a.view, GuiLayerFullscreen);

    while(!a.exit) {
        DateTime now;
        furi_hal_rtc_get_datetime(&now);

        uint32_t day = now.year * 10000 + now.month * 100 + now.day;

        if(a.dirty) settings_save(&a);

        if(a.ringing) {
            if(++a.ring_ticks >= 8) {
                a.ring_ticks = 0;
                notification_message(a.notification, &alarm_sequence);
            }
        } else if(a.snooze_ticks) {
            if(--a.snooze_ticks == 0) {
                a.ringing = true;
                a.ring_ticks = 0;
                notification_message(a.notification, &alarm_sequence);
                view_port_update(a.view);
            }
        } else if(a.enabled && now.hour == a.hour &&
                  now.minute == a.minute && now.second == 0 &&
                  day != a.last_day) {
            a.last_day = day;
            a.ringing = true;
            a.ring_ticks = 0;
            notification_message(a.notification, &alarm_sequence);
            view_port_update(a.view);
        }

        furi_delay_ms(100);
    }

    if(a.dirty) settings_save(&a);
    notification_message(a.notification, &alarm_stop_sequence);

    gui_remove_view_port(gui, a.view);
    view_port_free(a.view);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    return 0;
}