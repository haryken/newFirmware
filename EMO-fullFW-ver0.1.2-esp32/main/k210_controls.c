// k210_controls.c — named K210 commands behind POST /api/controls.
//
// Source: uart_commands.md (every ESP32 -> K210 small command and extended
// selector found in the stock firmware) plus the face assets listed in
// FIRMWARE_HANDOFF.md 4.3. Nothing here touches the mic/speaker state machine
// except mode_mic, which goes through microphone_open() so the link stays in sync.
#include "k210_controls.h"
#include "k210_link.h"
#include "k210_uart.h"
#include "microphone.h"
#include "video.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "ctl";

typedef enum { K_SHORT, K_EXT, K_ANIM, K_ANIM_ANY, K_MIC } kind_t;
typedef enum { A_NONE, A_V16, A_V24, A_D012, A_TEXT, A_ANIM } args_t;

typedef struct {
    const char *name, *label, *group, *hint;
    kind_t kind;
    uint8_t cmd;           // small command byte, extended selector, or anim loop flag
    uint8_t p[3];          // default D0 D1 D2 (K_SHORT) / {mp3,0,0} (K_ANIM)
    args_t args;
    const char *str;       // default payload (K_EXT) or asset name (K_ANIM)
} ctl_t;

#define G_ANIM  "Face animations"
#define G_MEDIA "Screen & media"
#define G_AUDIO "Audio"
#define G_BOOT  "Boot & link"
#define G_FILE  "Files & OTA"
#define G_BEH   "Behaviour & games"

#define S(nm, lbl, grp, c, a, b, d, ar, h)  { nm, lbl, grp, h, K_SHORT, c, {a, b, d}, ar, NULL }
#define X(nm, lbl, grp, sub, def, h)        { nm, lbl, grp, h, K_EXT, sub, {0, 0, 0}, A_TEXT, def }
#define A(nm, lbl, asset, loop, mp3, h)     { nm, lbl, G_ANIM, h, K_ANIM, loop, {mp3, 0, 0}, A_NONE, asset }

