#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "auth.h"

static const char *TAG = "auth";

#define NVS_NAMESPACE "auth"
#define ITERATIONS    4096   // PBKDF2, bremst Durchprobieren zusätzlich zur Sperre
#define SALT_LEN      16
#define HASH_LEN      32
#define SECRET_LEN    32
#define NONCE_LEN     16
#define FREE_TRIES    5      // danach Sperre, die sich mit jedem weiteren Fehlversuch verdoppelt
#define LOCK_BASE_S   30
#define LOCK_MAX_S    (15 * 60)

static SemaphoreHandle_t s_lock;
static bool s_set;
static bool s_skipped;
static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];
static uint8_t s_secret[SECRET_LEN];
static int s_fails;
static int64_t s_locked_until;   // µs seit dem Start

static bool hmac(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id;
    if (psa_import_key(&attr, key, key_len, &id) != PSA_SUCCESS) {
        return false;
    }
    size_t len;
    psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, msg_len, out, 32, &len);
    psa_destroy_key(id);
    return st == PSA_SUCCESS;
}

// PBKDF2-HMAC-SHA256 mit einem Block (RFC 8018); der Schlüssel wird nur einmal importiert
static bool pbkdf2(const char *password, const uint8_t salt[SALT_LEN], uint8_t out[HASH_LEN])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id;
    if (psa_import_key(&attr, (const uint8_t *)password, strlen(password), &id) != PSA_SUCCESS) {
        return false;
    }
    // Erste Runde: Salt + Blocknummer 1, danach das vorige Ergebnis
    _Static_assert(SALT_LEN + 4 <= HASH_LEN, "Puffer reicht für beide Runden");
    uint8_t in[HASH_LEN], u[HASH_LEN];
    memcpy(in, salt, SALT_LEN);
    memcpy(in + SALT_LEN, "\0\0\0\1", 4);
    size_t in_len = SALT_LEN + 4, len;
    bool ok = true;
    for (int i = 0; ok && i < ITERATIONS; i++) {
        ok = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), in, in_len, u, HASH_LEN, &len) == PSA_SUCCESS;
        memcpy(in, u, HASH_LEN);
        in_len = HASH_LEN;
        if (i == 0) {
            memcpy(out, u, HASH_LEN);
        } else {
            for (int j = 0; j < HASH_LEN; j++) {
                out[j] ^= u[j];
            }
        }
    }
    psa_destroy_key(id);
    return ok;
}

// Vergleich in konstanter Zeit
static bool same(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

void auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Kein Passwort für die Weboberfläche gesetzt");
        return;
    }
    size_t a = sizeof(s_salt), b = sizeof(s_hash), c = sizeof(s_secret);
    s_set = nvs_get_blob(nvs, "salt", s_salt, &a) == ESP_OK && nvs_get_blob(nvs, "hash", s_hash, &b) == ESP_OK &&
            nvs_get_blob(nvs, "secret", s_secret, &c) == ESP_OK && a == SALT_LEN && b == HASH_LEN && c == SECRET_LEN;
    uint8_t skip = 0;
    s_skipped = nvs_get_u8(nvs, "skip", &skip) == ESP_OK && skip;
    nvs_close(nvs);
    if (!s_set) {
        ESP_LOGW(TAG, "Kein Passwort für die Weboberfläche gesetzt");
    }
}

bool auth_password_set(void)
{
    return s_set;
}

bool auth_setup_pending(void)
{
    return !s_set && !s_skipped;
}

esp_err_t auth_skip(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, "skip", 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_skipped = true;
    }
    return err;
}

esp_err_t auth_remove(void)
{
    esp_err_t err = auth_erase();
    if (err == ESP_OK) {
        err = auth_skip();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Passwort entfernt, Weboberfläche ohne Anmeldung");
    }
    return err;
}

