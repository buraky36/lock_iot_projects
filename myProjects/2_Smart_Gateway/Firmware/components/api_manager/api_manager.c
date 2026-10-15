#include "api_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <string.h>

static const char *TAG = "API_MGR";

#define HTTP_MAX_RECEIVE_BUFFER_SIZE 2048

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        char *buf = (char *)evt->user_data;
        if (buf != NULL) {
            int offset = strlen(buf);
            if (offset + evt->data_len < HTTP_MAX_RECEIVE_BUFFER_SIZE - 1) {
                memcpy(buf + offset, evt->data, evt->data_len);
                buf[offset + evt->data_len] = '\0';
            }
        }
    }
    return ESP_OK;
}

// Encode raw bytes as base64url without padding (RFC 4648 §5)
static void base64url_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    size_t out_len = 0;
    dst[0] = '\0';
    if (mbedtls_base64_encode((unsigned char *)dst, dst_size, &out_len, src, src_len) != 0) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; i < out_len; i++) {
        if (dst[i] == '+') dst[i] = '-';
        else if (dst[i] == '/') dst[i] = '_';
    }
    while (out_len > 0 && dst[out_len - 1] == '=') {
        out_len--;
    }
    dst[out_len] = '\0';
}

// Copy a JSON string field safely (handles missing / non-string items)
static void copy_json_string(const cJSON *obj, const char *key, char *dst, size_t dst_size)
{
    const char *val = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    if (val != NULL) {
        strncpy(dst, val, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

// Gateway Protocol v1.2 (Section 2 Step 5): POST /v1/gateways/claim
esp_err_t api_manager_claim_gateway(const char *device_id, const char *device_code, const uint8_t *provisioning_token, api_claim_response_t *out_response)
{
    esp_err_t ret = ESP_FAIL;
    char url[128];
    snprintf(url, sizeof(url), "%s/v1/gateways/claim", API_BASE_URL);

    cJSON *req_json = cJSON_CreateObject();
    if (device_id && strlen(device_id) > 0) {
        cJSON_AddStringToObject(req_json, "deviceId", device_id);
    }
    cJSON_AddStringToObject(req_json, "deviceCode", device_code);
    cJSON_AddStringToObject(req_json, "firmwareVersion", "1.3.0");
    bool token_has_data = false;
    if (provisioning_token) {
        for (int i = 0; i < 32; i++) {
            if (provisioning_token[i] != 0) {
                token_has_data = true;
                break;
            }
        }
    }
    if (token_has_data) {
        char prov_b64url[64] = {0};
        base64url_encode(provisioning_token, 32, prov_b64url, sizeof(prov_b64url));
        cJSON_AddStringToObject(req_json, "provisioningToken", prov_b64url);
    }
    
    char *post_data = cJSON_PrintUnformatted(req_json);
    cJSON_Delete(req_json);

    char *resp_buf = malloc(HTTP_MAX_RECEIVE_BUFFER_SIZE);
    memset(resp_buf, 0, HTTP_MAX_RECEIVE_BUFFER_SIZE);

    esp_http_client_config_t config = {
        .url = url,
        //.crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = _http_event_handler,
        .user_data = resp_buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Claim POST Status: %d, Response: %s", status, resp_buf);
        
        if (status == 200 || status == 201) {
            cJSON *resp_json = cJSON_Parse(resp_buf);
            if (resp_json) {
                cJSON *mqtt = cJSON_GetObjectItem(resp_json, "mqtt");
                cJSON *auth_token = cJSON_GetObjectItem(resp_json, "gatewayAuthToken");
                if (!auth_token) {
                    auth_token = cJSON_GetObjectItem(resp_json, "deviceSecret");
                }
                cJSON *session_epoch = cJSON_GetObjectItem(resp_json, "sessionEpoch");
                
                if (cJSON_IsObject(mqtt)) {
                    copy_json_string(mqtt, "host", out_response->mqtt_uri, sizeof(out_response->mqtt_uri));
                    out_response->mqtt_port = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(mqtt, "port"));
                    copy_json_string(mqtt, "clientId", out_response->device_id, sizeof(out_response->device_id));
                    copy_json_string(mqtt, "username", out_response->mqtt_username, sizeof(out_response->mqtt_username));
                    copy_json_string(mqtt, "password", out_response->mqtt_password, sizeof(out_response->mqtt_password));
                    if (auth_token && cJSON_IsString(auth_token)) {
                        copy_json_string(resp_json, auth_token->string, out_response->device_secret, sizeof(out_response->device_secret));
                    }
                    
                    if (cJSON_IsNumber(session_epoch)) {
                        out_response->session_epoch = (uint16_t)session_epoch->valueint;
                    } else {
                        out_response->session_epoch = 1;
                    }
                    ret = ESP_OK;
                } else {
                    ESP_LOGE(TAG, "Claim response missing 'mqtt' object");
                }
                cJSON_Delete(resp_json);
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }

    free(resp_buf);
    free(post_data);
    esp_http_client_cleanup(client);
    return ret;
}

esp_err_t api_manager_sync_local_key(const char *device_id, const char *auth_token, const char *local_key_b64)
{
    esp_err_t ret = ESP_FAIL;
    char url[256];
    snprintf(url, sizeof(url), "%s/v1/gateways/%s/local-key", API_BASE_URL, device_id);

    cJSON *req_json = cJSON_CreateObject();
    cJSON_AddStringToObject(req_json, "localKey", local_key_b64);
    char *post_data = cJSON_PrintUnformatted(req_json);
    cJSON_Delete(req_json);

    esp_http_client_config_t config = {
        .url = url,
        //.crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", auth_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Sync Key POST Status: %d", status);
        if (status == 200 || status == 201 || status == 204) {
            ret = ESP_OK;
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }

    free(post_data);
    esp_http_client_cleanup(client);
    return ret;
}

esp_err_t api_manager_fetch_device_key(const char *sub_device_id, const char *gateway_auth_token, char *out_local_key_b64, size_t out_len)
{
    esp_err_t ret = ESP_FAIL;
    char url[256];
    snprintf(url, sizeof(url), "%s/v1/devices/%s/local-key", API_BASE_URL, sub_device_id);

    char *resp_buf = malloc(HTTP_MAX_RECEIVE_BUFFER_SIZE);
    memset(resp_buf, 0, HTTP_MAX_RECEIVE_BUFFER_SIZE);

    esp_http_client_config_t config = {
        .url = url,
        //.crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = _http_event_handler,
        .user_data = resp_buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", gateway_auth_token);
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Fetch Key GET Status: %d, Response: %s", status, resp_buf);
        
        if (status == 200) {
            cJSON *resp_json = cJSON_Parse(resp_buf);
            if (resp_json) {
                cJSON *key_item = cJSON_GetObjectItem(resp_json, "localKey");
                if (key_item && cJSON_IsString(key_item)) {
                    strncpy(out_local_key_b64, cJSON_GetStringValue(key_item), out_len - 1);
                    ret = ESP_OK;
                }
                cJSON_Delete(resp_json);
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
    }

    free(resp_buf);
    esp_http_client_cleanup(client);
    return ret;
}
