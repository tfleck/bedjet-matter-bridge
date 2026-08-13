#pragma once

#include <string>
#include <vector>
#include <esp_http_client.h>
#include <esp_https_ota.h>

namespace bedjet {

struct GitHubRelease {
    std::string tag_name;
    std::string bin_url;
    std::string sha256_hash;
    int version_major = 0;
    int version_minor = 0;
    int version_patch = 0;
    bool valid = false;
};

class BedjetOTA {
public:
    BedjetOTA(const char* owner, const char* repo);
    void set_current_version(const char* semver);
    bool check_for_release(GitHubRelease& out);
    esp_err_t trigger_ota(const char* bin_url);

private:
    const char* owner_;
    const char* repo_;
    std::string current_version_;
    std::vector<std::string> supported_targets_;

    std::string get_api_url() const;
    std::string get_latest_release_url() const;
    GitHubRelease parse_release_json(const char* json);
    bool compare_versions(const char* v1, const char* v2);
    esp_err_t http_get(const char* url, std::string& response);

    static const char* TAG;
};

} // namespace bedjet
