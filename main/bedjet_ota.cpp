#include "bedjet_ota.h"
#include "esp_log.h"
#include <algorithm>

static const char* TAG = "bedjet_ota";
namespace bedjet {

const char* BedjetOTA::TAG = "bedjet_ota";
const std::vector<std::string> SUPPORTED_TARGETS = {
    "esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6", "esp32h2"
};

BedjetOTA::BedjetOTA(const char* owner, const char* repo)
    : owner_(owner), repo_(repo) {
    supported_targets_ = SUPPORTED_TARGETS;
}

void BedjetOTA::set_current_version(const char* semver) {
    current_version_ = semver ? semver : "unknown";
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());
}

std::string BedjetOTA::get_api_url() const {
    return std::string("https://api.github.com/repos/") + owner_ + "/" + repo_;
}

std::string BedjetOTA::get_latest_release_url() const {
    return get_api_url() + "/releases/latest";
}

esp_err_t BedjetOTA::http_get(const char* url, std::string& response) {
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .user_agent = "BedJetMatter/1.0",
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int len = esp_http_client_get_content_length(client);
    response.reserve(len + 1);

    char buffer[512];
    int read_len;
    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        response.append(buffer, read_len);
    }

    esp_http_client_cleanup(client);
    return ESP_OK;
}

GitHubRelease BedjetOTA::parse_release_json(const char* json) {
    GitHubRelease rel;

    if (!json) return rel;

    const char* tag_search = "\"tag_name\":";
    const char* tag_pos = strstr(json, tag_search);
    if (tag_pos) {
        tag_pos += strlen(tag_search);
        const char* tag_end = strchr(tag_pos, '\"');
        if (tag_end) {
            rel.tag_name = std::string(tag_pos, tag_end - tag_pos);
        }
    }

    const char* url_search = "\"browser_download_url\":";
    const char* search_pos = strstr(json, url_search);

    while (search_pos) {
        const char* url_start = search_pos + strlen(url_search);
        const char* url_end = strchr(url_start, '\"');

        if (url_end) {
            std::string url(url_start, url_end - url_start);

            for (const auto& target : supported_targets_) {
                if (url.find(target) != std::string::npos) {
                    rel.bin_url = url;
                    rel.valid = true;
                    break;
                }
            }
        }

        search_pos = strstr(search_pos + 1, url_search);
    }

    std::string ver = rel.tag_name;
    if (ver.size() > 0 && ver[0] == 'v') ver = ver.substr(1);

    size_t d1 = ver.find('.');
    size_t d2 = ver.npos;
    if (d1 != std::string::npos) d2 = ver.find('.', d1 + 1);

    if (d1 != std::string::npos && d2 != std::string::npos) {
        try {
            rel.version_major = std::stoi(ver.substr(0, d1));
            rel.version_minor = std::stoi(ver.substr(d1 + 1, d2 - d1 - 1));
            size_t dash = ver.find('-', d2);
            size_t patch_end = dash != std::string::npos ? dash : ver.length();
            rel.version_patch = std::stoi(ver.substr(d2 + 1, patch_end - d2 - 1));
        } catch (...) {}
    }

    return rel;
}

bool BedjetOTA::compare_versions(const char* v1, const char* v2) {
    std::string s1 = v1, s2 = v2;
    if (s1.size() > 0 && s1[0] == 'v') s1 = s1.substr(1);
    if (s2.size() > 0 && s2[0] == 'v') s2 = s2.substr(1);

    auto parse = [](const std::string& v, int& maj, int& min, int& pat) {
        size_t d1 = v.find('.'), d2 = v.npos;
        if (d1 != std::string::npos) d2 = v.find('.', d1 + 1);
        if (d1 != std::string::npos && d2 != std::string::npos) {
            try {
                maj = std::stoi(v.substr(0, d1));
                min = std::stoi(v.substr(d1 + 1, d2 - d1 - 1));
                size_t end = v.find('-', d2);
                if (end == std::string::npos) end = v.length();
                pat = std::stoi(v.substr(d2 + 1, end - d2 - 1));
            } catch (...) { maj = min = pat = 0; }
        }
    };

    int m1, mi1, p1, m2, mi2, p2;
    parse(s1, m1, mi1, p1);
    parse(s2, m2, mi2, p2);

    if (m2 > m1) return true;
    if (m2 < m1) return false;
    if (mi2 > mi1) return true;
    if (mi2 < mi1) return false;
    return p2 > p1;
}

bool BedjetOTA::check_for_release(GitHubRelease& out) {
    std::string json;
    esp_err_t err = http_get(get_latest_release_url().c_str(), json);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to query GitHub API");
        return false;
    }

    out = parse_release_json(json.c_str());

    if (!out.valid || out.bin_url.empty()) {
        ESP_LOGW(TAG, "Could not find compatible firmware");
        return false;
    }

    if (compare_versions(current_version_.c_str(), out.tag_name.c_str())) {
        ESP_LOGI(TAG, "New version: %s -> %s", current_version_.c_str(), out.tag_name.c_str());
        return true;
    }

    ESP_LOGI(TAG, "Already on latest version");
    return false;
}

esp_err_t BedjetOTA::trigger_ota(const char* bin_url) {
    if (!bin_url) {
        ESP_LOGE(TAG, "No binary URL provided");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting OTA from %s", bin_url);

    esp_https_ota_config_t config = {
        .cert_pem = nullptr,
        .timeout_ms = 120000,
        .do_not_reboot = false,
    };

    esp_err_t err = esp_https_ota(bin_url, &config);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful! Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return err;
    }
}

} // namespace bedjet
