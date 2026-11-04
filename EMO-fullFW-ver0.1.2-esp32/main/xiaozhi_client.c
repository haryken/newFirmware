// xiaozhi_client.c — see xiaozhi_client.h.
#include "xiaozhi_client.h"
#include "settings.h"
#include "net.h"
#include "microphone.h"
#include "speaker.h"
#include "video.h"
#include "legs.h"
#include "k210_controls.h"
#include "tricks.h"
#include "behavior.h"
#include "k210_link.h"
#include "k210_uart.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_transport_ws.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "opus.h"

static const char *TAG = "xiaozhi";

#define OPUS_FRAME_MS       60
#define OPUS_SR_MIC         16000
#define OPUS_FRAME_SAMP     (OPUS_SR_MIC * OPUS_FRAME_MS / 1000)   // 960
#define OPUS_MAX_PACKET     1500
#define PCM_ACCUM_CAP       (OPUS_FRAME_SAMP * 2)
#define WS_RX_BUF           2048
#define IDLE_CLOSE_MS       30000   // no server reply for this long -> back to idle (speak4)
#define HELLO_TIMEOUT_MS    10000

static SemaphoreHandle_t s_lock;
static esp_websocket_client_handle_t s_ws;
static OpusEncoder *s_enc;
static OpusDecoder *s_dec;
static int s_dec_sr = 24000;

static char s_device_id[18];     // aa:bb:cc:dd:ee:ff
static char s_client_id[40];    // uuid
static char s_ws_url[160];
static char s_token[96];
static char s_session_id[64];
static char s_activation[16];
static char s_shown_code[16];   // last code drawn on face — don't redraw every OTA poll
static bool s_act_on_face;      // face is currently the pair-code text
static bool s_act_dismissed;    // user said already paired / hide code

static volatile bool s_hello_ok;
static volatile bool s_listening;
static volatile bool s_speaking;
static volatile bool s_session;
static volatile bool s_ws_self_close;       // close_ws() in progress: ignore its events
static volatile bool s_recover_pending;     // one post-TTS recover in flight
static volatile bool s_flush_req;           // xz_sess asks xz_enc to flush tail PCM
static volatile int64_t s_last_act_us;
static volatile uint32_t s_followup_epoch;   // bumps on every tts start — cancels stale follow-ups
static volatile uint32_t s_odec_pending;     // TTS opus chunks waiting to play

static int16_t s_pcm_acc[PCM_ACCUM_CAP];
static size_t  s_pcm_n;
static uint8_t s_opus_pkt[OPUS_MAX_PACKET];

static uint8_t *s_frag;
static size_t   s_frag_cap;
static int      s_frag_opcode;

static xiaozhi_stats_t s_st;

typedef enum {
    XZ_CMD_BEGIN    = 1,
    XZ_CMD_END      = 2,
    XZ_CMD_PREOPEN  = 3,
    XZ_CMD_FOLLOWUP = 4,
    XZ_CMD_RECOVER  = 5,
    XZ_CMD_HOME     = 6,    // ws lost / idle too long -> mic IDLE + "speak4" face
    XZ_CMD_SKILL_HOME = 7,  // a robot skill (sing/dance/...) was started: end the conversation quietly
} xz_cmd_t;

static volatile bool s_home_quiet;   // go_home_work: do not cover a playing skill with speak4

static QueueHandle_t s_cmd_q;

// Mic arrives on k210_link. Never opus_encode there — ring in PSRAM.
#define PCM_Q_SAMP   512
#define PCM_RING_N   16
typedef struct {
    uint16_t n;
    int16_t  s[PCM_Q_SAMP];
} pcm_chunk_t;
static pcm_chunk_t *s_pcm_ring;          // SPIRAM
static volatile uint8_t s_pcm_w, s_pcm_r;
static SemaphoreHandle_t s_pcm_sem;      // counts available chunks

// TTS Opus + face text — never decode/draw on websocket_task (8 KB).
#define ODEC_MAX  1500
#define ODEC_N    10
typedef struct {
    uint16_t n;
    uint8_t  d[ODEC_MAX];
} odec_chunk_t;
static odec_chunk_t *s_odec_ring;
static volatile uint8_t s_odec_w, s_odec_r;
static SemaphoreHandle_t s_odec_sem;
static char s_face_text[96];
static volatile bool s_face_pending;
static volatile int s_want_dec_sr;

static void frag_reset(void)
{
    // keep allocation; just mark unused via opcode
    s_frag_opcode = -1;
}

static bool frag_append(esp_websocket_event_data_t *ev)
{
    if (ev->payload_offset == 0) {
        s_frag_opcode = ev->op_code;
        if ((size_t)ev->payload_len + 1 > s_frag_cap) {
            size_t ncap = (size_t)ev->payload_len + 1;
            if (ncap < 2048) ncap = 2048;
            uint8_t *n = realloc(s_frag, ncap);
            if (!n) return false;
            s_frag = n;
            s_frag_cap = ncap;
        }
    }
    if (!s_frag || ev->payload_offset + ev->data_len > s_frag_cap) return false;
    if (ev->data_len > 0 && ev->data_ptr)
        memcpy(s_frag + ev->payload_offset, ev->data_ptr, (size_t)ev->data_len);
    return (ev->payload_offset + ev->data_len) >= ev->payload_len;
}

static void touch_act(void) { s_last_act_us = esp_timer_get_time(); }

// True only while the socket is REALLY usable. A server-initiated close
// (idle timeout) ends in WEBSOCKET_EVENT_CLOSED, not DISCONNECTED, so the
// s_hello_ok flag alone goes stale and the session keeps feeding a dead socket.
static bool ws_alive(void)
{
    return s_ws && s_hello_ok && esp_websocket_client_is_connected(s_ws);
}

static void queue_home(void)
{
    if (!s_cmd_q) return;
    xz_cmd_t cmd = XZ_CMD_HOME;
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void ensure_ids(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t h;
    size_t len = sizeof(s_client_id);
    if (nvs_open("emo", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "xz_uuid", s_client_id, &len) != ESP_OK) s_client_id[0] = 0;
        nvs_close(h);
    }
    if (!s_client_id[0]) {
        uint32_t a = esp_random(), b = esp_random(), c = esp_random(), d = esp_random();
        snprintf(s_client_id, sizeof(s_client_id),
                 "%08" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "%08" PRIx32,
                 a, (b >> 16) & 0xffffu, (0x4000u | ((b >> 4) & 0x0fffu)),
                 (0x8000u | (c & 0x3fffu)), (c >> 16) & 0xffffu, d);
        if (nvs_open("emo", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "xz_uuid", s_client_id);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

static bool voice_is_xz(void)
{
    emo_settings_t st; settings_get_copy(&st);
    return st.voice_mode[0] && strcmp(st.voice_mode, "xiaozhi") == 0;
}

bool xiaozhi_enabled(void) { return voice_is_xz(); }
bool xiaozhi_ready(void) { return s_ws_url[0] != 0; }
bool xiaozhi_connected(void) { return s_ws && s_hello_ok; }
bool xiaozhi_in_session(void) { return s_session; }

void xiaozhi_get_stats(xiaozhi_stats_t *out)
{
    if (!out) return;
    ensure_ids();
    *out = s_st;
    out->enabled = voice_is_xz();
    out->ready = s_ws_url[0] != 0;
    out->connected = s_ws && s_hello_ok;
    out->session = s_session;
    out->speaking = s_speaking;
    out->listening = s_listening;
    strncpy(out->activation, s_activation, sizeof(out->activation) - 1);
    strncpy(out->device_id, s_device_id, sizeof(out->device_id) - 1);
    strncpy(out->client_id, s_client_id, sizeof(out->client_id) - 1);
    strncpy(out->ws_url, s_ws_url, sizeof(out->ws_url) - 1);
}

static void clear_activation_face(void)
{
    if (!s_act_on_face) return;
    bool ack = false;
    // Replace the stuck text screen with a normal idle face.
    if (video_play_face("stay_1", 1, false, &ack) != ESP_OK) {
        video_show_text(" ", &ack);   // blank text as last resort
    }
    s_act_on_face = false;
    ESP_LOGI(TAG, "activation face cleared");
}

static void show_activation(const char *code)
{
    if (!code || !code[0]) return;
    if (s_act_dismissed && strcmp(s_shown_code, code) == 0) return;
    // Same code already on screen — OTA poll must not spam the face.
    if (s_act_on_face && strcmp(s_shown_code, code) == 0) return;

    char line[48];
    snprintf(line, sizeof(line), "Xiaozhi code\n%s", code);
    bool ack = false;
    video_show_text(line, &ack);
    strncpy(s_shown_code, code, sizeof(s_shown_code) - 1);
    s_act_on_face = true;
    s_act_dismissed = false;
    ESP_LOGW(TAG, "activation code: %s (enter at xiaozhi.me / your server console)", code);
}

void xiaozhi_dismiss_activation(void)
{
    s_act_dismissed = true;
    s_activation[0] = 0;
    clear_activation_face();
    ESP_LOGI(TAG, "activation dismissed by user");
}

static void ws_send_text(const char *json)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(2000));
}

static void send_listen(const char *state)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"}",
             s_session_id, state);
    ws_send_text(buf);
}