static const ctl_t CTL[] = {
    // ---- face animations (ext 0x02 playlist, after STOP_ANIM / ANIM_DONE) ----
    S("anim_stop",         "Stop animation",      G_ANIM, 0x09, 0x00, 0x00, 0x00, A_NONE, "55 AA 09 00: K210 answers ANIM_DONE (08)"),
    A("anim_boot_start",       "Boot start",          "boot_start",        0, 1, "boot face + sound (~4 s)"),
    A("anim_stay_wink_1",      "Wink",                "stay_wink_1",       0, 0, "idle wink"),
    A("anim_stay_a_b",         "Stay A-B",            "stay_a_b",          0, 0, "idle face"),
    A("anim_stay_c_d",         "Stay C-D",            "stay_c_d",          0, 0, "idle face"),
    A("anim_voice_wake_up",    "Wake up",             "voice_wake_up",     0, 1, "stock wake-word face + sound"),
    A("anim_listen_and_think", "Listen & think",      "listen_and_think",  1, 0, "looped while thinking"),
    A("anim_chatgpt_start",    "Chat start",          "chatgpt_start",     0, 0, ""),
    A("anim_chatgpt_hear",     "Chat hear",           "chatgpt_hear",      0, 1, "ready for a follow-up, with sound"),
    A("anim_chatgpt_stay",     "Chat stay",           "chatgpt_stay",      1, 0, "looped while thinking"),
    A("anim_chatgpt_speak",    "Chat speak",          "chatgpt_speak",     0, 0, "stock re-sends it on every ANIM_DONE while talking"),
    A("anim_speak4",           "Speak 4",             "speak4",            0, 0, ""),
    A("anim_speak8",           "Speak 8",             "speak8",            0, 0, ""),
    A("anim_speak10",          "Speak 10",            "speak10",           0, 0, ""),
    A("anim_keep_vigilant1",   "Keep vigilant",       "keep_vigilant1",    0, 0, ""),
    A("anim_emonews_loop1",    "News loop 1",         "EMOnews_loop1",     1, 0, ""),
    A("anim_emonews_loop2",    "News loop 2",         "EMOnews_loop2",     1, 0, ""),
    A("anim_emonews_loop3",    "News loop 3",         "EMOnews_loop3",     1, 0, ""),
    A("anim_emonews_loop4",    "News loop 4",         "EMOnews_loop4",     1, 0, ""),
    A("anim_daily_boxing_start", "Boxing start",      "Daily_Boxing_start", 0, 0, ""),
    A("anim_daily_boxing_loop1", "Boxing loop",       "Daily_Boxing_loop1", 1, 0, ""),
    A("anim_daily_boxing_end",   "Boxing end",        "Daily_Boxing_end",   0, 0, ""),
    { "anim", "Play any animation", G_ANIM, "name = /spiffs/avi/NAME.avi, loop 0/1, mp3=1 adds /spiffs/mp3/NAME_000.mp3",
      K_ANIM_ANY, 0, {0, 0, 0}, A_ANIM, "stay_wink_1" },

    // ---- screen & media ----
    X("play_video",      "Play video",          G_MEDIA, 0x18, "/spiffs/avi/stay_wink_1.avi",          "ext 0x18, play once"),
    X("play_video_loop", "Play video (loop)",   G_MEDIA, 0x1A, "/spiffs/avi/stay_wink_1.avi",          "ext 0x1A, flag=1 variant"),
    X("play_audio",      "Play audio",          G_MEDIA, 0x19, "/spiffs/mp3/voice_wake_up_000.mp3",    "ext 0x19, play once"),
    X("play_audio_loop", "Play audio (loop)",   G_MEDIA, 0x1B, "/spiffs/mp3/voice_wake_up_000.mp3",    "ext 0x1B, flag=1 variant"),
    X("screen_text",     "Show text",           G_MEDIA, 0x44, "Hello from EMO-Bridge",                "ext 0x44, factory-test text screen"),
    S("clock_show",      "Show clock",          G_MEDIA, 0x3E, 0x0C, 0x1E, 0x64, A_D012, "55 AA 3E d0 d1 d2: time fields, d2 = 0x64/0x65"),
    X("alarm_text",      "Alarm list",          G_MEDIA, 0x3D, "07:30 Mon alarm",                      "ext 0x3D, alarm screen text"),
    S("photo_show",      "Show photo",          G_MEDIA, 0x48, 0x01, 0x00, 0x00, A_V16, "55 AA 48 01"),
    S("screen_color_5a", "Screen 0x5A (colour?)", G_MEDIA, 0x5A, 0xFF, 0xFF, 0xFF, A_D012, "3 bytes, screen info / sleep; possibly a colour"),
    S("screen_info_60",  "Screen info 0x60",    G_MEDIA, 0x60, 0x6F, 0x00, 0x00, A_V16, "55 AA 60 6F"),
    S("screen_09",       "Screen 0x09 d0=3/4",  G_MEDIA, 0x09, 0x03, 0x00, 0x00, A_V24, "campfire / Fit / test-screen variants of STOP_ANIM"),
    X("image_search",    "Image search",        G_MEDIA, 0x0E, "cat",                                  "ext 0x0E payload"),
    X("cmd_28_mp3",      "Ext 0x28 \"mp3\"",    G_MEDIA, 0x28, "mp3",                                  "face_id.avi test path"),

    // ---- audio ----
    S("mode_idle",       "Mode: idle (wake word armed)", G_AUDIO, 0x06, 0x01, 0x10, 0x00, A_NONE, "55 AA 06 01 10 00"),
    { "mode_mic", "Mode: mic streaming", G_AUDIO, "4B + 55 AA 06 03 10 00 through the link (keeps the state machine in sync)",
      K_MIC, 0, {0, 0, 0}, A_NONE, NULL },
    S("mode_spk",        "Mode: speaker",       G_AUDIO, 0x06, 0x09, 0x10, 0x00, A_NONE, "55 AA 06 09 10 00; K210 then asks for blocks until mode_idle"),
    S("audio_mode",      "Audio mode (raw)",    G_AUDIO, 0x06, 0x01, 0x10, 0x00, A_V24, "bit field: 01 wake word, 02 mic, 08 speaker"),
    S("volume",          "Volume",              G_AUDIO, 0x17, 0x68, 0x00, 0x00, A_V16, "55 AA 17 v: boot sends 0x68 (104); BT volume handler"),
    S("volume_mute",     "Volume 0",            G_AUDIO, 0x17, 0x00, 0x00, 0x00, A_NONE, "55 AA 17 00"),
    S("volume_max",      "Volume max",          G_AUDIO, 0x17, 0xFF, 0x00, 0x00, A_NONE, "55 AA 17 FF"),
    S("followup_mic",    "Follow-up mic (0x4B)", G_AUDIO, 0x4B, 0x00, 0x00, 0x00, A_NONE, "sent before MODE_MIC on turns without a wake word"),

    // ---- boot & link ----
    S("boot_gate",       "Boot handshake 0x34", G_BOOT, 0x34, 0x00, 0x00, 0x00, A_NONE, "first command after the reset pulse"),
    S("get_version",     "Get version string",  G_BOOT, 0x69, 0x01, 0x00, 0x00, A_NONE, "K210 answers ext 0x69 \"3.1.0\""),
    S("fw_version_number", "Get version number 0x3C", G_BOOT, 0x3C, 0x00, 0x00, 0x00, A_NONE, "K210 answers small 0x3C"),
    S("request_0a",      "Request 0x0A",        G_BOOT, 0x0A, 0x00, 0x00, 0x00, A_NONE, "K210 answers ext 0x0A (painting / charging / shutdown flows)"),
    S("scene",           "Scene / mode 0x38",   G_BOOT, 0x38, 0x01, 0x00, 0x00, A_V16, "1 normal, 4 go-home, 5 games, 6 dialogue"),
    S("home_station_65", "Home station 0x65",   G_BOOT, 0x65, 0xEB, 0x00, 0x00, A_V16, "boot sends EB; also 01, 6F, FF"),
    S("boot_3b",         "Boot 0x3B",           G_BOOT, 0x3B, 0x01, 0x00, 0x00, A_V16, "once in app_main / update"),
    S("boot_70",         "Boot 0x70",           G_BOOT, 0x70, 0x00, 0x00, 0x00, A_V16, "once in app_main"),
    S("boot_6a",         "Boot 0x6A",           G_BOOT, 0x6A, 0x46, 0x00, 0x00, A_V16, "boot sends 46; sleep, DJ mode"),
    S("boot_6c",         "Boot 0x6C",           G_BOOT, 0x6C, 0x00, 0x00, 0x00, A_V16, "boot, test theme, DJ mode"),
    S("boot_74",         "Boot 0x74",           G_BOOT, 0x74, 0x00, 0x00, 0x00, A_V16, "boot, sleep, DJ mode"),
    S("main_6b",         "Main 0x6B",           G_BOOT, 0x6B, 0x01, 0x00, 0x00, A_V16, "app_main"),
    S("main_5d",         "Main 0x5D",           G_BOOT, 0x5D, 0x00, 0x00, 0x00, A_V16, "app_main, variable"),
    S("update_failed",   "OTA failed 0x46",     G_BOOT, 0x46, 0x00, 0x00, 0x00, A_NONE, "app_main when the update failed"),

    // ---- files & OTA ----
    S("file_begin",      "File session begin 0x2C", G_FILE, 0x2C, 0x00, 0x00, 0x00, A_NONE, "wraps every K210 SD read"),
    S("file_end",        "File session end 0x2D",   G_FILE, 0x2D, 0x00, 0x00, 0x00, A_NONE, "pairs with 0x2C"),
    X("read_file",       "Read SD file (avoid)", G_FILE, 0x03, "/spiffs/json/profile.json", "K210 pushes FILE_CHUNKs over SPI that this firmware does not read"),
    X("read_sd_buf",     "Read SD to buffer (avoid)", G_FILE, 0x40, "/spiffs/json/profile.json", "same SPI push, used for photos"),
    S("ota_begin",       "OTA begin 0x23",      G_FILE, 0x23, 0x00, 0x00, 0x00, A_NONE, "ota_init / debug TCP"),
    S("ota_file",        "OTA file step 0x25",  G_FILE, 0x25, 0x00, 0x00, 0x00, A_NONE, "download_file"),
    S("ota_batch",       "OTA batch 0x29",      G_FILE, 0x29, 0x00, 0x00, 0x00, A_NONE, "mot/resource batch"),
    S("ota_error",       "OTA error 0x2A",      G_FILE, 0x2A, 0x00, 0x00, 0x00, A_NONE, ""),
    S("ota_mot",         "OTA mot 0x2B",        G_FILE, 0x2B, 0x00, 0x00, 0x00, A_NONE, ""),
    X("ota_path",        "OTA path 0x26",       G_FILE, 0x26, "/spiffs/mot/test.mot", "path/name buffer"),
    S("k210_update",     "K210 update 0x3A",    G_FILE, 0x3A, 0x00, 0x00, 0x00, A_V16, "k210_update_receive"),
    S("ota_mot_66",      "OTA mot 0x66",        G_FILE, 0x66, 0x00, 0x00, 0x00, A_V16, ""),
    S("ota_run_68",      "OTA / photo 0x68",    G_FILE, 0x68, 0x01, 0x00, 0x00, A_V16, "01 or 65; K210 answers SEND_RUN_OTA"),
    S("photo_png_done",  "Photo PNG done 0x54", G_FILE, 0x54, 0x00, 0x00, 0x00, A_NONE, ""),
    S("photo_done",      "Photo done 0x56",     G_FILE, 0x56, 0x00, 0x00, 0x00, A_NONE, ""),
    S("photo_58",        "Photo 0x58",          G_FILE, 0x58, 0x00, 0x00, 0x00, A_NONE, "debug TCP photo path"),
    X("photo_57",        "Photo data 0x57",     G_FILE, 0x57, "photo", "photo / painting data or name"),

    // ---- behaviour & games ----
    S("cmd_0c",          "Voice task 0x0C",     G_BEH, 0x0C, 0x00, 0x00, 0x00, A_V24, "24-bit parameter"),
    S("showoff_15",      "Show-off 0x15",       G_BEH, 0x15, 0x01, 0x00, 0x00, A_V16, "0 / 1 / 3"),
    S("chat_1c",         "Chat flag 0x1C",      G_BEH, 0x1C, 0x01, 0x00, 0x00, A_V16, "on/off"),
    S("sleep_1f",        "Sleep 0x1F",          G_BEH, 0x1F, 0x01, 0x00, 0x00, A_V16, "on/off"),
    S("blob_detect",     "Colour blob 0x35",    G_BEH, 0x35, 0x00, 0x00, 0x00, A_V16, "d0 = colour; K210 answers ext 0x35 x/w/h"),
    S("voice_36",        "Voice 0x36",          G_BEH, 0x36, 0x01, 0x00, 0x00, A_V16, "on/off"),
    S("test_41",         "Test 0x41",           G_BEH, 0x41, 0x01, 0x00, 0x00, A_V16, "1 / 2 / 3"),
    S("test_42",         "Test 0x42",           G_BEH, 0x42, 0x01, 0x00, 0x00, A_V16, ""),
    S("ble_43",          "BLE / screen 0x43",   G_BEH, 0x43, 0x01, 0x00, 0x00, A_V16, "on/off; BLE connect, compass calibration"),
    S("sleep_4a",        "Sleep 0x4A",          G_BEH, 0x4A, 0x00, 0x00, 0x00, A_V16, ""),
    S("sleep_4d",        "Sleep 0x4D",          G_BEH, 0x4D, 0x00, 0x00, 0x00, A_V16, ""),
    S("sleep_4f",        "Sleep 0x4F",          G_BEH, 0x4F, 0x00, 0x00, 0x00, A_V16, ""),
    S("sleep_50",        "Sleep 0x50",          G_BEH, 0x50, 0x00, 0x00, 0x00, A_V16, ""),
    X("sleep_4c",        "Sleep ext 0x4C",      G_BEH, 0x4C, "0", ""),
    X("sleep_4e",        "Sleep ext 0x4E",      G_BEH, 0x4E, "0", ""),
    S("chat_52",         "Chat 0x52",           G_BEH, 0x52, 0x00, 0x00, 0x00, A_V16, ""),
    S("chat_5b",         "Chat 0x5B",           G_BEH, 0x5B, 0xFF, 0x01, 0x00, A_D012, "pairs seen: 09 02, FF 00, FF 01"),
    S("game_detect_53",  "Game detect 0x53",    G_BEH, 0x53, 0x00, 0x00, 0x00, A_V16, "0 / 1 / 2 / FF; spray-paint, zombie"),
    S("game_59",         "Game 0x59",           G_BEH, 0x59, 0x03, 0x00, 0x00, A_V16, "03 14 28 46 48 49 4B; games, eye stickers, chat"),
    S("fruit_5e",        "Fruit machine 0x5E",  G_BEH, 0x5E, 0x00, 0x00, 0x00, A_D012, "00 00 / 00 02"),
    S("showoff_5f",      "Show-off 0x5F",       G_BEH, 0x5F, 0x00, 0x03, 0x00, A_D012, "00 00 / 00 03 / 01 03"),
    S("daily_routine_61", "Daily routine 0x61", G_BEH, 0x61, 0x00, 0x00, 0x00, A_V16, "0..4; stock sends 61 00 on wake"),
    X("daily_routine_49", "Daily routine descriptor", G_BEH, 0x49, "Daily_Boxing(005+1)", "ext 0x49"),
    S("spray_62",        "Spray paint 0x62",    G_BEH, 0x62, 0x01, 0x00, 0x00, A_V16, "on/off"),
    S("spray_6d",        "Spray paint 0x6D",    G_BEH, 0x6D, 0x01, 0x00, 0x00, A_V16, "on/off"),
    S("voice_67",        "Voice 0x67",          G_BEH, 0x67, 0x00, 0x00, 0x00, A_V16, "00 / 64 / 65 / 66"),
    S("campfire_6e",     "Campfire 0x6E",       G_BEH, 0x6E, 0x01, 0x00, 0x00, A_V16, "on/off"),
    S("campfire_72",     "Campfire / Fit 0x72", G_BEH, 0x72, 0x01, 0x00, 0x00, A_V16, "on/off"),
    S("dice_6f",         "Dice game 0x6F",      G_BEH, 0x6F, 0x01, 0x00, 0x00, A_D012, "d0 step 1..0B, d1/d2 dice or 63"),
    S("fit_timer_73",    "Fit timer 0x73",      G_BEH, 0x73, 0x01, 0x1E, 0x00, A_D012, "00 00 00 / 01 1E 00"),
    X("face_name_51",    "Face name 0x51",      G_BEH, 0x51, "Sam", "\"hi %s.\" face data"),
};
#define N_CTL (sizeof(CTL) / sizeof(CTL[0]))