esp_err_t auth_set_password(const char *password)
{
    size_t n = strlen(password);
    if (n < AUTH_PASSWORD_MIN || n > AUTH_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t salt[SALT_LEN], hash[HASH_LEN], secret[SECRET_LEN];
    esp_fill_random(salt, sizeof(salt));
    esp_fill_random(secret, sizeof(secret));
    if (!pbkdf2(password, salt, hash)) {
        return ESP_FAIL;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "salt", salt, sizeof(salt));
        if (err == ESP_OK) {
            err = nvs_set_blob(nvs, "hash", hash, sizeof(hash));
        }
        if (err == ESP_OK) {
            err = nvs_set_blob(nvs, "secret", secret, sizeof(secret));
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Passwort nicht gespeichert: %s", esp_err_to_name(err));
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_salt, salt, sizeof(salt));
    memcpy(s_hash, hash, sizeof(hash));
    memcpy(s_secret, secret, sizeof(secret));
    s_set = true;
    s_fails = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Neues Passwort gesetzt, alle Sitzungen abgemeldet");
    return ESP_OK;
}

auth_result_t auth_check_password(const char *password, int *wait_s)
{
    *wait_s = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until) {
        *wait_s = (int)((s_locked_until - now + 999999) / 1000000);
        xSemaphoreGive(s_lock);
        return AUTH_LOCKED;
    }
    uint8_t salt[SALT_LEN], expected[HASH_LEN];
    memcpy(salt, s_salt, sizeof(salt));
    memcpy(expected, s_hash, sizeof(expected));
    xSemaphoreGive(s_lock);

    uint8_t hash[HASH_LEN];
    bool ok = s_set && pbkdf2(password, salt, hash) && same(hash, expected, HASH_LEN);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) {
        s_fails = 0;
    } else if (++s_fails >= FREE_TRIES) {
        int shift = s_fails - FREE_TRIES;
        int lock_s = shift >= 5 ? LOCK_MAX_S : LOCK_BASE_S << shift;
        if (lock_s > LOCK_MAX_S) {
            lock_s = LOCK_MAX_S;
        }
        s_locked_until = esp_timer_get_time() + (int64_t)lock_s * 1000000;
        ESP_LOGW(TAG, "%d Fehlversuche, Anmeldung für %d s gesperrt", s_fails, lock_s);
    }
    xSemaphoreGive(s_lock);
    return ok ? AUTH_OK : AUTH_WRONG;
}

static void to_hex(char *out, const uint8_t *in, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        snprintf(out + 2 * i, 3, "%02x", in[i]);
    }
}

// Token = Hex(Zufallswert) + Hex(HMAC(Geheimnis, Zufallswert), gekürzt)
static bool token_mac(const uint8_t nonce[NONCE_LEN], uint8_t mac[32])
{
    uint8_t secret[SECRET_LEN];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(secret, s_secret, sizeof(secret));
    xSemaphoreGive(s_lock);
    return hmac(secret, sizeof(secret), nonce, NONCE_LEN, mac);
}

void auth_new_token(char *out)
{
    uint8_t nonce[NONCE_LEN], mac[32];
    esp_fill_random(nonce, sizeof(nonce));
    if (!token_mac(nonce, mac)) {
        out[0] = '\0';
        return;
    }
    to_hex(out, nonce, NONCE_LEN);
    to_hex(out + 2 * NONCE_LEN, mac, AUTH_TOKEN_LEN / 2 - NONCE_LEN);
}

bool auth_token_valid(const char *token)
{
    if (!s_set || strlen(token) != AUTH_TOKEN_LEN) {
        return false;
    }
    uint8_t raw[AUTH_TOKEN_LEN / 2];
    for (int i = 0; i < AUTH_TOKEN_LEN; i++) {
        char c = token[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) {
            return false;
        }
        raw[i / 2] = i % 2 ? raw[i / 2] | v : v << 4;
    }
    uint8_t mac[32];
    return token_mac(raw, mac) && same(mac, raw + NONCE_LEN, sizeof(raw) - NONCE_LEN);
}

esp_err_t auth_erase(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_set = false;
        s_skipped = false;
        memset(s_secret, 0, sizeof(s_secret));
        xSemaphoreGive(s_lock);
    }
    return err;
}
