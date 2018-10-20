// Copyright (c) 2015-2016 The Bitcoin Core developers
// Copyright (c) 2018-2022 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <httpserver.h>

#include <chainparamsbase.h>
#include <compat.h>
#include <config.h>
#include <logging.h>
#include <netbase.h>
#include <rpc/protocol.h> // For HTTP status codes
#include <shutdown.h>
#include <sync.h>
#include <ui_interface.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/system.h>
#include <util/threadnames.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <support/events.h>

#ifdef EVENT__HAVE_NETINET_IN_H
#include <netinet/in.h>
#ifdef _XOPEN_SOURCE_EXTENDED
#include <arpa/inet.h>
#endif
#endif

#include <sys/stat.h>
#include <sys/types.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>

/** Maximum size of http request (request line + headers) */
static const size_t MAX_HEADERS_SIZE = 8192;

/**
 * Maximum HTTP post body size. Twice the maximum block size is added to this
 * value in practice.
 */
static const size_t MIN_SUPPORTED_BODY_SIZE = 0x02000000;

/** HTTP request work item */
class HTTPWorkItem final : public HTTPClosure {
public:
    HTTPWorkItem(Config &_config, std::unique_ptr<HTTPRequest> _req,
                 const std::string &_path, const HTTPRequestHandler &_func)
        : req(std::move(_req)), path(_path), func(_func), config(&_config) {}

    void operator()() override { func(*config, req.get(), path); }

    std::unique_ptr<HTTPRequest> req;

private:
    std::string path;
    HTTPRequestHandler func;
    Config *config;
};

/**
 * Simple work queue for distributing work over multiple threads.
 * Work items are simply callable objects.
 */
template <typename WorkItem> class WorkQueue {
private:
    /** Mutex protects entire object */
    Mutex cs;
    std::condition_variable cond;
    std::deque<std::unique_ptr<WorkItem>> queue;
    bool running;
    size_t maxDepth;

public:
    explicit WorkQueue(size_t _maxDepth) : running(true), maxDepth(_maxDepth) {}
    /**
     * Precondition: worker threads have all stopped (they have all been joined)
     */
    ~WorkQueue() {}

    /** Enqueue a work item */
    bool Enqueue(WorkItem *item) {
        LOCK(cs);
        if (queue.size() >= maxDepth) {
            return false;
        }
        queue.emplace_back(std::unique_ptr<WorkItem>(item));
        cond.notify_one();
        return true;
    }

    /** Thread function */
    void Run() {
        while (true) {
            std::unique_ptr<WorkItem> i;
            {
                WAIT_LOCK(cs, lock);
                while (running && queue.empty()) {
                    cond.wait(lock);
                }
                if (!running) {
                    break;
                }
                i = std::move(queue.front());
                queue.pop_front();
            }
            (*i)();
        }
    }

    /** Interrupt and exit loops */
    void Interrupt() {
        LOCK(cs);
        running = false;
        cond.notify_all();
    }
};

struct HTTPPathHandler {
    HTTPPathHandler(const std::string &_prefix, bool _exactMatch,
                    const HTTPRequestHandler &_handler)
        : prefix(_prefix), exactMatch(_exactMatch), handler(_handler) {}
    std::string prefix;
    bool exactMatch;
    HTTPRequestHandler handler;
};

/** HTTP module state */

//! libevent event loop
static struct event_base *eventBase = nullptr;
//! HTTP server
struct evhttp *eventHTTP = nullptr;
//! List of subnets to allow RPC connections from
static std::vector<CSubNet> rpc_allow_subnets;
//! Work queue for handling longer requests off the event loop thread
static WorkQueue<HTTPClosure> *workQueue = nullptr;
//! Handlers for (sub)paths
std::vector<HTTPPathHandler> pathHandlers;
//! Bound listening sockets
std::vector<evhttp_bound_socket *> boundSockets;

/** Check if a network address is allowed to access the HTTP server */
static bool ClientAllowed(const CNetAddr &netaddr) {
    if (!netaddr.IsValid()) {
        return false;
    }
    for (const CSubNet &subnet : rpc_allow_subnets) {
        if (subnet.Match(netaddr)) {
            return true;
        }
    }
    return false;
}