static void send_abort(void)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"wake_word_detected\"}",
             s_session_id);
    ws_send_text(buf);
}

static int s_vol = 104;   // last set volume (stock boot default ~104)

// Easy stock-like tricks: .mot (+ face/music). No RE / behavior engine needed.
static const char *const k_dances[] = {
    "d1_EmoDance", "d2_WontLetGo", "d3_Blindless", "d4_Click1", "d5_TimeOfMyLife",
    "d6_Rollercoaster", "d7_FlashBack", "d8_Click2", "d9_BlameYourself",
    "d10_CanITakeYouThere", "d11_OceanBlue", "Christmas_song",
};
static const char *const k_looks[] = {
    "look_around_1", "look_around_2", "look_around_3", "look_around_4",
    "look_around_5", "look_around_6", "look_around_7", "look_around_8",
};
static const char *const k_greet_hi[] = { "Hi", "Good_morning", "Good_afternoon" };
static const char *const k_greet_bye[] = { "Byebye", "Good_night" };
static const char *const k_poses[] = {
    "show_basketball", "show_tennis", "show_painting", "show_makeup",
    "show_light", "show_facemask", "Group_photo_gesture_1",
};
static const char *const k_animals[] = {
    "Pig", "Sheep", "Snake", "Tiger", "Wolf", "Cat", "Cattle", "Chicken",
    "Cicada", "Cuckoo", "Dog", "Duck", "Elephant", "Frog", "Horse", "Fox",
};

static const char *pick_str(const char *const *arr, size_t n)
{
    if (!n) return NULL;
    return arr[esp_random() % n];
}

static bool play_mot(const char *name, char *msg, size_t msg_len)
{
    if (!name || !name[0]) {
        if (msg && msg_len) snprintf(msg, msg_len, "no motion name");
        return false;
    }
    legs_opts_t opts = LEGS_OPTS_DEFAULT;
    opts.media = true;
    if (legs_play(name, opts) != ESP_OK) {
        if (msg && msg_len) snprintf(msg, msg_len, "play failed: %s", name);
        return false;
    }
    if (msg && msg_len) snprintf(msg, msg_len, "playing %s", name);
    return true;
}

static bool set_volume_raw(int vol, char *msg, size_t msg_len)
{
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "action", "volume");
    cJSON_AddNumberToObject(req, "value", vol);
    char out[128];
    bool ok = (k210_controls_run(req, out, sizeof(out)) == ESP_OK);
    cJSON_Delete(req);
    if (ok) s_vol = vol;
    if (msg && msg_len) {
        if (ok) snprintf(msg, msg_len, "volume %d", vol);
        else snprintf(msg, msg_len, "volume failed");
    }
    return ok;
}

static bool play_face_named(const char *face, char *msg, size_t msg_len)
{
    if (!face || !face[0]) return false;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "action", "anim");
    cJSON_AddStringToObject(req, "name", face);
    cJSON_AddNumberToObject(req, "loop", 0);
    cJSON_AddNumberToObject(req, "mp3", 0);
    char out[160];
    bool ok = (k210_controls_run(req, out, sizeof(out)) == ESP_OK);
    cJSON_Delete(req);
    if (msg && msg_len) snprintf(msg, msg_len, ok ? "face %s" : "face failed", face);
    return ok;
}

static volatile bool s_mcp_local;   // running a deferred skill locally: nothing goes to the server
static char s_pend_name[48];
static char s_pend_args[200];
static volatile bool s_pend;

static void mcp_reply(cJSON *id, bool ok, const char *text)
{
    if (s_mcp_local) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", s_session_id);
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(payload, "id", cJSON_Duplicate(id, 1));
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : (ok ? "true" : "false"));
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", !ok);
    cJSON_AddItemToObject(payload, "result", result);
    cJSON_AddItemToObject(root, "payload", payload);
    char *s = cJSON_PrintUnformatted(root);
    if (s) { ws_send_text(s); free(s); }
    cJSON_Delete(root);
}

static void mcp_tools_list(cJSON *id)
{
    // Easy Living.AI-like phrases → .mot / gait / volume. Hard ones (go home, photo, games) deferred.
    static const char *tools =
        "["
        "{\"name\":\"self.robot.walk\",\"description\":\"Walk forward or backward (stock move).\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"cycles\":{\"type\":\"number\",\"description\":\"gait cycles, default 2\"},"
        "\"reverse\":{\"type\":\"boolean\",\"description\":\"true = backward\"}}}},"
        "{\"name\":\"self.robot.turn\",\"description\":\"Turn in place (left if reverse=false).\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"cycles\":{\"type\":\"number\"},\"reverse\":{\"type\":\"boolean\"}}}},"
        "{\"name\":\"self.robot.sing_a_song\",\"description\":\"Sing a song: robot holds a microphone and sings (NOT a dance; use dance for that).\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"which\":{\"type\":\"string\",\"description\":\"optional sings1_1..sings1_5 or sings2_1..sings2_4\"}}}},"
        "{\"name\":\"self.robot.dance\",\"description\":\"Dance with music (stock: Dance / Let's dance).\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"which\":{\"type\":\"string\",\"description\":\"optional d1_EmoDance..d11_OceanBlue\"}}}},"
        "{\"name\":\"self.robot.look_around\",\"description\":\"Look around idle motion.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.robot.greet\",\"description\":\"Greeting motion: hi / bye / good morning style.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":\"string\",\"description\":\"hi|bye|morning|night\"}}}},"
        "{\"name\":\"self.robot.act_like\",\"description\":\"Act like an animal (stock: Act like a pig).\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"animal\":{\"type\":\"string\",\"description\":\"pig,dog,cat,duck,...\"}},\"required\":[\"animal\"]}},"
        "{\"name\":\"self.robot.laser_eyes\",\"description\":\"Laser eyes show (stock: Laser eyes).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.robot.pose\",\"description\":\"Do a random show-off pose (optional name). NOT for 'hands up' - use self.robot.hands_up for that.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"which\":{\"type\":\"string\"}}}},"
        "{\"name\":\"self.robot.play_motion\",\"description\":\"Play any .mot by exact name (582 on SPIFFS).\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}},"
        "{\"name\":\"self.robot.play_face\",\"description\":\"Show a K210 face animation by asset name.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"e.g. stay_wink_1\"}},\"required\":[\"name\"]}},"
        "{\"name\":\"self.robot.stop\",\"description\":\"Stop motion and tricks (camping/explore/game).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.robot.camping\",\"description\":\"Go camping (cam trai): tent, fishing, barbecue, frisbee, campfire. Optional activity.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"activity\":{\"type\":\"string\",\"description\":\"optional: tent|fishing|barbecue|frisbee|campfire (default: all)\"}}}},"
        "{\"name\":\"self.robot.fireworks\",\"description\":\"Watch / set off fireworks (phao hoa, ban phao hoa).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.robot.spray_paint\",\"description\":\"Spray paint game (phun son, son tuong).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.robot.roll_dice\",\"description\":\"Roll a dice (xuc xac, do xuc xac, tung xuc xac).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.robot.fight\",\"description\":\"Fight with a weapon (danh nhau, vu khi): boomerang, radish knife, slingshot, water bubble gun.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"weapon\":{\"type\":\"string\",\"description\":\"optional: boomerang|radish|slingshot|bubble\"}}}},"
        "{\"name\":\"self.robot.exercise\",\"description\":\"Workout / fitness with the robot (tap the duc, tap gym): warm-up, aerobic, core, power, relax.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.robot.drive\",\"description\":\"Drive a vehicle (lai xe): bicycle, car or ship.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"vehicle\":{\"type\":\"string\",\"description\":\"optional: bicycle|car|ship\"}}}},"
        "{\"name\":\"self.robot.play_instruments\",\"description\":\"Play musical instruments (choi nhac cu, dan, hoa tau cung nhau / ensemble): drum, maraca, piano, tambourine, trumpet. Without instrument = play together.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"instrument\":{\"type\":\"string\",\"description\":\"optional: drum|maraca|piano|tambourine|trumpet\"}}}},"
        "{\"name\":\"self.robot.timer\",\"description\":\"Local countdown timer then alarm motion.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"number\",\"description\":\"1..1800\"}},\"required\":[\"seconds\"]}},"
        "{\"name\":\"self.robot.explore\",\"description\":\"Gated explore walk (needs cliff sensor; not while docked).\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"number\",\"description\":\"5..120, default 20\"}}}},"
        "{\"name\":\"self.robot.hands_up\",\"description\":\"Use when the user says 'hands up', 'put your hands up', 'don't move', 'shoot', 'bang', or plays a gun game (gio tay len / ban sung): robot raises its hands, then dodges or gets shot.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.robot.play_game\",\"description\":\"Anim demo game: rps|ttt|zombie|fix_bugs (not full CV). Other activities have their own tools.\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"game\":{\"type\":\"string\"}},\"required\":[\"game\"]}},"
        "{\"name\":\"self.audio_speaker.set_volume\",\"description\":\"Set speaker volume 0-255 (stock ~104).\",\"inputSchema\":"
        "{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"number\"}},\"required\":[\"volume\"]}},"
        "{\"name\":\"self.audio_speaker.volume_up\",\"description\":\"Volume up (stock: Volume up).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.audio_speaker.volume_down\",\"description\":\"Volume down (stock: Volume down).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"self.audio_speaker.mute\",\"description\":\"Mute (stock: Be quiet).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
        "]";

    // Built as plain text (no cJSON tree): ~3 KB of tools would cost far more heap.
    size_t cap = strlen(tools) + 192;
    char *s = malloc(cap);
    if (!s) { ESP_LOGE(TAG, "tools/list: no heap"); return; }
    snprintf(s, cap,
             "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
             "\"result\":{\"tools\":%s}}}",
             s_session_id, (id && cJSON_IsNumber(id)) ? id->valueint : 0, tools);
    ws_send_text(s);
    ESP_LOGI(TAG, "mcp tools/list sent (%u bytes)", (unsigned)strlen(s));
    free(s);
}

