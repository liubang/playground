#include <curl/curl.h>
#include <iostream>

int main(int argc, char* argv[]) {
    CURL* curl = curl_easy_init();

    if (curl == nullptr) {
        std::cerr << "Failed to initialize CURL\n";
        return 1;
    }

    const char* url = "https://baidu.com";
    const char* json = "{}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "CURL Error: " << curl_easy_strerror(res) << "\n";
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return 0;
}