/** Initialize ACL list for HTTP server */
static bool InitHTTPAllowList() {
    rpc_allow_subnets.clear();
    CNetAddr localv4;
    CNetAddr localv6;
    LookupHost("127.0.0.1", localv4, false);
    LookupHost("::1", localv6, false);
    // always allow IPv4 local subnet.
    rpc_allow_subnets.push_back(CSubNet(localv4, 8));
    // always allow IPv6 localhost.
    rpc_allow_subnets.push_back(CSubNet(localv6));
    for (const std::string &strAllow : gArgs.GetArgs("-rpcallowip")) {
        CSubNet subnet;
        LookupSubNet(strAllow, subnet);
        if (!subnet.IsValid()) {
            uiInterface.ThreadSafeMessageBox(
                strprintf("Invalid -rpcallowip subnet specification: %s. "
                          "Valid are a single IP (e.g. 1.2.3.4), a "
                          "network/netmask (e.g. 1.2.3.4/255.255.255.0) or a "
                          "network/CIDR (e.g. 1.2.3.4/24).",
                          strAllow),
                "", CClientUIInterface::MSG_ERROR);
            return false;
        }
        rpc_allow_subnets.push_back(subnet);
    }
    std::string strAllowed;
    for (const CSubNet &subnet : rpc_allow_subnets) {
        strAllowed += subnet.ToString() + " ";
    }
    LogPrint(BCLog::HTTP, "Allowing HTTP connections from: %s\n", strAllowed);
    return true;
}

/** HTTP request method as string - use for logging only */
static std::string RequestMethodString(HTTPRequest::RequestMethod m) {
    switch (m) {
        case HTTPRequest::GET:
            return "GET";
        case HTTPRequest::POST:
            return "POST";
        case HTTPRequest::HEAD:
            return "HEAD";
        case HTTPRequest::PUT:
            return "PUT";
        case HTTPRequest::OPTIONS:
            return "OPTIONS";
        default:
            return "unknown";
    }
}

/** HTTP request callback */
static void http_request_cb(struct evhttp_request *req, void *arg) {
    Config &config = *reinterpret_cast<Config *>(arg);

    // Disable reading to work around a libevent bug, fixed in 2.1.9.
    if (event_get_version_number() >= 0x02010600 &&
        event_get_version_number() < 0x02010900) {
        evhttp_connection *conn = evhttp_request_get_connection(req);
        if (conn) {
            bufferevent *bev = evhttp_connection_get_bufferevent(conn);
            if (bev) {
                bufferevent_disable(bev, EV_READ);
            }
        }
    }
    auto hreq = std::make_unique<HTTPRequest>(req);
    const auto peer = hreq->GetPeer();

    // If HTTPTRACE is enabled, log the request immediately
    // Note: Unlike with regular HTTP logging, we *don't* sanitize any strings
    // coming from the user. HTTPTRACE is an advanced debugging option not
    // intended for general use, so it is felt that this is acceptable.
    if (LogAcceptCategory(BCLog::HTTPTRACE)) {
        const auto headersVec = hreq->GetAllInputHeaders();
        const std::string headers = Join(headersVec, "\n", [] (const auto &nvp) {
            return strprintf("%s: %s", nvp.first, nvp.second);
        });
        const std::string content = hreq->ReadBody(false);
        LogPrintf("<httptrace> Request from %s, method: \"%s\", URI: \"%s\", headers: %u, content: %u bytes\n"
                  "--- HEADERS ---\n%s\n--- CONTENT ---\n%s\n",
                  peer.ToString(), RequestMethodString(hreq->GetRequestMethod()), hreq->GetURI(),
                  headersVec.size(), content.size(), headers, content);
    }

    // Early address-based allow check
    if (!ClientAllowed(peer)) {
        LogPrint(BCLog::HTTP, "HTTP request from %s rejected: Client network is not allowed RPC access\n",
                 peer.ToString());
        hreq->WriteReply(HTTP_FORBIDDEN);
        return;
    }

    const auto method = hreq->GetRequestMethod();

    // Early reject unknown HTTP methods
    if (method == HTTPRequest::UNKNOWN) {
        LogPrint(BCLog::HTTP, "HTTP request from %s rejected: Unknown HTTP request method\n", peer.ToString());
        hreq->WriteReply(HTTP_BADMETHOD);
        return;
    }

    const std::string strURI = hreq->GetURI();

    LogPrint(BCLog::HTTP, "Received a %s request for %s from %s\n",
             RequestMethodString(method), SanitizeString(strURI, SAFE_CHARS_URI).substr(0, 100), peer.ToString());

    // Find registered handler for prefix
    std::string path;
    std::vector<HTTPPathHandler>::const_iterator i = pathHandlers.begin();
    std::vector<HTTPPathHandler>::const_iterator iend = pathHandlers.end();
    for (; i != iend; ++i) {
        bool match = false;
        if (i->exactMatch) {
            match = (strURI == i->prefix);
        } else {
            match = (strURI.substr(0, i->prefix.size()) == i->prefix);
        }
        if (match) {
            path = strURI.substr(i->prefix.size());
            break;
        }
    }

    // Dispatch to worker thread.
    if (i != iend) {
        std::unique_ptr<HTTPWorkItem> item(
            new HTTPWorkItem(config, std::move(hreq), path, i->handler));
        assert(workQueue);
        if (workQueue->Enqueue(item.get())) {
            /* if true, queue took ownership */
            item.release();
        } else {
            LogPrintf("WARNING: request rejected because http work queue depth "
                      "exceeded, it can be increased with the -rpcworkqueue= "
                      "setting\n");
            item->req->WriteReply(HTTP_INTERNAL, "Work queue depth exceeded");
        }
    } else {
        hreq->WriteReply(HTTP_NOTFOUND);
    }
}

