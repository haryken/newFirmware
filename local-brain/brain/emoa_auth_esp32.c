// EMOA v2 handshake for the robot's ESP32 firmware (ESP-IDF or Arduino; mbedtls is built in).
// Replaces sending the v1 hello. Put the 32 bytes of brain\stream_key.txt (64 hex chars) in
// EMOA_KEY, e.g. via menuconfig / NVS rather than in source control.
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include "mbedtls/md.h"

static const uint8_t EMOA_KEY[32] = { /* 0x.., 32 bytes from stream_key.txt */ };

static int send_all(int s, const void *b, size_t n) {
    const uint8_t *p = b;
    while (n) { int k = send(s, p, n, 0); if (k <= 0) return -1; p += k; n -= k; }
    return 0;
}

static int recv_all(int s, void *b, size_t n) {
    uint8_t *p = b;
    while (n) { int k = recv(s, p, n, 0); if (k <= 0) return -1; p += k; n -= k; }
    return 0;
}

// Call right after connect(); then stream chunks exactly as before. Returns 0 on success.
int emoa_hello_v2(int sock, uint32_t rate, uint64_t offset) {
    uint8_t hello[19] = {'E', 'M', 'O', 'A', 2};
    memcpy(hello + 5, &rate, 4);          // little-endian on the ESP32, as the server expects
    hello[9] = 1;                          // channels
    hello[10] = 16;                        // bits
    memcpy(hello + 11, &offset, 8);
    if (send_all(sock, hello, sizeof hello)) return -1;

    uint8_t chal[20];                      // "EMOC" + 16-byte nonce
    if (recv_all(sock, chal, sizeof chal) || memcmp(chal, "EMOC", 4)) return -1;

    uint8_t msg[16 + 19], mac[32];
    memcpy(msg, chal + 4, 16);
    memcpy(msg + 16, hello, 19);
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), EMOA_KEY, sizeof EMOA_KEY,
                        msg, sizeof msg, mac)) return -1;
    return send_all(sock, mac, sizeof mac);
}
