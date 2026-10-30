// k210_controls.h — every ESP32 -> K210 command from uart_commands.md, hardcoded,
// addressed by a friendly name. Driven by GET/POST /api/controls (rest_api.c).
//
//   GET  /api/controls   -> JSON array [{name,label,group,hint,args:[{name,type,def}]}]
//   POST /api/controls   <- {"action":"<name>", ...optional args}
//        args: "value" (number; 16/24-bit commands), "d0" "d1" "d2" (numbers),
//              "text" (string; extended frames), "name" "loop" "mp3" (generic anim)
//                        -> {"ok":true,"sent":"55 AA 09 00 00 00 08","ack":true}
//                        -> {"ok":false,"error":"..."}
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Describe all controls as a JSON array string (for the UI). Caller frees.
char *k210_controls_list_json(void);

// Run one control from a parsed request body. Always writes a JSON result into
// out. Returns ESP_OK when the frame went out, ESP_ERR_NOT_FOUND for an unknown
// action, ESP_ERR_INVALID_STATE while the K210 boots, ESP_ERR_INVALID_ARG otherwise.
esp_err_t k210_controls_run(const cJSON *req, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