static void mcp_initialize(cJSON *id)
{
    char s[320];
    snprintf(s, sizeof(s),
             "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
             "\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
             "\"serverInfo\":{\"name\":\"emo-bridge\",\"version\":\"0.1.2\"}}}}",
             s_session_id, (id && cJSON_IsNumber(id)) ? id->valueint : 0);
    ws_send_text(s);
    ESP_LOGI(TAG, "mcp initialize answered");
}
static void mcp_call(cJSON *id, const char *name, cJSON *args)
{
    if (!name) { mcp_reply(id, false, "no tool name"); return; }
    ESP_LOGI(TAG, "mcp tools/call %s", name);

    // Robot skills run only AFTER the conversation is over: Xiaozhi finishes
    // speaking, the ws is closed, and only then the motion + music start, so
    // the TTS cannot talk over the song.
    if (!s_mcp_local && s_cmd_q && !strncmp(name, "self.robot.", 11) &&
        strcmp(name, "self.robot.stop") && strcmp(name, "self.robot.play_face") &&
        strcmp(name, "self.robot.timer")) {
        snprintf(s_pend_name, sizeof(s_pend_name), "%s", name);
        s_pend_args[0] = 0;
        if (args) {
            char *as = cJSON_PrintUnformatted(args);
            if (as) { snprintf(s_pend_args, sizeof(s_pend_args), "%s", as); free(as); }
        }
        s_pend = true;
        mcp_reply(id, true, "ok, the robot will do it as soon as this chat ends");
        xz_cmd_t c = XZ_CMD_SKILL_HOME;
        xQueueSend(s_cmd_q, &c, 0);
        ESP_LOGI(TAG, "skill %s deferred until the chat is over", name);
        return;
    }
    bool ok = false;
    char msg[96] = "ok";

    if (!strcmp(name, "self.robot.walk") || !strcmp(name, "self.robot.turn")) {
        bool turn = !strcmp(name, "self.robot.turn");
        bool rev = false;
        float cycles = 2.0f;
        if (args) {
            cJSON *r = cJSON_GetObjectItem(args, "reverse");
            if (cJSON_IsTrue(r) || (cJSON_IsNumber(r) && r->valueint)) rev = true;
            cJSON *c = cJSON_GetObjectItem(args, "cycles");
            if (cJSON_IsNumber(c) && c->valuedouble > 0) cycles = (float)c->valuedouble;
        }
        legs_gait_t g;
        legs_gait_preset(turn, rev, &g);
        g.cycles = cycles;
        ok = (legs_gait(&g) == ESP_OK);
        if (!ok) strcpy(msg, "gait failed");
        else snprintf(msg, sizeof(msg), "%s cycles=%.1f%s", turn ? "turn" : "walk", (double)cycles, rev ? " rev" : "");
    } else if (!strcmp(name, "self.robot.sing_a_song")) {
        const char *n = NULL;
        if (args) {
            cJSON *w = cJSON_GetObjectItem(args, "which");
            if (cJSON_IsString(w) && w->valuestring[0]) n = w->valuestring;
        }
        // Stock "Sing a song" (profile.json): Singer_ready (mic pick-up, 3 s), then a random
        // song = sings<g>_1.avi (singing face) + /spiffs/mp3/<song>.mp3 on the K210, while a
        // sings*.mot moves the body (the mot is shared: song k -> mot ((k-1) % n) + 1).
        static const struct { uint8_t g, k; const char *mp3; } k_songs[] = {
    { 1, 1, "sings1_1_ballroom" },
    { 1, 2, "sings1_2_dance_monkey" },
    { 1, 3, "sings1_3_guan_lan_gao_shou" },
    { 1, 4, "sings1_4_love_the_way_you_lie" },
    { 1, 5, "sings1_5_shape_of_you" },
    { 1, 6, "sings1_6_wallerman" },
    { 1, 7, "sings1_7_foxsay" },
    { 1, 8, "sings1_8_bao_ke_meng" },
    { 1, 9, "sings1_9_fall_in_love_for_the_night" },
    { 1, 10, "sings1_10_sad" },
    { 1, 11, "sings1_11_cant_stop_the_feeling" },
    { 1, 12, "sings1_12_closer" },
    { 1, 13, "sings1_13_sunflower" },
    { 1, 14, "sings1_14_APT" },
    { 1, 15, "sings1_15_Moves_Like_Jagger" },
    { 1, 16, "sings1_16_Beauty_and_a_Beat" },
    { 2, 1, "sings2_1_baby" },
    { 2, 2, "sings2_2_huan_le_song" },
    { 2, 3, "sings2_3_jingle_bell" },
    { 2, 4, "sings2_4_lian_ai_xun_huan" },
    { 2, 5, "sings2_5_Orpheus_in_the_Underworld" },
    { 2, 6, "sings2_6_tom" },
    { 2, 7, "sings2_7_tuziwu" },
    { 2, 8, "sings2_8_duo_la_A_meng1" },
    { 2, 9, "sings2_9_duo_la_A_meng2" },
    { 2, 10, "sings2_10_shi_jie_bei_1998" },
    { 2, 11, "sings2_11_see_you_again" },
    { 2, 12, "sings2_12_California_Gurls" },
    { 2, 13, "sings2_13_Jiggle" },
        };
        int si = (int)(esp_random() % (sizeof(k_songs) / sizeof(k_songs[0])));
        if (n) {                                              // which = "sings1_5" etc.
            int g = 0, k = 0;
            if (sscanf(n, "sings%d_%d", &g, &k) == 2)
                for (size_t q = 0; q < sizeof(k_songs) / sizeof(k_songs[0]); q++)
                    if (k_songs[q].g == g && k_songs[q].k == k) si = (int)q;
        }
        int sg = k_songs[si].g, sk = k_songs[si].k;
        char motn[16], avin[16];
        snprintf(motn, sizeof(motn), "sings%d_%d", sg, ((sk - 1) % (sg == 1 ? 5 : 4)) + 1);
        snprintf(avin, sizeof(avin), "sings%d_1", sg);
        bool sack = false;
        video_play_face("Singer_ready", 0, true, &sack);
        vTaskDelay(pdMS_TO_TICKS(3000));                       // mic pick-up clip
        legs_opts_t so = LEGS_OPTS_DEFAULT;
        so.media = false;
        ok = (legs_play(motn, so) == ESP_OK);
        video_play_playlist(avin, k_songs[si].mp3, &sack);
        if (ok) snprintf(msg, sizeof(msg), "singing %s", k_songs[si].mp3);
        else snprintf(msg, sizeof(msg), "play failed: %s", motn);
    } else if (!strcmp(name, "self.robot.dance")) {
        const char *n = NULL;
        if (args) {
            cJSON *w = cJSON_GetObjectItem(args, "which");
            if (cJSON_IsString(w) && w->valuestring[0]) n = w->valuestring;
        }
        if (!n) n = pick_str(k_dances, sizeof(k_dances) / sizeof(k_dances[0]));
        ok = play_mot(n, msg, sizeof(msg));
    } else if (!strcmp(name, "self.robot.look_around")) {
        ok = play_mot(pick_str(k_looks, sizeof(k_looks) / sizeof(k_looks[0])), msg, sizeof(msg));
    } else if (!strcmp(name, "self.robot.greet")) {
        const char *kind = "hi";
        if (args) {
            cJSON *k = cJSON_GetObjectItem(args, "kind");
            if (cJSON_IsString(k) && k->valuestring[0]) kind = k->valuestring;
        }
        const char *n;
        if (!strcmp(kind, "bye") || !strcmp(kind, "goodbye"))
            n = pick_str(k_greet_bye, sizeof(k_greet_bye) / sizeof(k_greet_bye[0]));
        else if (!strcmp(kind, "morning")) n = "Good_morning";
        else if (!strcmp(kind, "afternoon")) n = "Good_afternoon";
        else if (!strcmp(kind, "night") || !strcmp(kind, "evening")) n = "Good_night";
        else n = pick_str(k_greet_hi, sizeof(k_greet_hi) / sizeof(k_greet_hi[0]));
        ok = play_mot(n, msg, sizeof(msg));
    } else if (!strcmp(name, "self.robot.act_like")) {
        const char *animal = NULL;
        if (args) {
            cJSON *a = cJSON_GetObjectItem(args, "animal");
            if (cJSON_IsString(a)) animal = a->valuestring;
        }
        if (!animal || !animal[0]) { mcp_reply(id, false, "animal required"); return; }
        // Match SPIFFS names (Pig, Dog, Cattle for cow…).
        char want[24];
        snprintf(want, sizeof(want), "%s", animal);
        if (want[0] >= 'a' && want[0] <= 'z') want[0] = (char)(want[0] - 'a' + 'A');
        for (char *p = want + 1; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        if (!strcasecmp(want, "Cow")) strcpy(want, "Cattle");
        const char *n = NULL;
        for (size_t i = 0; i < sizeof(k_animals) / sizeof(k_animals[0]); i++) {
            if (!strcasecmp(k_animals[i], want) || !strcasecmp(k_animals[i], animal)) {
                n = k_animals[i];
                break;
            }
        }
        if (!n) n = want;
        ok = play_mot(n, msg, sizeof(msg));
    } else if (!strcmp(name, "self.robot.laser_eyes")) {
        ok = play_mot((esp_random() & 1) ? "laser_eye" : "laser_eye_2", msg, sizeof(msg));
    } else if (!strcmp(name, "self.robot.pose")) {
        const char *n = NULL;
        if (args) {
            cJSON *w = cJSON_GetObjectItem(args, "which");
            if (cJSON_IsString(w) && w->valuestring[0]) n = w->valuestring;
        }
        if (!n) n = pick_str(k_poses, sizeof(k_poses) / sizeof(k_poses[0]));
        if (!strncmp(n, "hand", 4) || strstr(n, "gun") || strstr(n, "hands")) {   // model asked for 'hands_up' as a pose
            ok = (tricks_game_start("gun") == ESP_OK);
            snprintf(msg, sizeof(msg), ok ? "hands up game" : "game busy/failed");
        } else {
            ok = play_mot(n, msg, sizeof(msg));
            if (!ok) ok = play_mot(pick_str(k_poses, sizeof(k_poses) / sizeof(k_poses[0])), msg, sizeof(msg));
        }
    } else if (!strcmp(name, "self.robot.play_motion")) {
        const char *n = NULL;
        if (args) {
            cJSON *jn = cJSON_GetObjectItem(args, "name");
            if (cJSON_IsString(jn)) n = jn->valuestring;
        }
        if (!n || !n[0]) { mcp_reply(id, false, "name required"); return; }
        ok = play_mot(n, msg, sizeof(msg));
    } else if (!strcmp(name, "self.robot.play_face")) {
        const char *n = NULL;
        if (args) {
            cJSON *jn = cJSON_GetObjectItem(args, "name");
            if (cJSON_IsString(jn)) n = jn->valuestring;
        }
        if (!n || !n[0]) { mcp_reply(id, false, "name required"); return; }
        ok = play_face_named(n, msg, sizeof(msg));
    } else if (!strcmp(name, "self.robot.stop")) {
        tricks_stop();
        legs_stop();
        ok = true;
        strcpy(msg, "stopped");
    } else if (!strcmp(name, "self.robot.camping") || !strcmp(name, "self.robot.fireworks") ||
               !strcmp(name, "self.robot.spray_paint") || !strcmp(name, "self.robot.roll_dice") ||
               !strcmp(name, "self.robot.fight") || !strcmp(name, "self.robot.exercise") ||
               !strcmp(name, "self.robot.drive") || !strcmp(name, "self.robot.play_instruments")) {
        // stock profile "shows" (tricks.c show_*): optional free-text sub-choice after ':'
        static const struct { const char *tool, *show, *key; } k_shows[] = {
            { "self.robot.camping", "camp", "activity" }, { "self.robot.fireworks", "fireworks", NULL },
            { "self.robot.spray_paint", "spray", NULL }, { "self.robot.roll_dice", "dice", NULL },
            { "self.robot.fight", "fight", "weapon" }, { "self.robot.exercise", "fit", NULL },
            { "self.robot.drive", "drive", "vehicle" }, { "self.robot.play_instruments", "band", "instrument" },
        };
        char game[32] = "";
        for (size_t i = 0; i < sizeof(k_shows) / sizeof(k_shows[0]); i++) {
            if (strcmp(name, k_shows[i].tool)) continue;
            const char *sub = NULL;
            if (args && k_shows[i].key) {
                cJSON *a = cJSON_GetObjectItem(args, k_shows[i].key);
                if (cJSON_IsString(a) && a->valuestring[0]) sub = a->valuestring;
            }
            if (sub) snprintf(game, sizeof(game), "%s:%.14s", k_shows[i].show, sub);
            else snprintf(game, sizeof(game), "%s", k_shows[i].show);
            for (char *c = game; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        }
        ok = (tricks_game_start(game) == ESP_OK);
        snprintf(msg, sizeof(msg), ok ? "started %.60s" : "busy/failed %.60s", game);
    } else if (!strcmp(name, "self.robot.timer")) {
        double sec = 0;
        if (args) {
            cJSON *s = cJSON_GetObjectItem(args, "seconds");
            if (cJSON_IsNumber(s)) sec = s->valuedouble;
        }
        if (sec < 1) { mcp_reply(id, false, "seconds required"); return; }
        ok = (tricks_timer_start((uint32_t)(sec * 1000.0)) == ESP_OK);
        snprintf(msg, sizeof(msg), ok ? "timer %.0fs" : "timer failed", sec);
    } else if (!strcmp(name, "self.robot.explore")) {
        uint32_t sec = 20;
        if (args) {
            cJSON *s = cJSON_GetObjectItem(args, "seconds");
            if (cJSON_IsNumber(s) && s->valuedouble >= 5) sec = (uint32_t)s->valuedouble;
        }
        esp_err_t e = tricks_explore_start(sec);
        ok = (e == ESP_OK);
        if (e == ESP_ERR_NOT_SUPPORTED) strcpy(msg, "explore needs cliff FOOT");
        else if (e == ESP_ERR_INVALID_STATE) strcpy(msg, "explore busy or docked");
        else if (ok) snprintf(msg, sizeof(msg), "explore %us", (unsigned)sec);
        else strcpy(msg, "explore failed");
    } else if (!strcmp(name, "self.robot.hands_up")) {
        ok = (tricks_game_start("gun") == ESP_OK);
        snprintf(msg, sizeof(msg), ok ? "hands up game" : "game busy/failed");
    } else if (!strcmp(name, "self.robot.play_game")) {
        const char *g = NULL;
        if (args) {
            cJSON *jg = cJSON_GetObjectItem(args, "game");
            if (cJSON_IsString(jg)) g = jg->valuestring;
        }
        if (!g) { mcp_reply(id, false, "game required: rps|ttt|zombie|fix_bugs"); return; }
        ok = (tricks_game_start(g) == ESP_OK);
        snprintf(msg, sizeof(msg), ok ? "game %s" : "game failed", g);
    } else if (!strcmp(name, "self.audio_speaker.set_volume")) {
        int vol = s_vol;
        if (args) {
            cJSON *v = cJSON_GetObjectItem(args, "volume");
            if (cJSON_IsNumber(v)) vol = v->valueint;
        }
        ok = set_volume_raw(vol, msg, sizeof(msg));
    } else if (!strcmp(name, "self.audio_speaker.volume_up")) {
        ok = set_volume_raw(s_vol + 32, msg, sizeof(msg));
    } else if (!strcmp(name, "self.audio_speaker.volume_down")) {
        ok = set_volume_raw(s_vol - 32, msg, sizeof(msg));
    } else if (!strcmp(name, "self.audio_speaker.mute")) {
        ok = set_volume_raw(0, msg, sizeof(msg));
    } else {
        mcp_reply(id, false, "unknown tool");
        return;
    }
    ESP_LOGI(TAG, "mcp %s -> ok=%d '%s' (heap=%u)", name, (int)ok, msg, (unsigned)esp_get_free_internal_heap_size());
    mcp_reply(id, ok, msg);
}

static void handle_mcp(cJSON *payload)
{
    if (!cJSON_IsObject(payload)) return;
    cJSON *method = cJSON_GetObjectItem(payload, "method");
    cJSON *id = cJSON_GetObjectItem(payload, "id");
    if (!cJSON_IsString(method)) return;
    ESP_LOGI(TAG, "mcp rx method=%s", method->valuestring);
    if (!strcmp(method->valuestring, "initialize")) { mcp_initialize(id); return; }
    if (!strcmp(method->valuestring, "tools/list") ||
        !strcmp(method->valuestring, "notifications/initialized")) {
        ESP_LOGI(TAG, "mcp %s", method->valuestring);
        if (!strcmp(method->valuestring, "tools/list")) mcp_tools_list(id);
        return;
    }
    if (!strcmp(method->valuestring, "tools/call")) {
        cJSON *params = cJSON_GetObjectItem(payload, "params");
        const char *name = NULL;
        cJSON *args = NULL;
        if (cJSON_IsObject(params)) {
            cJSON *n = cJSON_GetObjectItem(params, "name");
            if (cJSON_IsString(n)) name = n->valuestring;
            args = cJSON_GetObjectItem(params, "arguments");
        }
        mcp_call(id, name, args);
    }
}

static void ensure_decoder(int sr)
{
    if (sr < 8000) sr = 24000;
    if (s_dec && s_dec_sr == sr) return;
    if (s_dec) { opus_decoder_destroy(s_dec); s_dec = NULL; }
    int err = 0;
    s_dec = opus_decoder_create(sr, 1, &err);
    s_dec_sr = sr;
    if (err != OPUS_OK || !s_dec) {
        ESP_LOGE(TAG, "opus decoder create failed %d", err);
        s_dec = NULL;
    }
}

static void play_opus(const uint8_t *data, size_t len)
{
    if (!s_dec || !data || !len) return;
    enum { MAX_FRAME = 5760 };   // 120 ms @ 48 kHz
    int16_t *pcm = heap_caps_malloc(MAX_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) pcm = malloc(MAX_FRAME * sizeof(int16_t));
    if (!pcm) return;
    int n = opus_decode(s_dec, data, (opus_int32)len, pcm, MAX_FRAME, 0);
    if (n <= 0) { free(pcm); return; }

    // Upsample to 48 kHz for speaker.c (duplicate / linear).
    int out_n = n * (48000 / s_dec_sr);
    if (s_dec_sr == 48000) {
        size_t off = 0;
        while (off < (size_t)n) {
            size_t d = speaker_play(pcm + off, (size_t)n - off);
            if (!d) vTaskDelay(pdMS_TO_TICKS(10));
            else off += d;
        }
    } else {
        int16_t *up = heap_caps_malloc(out_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!up) up = malloc(out_n * sizeof(int16_t));
        if (!up) { free(pcm); return; }
        if (s_dec_sr == 24000) {
            for (int i = 0; i < n; i++) {
                up[i * 2] = pcm[i];
                up[i * 2 + 1] = pcm[i];
            }
        } else if (s_dec_sr == 16000) {
            for (int i = 0; i < n; i++) {
                up[i * 3] = pcm[i];
                up[i * 3 + 1] = pcm[i];
                up[i * 3 + 2] = pcm[i];
            }
            out_n = n * 3;
        } else {
            // Generic nearest-neighbour.
            for (int i = 0; i < out_n; i++) {
                int src = i * s_dec_sr / 48000;
                if (src >= n) src = n - 1;
                up[i] = pcm[src];
            }
        }
        size_t off = 0;
        while (off < (size_t)out_n) {
            size_t d = speaker_play(up + off, (size_t)out_n - off);
            if (!d) vTaskDelay(pdMS_TO_TICKS(10));
            else off += d;
        }
        free(up);
    }
    free(pcm);
    s_st.opus_recv++;
    touch_act();
}

static void handle_json(const char *data, int len)
{
    if (!data || len <= 0) return;
    char *tmp = malloc((size_t)len + 1);
    if (!tmp) return;
    memcpy(tmp, data, (size_t)len);
    tmp[len] = 0;
    cJSON *root = cJSON_Parse(tmp);
    free(tmp);
    if (!root) return;
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) { cJSON_Delete(root); return; }
    const char *t = type->valuestring;
    touch_act();

    if (!strcmp(t, "hello")) {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid)) {
            strncpy(s_session_id, sid->valuestring, sizeof(s_session_id) - 1);
        }
        cJSON *ap = cJSON_GetObjectItem(root, "audio_params");
        int sr = 24000;
        if (cJSON_IsObject(ap)) {
            cJSON *srj = cJSON_GetObjectItem(ap, "sample_rate");
            if (cJSON_IsNumber(srj)) sr = srj->valueint;
        }
        s_want_dec_sr = sr;   // encode/play task creates decoder
        s_hello_ok = true;
        ESP_LOGI(TAG, "server hello ok session=%s", s_session_id);
    } else if (!strcmp(t, "tts")) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state)) {
            if (!strcmp(state->valuestring, "start")) {
                s_speaking = true;
                s_listening = false;
                s_pcm_n = 0;
                s_followup_epoch++;   // invalidate any follow-up already waiting
                microphone_set_listen_grace_ms(0);
                s_recover_pending = false;
                ESP_LOGI(TAG, "tts start");
            } else if (!strcmp(state->valuestring, "stop")) {
                s_speaking = false;
                ESP_LOGI(TAG, "tts stop (odec_pending=%" PRIu32 " spk_busy=%d)",
                         s_odec_pending, (int)speaker_busy());
                // Follow-up mic after MODE_SPK is unreliable on this K210; wake word resumes.
            } else if (!strcmp(state->valuestring, "sentence_start")) {
                cJSON *text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text) && text->valuestring[0]) {
                    strncpy(s_face_text, text->valuestring, sizeof(s_face_text) - 1);
                    s_face_text[sizeof(s_face_text) - 1] = 0;
                    s_face_pending = true;
                }
            }
        }
    } else if (!strcmp(t, "stt")) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) {
            ESP_LOGI(TAG, "stt: %s", text->valuestring);
            strncpy(s_face_text, text->valuestring, sizeof(s_face_text) - 1);
            s_face_text[sizeof(s_face_text) - 1] = 0;
            s_face_pending = true;
        }
    } else if (!strcmp(t, "llm")) {
        cJSON *emo = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emo) && emo->valuestring[0]) {
            char name[40];
            snprintf(name, sizeof(name), "stay_%s", emo->valuestring);
            strncpy(s_face_text, name, sizeof(s_face_text) - 1);
            s_face_text[sizeof(s_face_text) - 1] = 0;
            s_face_pending = true;  // play path treats stay_* as face name
        }
    } else if (!strcmp(t, "goodbye")) {
        // Stock xiaozhi: server ends the conversation -> device goes back to idle.
        ESP_LOGI(TAG, "server goodbye -> idle");
        if (s_session) queue_home();
    } else if (!strcmp(t, "mcp")) {
        handle_mcp(cJSON_GetObjectItem(root, "payload"));
    } else if (!strcmp(t, "system")) {
        cJSON *cmd = cJSON_GetObjectItem(root, "command");
        // Cloud sometimes sends reboot after bind / protocol nudge. Never hard-reset
        // EMO here — it kills the K210 link mid-session and looks like a crash.
        if (cJSON_IsString(cmd)) {
            ESP_LOGW(TAG, "system command ignored: %s", cmd->valuestring);
        }
    }
    cJSON_Delete(root);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected, sending hello");
        {
            char hello[320];
            snprintf(hello, sizeof(hello),
                     "{\"type\":\"hello\",\"version\":1,"
                     "\"features\":{\"mcp\":true},"
                     "\"transport\":\"websocket\","
                     "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                     "\"channels\":1,\"frame_duration\":%d}}",
                     OPUS_FRAME_MS);
            ws_send_text(hello);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:    // server idle-close (close frame) lands here
    {
        if (s_ws_self_close) break;     // we are tearing it down ourselves
        ESP_LOGW(TAG, "ws %s (session=%d)",
                 id == WEBSOCKET_EVENT_CLOSED ? "closed by server" : "disconnected",
                 (int)s_session);
        s_hello_ok = false;
        s_listening = false;
        s_speaking = false;
        // Do NOT touch the mic/face here (ws task, 8 KB). Hand over to xz_sess.
        if (s_session) queue_home();
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == WS_TRANSPORT_OPCODES_TEXT ||
            ev->op_code == WS_TRANSPORT_OPCODES_BINARY ||
            ev->op_code == WS_TRANSPORT_OPCODES_CONT) {
            if (!frag_append(ev)) break;
            int opc = (s_frag_opcode >= 0) ? s_frag_opcode : ev->op_code;
            if (opc == WS_TRANSPORT_OPCODES_TEXT) {
                handle_json((const char *)s_frag, ev->payload_len);
            } else if (opc == WS_TRANSPORT_OPCODES_BINARY) {
                if ((s_speaking || !s_listening) && s_odec_ring && s_odec_sem &&
                    ev->payload_len > 0 && (size_t)ev->payload_len <= ODEC_MAX) {
                    uint8_t next = (uint8_t)((s_odec_w + 1) % ODEC_N);
                    if (next != s_odec_r) {
                        odec_chunk_t *ch = &s_odec_ring[s_odec_w];
                        ch->n = (uint16_t)ev->payload_len;
                        memcpy(ch->d, s_frag, (size_t)ev->payload_len);
                        s_odec_w = next;
                        s_odec_pending++;
                        xSemaphoreGive(s_odec_sem);
                    }
                }
            }
            frag_reset();
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "ws error");
        break;
    default:
        break;
    }
}