static const ctl_t *find(const char *name)
{
    for (size_t i = 0; i < N_CTL; i++)
        if (strcmp(CTL[i].name, name) == 0) return &CTL[i];
    return NULL;
}

// Commands that, in the stock firmware, start the OTA / file-download / photo
// flows: the only paths that write to the K210's SD card or update its firmware.
// Sent bare (no SPI data phase) their effect is unknown, so they need "confirm":true.
static const char *const DANGER[] = {
    "ota_begin", "ota_file", "ota_batch", "ota_error", "ota_mot", "ota_path", "k210_update",
    "ota_mot_66", "ota_run_68", "update_failed", "photo_png_done", "photo_done", "photo_58", "photo_57",
    "read_file", "read_sd_buf",
};

static bool is_danger(const char *name)
{
    for (size_t i = 0; i < sizeof(DANGER) / sizeof(DANGER[0]); i++)
        if (strcmp(DANGER[i], name) == 0) return true;
    return false;
}

static void add_arg(cJSON *arr, const char *name, const char *type, int num, const char *str)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "name", name);
    cJSON_AddStringToObject(a, "type", type);
    if (str) cJSON_AddStringToObject(a, "def", str);
    else     cJSON_AddNumberToObject(a, "def", num);
    cJSON_AddItemToArray(arr, a);
}

