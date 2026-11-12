#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <notification/notification_messages.h>
#include <stdio.h>

typedef struct {
    ViewPort* view;
    void* notification;
    uint8_t hour, minute;
    bool enabled, ringing, exit;
    uint8_t field;
    uint32_t last_day;
} App;

static const NotificationSequence alarm_sequence = {
    &message_vibro_on, &message_note_c5, &message_delay_100,
    &message_note_e5, &message_delay_100, &message_note_g5,
    &message_vibro_off, NULL,
};

static void draw(Canvas* canvas, void* ctx) {
    App* a = ctx;
    char text[32];
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 30, 12, "Alarm Clock");
    canvas_set_font(canvas, FontSecondary);
    snprintf(text, sizeof(text), "Alarm: %02u:%02u", a->hour, a->minute);
    canvas_draw_str(canvas, 20, 30, text);
    canvas_draw_str(canvas, 20, 42, a->enabled ? "Enabled (daily)" : "Disabled");
    canvas_draw_str(canvas, 2, 56, a->ringing ? "Ringing - press any key" : "Left/Right: field");
    canvas_draw_str(canvas, 2, 63, "Up/Down: set  OK: enable");
}

static void input(InputEvent* e, void* ctx) {
    App* a = ctx;
    if(e->type != InputTypePress) return;
    if(a->ringing) a->ringing = false;
    else if(e->key == InputKeyBack) a->exit = true;
    else if(e->key == InputKeyLeft || e->key == InputKeyRight) a->field ^= 1;
    else if(e->key == InputKeyOk) a->enabled = !a->enabled;
    else if(e->key == InputKeyUp || e->key == InputKeyDown) {
        int d = e->key == InputKeyUp ? 1 : -1;
        if(a->field == 0) a->hour = (a->hour + 24 + d) % 24;
        else a->minute = (a->minute + 60 + d) % 60;
    }
    view_port_update(a->view);
}

int32_t alarm_clock_app(void* p) {
    UNUSED(p);
    App a = {.hour = 7, .minute = 0};
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
        if(a.enabled && !a.ringing && now.hour == a.hour &&
           now.minute == a.minute && now.second == 0 && day != a.last_day) {
            a.last_day = day;
            a.ringing = true;
            notification_message(a.notification, &alarm_sequence);
            view_port_update(a.view);
        }
        furi_delay_ms(100);
    }

    gui_remove_view_port(gui, a.view);
    view_port_free(a.view);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    return 0;
}