/** Callback to reject HTTP requests after shutdown. */
static void http_reject_request_cb(struct evhttp_request *req, void *) {
    LogPrint(BCLog::HTTP, "Rejecting request while shutting down\n");
    evhttp_send_error(req, HTTP_SERVUNAVAIL, nullptr);
}

/** Event dispatcher thread */
static bool ThreadHTTP(struct event_base* base)
{
    util::ThreadRename("http");
    LogPrint(BCLog::HTTP, "Entering http event loop\n");
    event_base_dispatch(base);
    // Event loop will be interrupted by InterruptHTTPServer()
    LogPrint(BCLog::HTTP, "Exited http event loop\n");
    return event_base_got_break(base) == 0;
}

/** Bind HTTP server to specified addresses */
static bool HTTPBindAddresses(struct evhttp *http) {
    int http_port = gArgs.GetArg("-rpcport", BaseParams().RPCPort());
    std::vector<std::pair<std::string, uint16_t>> endpoints;

    // Determine what addresses to bind to
    if (!(gArgs.IsArgSet("-rpcallowip") && gArgs.IsArgSet("-rpcbind"))) {
        // Default to loopback if not allowing external IPs
        endpoints.push_back(std::make_pair("::1", http_port));
        endpoints.push_back(std::make_pair("127.0.0.1", http_port));
        if (gArgs.IsArgSet("-rpcallowip")) {
            LogPrintf("WARNING: option -rpcallowip was specified without -rpcbind; this doesn't usually make sense\n");
        }
        if (gArgs.IsArgSet("-rpcbind")) {
            LogPrintf("WARNING: option -rpcbind was ignored because "
                      "-rpcallowip was not specified, refusing to allow "
                      "everyone to connect\n");
        }
    } else if (gArgs.IsArgSet("-rpcbind")) {
        // Specific bind address.
        for (const std::string &strRPCBind : gArgs.GetArgs("-rpcbind")) {
            int port = http_port;
            std::string host;
            SplitHostPort(strRPCBind, port, host);
            endpoints.push_back(std::make_pair(host, port));
        }
    }

    // Bind addresses
    for (std::vector<std::pair<std::string, uint16_t>>::iterator i =
             endpoints.begin();
         i != endpoints.end(); ++i) {
        LogPrint(BCLog::HTTP, "Binding RPC on address %s port %i\n", i->first,
                 i->second);
        evhttp_bound_socket *bind_handle = evhttp_bind_socket_with_handle(
            http, i->first.empty() ? nullptr : i->first.c_str(), i->second);
        if (bind_handle) {
            boundSockets.push_back(bind_handle);
        } else {
            LogPrintf("Binding RPC on address %s port %i failed.\n", i->first,
                      i->second);
        }
    }
    return !boundSockets.empty();
}

/** Simple wrapper to set thread name and run work queue */
static void HTTPWorkQueueRun(WorkQueue<HTTPClosure>* queue, int worker_num)
{
    util::ThreadRename(strprintf("httpworker.%i", worker_num));
    queue->Run();
}