char *k210_controls_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < N_CTL; i++) {
        const ctl_t *c = &CTL[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", c->name);
        cJSON_AddStringToObject(o, "label", c->label);
        cJSON_AddStringToObject(o, "group", c->group);
        cJSON_AddStringToObject(o, "hint", c->hint);
        if (is_danger(c->name)) {
            cJSON_AddTrueToObject(o, "danger");
#if !CONFIG_EMO_ALLOW_SD_COMMANDS
            cJSON_AddTrueToObject(o, "disabled");
#endif
        }
        cJSON *args = cJSON_AddArrayToObject(o, "args");
        switch (c->args) {
        case A_V16:  add_arg(args, "value", "num", c->p[0] | c->p[1] << 8, NULL); break;
        case A_V24:  add_arg(args, "value", "num", c->p[0] | c->p[1] << 8 | c->p[2] << 16, NULL); break;
        case A_D012: add_arg(args, "d0", "num", c->p[0], NULL);
                     add_arg(args, "d1", "num", c->p[1], NULL);
                     add_arg(args, "d2", "num", c->p[2], NULL); break;
        case A_TEXT: add_arg(args, "text", "str", 0, c->str); break;
        case A_ANIM: add_arg(args, "name", "str", 0, c->str);
                     add_arg(args, "loop", "num", 0, NULL);
                     add_arg(args, "mp3", "num", 0, NULL); break;
        default: break;
        }
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}

static int num_or(const cJSON *req, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, key);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring[0]) return (int)strtol(v->valuestring, NULL, 0);
    return def;
}