static void close_ws(void)
{
    if (!s_ws) return;
    s_ws_self_close = true;
    esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_ws_self_close = false;
    s_hello_ok = false;
    s_listening = false;
    s_speaking = false;
    s_session = false;
    behavior_set_voice_busy(false);
}

static bool open_ws(void)
{
    if (!s_ws_url[0]) return false;
    close_ws();
    s_hello_ok = false;
    s_session_id[0] = 0;

    char headers[320];
    const char *tok = s_token[0] ? s_token : "";
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n"
             "Protocol-Version: 1\r\n"
             "Device-Id: %s\r\n"
             "Client-Id: %s\r\n",
             tok, s_device_id, s_client_id);

    ESP_LOGI(TAG, "open_ws heap int=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_websocket_client_config_t cfg = {
        .uri = s_ws_url,
        .buffer_size = WS_RX_BUF,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .headers = headers,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "ws init failed (OOM?)");
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        close_ws();
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    while (!s_hello_ok && (esp_timer_get_time() - t0) < (int64_t)HELLO_TIMEOUT_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_hello_ok) {
        ESP_LOGE(TAG, "hello timeout");
        close_ws();
        return false;
    }
    return true;
}

static esp_err_t http_post_ota(const char *url, char **out_body)
{
    *out_body = NULL;
    char post[256];
    snprintf(post, sizeof(post),
             "{\"version\":1,\"language\":\"en\",\"flash_size\":4194304,"
             "\"chip_model_name\":\"esp32\",\"application\":{\"name\":\"emo-bridge\",\"version\":\"0.1.2\"},"
             "\"board\":{\"type\":\"emo-living-ai\"}}");

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return ESP_FAIL;
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_header(http, "Device-Id", s_device_id);
    esp_http_client_set_header(http, "Client-Id", s_client_id);
    esp_http_client_set_header(http, "Activation-Version", "1");
    esp_http_client_set_header(http, "User-Agent", "emo-bridge/0.1.2");

    esp_err_t err = esp_http_client_open(http, strlen(post));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA open fail: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http);
        return err;
    }
    int w = esp_http_client_write(http, post, strlen(post));
    if (w < 0) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        return ESP_FAIL;
    }
    int content_length = esp_http_client_fetch_headers(http);
    int code = esp_http_client_get_status_code(http);
    if (code != 200) {
        ESP_LOGE(TAG, "OTA status %d", code);
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        return ESP_FAIL;
    }
    if (content_length < 0) content_length = 8192;
    if (content_length > 65536) content_length = 65536;
    char *body = malloc((size_t)content_length + 1);
    if (!body) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        return ESP_ERR_NO_MEM;
    }
    int total = 0;
    while (total < content_length) {
        int n = esp_http_client_read(http, body + total, content_length - total);
        if (n <= 0) break;
        total += n;
    }
    body[total] = 0;
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    *out_body = body;
    return ESP_OK;
}