/** libevent event log callback */
static void libevent_log_cb(int severity, const char *msg) {
#ifndef EVENT_LOG_WARN
// EVENT_LOG_WARN was added in 2.0.19; but before then _EVENT_LOG_WARN existed.
#define EVENT_LOG_WARN _EVENT_LOG_WARN
#endif
    // Log warn messages and higher without debug category.
    if (severity >= EVENT_LOG_WARN) {
        LogPrintf("libevent: %s\n", msg);
    } else {
        LogPrint(BCLog::LIBEVENT, "libevent: %s\n", msg);
    }
}

bool InitHTTPServer(Config &config) {
    if (!InitHTTPAllowList()) {
        return false;
    }

    // Redirect libevent's logging to our own log
    event_set_log_callback(&libevent_log_cb);
    // Update libevent's log handling. Returns false if our version of
    // libevent doesn't support debug logging, in which case we should
    // clear the BCLog::LIBEVENT flag.
    if (!UpdateHTTPServerLogging(
            LogInstance().WillLogCategory(BCLog::LIBEVENT))) {
        LogInstance().DisableCategory(BCLog::LIBEVENT);
    }

#ifdef WIN32
    evthread_use_windows_threads();
#else
    evthread_use_pthreads();
#endif

    raii_event_base base_ctr = obtain_event_base();

    /* Create a new evhttp object to handle requests. */
    raii_evhttp http_ctr = obtain_evhttp(base_ctr.get());
    struct evhttp *http = http_ctr.get();
    if (!http) {
        LogPrintf("couldn't create evhttp. Exiting.\n");
        return false;
    }

    evhttp_set_timeout(
        http, gArgs.GetArg("-rpcservertimeout", DEFAULT_HTTP_SERVER_TIMEOUT));
    evhttp_set_max_headers_size(http, MAX_HEADERS_SIZE);
    // scale the max body size with our block size so RPC always works for large blocks
    evhttp_set_max_body_size(http, MIN_SUPPORTED_BODY_SIZE +
                                       2 * config.GetExcessiveBlockSize());
    evhttp_set_gencb(http, http_request_cb, &config);

    // Only POST and OPTIONS are supported, but we return HTTP 405 for the
    // others
    evhttp_set_allowed_methods(
        http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD |
                  EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS);

    if (!HTTPBindAddresses(http)) {
        LogPrintf("Unable to bind any endpoint for RPC server\n");
        return false;
    }

    LogPrint(BCLog::HTTP, "Initialized HTTP server\n");
    int workQueueDepth = std::max(
        (long)gArgs.GetArg("-rpcworkqueue", DEFAULT_HTTP_WORKQUEUE), 1L);
    LogPrintf("HTTP: creating work queue of depth %d\n", workQueueDepth);

    workQueue = new WorkQueue<HTTPClosure>(workQueueDepth);
    // transfer ownership to eventBase/HTTP via .release()
    eventBase = base_ctr.release();
    eventHTTP = http_ctr.release();
    return true;
}

bool UpdateHTTPServerLogging(bool enable) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010100
    if (enable) {
        event_enable_debug_logging(EVENT_DBG_ALL);
    } else {
        event_enable_debug_logging(EVENT_DBG_NONE);
    }
    return true;
#else
    // Can't update libevent logging if version < 02010100
    return false;
#endif
}

std::thread threadHTTP;
static std::vector<std::thread> g_thread_http_workers;

void StartHTTPServer() {
    LogPrint(BCLog::HTTP, "Starting HTTP server\n");
    int rpcThreads =
        std::max((long)gArgs.GetArg("-rpcthreads", DEFAULT_HTTP_THREADS), 1L);
    LogPrintf("HTTP: starting %d worker threads\n", rpcThreads);
    threadHTTP = std::thread(ThreadHTTP, eventBase);

    for (int i = 0; i < rpcThreads; i++) {
        g_thread_http_workers.emplace_back(HTTPWorkQueueRun, workQueue, i);
    }
}

void InterruptHTTPServer() {
    LogPrint(BCLog::HTTP, "Interrupting HTTP server\n");
    if (eventHTTP) {
        // Reject requests on current connections
        evhttp_set_gencb(eventHTTP, http_reject_request_cb, nullptr);
    }
    if (workQueue) {
        workQueue->Interrupt();
    }
}

