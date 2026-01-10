#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_rtc.h>
#include <furi_hal_speaker.h>
#include <gui/gui.h>
#include <input/input.h>
#include <datetime/datetime.h>
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define SETTINGS_DIR "/ext/apps_data/alarm_clock"
#define SETTINGS_PATH SETTINGS_DIR "/settings.dat"
#define ALARM_AUTO_STOP_SECONDS 300 
#define ALARM_SNOOZE_MINUTES 10     

typedef struct {
    int hour;
    int minute;
    bool active;
    bool triggered;
    uint32_t triggered_start_seconds;
    bool hardware_on;
    int last_trigger_minute; 
} AlarmSettings;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    AlarmSettings alarm;
} AlarmApp;

typedef struct {
    int hour;
    int minute;
    bool active;
} AlarmSettingsPersist;

static inline uint32_t seconds_of_day(const DateTime* dt) {
    return (uint32_t)dt->hour * 3600u + (uint32_t)dt->minute * 60u + (uint32_t)dt->second;
}

static bool save_settings(const AlarmSettings* alarm) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, SETTINGS_DIR);
    
    StorageFile* file = storage_file_alloc(storage);
    bool result = false;

    AlarmSettingsPersist p = {
        .hour = alarm->hour,
        .minute = alarm->minute,
        .active = alarm->active,
    };

    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ssize_t wrote = storage_file_write(file, &p, sizeof(p));
        result = (wrote == (ssize_t)sizeof(p));
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return result;
}

static bool load_settings(AlarmSettings* alarm) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    StorageFile* file = storage_file_alloc(storage);
    bool result = false;

    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        AlarmSettingsPersist p;
        ssize_t read = storage_file_read(file, &p, sizeof(p));
        if(read == (ssize_t)sizeof(p)) {
            alarm->hour = p.hour;
            alarm->minute = p.minute;
            alarm->active = p.active;
            result = true;
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return result;
}

static void alarm_hw_start(void) {
    if(furi_hal_speaker_acquire(1000)) {
        furi_hal_speaker_start(440.0f, 1.0f);
    }
    furi_hal_vibro_on(true);
}

static void alarm_hw_stop(void) {
    if(furi_hal_speaker_is_mine()) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
    furi_hal_vibro_on(false);
}

static void render_callback(Canvas* canvas, void* ctx) {
    AlarmApp* app = ctx;
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 28, 12, "ALARM CLOCK");

    char buf[32];
    snprintf(buf, sizeof(buf), "Now: %02d:%02d:%02d", dt.hour, dt.minute, dt.second);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 25, 24, buf);

    snprintf(buf, sizeof(buf), "%02d:%02d", app->alarm.hour, app->alarm.minute);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, 35, 45, buf);

    canvas_set_font(canvas, FontSecondary);
    if(app->alarm.triggered) {
        canvas_draw_str(canvas, 0, 54, "OK: Snooze | OK (Long): Stop");
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 32, 63, "!!! RINGING !!!");
    } else {
        canvas_draw_str(canvas, 0, 62, app->alarm.active ? "UP/DN:H L/R:M OK:OFF" : "UP/DN:H L/R:M OK:ON");
    }
}

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, input_event, FuriWaitForever);
}

int32_t alarm_clock_app(void* p) {
    (void)p;
    AlarmApp* app = malloc(sizeof(AlarmApp));
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    app->alarm.hour = 7;
    app->alarm.minute = 0;
    app->alarm.active = false;
    app->alarm.triggered = false;
    app->alarm.hardware_on = false;
    app->alarm.last_trigger_minute = -1;

    load_settings(&app->alarm);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app->event_queue);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    InputEvent event;
    while(1) {
        FuriStatus event_status = furi_message_queue_get(app->event_queue, &event, 100);

        DateTime now;
        furi_hal_rtc_get_datetime(&now);

        if(app->alarm.active && !app->alarm.triggered &&
           now.hour == app->alarm.hour && now.minute == app->alarm.minute &&
           app->alarm.last_trigger_minute != now.minute) {
            
            app->alarm.triggered = true;
            app->alarm.last_trigger_minute = now.minute;
            app->alarm.triggered_start_seconds = seconds_of_day(&now);
            if(!app->alarm.hardware_on) {
                alarm_hw_start();
                app->alarm.hardware_on = true;
            }
        }

        if(app->alarm.triggered) {
            uint32_t now_s = seconds_of_day(&now);
            uint32_t start_s = app->alarm.triggered_start_seconds;
            uint32_t elapsed = (now_s >= start_s) ? (now_s - start_s) : (86400u - start_s + now_s);

            if(elapsed >= ALARM_AUTO_STOP_SECONDS) {
                if(app->alarm.hardware_on) { alarm_hw_stop(); app->alarm.hardware_on = false; }
                app->alarm.triggered = false;
            }
        }

        if(event_status == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) break;

            if(!app->alarm.triggered) {
                if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                    if(event.key == InputKeyUp) app->alarm.hour = (app->alarm.hour + 1) % 24;
                    else if(event.key == InputKeyDown) app->alarm.hour = (app->alarm.hour + 23) % 24;
                    else if(event.key == InputKeyRight) app->alarm.minute = (app->alarm.minute + 1) % 60;
                    else if(event.key == InputKeyLeft) app->alarm.minute = (app->alarm.minute + 59) % 60;
                    save_settings(&app->alarm);
                }
                if(event.key == InputKeyOk && event.type == InputTypeShort) {
                    app->alarm.active = !app->alarm.active;
                    save_settings(&app->alarm);
                }
            } else {
                if(event.key == InputKeyOk) {
                    if(app->alarm.hardware_on) { alarm_hw_stop(); app->alarm.hardware_on = false; }
                    app->alarm.triggered = false;

                    if(event.type == InputTypeShort) { 
                        int total_min = (now.hour * 60 + now.minute + ALARM_SNOOZE_MINUTES) % 1440;
                        app->alarm.hour = total_min / 60;
                        app->alarm.minute = total_min % 60;
                        app->alarm.last_trigger_minute = -1;
                    } else if(event.type == InputTypeLong) { 
                        app->alarm.active = false;
                    }
                    save_settings(&app->alarm);
                }
            }
        }
        view_port_update(app->view_port);
    }

    if(app->alarm.hardware_on) alarm_hw_stop();
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
