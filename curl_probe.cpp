#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Config {
    std::string url;
    std::string userAgent = "AAMP-DNS-Lab/1.0";
    std::string proxy;
    std::vector<std::string> headers;
    long timeout = 20;
    long connectTimeout = 10;
    long dnsCache = 180;
    long startTimeout = 0;
    long stallTimeout = 10;
    long retries = 1;
    long retry502 = 10;
    long stopAfterMs = -1;
    bool verifyPeer = false;
    bool verbose = false;
};

struct State {
    Config config;
    Clock::time_point origin = Clock::now();
    Clock::time_point attemptStart;
    Clock::time_point updated;
    std::atomic<bool> active{true};
    std::mutex logMutex;
    std::mutex doneMutex;
    std::condition_variable doneCondition;
    bool done = false;
    bool cancelled = false;
    std::size_t bytes = 0;
    int result = 2;
};

static double milliseconds(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

static void event(State& state, const std::string& message) {
    std::lock_guard<std::mutex> guard(state.logMutex);
    std::cout << std::fixed << std::setprecision(3)
              << "elapsed_ms=" << milliseconds(state.origin) << ' '
              << message << std::endl;
}

static long number(const std::string& text, long minimum, long maximum) {
    std::size_t consumed = 0;
    long value = std::stol(text, &consumed);
    if (consumed != text.size() || value < minimum || value > maximum) {
        throw std::invalid_argument("numeric option out of range: " + text);
    }
    return value;
}

static Config arguments(int argc, char** argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        std::string option = argv[index];
        if (option == "--help") {
            std::cout << "curl-probe [--url URL] [--timeout SEC] "
                "[--connect-timeout SEC] [--dns-cache SEC]\n"
                "  [--stop-after-ms MS] [--start-timeout SEC] "
                "[--stall-timeout SEC] [--retries N] [--retry-502 N]\n"
                "  [--user-agent TEXT] [--header TEXT] [--proxy URL] "
                "[--verify-peer] [--verbose]\n";
            std::exit(0);
        }
        if (option == "--verify-peer") { config.verifyPeer = true; continue; }
        if (option == "--verbose") { config.verbose = true; continue; }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + option);
        }
        std::string value = argv[++index];
        if (option == "--url") config.url = value;
        else if (option == "--user-agent") config.userAgent = value;
        else if (option == "--header") config.headers.push_back(value);
        else if (option == "--proxy") config.proxy = value;
        else if (option == "--timeout") config.timeout = number(value, 0, 3600);
        else if (option == "--connect-timeout") config.connectTimeout = number(value, 0, 3600);
        else if (option == "--dns-cache") config.dnsCache = number(value, 0, 86400);
        else if (option == "--start-timeout") config.startTimeout = number(value, 0, 3600);
        else if (option == "--stall-timeout") config.stallTimeout = number(value, 0, 3600);
        else if (option == "--retries") config.retries = number(value, 0, 100);
        else if (option == "--retry-502") config.retry502 = number(value, 0, 100);
        else if (option == "--stop-after-ms") config.stopAfterMs = number(value, 0, 3600000);
        else throw std::invalid_argument("unknown option: " + option);
    }
    if (config.url.empty()) {
        throw std::invalid_argument("--url is required; supply your test endpoint");
    }
    return config;
}

static size_t receive(char*, size_t size, size_t count, void* context) {
    auto& state = *static_cast<State*>(context);
    state.bytes += size * count;
    state.updated = Clock::now();
    return size * count;
}

static size_t receiveHeader(char*, size_t size, size_t count, void*) {
    return size * count;
}

static int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto& state = *static_cast<State*>(context);
    if (!state.active.load()) {
        if (!state.cancelled) event(state, "event=progress_abort reason=release_flag");
        state.cancelled = true;
        return 1;
    }
    if (state.bytes == 0 && state.config.startTimeout > 0 &&
        milliseconds(state.attemptStart) >= state.config.startTimeout * 1000.0) {
        event(state, "event=progress_abort reason=start_timeout");
        return 1;
    }
    if (state.bytes > 0 && state.config.stallTimeout > 0 &&
        milliseconds(state.updated) >= state.config.stallTimeout * 1000.0) {
        event(state, "event=progress_abort reason=stall_timeout");
        return 1;
    }
    return 0;
}

template<typename Value>
static void option(CURL* handle, CURLoption name, Value value) {
    CURLcode result = curl_easy_setopt(handle, name, value);
    if (result != CURLE_OK) throw std::runtime_error(curl_easy_strerror(result));
}