static bool parse_ota(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;

    cJSON *act = cJSON_GetObjectItem(root, "activation");
    bool has_code = false;
    if (cJSON_IsObject(act)) {
        cJSON *code = cJSON_GetObjectItem(act, "code");
        if (cJSON_IsString(code) && code->valuestring[0]) {
            has_code = true;
            // User already dismissed this code (linked on xiaozhi.me) — don't keep it alive.
            if (s_act_dismissed && strcmp(s_shown_code, code->valuestring) == 0) {
                s_activation[0] = 0;
            } else {
                strncpy(s_activation, code->valuestring, sizeof(s_activation) - 1);
                show_activation(s_activation);
            }
        }
    }
    if (!has_code) {
        // Paired / no challenge — drop code and take the text off the face.
        if (s_activation[0] || s_act_on_face) {
            s_activation[0] = 0;
            clear_activation_face();
        }
    }

    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws)) {
        cJSON *url = cJSON_GetObjectItem(ws, "url");
        cJSON *tok = cJSON_GetObjectItem(ws, "token");
        if (cJSON_IsString(url) && url->valuestring[0]) {
            strncpy(s_ws_url, url->valuestring, sizeof(s_ws_url) - 1);
            ok = true;
        }
        if (cJSON_IsString(tok) && tok->valuestring[0]) {
            strncpy(s_token, tok->valuestring, sizeof(s_token) - 1);
        }
    }
    cJSON_Delete(root);

    // Persist so a later boot can wake even if OTA TLS flakes.
    if (ok && s_ws_url[0]) {
        emo_settings_t st; settings_get_copy(&st);
        bool dirty = false;
        if (strcmp(st.ws_url, s_ws_url) != 0) {
            strncpy(st.ws_url, s_ws_url, sizeof(st.ws_url) - 1);
            dirty = true;
        }
        if (s_token[0] && strcmp(st.xz_token, s_token) != 0) {
            strncpy(st.xz_token, s_token, sizeof(st.xz_token) - 1);
            dirty = true;
        }
        if (dirty) settings_save(&st);
    }
    return ok;
}