void StopHTTPServer() {
    LogPrint(BCLog::HTTP, "Stopping HTTP server\n");
    if (workQueue) {
        LogPrint(BCLog::HTTP, "Waiting for HTTP worker threads to exit\n");
        for (auto &thread : g_thread_http_workers) {
            thread.join();
        }
        g_thread_http_workers.clear();
        delete workQueue;
        workQueue = nullptr;
    }
    // Unlisten sockets, these are what make the event loop running, which means
    // that after this and all connections are closed the event loop will quit.
    for (evhttp_bound_socket *socket : boundSockets) {
        evhttp_del_accept_socket(eventHTTP, socket);
    }
    boundSockets.clear();
    if (eventBase) {
        LogPrint(BCLog::HTTP, "Waiting for HTTP event thread to exit\n");
        threadHTTP.join();
    }
    if (eventHTTP) {
        evhttp_free(eventHTTP);
        eventHTTP = nullptr;
    }
    if (eventBase) {
        event_base_free(eventBase);
        eventBase = nullptr;
    }
    LogPrint(BCLog::HTTP, "Stopped HTTP server\n");
}

struct event_base *EventBase() {
    return eventBase;
}

static void httpevent_callback_fn(evutil_socket_t, short, void *data) {
    // Static handler: simply call inner handler
    HTTPEvent *self = static_cast<HTTPEvent *>(data);
    self->handler();
    if (self->deleteWhenTriggered) {
        delete self;
    }
}

HTTPEvent::HTTPEvent(struct event_base *base, bool _deleteWhenTriggered,
                     const std::function<void()> &_handler)
    : deleteWhenTriggered(_deleteWhenTriggered), handler(_handler) {
    ev = event_new(base, -1, 0, httpevent_callback_fn, this);
    assert(ev);
}
HTTPEvent::~HTTPEvent() {
    event_free(ev);
}
void HTTPEvent::trigger(struct timeval *tv) {
    if (tv == nullptr) {
        // Immediately trigger event in main thread.
        event_active(ev, 0, 0);
    } else {
        // Trigger after timeval passed.
        evtimer_add(ev, tv);
    }
}
HTTPRequest::HTTPRequest(struct evhttp_request *_req)
    : req(_req), replySent(false) {}
HTTPRequest::~HTTPRequest() {
    if (!replySent) {
        // Keep track of whether reply was sent to avoid request leaks
        LogPrintf("%s: Unhandled request\n", __func__);
        WriteReply(HTTP_INTERNAL, "Unhandled request");
    }
    // evhttpd cleans up the request, as long as a reply was sent.
}

std::optional<std::string> HTTPRequest::GetHeader(const std::string &hdr) const {
    const struct evkeyvalq *headers = evhttp_request_get_input_headers(req);
    assert(headers);
    const char *val = evhttp_find_header(headers, hdr.c_str());
    if (val) {
        return val;
    } else {
        return std::nullopt;
    }
}

std::vector<HTTPRequest::NameValuePair> HTTPRequest::GetAllHeaders(bool input) const {
    std::vector<NameValuePair> ret;
    const evkeyvalq *headers = input ? evhttp_request_get_input_headers(req)
                                     : evhttp_request_get_output_headers(req);
    assert(headers);

    // Note: we would use TAILQ_FOREACH here but that's not always defined on all platforms, so
    // we must do this.
    for (const evkeyval *header = headers->tqh_first; header != nullptr; header = header->next.tqe_next) {
        ret.emplace_back(header->key, header->value);
    }

    return ret;
}

std::string HTTPRequest::ReadBody(bool drain) {
    std::string ret;
    struct evbuffer *buf = evhttp_request_get_input_buffer(req);
    if (!buf) {
        return ret;
    }
    const size_t size = evbuffer_get_length(buf);
    /**
     * Trivial implementation: if this is ever a performance bottleneck,
     * internal copying can be avoided in multi-segment buffers by using
     * evbuffer_peek and an awkward loop. Though in that case, it'd be even
     * better to not copy into an intermediate string but use a stream
     * abstraction to consume the evbuffer on the fly in the parsing algorithm.
     */
    const char *data = (const char *)evbuffer_pullup(buf, size);

    // returns nullptr in case of empty buffer.
    if (!data) {
        return ret;
    }
    ret.assign(data, size);
    if (drain) evbuffer_drain(buf, size);
    return ret;
}

void HTTPRequest::WriteHeader(const std::string &hdr,
                              const std::string &value) {
    struct evkeyvalq *headers = evhttp_request_get_output_headers(req);
    assert(headers);
    evhttp_add_header(headers, hdr.c_str(), value.c_str());
}

/**
 * Closure sent to main thread to request a reply to be sent to a HTTP request.
 * Replies must be sent in the main loop in the main http thread, this cannot be
 * done from worker threads.
 */
