#include <cstdlib>
#include <iterator>
#include <string>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/SharedPtr.h>
#include <Poco/URI.h>
#include <Poco/StringTokenizer.h>
#include <Poco/String.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>

#include "privmx/drv/net.h"

struct privmxDrvNet_Http {
    Poco::SharedPtr<Poco::Net::HTTPClientSession> session;
    Poco::URI uri;
};

struct privmxDrvNet_Ws {
    Poco::SharedPtr<Poco::Net::WebSocket> websocket;
    std::mutex sendMutex;
    std::atomic_bool connected;
    std::mutex pingMutex;
    std::condition_variable pingCv;
    std::mutex exitPingLoopMutex;
    std::condition_variable exitPingLoopCv;
    std::thread processIncomingDataThread;
    std::thread pingThread;
    static const std::chrono::seconds PING_INTERVAL;
    static const std::chrono::seconds PING_TIMEOUT;
    void(*onopen)(void* ctx);
    void(*onmessage)(void* ctx, const char* msg, int msglen);
    void(*onerror)(void* ctx, const char* msg, int msglen);
    void(*onclose)(void* ctx, int wasClean);
    void* ctx;
    privmxDrvNet_Ws(Poco::SharedPtr<Poco::Net::WebSocket> websocket);
    ~privmxDrvNet_Ws();
    void start();
    void send(const char* data, int datalen);
    void processIncomingDataLoop();
    void pingLoop();
    void tryJoinThreads();
    void disconnect();
};
const std::chrono::seconds privmxDrvNet_Ws::PING_INTERVAL{10};
const std::chrono::seconds privmxDrvNet_Ws::PING_TIMEOUT{3};

static std::unordered_map<std::string, std::string> privmxDrvNet_config;

privmxDrvNet_Ws::privmxDrvNet_Ws(Poco::SharedPtr<Poco::Net::WebSocket> websocket) : websocket(websocket) {}

privmxDrvNet_Ws::~privmxDrvNet_Ws() {
    disconnect();
}

void privmxDrvNet_Ws::send(const char* data, int datalen) {
    std::lock_guard lock(sendMutex);
    try {
        websocket->sendFrame(data, datalen, Poco::Net::WebSocket::FRAME_BINARY);
    } catch(...) {
        connected = false;
        throw;
    }
}

void privmxDrvNet_Ws::disconnect() {
    try {
        if (connected) {
            std::lock_guard lock2(sendMutex);
            websocket->shutdown();
        }
    } catch(...) {}
    tryJoinThreads();
}

void privmxDrvNet_Ws::start() {
    connected = true;
    processIncomingDataThread = std::thread(&privmxDrvNet_Ws::processIncomingDataLoop, this);
    pingThread = std::thread(&privmxDrvNet_Ws::pingLoop, this);
}

void privmxDrvNet_Ws::processIncomingDataLoop() {
    try {
        onopen(ctx);
    } catch(...) {}
    bool clean = true;
    try {
        while (true) {
            Poco::Buffer<char> buf(0);
            int flags;
            if (!connected) break;
            int n = websocket->receiveFrame(buf, flags);
            if (n == 0 && flags == 0) {
                break;
            }
            auto opcode = (flags & Poco::Net::WebSocket::FRAME_OP_BITMASK);
            if (opcode == Poco::Net::WebSocket::FRAME_OP_PING) {
                std::lock_guard lock(sendMutex);
                if (!connected) break;
                websocket->sendFrame("", 0, Poco::Net::WebSocket::FRAME_FLAG_FIN | Poco::Net::WebSocket::FRAME_OP_PONG);
                continue;
            }
            if (opcode == Poco::Net::WebSocket::FRAME_OP_PONG) {
                pingCv.notify_one();
                continue;
            }
            if (n < 4) {
                continue;
            }
            std::string payload(buf.begin(), buf.begin() + n);
            try {
                onmessage(ctx, payload.data(), payload.size());
            } catch(...) {}
        }
    } catch (...) {
        clean = false;
    }
    connected = false;
    exitPingLoopCv.notify_all();
    pingCv.notify_all();
    if (!clean) {
        try {
            onerror(ctx, "", 0);
        } catch (...) {}
    }
    try {
        onclose(ctx, clean);
    } catch (...) {}
}

void privmxDrvNet_Ws::pingLoop() {
    try {
        std::unique_lock lock(exitPingLoopMutex);
        while (true) {
            if (!connected) return;
            auto wait_status = exitPingLoopCv.wait_for(lock, PING_INTERVAL);
            if (wait_status == std::cv_status::no_timeout) {
                throw ("Ping loop stopped");
            }
            if (!connected) return;
            std::unique_lock lock2(pingMutex);
            {
                std::lock_guard lock3(sendMutex);
                if (!connected) return;
                websocket->sendFrame("", 0, Poco::Net::WebSocket::FRAME_FLAG_FIN | Poco::Net::WebSocket::FRAME_OP_PING);
            }
            auto status = pingCv.wait_for(lock2, PING_TIMEOUT);
            if (status == std::cv_status::timeout) {
                throw ("Ping timeout");
            }
        }
    } catch (...) {
        try {
            std::lock_guard lock(sendMutex);
            websocket->close();
            connected = false;
        } catch (...) {}
    }
}

void privmxDrvNet_Ws::tryJoinThreads() {
    if (processIncomingDataThread.joinable()) {
        try {
            processIncomingDataThread.join();
        } catch (...) {}
    }
    if (pingThread.joinable()) {
        try {
            pingThread.join();
        } catch (...) {}
    }
}