static void apply_settings_urls(void)
{
    emo_settings_t st; settings_get_copy(&st);
    if (st.ws_url[0]) {
        strncpy(s_ws_url, st.ws_url, sizeof(s_ws_url) - 1);
    } else if (!s_ws_url[0]) {
        // Cloud default — same host the OTA response usually returns.
        strncpy(s_ws_url, "wss://api.tenclass.net/xiaozhi/v1/", sizeof(s_ws_url) - 1);
    }
    if (st.xz_token[0]) {
        strncpy(s_token, st.xz_token, sizeof(s_token) - 1);
    }
}

static void ota_task(void *arg)
{
    (void)arg;
    ensure_ids();
    apply_settings_urls();

    while (1) {
        if (!voice_is_xz()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!net_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        emo_settings_t st; settings_get_copy(&st);
        apply_settings_urls();

        // Prefer explicit ws_url; else OTA.
        if (!s_ws_url[0] && st.ota_url[0]) {
            char *body = NULL;
            if (http_post_ota(st.ota_url, &body) == ESP_OK && body) {
                parse_ota(body);
                free(body);
            }
        }

        if (s_ws_url[0] && s_activation[0] && st.ota_url[0]) {
            // Keep polling OTA until activation cleared (user entered the code).
            vTaskDelay(pdMS_TO_TICKS(5000));
            char *body = NULL;
            if (http_post_ota(st.ota_url, &body) == ESP_OK && body) {
                parse_ota(body);
                free(body);
            }
            continue;
        }

        if (s_ws_url[0]) {
            ESP_LOGI(TAG, "ready ws=%s", s_ws_url);
            // Warm WS on xz_sess (fat stack) while heap is still free.
            if (s_cmd_q && (!s_ws || !s_hello_ok)) {
                xz_cmd_t pre = XZ_CMD_PREOPEN;
                xQueueSend(s_cmd_q, &pre, 0);
            }
            for (int i = 0; i < 60 && voice_is_xz(); i++) {
                while (s_session) vTaskDelay(pdMS_TO_TICKS(500));
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            apply_settings_urls();
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void idle_watch_task(void *arg)
{
    (void)arg;
    int dead = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_session) { dead = 0; continue; }

        // Safety net: socket gone but no event reached us.
        if (s_ws && !esp_websocket_client_is_connected(s_ws)) {
            if (++dead >= 2) {
                ESP_LOGW(TAG, "ws not connected during session -> home");
                dead = 0;
                queue_home();
            }
            continue;
        }
        dead = 0;

        if (s_speaking || s_odec_pending || s_recover_pending || speaker_busy()) continue;
        int64_t now = esp_timer_get_time();
        if (s_last_act_us && (now - s_last_act_us) > (int64_t)IDLE_CLOSE_MS * 1000) {
            ESP_LOGI(TAG, "no conversation for %d s -> home", IDLE_CLOSE_MS / 1000);
            queue_home();
        }
    }
}

esp_err_t xiaozhi_pair_refresh(void)
{
    ensure_ids();
    apply_settings_urls();
    if (!net_is_connected()) return ESP_ERR_INVALID_STATE;

    emo_settings_t st; settings_get_copy(&st);
    // NVS may have an empty ota_url from an earlier Save; fall back to cloud default.
    const char *ota = st.ota_url[0] ? st.ota_url : "https://api.tenclass.net/xiaozhi/ota/";
    if (!st.ota_url[0]) {
        ESP_LOGW(TAG, "ota_url empty in settings — using default %s", ota);
    }

    char *body = NULL;
    esp_err_t e = http_post_ota(ota, &body);
    if (e != ESP_OK) return e;
    if (body) {
        parse_ota(body);
        free(body);
    }
    ESP_LOGI(TAG, "pair refresh: activation=%s device=%s client=%s ws=%s",
             s_activation[0] ? s_activation : "(none)",
             s_device_id, s_client_id,
             s_ws_url[0] ? s_ws_url : "(none)");
    return ESP_OK;
}

static void encode_flush_frame(void)
{
    if (!s_enc || s_pcm_n < OPUS_FRAME_SAMP) return;
    int enc = opus_encode(s_enc, s_pcm_acc, OPUS_FRAME_SAMP, s_opus_pkt, OPUS_MAX_PACKET);
    s_pcm_n = 0;
    if (enc > 0 && ws_alive()) {
        // NOT touch_act(): streaming mic silence is not conversation activity,
        // otherwise the idle timeout can never fire.
        esp_websocket_client_send_bin(s_ws, (const char *)s_opus_pkt, enc, pdMS_TO_TICKS(500));
        s_st.opus_sent++;
    }
}

static void encode_drain(void)
{
    while (s_pcm_sem && xSemaphoreTake(s_pcm_sem, 0) == pdTRUE) {
        uint8_t r = s_pcm_r;
        pcm_chunk_t *ch = &s_pcm_ring[r % PCM_RING_N];
        s_pcm_r = (uint8_t)((r + 1) % PCM_RING_N);
        if (!s_session || !s_listening || s_speaking || !ws_alive()) {
            s_pcm_n = 0;
            continue;
        }
        size_t off = 0;
        while (off < ch->n) {
            size_t space = OPUS_FRAME_SAMP - s_pcm_n;
            size_t n = ch->n - off;
            if (n > space) n = space;
            memcpy(s_pcm_acc + s_pcm_n, ch->s + off, n * sizeof(int16_t));
            s_pcm_n += n;
            off += n;
            if (s_pcm_n >= OPUS_FRAME_SAMP) encode_flush_frame();
        }
    }
}

static void session_begin_work(void)
{
    apply_settings_urls();
    if (!s_ws_url[0]) {
        ESP_LOGW(TAG, "wake but no websocket URL yet (set ota_url or ws_url)");
        return;
    }

    ESP_LOGI(TAG, "wake -> session begin (ws=%s hello=%d)",
             s_ws_url, (int)s_hello_ok);

    behavior_set_voice_busy(true);
    legs_stop();

    // Do NOT soft-reset K210 on wake — that looks like a full restart and
    // kills the turn. Wake already scheduled MODE_MIC; only reopen if needed.

    bool need_open = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_speaking) {
        send_abort();
        speaker_interrupt("xiaozhi barge-in");
        s_speaking = false;
    }
    if (s_session && !ws_alive()) {
        ESP_LOGW(TAG, "wake: stale session on dead ws -> reconnect");
        s_session = false;
    }
    need_open = !ws_alive();
    xSemaphoreGive(s_lock);

    // Never hold s_lock across open_ws (TLS + hello wait up to 10 s).
    if (need_open) {
        ESP_LOGI(TAG, "opening websocket...");
        if (!open_ws()) {
            ESP_LOGE(TAG, "open_ws failed");
            behavior_set_voice_busy(false);
            return;
        }
        behavior_set_voice_busy(true);   // close_ws() inside open_ws cleared it
    }

    // Mic after recover — wake's schedule_open may have fired during RESETN.
    if (k210_link_state() != LINK_LISTEN) {
        if (microphone_open() != ESP_OK)
            ESP_LOGW(TAG, "mic open after wake failed");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_session = true;
    s_listening = true;
    s_pcm_n = 0;
    s_pcm_w = s_pcm_r = 0;
    while (s_pcm_sem && xSemaphoreTake(s_pcm_sem, 0) == pdTRUE) { /* drain */ }
    s_st.sessions++;
    touch_act();
    send_listen("detect");
    send_listen("start");
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "listen start");
}

static void show_listen_face(void);

// Conversation is over (server closed the ws, goodbye, or nobody talked for
// IDLE_CLOSE_MS): drop the socket, put the K210 back in MODE_IDLE so the wake
// word ("emo") is armed again, and show the web ":8080 Speak 4" animation.
static void go_home_work(void)
{
    if (!s_session && !s_ws) return;          // already home
    ESP_LOGI(TAG, "go home: session=%d ws_alive=%d", (int)s_session, (int)ws_alive());

    s_session = false;
    s_listening = false;
    s_speaking = false;
    s_recover_pending = false;
    s_followup_epoch++;
    microphone_set_listen_grace_ms(0);

    if (speaker_busy()) {
        speaker_interrupt("xiaozhi session over");
        for (int i = 0; i < 30 && speaker_busy(); i++) vTaskDelay(pdMS_TO_TICKS(50));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    close_ws();                               // also clears s_session / voice_busy
    xSemaphoreGive(s_lock);

    if (!k210_link_booting() && k210_link_state() != LINK_SPEAK) {
        k210_uart_send_mode(K210_MODE_IDLE);  // wake word armed again
        k210_link_set_state(LINK_IDLE);
    }
    microphone_mark_closed();                 // else "wake while an utterance is running" would eat emo

    bool ack = false;
    if (!s_home_quiet) video_play_face("speak4", 0, false, &ack);   // same as web :8080 "Speak 4"
    behavior_set_voice_busy(false);
    ESP_LOGI(TAG, "home: idle, say emo (speak4 ack=%d quiet=%d)", (int)ack, (int)s_home_quiet);
}

static void session_end_work(void)
{
    if (!s_session) return;
    // Stale EOS while TTS is playing / draining must not kill the turn.
    if (s_speaking || speaker_busy() || s_odec_pending || s_recover_pending) {
        ESP_LOGI(TAG, "listen stop ignored (tts/recover in flight)");
        return;
    }
    // Opus needs ~20 KB of stack: NEVER encode on this 12 KB task (that
    // overflowed and panicked right after "listen stop"). Ask xz_enc to flush
    // the tail frame, then wait for it.
    if (s_listening && !s_speaking && s_enc) {
        s_flush_req = true;   // encode task polls every ~20 ms
        for (int i = 0; i < 60 && s_flush_req; i++) vTaskDelay(pdMS_TO_TICKS(10));
        s_flush_req = false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stop_utt = (s_listening && !s_speaking);
    if (stop_utt) {
        send_listen("stop");
        s_listening = false;
        ESP_LOGI(TAG, "listen stop (eos)");
    }
    xSemaphoreGive(s_lock);
    if (!stop_utt) return;

    // Server may still push STT→TTS for the utterance we just closed — wait
    // briefly so we don't open the mic under a reply.
    vTaskDelay(pdMS_TO_TICKS(1800));
    if (s_session && !ws_alive()) { go_home_work(); return; }
    if (!s_session || s_speaking || s_odec_pending || speaker_busy() ||
        s_recover_pending || s_listening || k210_link_booting())
        return;

    // After mic EOS we are MODE_IDLE — reopen without K210 soft-reset.
    if (microphone_open() != ESP_OK) {
        bool ack = false;
        video_play_face("stay_a_b", 0, false, &ack);   // eyes, not text
        behavior_set_voice_busy(false);
        ESP_LOGW(TAG, "re-listen failed — say emo");
        return;
    }
    microphone_set_listen_grace_ms(20000);
    show_listen_face();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_listening = true;
    s_pcm_n = 0;
    s_pcm_w = s_pcm_r = 0;
    while (s_pcm_sem && xSemaphoreTake(s_pcm_sem, 0) == pdTRUE) { /* drain */ }
    send_listen("start");
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "silence → listening again (no wake)");
}

static bool tts_fully_idle(void)
{
    return !s_speaking && s_odec_pending == 0 && !speaker_busy();
}

static void followup_work(void)
{
    (void)s_followup_epoch;
}

// Looping "listening" face so the screen never sits black between turns
// (idle wander is frozen while a voice session is active).
static void show_listen_face_ex(bool with_sound)
{
    bool ack = false;
    // ChatGPT logo = "mic is open, talk now". with_sound = exactly the web
    // :8080 "Chat hear" button (face + sound, played once).
    if (video_play_face("chatgpt_hear", with_sound ? 0 : 1, with_sound, &ack) != ESP_OK || !ack)
        video_play_face("stay_a_b", 1, false, &ack);
}

static void show_listen_face(void) { show_listen_face_ex(false); }

static bool wait_mic_frames(uint32_t baseline_samples, uint32_t timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < (int64_t)timeout_ms * 1000) {
        microphone_stats_t st;
        microphone_get_stats(&st);
        if (st.samples > baseline_samples + 512) return true;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return false;
}

static void recover_work(void)
{
    s_recover_pending = false;
    if (!s_session) return;
    if (!ws_alive()) { go_home_work(); return; }
    touch_act();             // playback time must not count as "idle"
    s_speaking = false;
    s_listening = false;

    microphone_stats_t before;
    microphone_get_stats(&before);
    bool got_mic = false;

    // SPEAK→MIC never gets MIC_READY on this K210 after Xiaozhi TTS (verified
    // many times), so go straight to the K210 reset: the screen is only black
    // while the K210 boots (~4 s), and the logo goes up the moment it is alive.
    microphone_mark_closed();

    // EXPERIMENT: plain playback (/api/tone) leaves the K210 alive, so try the
    // stock sequence first (drain -> MODE_IDLE -> ping -> logo -> 4B+MIC). Only
    // if the K210 is really dead do we pay the ~4 s reset.
    speaker_end_handoff();
    k210_uart_send_mode(K210_MODE_IDLE);
    k210_link_set_state(LINK_IDLE);
    vTaskDelay(pdMS_TO_TICKS(250));
    bool alive = k210_link_ping(400);
    ESP_LOGI(TAG, "post-tts: K210 after IDLE %s", alive ? "ALIVE (no reset)" : "DEAD");
    bool ack = false;
    if (alive) {
        show_listen_face_ex(true);         // web "Chat hear": logo + sound, instant
        microphone_get_stats(&before);
        if (microphone_open() == ESP_OK && wait_mic_frames(before.samples, 1500)) {
            got_mic = true;
            ESP_LOGI(TAG, "post-tts: IDLE->MIC ok, no reset");
        } else {
            ESP_LOGW(TAG, "post-tts: alive but no mic frames");
            microphone_mark_closed();
        }
    }

    if (!got_mic) {
        k210_link_recover_after_spk();     // ping again, else RESETN
        show_listen_face_ex(true);         // web "Chat hear": logo + sound, mic is open
        microphone_get_stats(&before);
        if (microphone_open() != ESP_OK ||
            !wait_mic_frames(before.samples, 2000)) {
            video_play_face("stay_a_b", 0, false, &ack);   // eyes, not text
            behavior_set_voice_busy(false);
            ESP_LOGW(TAG, "tts done — mic dead, say emo (ack=%d)", (int)ack);
            return;
        }
    }

    microphone_set_listen_grace_ms(20000);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_listening = true;
    s_pcm_n = 0;
    s_pcm_w = s_pcm_r = 0;
    while (s_pcm_sem && xSemaphoreTake(s_pcm_sem, 0) == pdTRUE) { /* drain */ }
    send_listen("start");
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "tts done — listening again (grace 20s, no wake needed)");
}

static bool on_speaker_idle(void)
{
    if (!s_session || s_listening) return false;
    if (s_odec_pending) return false;
    if (s_recover_pending) return true;   // already handing off
    s_speaking = false;
    s_listening = false;
    if (s_cmd_q) {
        s_recover_pending = true;
        xz_cmd_t cmd = XZ_CMD_RECOVER;
        xQueueSend(s_cmd_q, &cmd, 0);
    }
    // Skip MODE_IDLE — it wedges K210 UART after MODE_SPK on this hardware.
    return true;
}

// A robot skill tool (sing, dance, ...) answered ok: let the server's short
// "ok, singing" reply play out, then end the conversation without speak4 so the
// skill owns the face/speaker. Wake word ("emo") is armed again; the user must
// wake the robot to talk after the skill ends.
static void skill_home_work(void)
{
    // 1) the tool reply is already sent: cut Xiaozhi off right away (go_home_work stops the speaker and closes the ws)
    vTaskDelay(pdMS_TO_TICKS(120));                              // let the reply frame leave first
    // 2) conversation over: close the ws, mic idle, wake word armed
    s_home_quiet = true;
    go_home_work();
    s_home_quiet = false;
    // The decoder may still push the rest of the cut sentence into the speaker (MODE_SPK again):
    // let it finish, then throw that audio away so the skill's first clip is not swallowed.
    for (int i = 0; i < 40 && s_odec_pending; i++) vTaskDelay(pdMS_TO_TICKS(50));
    speaker_interrupt("skill start");
    // K210 was left in speaker mode by the TTS: bring it back to a state that
    // answers (same sequence as recover_work) or the song's face/mp3 gets no ACK.
    speaker_end_handoff();
    k210_uart_send_mode(K210_MODE_IDLE);
    k210_link_set_state(LINK_IDLE);
    vTaskDelay(pdMS_TO_TICKS(150));
    bool alive = k210_link_ping(400);
    ESP_LOGI(TAG, "skill: K210 after IDLE %s", alive ? "ALIVE" : "DEAD -> recover");
    if (!alive) k210_link_recover_after_spk();
    microphone_mark_closed();
    // 3) now the skill owns the speaker/face
    if (s_pend) {
        s_pend = false;
        cJSON *a = s_pend_args[0] ? cJSON_Parse(s_pend_args) : NULL;
        s_mcp_local = true;
        mcp_call(NULL, s_pend_name, a);
        s_mcp_local = false;
        if (a) cJSON_Delete(a);
    }
}
static void session_task(void *arg)
{
    (void)arg;
    xz_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd == XZ_CMD_BEGIN) session_begin_work();
        else if (cmd == XZ_CMD_END) session_end_work();
        else if (cmd == XZ_CMD_FOLLOWUP) followup_work();
        else if (cmd == XZ_CMD_RECOVER) recover_work();
        else if (cmd == XZ_CMD_HOME) go_home_work();
        else if (cmd == XZ_CMD_SKILL_HOME) skill_home_work();
        else if (cmd == XZ_CMD_PREOPEN) {
            apply_settings_urls();
            if (s_ws_url[0] && (!s_ws || !s_hello_ok)) {
                ESP_LOGI(TAG, "pre-open websocket");
                open_ws();
            }
        }
    }
}