static void download(State& state) {
    CURL* handle = nullptr;
    curl_slist* headers = nullptr;
    try {
        handle = curl_easy_init();
        if (!handle) throw std::runtime_error("curl_easy_init failed");
        for (const auto& header : state.config.headers) {
            auto* next = curl_slist_append(headers, header.c_str());
            if (!next) throw std::runtime_error("curl_slist_append failed");
            headers = next;
        }
        const auto& config = state.config;
        option(handle, CURLOPT_URL, config.url.c_str());
        option(handle, CURLOPT_HTTPGET, 1L);
        option(handle, CURLOPT_NOSIGNAL, 1L);
        option(handle, CURLOPT_TIMEOUT, config.timeout);
        option(handle, CURLOPT_CONNECTTIMEOUT, config.connectTimeout);
        option(handle, CURLOPT_DNS_CACHE_TIMEOUT, config.dnsCache);
        option(handle, CURLOPT_IPRESOLVE, static_cast<long>(CURL_IPRESOLVE_WHATEVER));
        option(handle, CURLOPT_FOLLOWLOCATION, 1L);
        option(handle, CURLOPT_ACCEPT_ENCODING, "");
        option(handle, CURLOPT_USERAGENT, config.userAgent.c_str());
        option(handle, CURLOPT_PROXY, config.proxy.c_str());
        option(handle, CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_ANY));
        option(handle, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
        option(handle, CURLOPT_SSL_VERIFYPEER, config.verifyPeer ? 1L : 0L);
        option(handle, CURLOPT_SSL_VERIFYHOST, config.verifyPeer ? 2L : 0L);
        option(handle, CURLOPT_VERBOSE, config.verbose ? 1L : 0L);
        option(handle, CURLOPT_NOPROGRESS, 0L);
        option(handle, CURLOPT_XFERINFOFUNCTION, progress);
        option(handle, CURLOPT_XFERINFODATA, &state);
        option(handle, CURLOPT_WRITEFUNCTION, receive);
        option(handle, CURLOPT_WRITEDATA, &state);
        option(handle, CURLOPT_HEADERFUNCTION, receiveHeader);
        option(handle, CURLOPT_HEADERDATA, &state);
        if (headers) option(handle, CURLOPT_HTTPHEADER, headers);
        long attempt = 0;
        bool again;
        do {
            ++attempt;
            state.bytes = 0;
            state.attemptStart = state.updated = Clock::now();
            event(state, "event=perform_begin attempt=" + std::to_string(attempt));
            CURLcode result = curl_easy_perform(handle);
            double elapsed = milliseconds(state.attemptStart);
            long status = 0;
            double lookup = 0, connect = 0, total = 0;
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_getinfo(handle, CURLINFO_NAMELOOKUP_TIME, &lookup);
            curl_easy_getinfo(handle, CURLINFO_CONNECT_TIME, &connect);
            curl_easy_getinfo(handle, CURLINFO_TOTAL_TIME, &total);
            event(state, "event=perform_return attempt=" + std::to_string(attempt) +
                " curl_code=" + std::to_string(result) + " http=" + std::to_string(status) +
                " wall_ms=" + std::to_string(elapsed) +
                " namelookup_s=" + std::to_string(lookup) +
                " connect_s=" + std::to_string(connect) +
                " curl_total_s=" + std::to_string(total));
            long retries = status == 502 ? config.retry502 : config.retries;
            if (status == 408 && retries == 0) retries = 1;
            again = attempt <= retries &&
                ((result == CURLE_OK && (status == 408 || status >= 500)) ||
                 result == CURLE_OPERATION_TIMEDOUT || result == CURLE_COULDNT_CONNECT ||
                 result == CURLE_SEND_ERROR);
            if (again && result == CURLE_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(status == 502 ? 1000 : 50));
            }
            state.result = result == CURLE_OK && status >= 200 && status < 300 ? 0 : 1;
        } while (again);
    } catch (const std::exception& error) {
        event(state, std::string("event=error message=") + error.what());
        state.result = 2;
    }
    event(state, "event=cleanup_begin");
    if (handle) curl_easy_cleanup(handle);
    curl_slist_free_all(headers);
    event(state, "event=worker_exit");
    {
        std::lock_guard<std::mutex> guard(state.doneMutex);
        state.done = true;
    }
    state.doneCondition.notify_one();
}

int main(int argc, char** argv) {
    State state;
    try { state.config = arguments(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 2;
    const auto* version = curl_version_info(CURLVERSION_NOW);
    event(state, std::string("event=runtime libcurl=") + version->version +
        " asynch_dns=" + ((version->features & CURL_VERSION_ASYNCHDNS) ? "yes" : "no") +
        " c_ares=" + (version->ares ? version->ares : "none") +
        " timeout_s=" + std::to_string(state.config.timeout) +
        " connect_timeout_s=" + std::to_string(state.config.connectTimeout) +
        " dns_cache_s=" + std::to_string(state.config.dnsCache) + " nosignal=1");
    std::thread worker(download, std::ref(state));
    if (state.config.stopAfterMs >= 0) {
        std::unique_lock<std::mutex> lock(state.doneMutex);
        if (!state.doneCondition.wait_for(lock,
                std::chrono::milliseconds(state.config.stopAfterMs), [&state] { return state.done; })) {
            event(state, "event=release_requested");
            state.active.store(false);
        } else {
            event(state, "event=completed_before_release");
        }
    }
    auto joinStart = Clock::now();
    event(state, "event=join_begin");
    worker.join();
    event(state, "event=join_end wait_ms=" + std::to_string(milliseconds(joinStart)));
    curl_global_cleanup();
    return state.result;
}