void HTTPRequest::WriteReply(int nStatus, const std::string &strReply) {
    assert(!replySent && req);
    if (ShutdownRequested()) {
        WriteHeader("Connection", "close");
    }

    // If HTTPTRACE is enabled, log what we are replying with
    if (LogAcceptCategory(BCLog::HTTPTRACE)) {
        const auto headersVec = GetAllOutputHeaders();
        bool isBinary = false;
        const std::string headers = Join(headersVec, "\n", [&isBinary] (const auto &nvp) {
            const auto & [name, value] = nvp;
            // Set the isBinary flag if we are outputting binary (this is for REST .bin output mode)
            if (!isBinary && name == "Content-Type" && value == "application/octet-stream") isBinary = true;
            return strprintf("%s: %s", nvp.first, nvp.second);
        });
        const char *content_desc = "";
        std::string hexStrReply;
        if (isBinary) {
            // If we are outputting binary (REST .bin mode), we will encode the data as hex first,
            // to keep log files tidy.
            content_desc = " (binary data, hex encoded)";
            hexStrReply = HexStr(strReply);
        }
        const std::string &content = isBinary ? hexStrReply : strReply;
        LogPrintf("<httptrace> Writing reply to %s, status: %d, headers: %u, content: %u bytes\n"
                  "--- HEADERS ---\n%s\n--- CONTENT%s ---\n%s\n",
                  GetPeer().ToString(), nStatus, headersVec.size(), strReply.size(), headers, content_desc, content);
    }

    // Send event to main http thread to send reply message
    struct evbuffer *evb = evhttp_request_get_output_buffer(req);
    assert(evb);
    evbuffer_add(evb, strReply.data(), strReply.size());
    auto req_copy = req;
    HTTPEvent *ev = new HTTPEvent(eventBase, true, [req_copy, nStatus] {
        evhttp_send_reply(req_copy, nStatus, nullptr, nullptr);
        // Re-enable reading from the socket. This is the second part of the
        // libevent workaround above.
        if (event_get_version_number() >= 0x02010600 &&
            event_get_version_number() < 0x02010900) {
            evhttp_connection *conn = evhttp_request_get_connection(req_copy);
            if (conn) {
                bufferevent *bev = evhttp_connection_get_bufferevent(conn);
                if (bev) {
                    bufferevent_enable(bev, EV_READ | EV_WRITE);
                }
            }
        }
    });
    ev->trigger(nullptr);
    replySent = true;
    // transferred back to main thread.
    req = nullptr;
}

CService HTTPRequest::GetPeer() const {
    evhttp_connection *con = evhttp_request_get_connection(req);
    CService peer;
    if (con) {
        // evhttp retains ownership over returned address string
        const char *address = "";
        uint16_t port = 0;
        evhttp_connection_get_peer(con, (char **)&address, &port);
        peer = LookupNumeric(address, port);
    }
    return peer;
}

std::string HTTPRequest::GetURI() const {
    return evhttp_request_get_uri(req);
}

HTTPRequest::RequestMethod HTTPRequest::GetRequestMethod() const {
    switch (evhttp_request_get_command(req)) {
        case EVHTTP_REQ_GET:
            return GET;
        case EVHTTP_REQ_POST:
            return POST;
        case EVHTTP_REQ_HEAD:
            return HEAD;
        case EVHTTP_REQ_PUT:
            return PUT;
        case EVHTTP_REQ_OPTIONS:
            return OPTIONS;
        default:
            return UNKNOWN;
    }
}

void RegisterHTTPHandler(const std::string &prefix, bool exactMatch,
                         const HTTPRequestHandler &handler) {
    LogPrint(BCLog::HTTP, "Registering HTTP handler for %s (exactmatch %d)\n",
             prefix, exactMatch);
    pathHandlers.emplace_back(prefix, exactMatch, handler);
}

void UnregisterHTTPHandler(const std::string &prefix, bool exactMatch) {
    std::vector<HTTPPathHandler>::iterator i = pathHandlers.begin();
    std::vector<HTTPPathHandler>::iterator iend = pathHandlers.end();
    for (; i != iend; ++i) {
        if (i->prefix == prefix && i->exactMatch == exactMatch) {
            break;
        }
    }
    if (i != iend) {
        LogPrint(BCLog::HTTP,
                 "Unregistering HTTP handler for %s (exactmatch %d)\n", prefix,
                 exactMatch);
        pathHandlers.erase(i);
    }
}