static void encode_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_want_dec_sr > 0) {
            int sr = s_want_dec_sr;
            s_want_dec_sr = 0;
            ensure_decoder(sr);
        }
        if (s_face_pending) {
            s_face_pending = false;
            // No face/text commands while a reply is playing: an avi opened on
            // the K210 SD card during MODE_SPK is the suspect for the post-TTS
            // UART wedge (plain /api/tone playback leaves the K210 alive).
            // STT/LLM text is NOT drawn either: text mode replaces the eyes.
        }
        if (s_flush_req) {
            encode_drain();                       // everything captured so far
            if (s_enc && s_pcm_n > 0 && s_pcm_n < OPUS_FRAME_SAMP) {
                memset(s_pcm_acc + s_pcm_n, 0, (OPUS_FRAME_SAMP - s_pcm_n) * sizeof(int16_t));
                s_pcm_n = OPUS_FRAME_SAMP;
                encode_flush_frame();
            }
            s_flush_req = false;
        }
        if (s_odec_sem && xSemaphoreTake(s_odec_sem, 0) == pdTRUE) {
            uint8_t r = s_odec_r;
            odec_chunk_t *ch = &s_odec_ring[r % ODEC_N];
            s_odec_r = (uint8_t)((r + 1) % ODEC_N);
            if (s_odec_pending) s_odec_pending--;
            play_opus(ch->d, ch->n);
            continue;
        }
        if (s_pcm_sem && xSemaphoreTake(s_pcm_sem, pdMS_TO_TICKS(20)) == pdTRUE) {
            xSemaphoreGive(s_pcm_sem);
            encode_drain();
        }
    }
}