static const char *str_or(const cJSON *req, const char *key, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static esp_err_t fail(char *out, size_t n, esp_err_t e, const char *msg)
{
    snprintf(out, n, "{\"ok\":false,\"error\":\"%s\"}", msg);
    return e;
}

esp_err_t k210_controls_run(const cJSON *req, char *out, size_t out_len)
{
    const char *action = str_or(req, "action", NULL);
    if (!action) return fail(out, out_len, ESP_ERR_INVALID_ARG, "missing action");
    const ctl_t *c = find(action);
    if (!c) return fail(out, out_len, ESP_ERR_NOT_FOUND, "unknown action");
#if !CONFIG_EMO_ALLOW_SD_COMMANDS
    if (is_danger(action))
        return fail(out, out_len, ESP_ERR_NOT_SUPPORTED,
                    "OTA / SD-card command: disabled in this build (menuconfig EMO_ALLOW_SD_COMMANDS)");
#endif
    if (is_danger(action) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "confirm")))
        return fail(out, out_len, ESP_ERR_NOT_SUPPORTED,
                    "OTA / SD-card command: may write the K210 SD card or start an update; resend with confirm:true");

    esp_err_t e;
    bool ack = false;
    switch (c->kind) {
    case K_SHORT: {
        uint8_t p[3] = { c->p[0], c->p[1], c->p[2] };
        int v;
        switch (c->args) {
        case A_V16:  v = num_or(req, "value", p[0] | p[1] << 8);
                     p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; break;
        case A_V24:  v = num_or(req, "value", p[0] | p[1] << 8 | p[2] << 16);
                     p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; break;
        case A_D012: p[0] = num_or(req, "d0", p[0]) & 0xFF;
                     p[1] = num_or(req, "d1", p[1]) & 0xFF;
                     p[2] = num_or(req, "d2", p[2]) & 0xFF; break;
        default: break;
        }
        e = k210_send_short(c->cmd, p[0], p[1], p[2]);
        if (e != ESP_OK) break;
        snprintf(out, out_len, "{\"ok\":true,\"sent\":\"55 AA %02X %02X %02X %02X %02X\"}",
                 c->cmd, p[0], p[1], p[2], (uint8_t)(c->cmd + p[0] + p[1] + p[2] - 1));
        return ESP_OK;
    }
    case K_EXT: {
        const char *text = str_or(req, "text", c->str);
        size_t len = strlen(text) + 1;
        e = k210_send_ext(c->cmd, text, &ack);
        if (e != ESP_OK) break;
        snprintf(out, out_len, "{\"ok\":true,\"sent\":\"55 AA FF %02X %02X %02X %02X +%u bytes crc %02X\",\"ack\":%s}",
                 (unsigned)(len & 0xFF), (unsigned)(len >> 8), c->cmd,
                 (uint8_t)(0xFF + (len & 0xFF) + (len >> 8) + c->cmd - 1), (unsigned)len,
                 k210_crc8((const uint8_t *)text, len), ack ? "true" : "false");
        return ESP_OK;
    }
    case K_ANIM:
    case K_ANIM_ANY: {
        const char *name = c->kind == K_ANIM ? c->str : str_or(req, "name", c->str);
        int loop = c->kind == K_ANIM ? c->cmd : num_or(req, "loop", 0);
        bool mp3 = c->kind == K_ANIM ? c->p[0] : num_or(req, "mp3", 0) != 0;
        e = video_play_face(name, loop, mp3, &ack);
        if (e != ESP_OK) break;
        snprintf(out, out_len, "{\"ok\":true,\"sent\":\"STOP_ANIM, PLAY_ANIM %s loop=%d%s\",\"ack\":%s}",
                 name, loop, mp3 ? " +mp3" : "", ack ? "true" : "false");
        return ESP_OK;
    }
    case K_MIC:
        e = microphone_open();
        if (e != ESP_OK) break;
        snprintf(out, out_len, "{\"ok\":true,\"sent\":\"55 AA 4B 00 00 00 4A, 55 AA 06 03 10 00 18\"}");
        return ESP_OK;
    default:
        e = ESP_ERR_INVALID_ARG;
    }
    ESP_LOGW(TAG, "%s: %s", action, esp_err_to_name(e));
    return fail(out, out_len, e,
                e == ESP_ERR_INVALID_STATE ? "K210 busy (booting or speaking)" :
                e == ESP_ERR_INVALID_SIZE  ? "payload too long (max 254 chars)" :
                e == ESP_ERR_INVALID_ARG   ? "bad argument (anim name: letters, digits, _ -)" : esp_err_to_name(e));
}