Poco::SharedPtr<Poco::Net::HTTPClientSession> createHttpSession(const Poco::URI& uri, const std::string& caCertPath) {
    if (uri.getScheme() == "https") {
        if (caCertPath.empty()) {
            return new Poco::Net::HTTPSClientSession(uri.getHost(), uri.getPort());
        } else {
            Poco::Net::Context::Ptr context = new Poco::Net::Context(Poco::Net::Context::TLS_CLIENT_USE, caCertPath, Poco::Net::Context::VERIFY_RELAXED, 9, false);
            return new Poco::Net::HTTPSClientSession(uri.getHost(), uri.getPort(), context);
        }
    }
    return new Poco::Net::HTTPClientSession(uri.getHost(), uri.getPort());
}

privmxDrvNet_Http* createHttp(const privmxDrvNet_HttpOptions* options) {
    Poco::URI uri(options->baseUrl);
    std::string caCertPath = privmxDrvNet_config["caCertPath"];
    return new privmxDrvNet_Http{createHttpSession(uri, caCertPath), uri};
}

privmxDrvNet_Ws* createWs(const privmxDrvNet_WsOptions* options) {
    Poco::URI uri(options->url);
    std::string caCertPath = privmxDrvNet_config["caCertPath"];
    Poco::SharedPtr<Poco::Net::HTTPClientSession> session = createHttpSession(uri, caCertPath);
    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, uri.getPathAndQuery());
    Poco::Net::HTTPResponse response;
    Poco::SharedPtr<Poco::Net::WebSocket> websocket = new Poco::Net::WebSocket(*session, request, response);
    return new privmxDrvNet_Ws{websocket};
}

std::string getMethod(const char* method) {
    if (method == NULL) {
        return Poco::Net::HTTPRequest::HTTP_POST;
    }
    if (strcmp(method, "POST") == 0) {
        return Poco::Net::HTTPRequest::HTTP_POST;
    }
    if (strcmp(method, "GET") == 0) {
        return Poco::Net::HTTPRequest::HTTP_GET;
    }
    return Poco::Net::HTTPRequest::HTTP_POST;
}


int privmxDrvNet_version(unsigned int* version) {
    *version = 1;
    return 0;
}

int privmxDrvNet_setConfig(const char* config) {
    try {
        std::unordered_map<std::string, std::string> result;
        Poco::StringTokenizer items(config, ";");
        for (auto& item : items) {
            std::size_t pos = item.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            std::string key = Poco::trim(item.substr(0, pos));
            std::string value = Poco::trim(item.substr(pos + 1));
            result.emplace(key, value);
        }
        privmxDrvNet_config = result;
        return 0;
    } catch (...) {}
    return 1;
}

int privmxDrvNet_httpCreateSession(const privmxDrvNet_HttpOptions* options, privmxDrvNet_Http** res) {
    try {
        privmxDrvNet_Http* http = createHttp(options);
        http->session->setKeepAlive(options->keepAlive);
        *res = http;
        return 0;
    } catch(...) {}
    return 1;
}

int privmxDrvNet_httpDestroySession(privmxDrvNet_Http* http) {
    try {
        http->session->reset();
        return 0;
    } catch(...) {}
    return 1;
}

int privmxDrvNet_httpFree(privmxDrvNet_Http* http) {
    delete http;
    return 0;
}

int privmxDrvNet_httpRequest(privmxDrvNet_Http* http, const char* data, int datalen, const privmxDrvNet_HttpRequestOptions* options, int* statusCode, char** out, unsigned int* outlen) {
    try {
        Poco::Net::HTTPRequest request(getMethod(options->method), options->path);
        request.setKeepAlive(options->keepAlive);
        request.setContentLength(datalen);
        request.setContentType(options->contentType != NULL ? options->contentType : "application/octet-stream");
        for (int i = 0; i < options->headerslen; ++i) {
            request.set(options->headers[i].name, options->headers[i].value);
        }
        std::ostream& os = http->session->sendRequest(request);
        os.write(data, datalen).flush();
        Poco::Net::HTTPResponse response;
        std::istream& stream = http->session->receiveResponse(response);
        std::string result{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        char* buf = reinterpret_cast<char*>(malloc(result.size()));
        memcpy(buf, result.data(), result.size());
        *statusCode = response.getStatus();
        *out = buf;
        *outlen = result.size();
        return 0;
    } catch (const Poco::Net::NetException& e) {
        return 2;
    } catch (const Poco::TimeoutException& e) {
        return 3;
    } catch (...) {
        return 4;
    }
    return 1;
}

int privmxDrvNet_wsConnect(const privmxDrvNet_WsOptions* options, void(*onopen)(void* ctx), void(*onmessage)(void* ctx, const char* msg, int msglen), void(*onerror)(void* ctx, const char* msg, int msglen), void(*onclose)(void* ctx, int wasClean), void* ctx, privmxDrvNet_Ws** res) {
    try {
        privmxDrvNet_Ws* ws = createWs(options);
        ws->onopen = onopen;
        ws->onmessage = onmessage;
        ws->onerror = onerror;
        ws->onclose = onclose;
        ws->ctx = ctx;
        ws->start();
        *res = ws;
        return 0;
    } catch(...) {}
    return 1;
}

int privmxDrvNet_wsClose(privmxDrvNet_Ws* ws) {
    try {
        ws->disconnect();
        return 0;
    } catch(...) {}
    return 1;
}

int privmxDrvNet_wsFree(privmxDrvNet_Ws* ws) {
    delete ws;
    return 0;
}

int privmxDrvNet_wsSend(privmxDrvNet_Ws* ws, const char* data, int datalen) {
    try {
        ws->send(data, datalen);
        return 0;
    } catch(...) {}
    return 1;
}

int privmxDrvNet_freeMem(void* ptr) {
    free(ptr);
    return 0;
}