void xiaozhi_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(4, sizeof(xz_cmd_t));
    s_pcm_sem = xSemaphoreCreateCounting(PCM_RING_N, 0);
    s_pcm_ring = heap_caps_malloc(sizeof(pcm_chunk_t) * PCM_RING_N,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm_ring) s_pcm_ring = malloc(sizeof(pcm_chunk_t) * PCM_RING_N);
    if (!s_pcm_ring) ESP_LOGE(TAG, "pcm ring alloc failed");
    s_odec_sem = xSemaphoreCreateCounting(ODEC_N, 0);
    s_odec_ring = heap_caps_malloc(sizeof(odec_chunk_t) * ODEC_N,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_odec_ring) s_odec_ring = malloc(sizeof(odec_chunk_t) * ODEC_N);
    if (!s_odec_ring) ESP_LOGE(TAG, "odec ring alloc failed");
    memset(&s_st, 0, sizeof(s_st));
    ensure_ids();
    int err = 0;
    s_enc = opus_encoder_create(OPUS_SR_MIC, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !s_enc) {
        ESP_LOGE(TAG, "opus encoder create failed %d", err);
        s_enc = NULL;
    } else {
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(16000));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));
        opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }
    s_want_dec_sr = 24000;
}

void xiaozhi_start(void)
{
    speaker_set_idle_cb(on_speaker_idle);
    // http_post_ota + mbedTLS needs a fat stack; 8 KB overflowed mid-session.
    xTaskCreatePinnedToCore(ota_task, "xz_ota", 12288, NULL, 4, NULL, 0);
    xTaskCreate(idle_watch_task, "xz_idle", 3072, NULL, 3, NULL);
    xTaskCreatePinnedToCore(session_task, "xz_sess", 12288, NULL, 5, NULL, 0);

    // Opus needs ~20 KB stack — put it in PSRAM so internal heap stays for TLS.
    const uint32_t enc_stack_bytes = 24576;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY || CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    StackType_t *enc_stack = heap_caps_malloc(enc_stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *enc_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (enc_stack && enc_tcb) {
        xTaskCreateStaticPinnedToCore(encode_task, "xz_enc",
                                      enc_stack_bytes / sizeof(StackType_t),
                                      NULL, 5, enc_stack, enc_tcb, 0);
    } else
#endif
    {
        xTaskCreatePinnedToCore(encode_task, "xz_enc", enc_stack_bytes, NULL, 5, NULL, 0);
    }
}

void xiaozhi_session_begin(void)
{
    if (!voice_is_xz() || !s_cmd_q) return;
    xz_cmd_t cmd = XZ_CMD_BEGIN;
    xQueueSend(s_cmd_q, &cmd, 0);
}

void xiaozhi_session_end(void)
{
    if (!voice_is_xz() || !s_cmd_q) return;
    xz_cmd_t cmd = XZ_CMD_END;
    xQueueSend(s_cmd_q, &cmd, 0);
}

void xiaozhi_feed(const int16_t *pcm, size_t samples)
{
    // RX-safe: copy into PSRAM ring only — no Opus, no WS.
    if (!voice_is_xz() || !s_pcm_ring || !s_pcm_sem) return;
    if (!s_session || !s_listening || s_speaking) return;
    if (!s_enc || !ws_alive() || !pcm || !samples) return;

    size_t off = 0;
    while (off < samples) {
        uint8_t next = (uint8_t)((s_pcm_w + 1) % PCM_RING_N);
        if (next == s_pcm_r) {
            static uint32_t drops;
            if ((++drops % 20) == 1) ESP_LOGW(TAG, "pcm ring full, drop (n=%" PRIu32 ")", drops);
            break;
        }
        pcm_chunk_t *ch = &s_pcm_ring[s_pcm_w];
        size_t n = samples - off;
        if (n > PCM_Q_SAMP) n = PCM_Q_SAMP;
        ch->n = (uint16_t)n;
        // Robot mic is quiet (speech ~1.5-4k): x2 digital gain, clipped.
        for (size_t i = 0; i < n; i++) {
            int v = (int)pcm[off + i] * 2;
            ch->s[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        s_pcm_w = next;
        xSemaphoreGive(s_pcm_sem);
        off += n;
    }
}
