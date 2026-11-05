// anim.c - see anim.h
#include "anim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "legs.h"
#include "video.h"

static const char *TAG = "anim";
#define TBL_PATH "/spiffs/anim_tbl.txt"

// Find the first line "<kind>|<name>|rest" and copy `rest` (without newline) into out.
static bool find_line(char kind, const char *name, char *out, size_t cap)
{
    FILE *f = fopen(TBL_PATH, "r");
    if (!f) { ESP_LOGW(TAG, "no %s", TBL_PATH); return false; }
    char line[200];
    size_t nl = strlen(name);
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != kind || line[1] != '|') continue;
        if (strncmp(line + 2, name, nl) != 0 || line[2 + nl] != '|') continue;
        strlcpy(out, line + 3 + nl, cap);
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
        found = true;
        break;
    }
    fclose(f);
    return found;
}

bool anim_exists(const char *name)
{
    char rest[160];
    return name && *name && (find_line('A', name, rest, sizeof(rest)) || find_line('M', name, rest, sizeof(rest)));
}

esp_err_t anim_play(const char *name, uint32_t *dur_ms)
{
    if (dur_ms) *dur_ms = 0;
    if (!name || !*name) return ESP_ERR_INVALID_ARG;

    char mot[64] = "-";
    char media[64];
    strlcpy(media, name, sizeof(media));
    char rest[160];
    if (find_line('A', name, rest, sizeof(rest))) {
        char *bar = strchr(rest, '|');
        if (bar) {
            *bar = 0;
            strlcpy(mot, rest, sizeof(mot));
            char *list = bar + 1;
            int cnt = 1;
            for (char *c = list; *c; c++) if (*c == ';') cnt++;
            int pick = (int)(esp_random() % (unsigned)cnt);
            char *s = list;
            while (pick-- > 0) { s = strchr(s, ';'); if (!s) { s = list; break; } s++; }
            char *e = strchr(s, ';');
            if (e) *e = 0;
            strlcpy(media, s, sizeof(media));
        }
    }

    char avi[64] = "", mp3[96] = "-";
    uint32_t sec = 0;
    bool have_media = false;
    if (strcmp(media, "-") && find_line('M', media, rest, sizeof(rest))) {
        // avi|mp3|seconds
        char *p1 = strchr(rest, '|');
        char *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
        if (p1 && p2) {
            *p1 = 0; *p2 = 0;
            strlcpy(avi, rest, sizeof(avi));
            strlcpy(mp3, p1 + 1, sizeof(mp3));
            sec = (uint32_t)atoi(p2 + 1);
            have_media = true;
        }
    }
    if (!have_media && !strcmp(mot, "-")) {
        ESP_LOGW(TAG, "%s: not in table", name);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "play %s: mot=%s avi=%s mp3=%s %us", name, mot, avi, mp3, (unsigned)sec);

    if (strcmp(mot, "-")) {
        legs_opts_t o = LEGS_OPTS_DEFAULT;
        o.media = false;
        if (legs_play(mot, o) != ESP_OK) ESP_LOGW(TAG, "mot %s failed", mot);
    }
    if (have_media) {
        bool ack = false;
        video_play_playlist(avi, mp3, &ack);
        ESP_LOGI(TAG, "  face %s ack=%d", avi, (int)ack);
    }
    if (dur_ms) *dur_ms = sec * 1000;
    return ESP_OK;
}
