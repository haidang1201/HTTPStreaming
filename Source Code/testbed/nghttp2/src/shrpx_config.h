/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef SHRPX_CONFIG_H
#define SHRPX_CONFIG_H

#include "shrpx.h"

#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif // HAVE_SYS_SOCKET_H
#include <sys/un.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif // HAVE_NETINET_IN_H
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif // HAVE_ARPA_INET_H
#include <cinttypes>
#include <cstdio>
#include <vector>
#include <memory>
#include <set>

#include <openssl/ssl.h>

#include <ev.h>

#include <nghttp2/nghttp2.h>

#include "shrpx_router.h"
#include "template.h"
#include "http2.h"
#include "network.h"
#include "allocator.h"

using namespace nghttp2;

namespace shrpx {

struct LogFragment;
class ConnectBlocker;
class Http2Session;

namespace ssl {

class CertLookupTree;

} // namespace ssl

constexpr auto SHRPX_OPT_PRIVATE_KEY_FILE =
    StringRef::from_lit("private-key-file");
constexpr auto SHRPX_OPT_PRIVATE_KEY_PASSWD_FILE =
    StringRef::from_lit("private-key-passwd-file");
constexpr auto SHRPX_OPT_CERTIFICATE_FILE =
    StringRef::from_lit("certificate-file");
constexpr auto SHRPX_OPT_DH_PARAM_FILE = StringRef::from_lit("dh-param-file");
constexpr auto SHRPX_OPT_SUBCERT = StringRef::from_lit("subcert");
constexpr auto SHRPX_OPT_BACKEND = StringRef::from_lit("backend");
constexpr auto SHRPX_OPT_FRONTEND = StringRef::from_lit("frontend");
constexpr auto SHRPX_OPT_WORKERS = StringRef::from_lit("workers");
constexpr auto SHRPX_OPT_HTTP2_MAX_CONCURRENT_STREAMS =
    StringRef::from_lit("http2-max-concurrent-streams");
constexpr auto SHRPX_OPT_LOG_LEVEL = StringRef::from_lit("log-level");
constexpr auto SHRPX_OPT_DAEMON = StringRef::from_lit("daemon");
constexpr auto SHRPX_OPT_HTTP2_PROXY = StringRef::from_lit("http2-proxy");
constexpr auto SHRPX_OPT_HTTP2_BRIDGE = StringRef::from_lit("http2-bridge");
constexpr auto SHRPX_OPT_CLIENT_PROXY = StringRef::from_lit("client-proxy");
constexpr auto SHRPX_OPT_ADD_X_FORWARDED_FOR =
    StringRef::from_lit("add-x-forwarded-for");
constexpr auto SHRPX_OPT_STRIP_INCOMING_X_FORWARDED_FOR =
    StringRef::from_lit("strip-incoming-x-forwarded-for");
constexpr auto SHRPX_OPT_NO_VIA = StringRef::from_lit("no-via");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_READ_TIMEOUT =
    StringRef::from_lit("frontend-http2-read-timeout");
constexpr auto SHRPX_OPT_FRONTEND_READ_TIMEOUT =
    StringRef::from_lit("frontend-read-timeout");
constexpr auto SHRPX_OPT_FRONTEND_WRITE_TIMEOUT =
    StringRef::from_lit("frontend-write-timeout");
constexpr auto SHRPX_OPT_BACKEND_READ_TIMEOUT =
    StringRef::from_lit("backend-read-timeout");
constexpr auto SHRPX_OPT_BACKEND_WRITE_TIMEOUT =
    StringRef::from_lit("backend-write-timeout");
constexpr auto SHRPX_OPT_STREAM_READ_TIMEOUT =
    StringRef::from_lit("stream-read-timeout");
constexpr auto SHRPX_OPT_STREAM_WRITE_TIMEOUT =
    StringRef::from_lit("stream-write-timeout");
constexpr auto SHRPX_OPT_ACCESSLOG_FILE = StringRef::from_lit("accesslog-file");
constexpr auto SHRPX_OPT_ACCESSLOG_SYSLOG =
    StringRef::from_lit("accesslog-syslog");
constexpr auto SHRPX_OPT_ACCESSLOG_FORMAT =
    StringRef::from_lit("accesslog-format");
constexpr auto SHRPX_OPT_ERRORLOG_FILE = StringRef::from_lit("errorlog-file");
constexpr auto SHRPX_OPT_ERRORLOG_SYSLOG =
    StringRef::from_lit("errorlog-syslog");
constexpr auto SHRPX_OPT_BACKEND_KEEP_ALIVE_TIMEOUT =
    StringRef::from_lit("backend-keep-alive-timeout");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_WINDOW_BITS =
    StringRef::from_lit("frontend-http2-window-bits");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_WINDOW_BITS =
    StringRef::from_lit("backend-http2-window-bits");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS =
    StringRef::from_lit("frontend-http2-connection-window-bits");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_CONNECTION_WINDOW_BITS =
    StringRef::from_lit("backend-http2-connection-window-bits");
constexpr auto SHRPX_OPT_FRONTEND_NO_TLS =
    StringRef::from_lit("frontend-no-tls");
constexpr auto SHRPX_OPT_BACKEND_NO_TLS = StringRef::from_lit("backend-no-tls");
constexpr auto SHRPX_OPT_BACKEND_TLS_SNI_FIELD =
    StringRef::from_lit("backend-tls-sni-field");
constexpr auto SHRPX_OPT_PID_FILE = StringRef::from_lit("pid-file");
constexpr auto SHRPX_OPT_USER = StringRef::from_lit("user");
constexpr auto SHRPX_OPT_SYSLOG_FACILITY =
    StringRef::from_lit("syslog-facility");
constexpr auto SHRPX_OPT_BACKLOG = StringRef::from_lit("backlog");
constexpr auto SHRPX_OPT_CIPHERS = StringRef::from_lit("ciphers");
constexpr auto SHRPX_OPT_CLIENT = StringRef::from_lit("client");
constexpr auto SHRPX_OPT_INSECURE = StringRef::from_lit("insecure");
constexpr auto SHRPX_OPT_CACERT = StringRef::from_lit("cacert");
constexpr auto SHRPX_OPT_BACKEND_IPV4 = StringRef::from_lit("backend-ipv4");
constexpr auto SHRPX_OPT_BACKEND_IPV6 = StringRef::from_lit("backend-ipv6");
constexpr auto SHRPX_OPT_BACKEND_HTTP_PROXY_URI =
    StringRef::from_lit("backend-http-proxy-uri");
constexpr auto SHRPX_OPT_READ_RATE = StringRef::from_lit("read-rate");
constexpr auto SHRPX_OPT_READ_BURST = StringRef::from_lit("read-burst");
constexpr auto SHRPX_OPT_WRITE_RATE = StringRef::from_lit("write-rate");
constexpr auto SHRPX_OPT_WRITE_BURST = StringRef::from_lit("write-burst");
constexpr auto SHRPX_OPT_WORKER_READ_RATE =
    StringRef::from_lit("worker-read-rate");
constexpr auto SHRPX_OPT_WORKER_READ_BURST =
    StringRef::from_lit("worker-read-burst");
constexpr auto SHRPX_OPT_WORKER_WRITE_RATE =
    StringRef::from_lit("worker-write-rate");
constexpr auto SHRPX_OPT_WORKER_WRITE_BURST =
    StringRef::from_lit("worker-write-burst");
constexpr auto SHRPX_OPT_NPN_LIST = StringRef::from_lit("npn-list");
constexpr auto SHRPX_OPT_TLS_PROTO_LIST = StringRef::from_lit("tls-proto-list");
constexpr auto SHRPX_OPT_VERIFY_CLIENT = StringRef::from_lit("verify-client");
constexpr auto SHRPX_OPT_VERIFY_CLIENT_CACERT =
    StringRef::from_lit("verify-client-cacert");
constexpr auto SHRPX_OPT_CLIENT_PRIVATE_KEY_FILE =
    StringRef::from_lit("client-private-key-file");
constexpr auto SHRPX_OPT_CLIENT_CERT_FILE =
    StringRef::from_lit("client-cert-file");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_DUMP_REQUEST_HEADER =
    StringRef::from_lit("frontend-http2-dump-request-header");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_DUMP_RESPONSE_HEADER =
    StringRef::from_lit("frontend-http2-dump-response-header");
constexpr auto SHRPX_OPT_HTTP2_NO_COOKIE_CRUMBLING =
    StringRef::from_lit("http2-no-cookie-crumbling");
constexpr auto SHRPX_OPT_FRONTEND_FRAME_DEBUG =
    StringRef::from_lit("frontend-frame-debug");
constexpr auto SHRPX_OPT_PADDING = StringRef::from_lit("padding");
constexpr auto SHRPX_OPT_ALTSVC = StringRef::from_lit("altsvc");
constexpr auto SHRPX_OPT_ADD_REQUEST_HEADER =
    StringRef::from_lit("add-request-header");
constexpr auto SHRPX_OPT_ADD_RESPONSE_HEADER =
    StringRef::from_lit("add-response-header");
constexpr auto SHRPX_OPT_WORKER_FRONTEND_CONNECTIONS =
    StringRef::from_lit("worker-frontend-connections");
constexpr auto SHRPX_OPT_NO_LOCATION_REWRITE =
    StringRef::from_lit("no-location-rewrite");
constexpr auto SHRPX_OPT_NO_HOST_REWRITE =
    StringRef::from_lit("no-host-rewrite");
constexpr auto SHRPX_OPT_BACKEND_HTTP1_CONNECTIONS_PER_HOST =
    StringRef::from_lit("backend-http1-connections-per-host");
constexpr auto SHRPX_OPT_BACKEND_HTTP1_CONNECTIONS_PER_FRONTEND =
    StringRef::from_lit("backend-http1-connections-per-frontend");
constexpr auto SHRPX_OPT_LISTENER_DISABLE_TIMEOUT =
    StringRef::from_lit("listener-disable-timeout");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_FILE =
    StringRef::from_lit("tls-ticket-key-file");
constexpr auto SHRPX_OPT_RLIMIT_NOFILE = StringRef::from_lit("rlimit-nofile");
constexpr auto SHRPX_OPT_BACKEND_REQUEST_BUFFER =
    StringRef::from_lit("backend-request-buffer");
constexpr auto SHRPX_OPT_BACKEND_RESPONSE_BUFFER =
    StringRef::from_lit("backend-response-buffer");
constexpr auto SHRPX_OPT_NO_SERVER_PUSH = StringRef::from_lit("no-server-push");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_CONNECTIONS_PER_WORKER =
    StringRef::from_lit("backend-http2-connections-per-worker");
constexpr auto SHRPX_OPT_FETCH_OCSP_RESPONSE_FILE =
    StringRef::from_lit("fetch-ocsp-response-file");
constexpr auto SHRPX_OPT_OCSP_UPDATE_INTERVAL =
    StringRef::from_lit("ocsp-update-interval");
constexpr auto SHRPX_OPT_NO_OCSP = StringRef::from_lit("no-ocsp");
constexpr auto SHRPX_OPT_HEADER_FIELD_BUFFER =
    StringRef::from_lit("header-field-buffer");
constexpr auto SHRPX_OPT_MAX_HEADER_FIELDS =
    StringRef::from_lit("max-header-fields");
constexpr auto SHRPX_OPT_INCLUDE = StringRef::from_lit("include");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_CIPHER =
    StringRef::from_lit("tls-ticket-key-cipher");
constexpr auto SHRPX_OPT_HOST_REWRITE = StringRef::from_lit("host-rewrite");
constexpr auto SHRPX_OPT_TLS_SESSION_CACHE_MEMCACHED =
    StringRef::from_lit("tls-session-cache-memcached");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED =
    StringRef::from_lit("tls-ticket-key-memcached");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED_INTERVAL =
    StringRef::from_lit("tls-ticket-key-memcached-interval");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED_MAX_RETRY =
    StringRef::from_lit("tls-ticket-key-memcached-max-retry");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED_MAX_FAIL =
    StringRef::from_lit("tls-ticket-key-memcached-max-fail");
constexpr auto SHRPX_OPT_MRUBY_FILE = StringRef::from_lit("mruby-file");
constexpr auto SHRPX_OPT_ACCEPT_PROXY_PROTOCOL =
    StringRef::from_lit("accept-proxy-protocol");
constexpr auto SHRPX_OPT_FASTOPEN = StringRef::from_lit("fastopen");
constexpr auto SHRPX_OPT_TLS_DYN_REC_WARMUP_THRESHOLD =
    StringRef::from_lit("tls-dyn-rec-warmup-threshold");
constexpr auto SHRPX_OPT_TLS_DYN_REC_IDLE_TIMEOUT =
    StringRef::from_lit("tls-dyn-rec-idle-timeout");
constexpr auto SHRPX_OPT_ADD_FORWARDED = StringRef::from_lit("add-forwarded");
constexpr auto SHRPX_OPT_STRIP_INCOMING_FORWARDED =
    StringRef::from_lit("strip-incoming-forwarded");
constexpr auto SHRPX_OPT_FORWARDED_BY = StringRef::from_lit("forwarded-by");
constexpr auto SHRPX_OPT_FORWARDED_FOR = StringRef::from_lit("forwarded-for");
constexpr auto SHRPX_OPT_REQUEST_HEADER_FIELD_BUFFER =
    StringRef::from_lit("request-header-field-buffer");
constexpr auto SHRPX_OPT_MAX_REQUEST_HEADER_FIELDS =
    StringRef::from_lit("max-request-header-fields");
constexpr auto SHRPX_OPT_RESPONSE_HEADER_FIELD_BUFFER =
    StringRef::from_lit("response-header-field-buffer");
constexpr auto SHRPX_OPT_MAX_RESPONSE_HEADER_FIELDS =
    StringRef::from_lit("max-response-header-fields");
constexpr auto SHRPX_OPT_NO_HTTP2_CIPHER_BLACK_LIST =
    StringRef::from_lit("no-http2-cipher-black-list");
constexpr auto SHRPX_OPT_BACKEND_HTTP1_TLS =
    StringRef::from_lit("backend-http1-tls");
constexpr auto SHRPX_OPT_TLS_SESSION_CACHE_MEMCACHED_TLS =
    StringRef::from_lit("tls-session-cache-memcached-tls");
constexpr auto SHRPX_OPT_TLS_SESSION_CACHE_MEMCACHED_CERT_FILE =
    StringRef::from_lit("tls-session-cache-memcached-cert-file");
constexpr auto SHRPX_OPT_TLS_SESSION_CACHE_MEMCACHED_PRIVATE_KEY_FILE =
    StringRef::from_lit("tls-session-cache-memcached-private-key-file");
constexpr auto SHRPX_OPT_TLS_SESSION_CACHE_MEMCACHED_ADDRESS_FAMILY =
    StringRef::from_lit("tls-session-cache-memcached-address-family");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED_TLS =
    StringRef::from_lit("tls-ticket-key-memcached-tls");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED_CERT_FILE =
    StringRef::from_lit("tls-ticket-key-memcached-cert-file");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED_PRIVATE_KEY_FILE =
    StringRef::from_lit("tls-ticket-key-memcached-private-key-file");
constexpr auto SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED_ADDRESS_FAMILY =
    StringRef::from_lit("tls-ticket-key-memcached-address-family");
constexpr auto SHRPX_OPT_BACKEND_ADDRESS_FAMILY =
    StringRef::from_lit("backend-address-family");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_MAX_CONCURRENT_STREAMS =
    StringRef::from_lit("frontend-http2-max-concurrent-streams");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_MAX_CONCURRENT_STREAMS =
    StringRef::from_lit("backend-http2-max-concurrent-streams");
constexpr auto SHRPX_OPT_BACKEND_CONNECTIONS_PER_FRONTEND =
    StringRef::from_lit("backend-connections-per-frontend");
constexpr auto SHRPX_OPT_BACKEND_TLS = StringRef::from_lit("backend-tls");
constexpr auto SHRPX_OPT_BACKEND_CONNECTIONS_PER_HOST =
    StringRef::from_lit("backend-connections-per-host");
constexpr auto SHRPX_OPT_ERROR_PAGE = StringRef::from_lit("error-page");
constexpr auto SHRPX_OPT_NO_KQUEUE = StringRef::from_lit("no-kqueue");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_SETTINGS_TIMEOUT =
    StringRef::from_lit("frontend-http2-settings-timeout");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_SETTINGS_TIMEOUT =
    StringRef::from_lit("backend-http2-settings-timeout");
constexpr auto SHRPX_OPT_API_MAX_REQUEST_BODY =
    StringRef::from_lit("api-max-request-body");
constexpr auto SHRPX_OPT_BACKEND_MAX_BACKOFF =
    StringRef::from_lit("backend-max-backoff");
constexpr auto SHRPX_OPT_SERVER_NAME = StringRef::from_lit("server-name");
constexpr auto SHRPX_OPT_NO_SERVER_REWRITE =
    StringRef::from_lit("no-server-rewrite");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_OPTIMIZE_WRITE_BUFFER_SIZE =
    StringRef::from_lit("frontend-http2-optimize-write-buffer-size");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_OPTIMIZE_WINDOW_SIZE =
    StringRef::from_lit("frontend-http2-optimize-window-size");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_WINDOW_SIZE =
    StringRef::from_lit("frontend-http2-window-size");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_CONNECTION_WINDOW_SIZE =
    StringRef::from_lit("frontend-http2-connection-window-size");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_WINDOW_SIZE =
    StringRef::from_lit("backend-http2-window-size");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_CONNECTION_WINDOW_SIZE =
    StringRef::from_lit("backend-http2-connection-window-size");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_ENCODER_DYNAMIC_TABLE_SIZE =
    StringRef::from_lit("frontend-http2-encoder-dynamic-table-size");
constexpr auto SHRPX_OPT_FRONTEND_HTTP2_DECODER_DYNAMIC_TABLE_SIZE =
    StringRef::from_lit("frontend-http2-decoder-dynamic-table-size");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_ENCODER_DYNAMIC_TABLE_SIZE =
    StringRef::from_lit("backend-http2-encoder-dynamic-table-size");
constexpr auto SHRPX_OPT_BACKEND_HTTP2_DECODER_DYNAMIC_TABLE_SIZE =
    StringRef::from_lit("backend-http2-decoder-dynamic-table-size");
constexpr auto SHRPX_OPT_ECDH_CURVES = StringRef::from_lit("ecdh-curves");

constexpr size_t SHRPX_OBFUSCATED_NODE_LENGTH = 8;

constexpr char DEFAULT_DOWNSTREAM_HOST[] = "127.0.0.1";
constexpr int16_t DEFAULT_DOWNSTREAM_PORT = 80;

enum shrpx_proto { PROTO_NONE, PROTO_HTTP1, PROTO_HTTP2, PROTO_MEMCACHED };

enum shrpx_session_affinity {
  // No session affinity
  AFFINITY_NONE,
  // Client IP affinity
  AFFINITY_IP,
};

enum shrpx_forwarded_param {
  FORWARDED_NONE = 0,
  FORWARDED_BY = 0x1,
  FORWARDED_FOR = 0x2,
  FORWARDED_HOST = 0x4,
  FORWARDED_PROTO = 0x8,
};

enum shrpx_forwarded_node_type {
  FORWARDED_NODE_OBFUSCATED,
  FORWARDED_NODE_IP,
};

struct AltSvc {
  StringRef protocol_id, host, origin, service;

  uint16_t port;
};

enum UpstreamAltMode {
  // No alternative mode
  ALTMODE_NONE,
  // API processing mode
  ALTMODE_API,
  // Health monitor mode
  ALTMODE_HEALTHMON,
};

struct UpstreamAddr {
  // The frontend address (e.g., FQDN, hostname, IP address).  If
  // |host_unix| is true, this is UNIX domain socket path.  This must
  // be NULL terminated string.
  StringRef host;
  // For TCP socket, this is <IP address>:<PORT>.  For IPv6 address,
  // address is surrounded by square brackets.  If socket is UNIX
  // domain socket, this is "localhost".
  StringRef hostport;
  // frontend port.  0 if |host_unix| is true.
  uint16_t port;
  // For TCP socket, this is either AF_INET or AF_INET6.  For UNIX
  // domain socket, this is 0.
  int family;
  // Alternate mode
  int alt_mode;
  // true if |host| contains UNIX domain socket path.
  bool host_unix;
  // true if TLS is enabled.
  bool tls;
  int fd;
};

struct DownstreamAddrConfig {
  Address addr;
  // backend address.  If |host_unix| is true, this is UNIX domain
  // socket path.  This must be NULL terminated string.
  StringRef host;
  // <HOST>:<PORT>.  This does not treat 80 and 443 specially.  If
  // |host_unix| is true, this is "localhost".
  StringRef hostport;
  // hostname sent as SNI field
  StringRef sni;
  size_t fall;
  size_t rise;
  // Application protocol used in this group
  shrpx_proto proto;
  // backend port.  0 if |host_unix| is true.
  uint16_t port;
  // true if |host| contains UNIX domain socket path.
  bool host_unix;
  bool tls;
};

// Mapping hash to idx which is an index into
// DownstreamAddrGroupConfig::addrs.
struct AffinityHash {
  AffinityHash(size_t idx, uint32_t hash) : idx(idx), hash(hash) {}

  size_t idx;
  uint32_t hash;
};

struct DownstreamAddrGroupConfig {
  DownstreamAddrGroupConfig(const StringRef &pattern)
      : pattern(pattern), affinity(AFFINITY_NONE) {}

  StringRef pattern;
  std::vector<DownstreamAddrConfig> addrs;
  // Bunch of session affinity hash.  Only used if affinity ==
  // AFFINITY_IP.
  std::vector<AffinityHash> affinity_hash;
  // Session affinity
  shrpx_session_affinity affinity;
};

struct TicketKey {
  const EVP_CIPHER *cipher;
  const EVP_MD *hmac;
  size_t hmac_keylen;
  struct {
    // name of this ticket configuration
    std::array<uint8_t, 16> name;
    // encryption key for |cipher|
    std::array<uint8_t, 32> enc_key;
    // hmac key for |hmac|
    std::array<uint8_t, 32> hmac_key;
  } data;
};

struct TicketKeys {
  ~TicketKeys();
  std::vector<TicketKey> keys;
};

struct HttpProxy {
  Address addr;
  // host in http proxy URI
  StringRef host;
  // userinfo in http proxy URI, not percent-encoded form
  StringRef userinfo;
  // port in http proxy URI
  uint16_t port;
};

struct TLSConfig {
  // RFC 5077 Session ticket related configurations
  struct {
    struct {
      Address addr;
      uint16_t port;
      // Hostname of memcached server.  This is also used as SNI field
      // if TLS is enabled.
      StringRef host;
      // Client private key and certificate for authentication
      StringRef private_key_file;
      StringRef cert_file;
      ev_tstamp interval;
      // Maximum number of retries when getting TLS ticket key from
      // mamcached, due to network error.
      size_t max_retry;
      // Maximum number of consecutive error from memcached, when this
      // limit reached, TLS ticket is disabled.
      size_t max_fail;
      // Address family of memcached connection.  One of either
      // AF_INET, AF_INET6 or AF_UNSPEC.
      int family;
      bool tls;
    } memcached;
    std::vector<StringRef> files;
    const EVP_CIPHER *cipher;
    // true if --tls-ticket-key-cipher is used
    bool cipher_given;
  } ticket;

  // Session cache related configurations
  struct {
    struct {
      Address addr;
      uint16_t port;
      // Hostname of memcached server.  This is also used as SNI field
      // if TLS is enabled.
      StringRef host;
      // Client private key and certificate for authentication
      StringRef private_key_file;
      StringRef cert_file;
      // Address family of memcached connection.  One of either
      // AF_INET, AF_INET6 or AF_UNSPEC.
      int family;
      bool tls;
    } memcached;
  } session_cache;

  // Dynamic record sizing configurations
  struct {
    size_t warmup_threshold;
    ev_tstamp idle_timeout;
  } dyn_rec;

  // OCSP realted configurations
  struct {
    ev_tstamp update_interval;
    StringRef fetch_ocsp_response_file;
    bool disabled;
  } ocsp;

  // Client verification configurations
  struct {
    // Path to file containing CA certificate solely used for client
    // certificate validation
    StringRef cacert;
    bool enabled;
  } client_verify;

  // Client private key and certificate used in backend connections.
  struct {
    StringRef private_key_file;
    StringRef cert_file;
  } client;

  // The list of (private key file, certificate file) pair
  std::vector<std::pair<StringRef, StringRef>> subcerts;
  std::vector<unsigned char> alpn_prefs;
  // list of supported NPN/ALPN protocol strings in the order of
  // preference.
  std::vector<StringRef> npn_list;
  // list of supported SSL/TLS protocol strings.
  std::vector<StringRef> tls_proto_list;
  BIO_METHOD *bio_method;
  // Bit mask to disable SSL/TLS protocol versions.  This will be
  // passed to SSL_CTX_set_options().
  long int tls_proto_mask;
  StringRef backend_sni_name;
  std::chrono::seconds session_timeout;
  StringRef private_key_file;
  StringRef private_key_passwd;
  StringRef cert_file;
  StringRef dh_param_file;
  StringRef ciphers;
  StringRef ecdh_curves;
  StringRef cacert;
  bool insecure;
  bool no_http2_cipher_black_list;
};

// custom error page
struct ErrorPage {
  // not NULL-terminated
  std::vector<uint8_t> content;
  // 0 is special value, and it matches all HTTP status code.
  unsigned int http_status;
};

struct HttpConfig {
  struct {
    // obfuscated value used in "by" parameter of Forwarded header
    // field.  This is only used when user defined static obfuscated
    // string is provided.
    StringRef by_obfuscated;
    // bitwise-OR of one or more of shrpx_forwarded_param values.
    uint32_t params;
    // type of value recorded in "by" parameter of Forwarded header
    // field.
    shrpx_forwarded_node_type by_node_type;
    // type of value recorded in "for" parameter of Forwarded header
    // field.
    shrpx_forwarded_node_type for_node_type;
    bool strip_incoming;
  } forwarded;
  struct {
    bool add;
    bool strip_incoming;
  } xff;
  std::vector<AltSvc> altsvcs;
  std::vector<ErrorPage> error_pages;
  HeaderRefs add_request_headers;
  HeaderRefs add_response_headers;
  StringRef server_name;
  size_t request_header_field_buffer;
  size_t max_request_header_fields;
  size_t response_header_field_buffer;
  size_t max_response_header_fields;
  bool no_via;
  bool no_location_rewrite;
  bool no_host_rewrite;
  bool no_server_rewrite;
};

struct Http2Config {
  struct {
    struct {
      struct {
        StringRef request_header_file;
        StringRef response_header_file;
        FILE *request_header;
        FILE *response_header;
      } dump;
      bool frame_debug;
    } debug;
    struct {
      ev_tstamp settings;
    } timeout;
    nghttp2_option *option;
    nghttp2_option *alt_mode_option;
    nghttp2_session_callbacks *callbacks;
    size_t max_concurrent_streams;
    size_t encoder_dynamic_table_size;
    size_t decoder_dynamic_table_size;
    int32_t window_size;
    int32_t connection_window_size;
    bool optimize_write_buffer_size;
    bool optimize_window_size;
  } upstream;
  struct {
    struct {
      ev_tstamp settings;
    } timeout;
    nghttp2_option *option;
    nghttp2_session_callbacks *callbacks;
    size_t encoder_dynamic_table_size;
    size_t decoder_dynamic_table_size;
    int32_t window_size;
    int32_t connection_window_size;
    size_t max_concurrent_streams;
  } downstream;
  struct {
    ev_tstamp stream_read;
    ev_tstamp stream_write;
  } timeout;
  bool no_cookie_crumbling;
  bool no_server_push;
};

struct LoggingConfig {
  struct {
    std::vector<LogFragment> format;
    StringRef file;
    // Send accesslog to syslog, ignoring accesslog_file.
    bool syslog;
  } access;
  struct {
    StringRef file;
    // Send errorlog to syslog, ignoring errorlog_file.
    bool syslog;
  } error;
  int syslog_facility;
};

struct RateLimitConfig {
  size_t rate;
  size_t burst;
};

// Wildcard host pattern routing.  We strips left most '*' from host
// field.  router includes all path patterns sharing the same wildcard
// host.
struct WildcardPattern {
  WildcardPattern(const StringRef &host) : host(host) {}

  // This might not be NULL terminated.  Currently it is only used for
  // comparison.
  StringRef host;
  Router router;
};

// Configuration to select backend to forward request
struct RouterConfig {
  Router router;
  // Router for reversed wildcard hosts.  Since this router has
  // wildcard hosts reversed without '*', one should call match()
  // function with reversed host stripping last character.  This is
  // because we require at least one character must match for '*'.
  // The index stored in this router is index of wildcard_patterns.
  Router rev_wildcard_router;
  std::vector<WildcardPattern> wildcard_patterns;
};

struct DownstreamConfig {
  DownstreamConfig()
      : balloc(1024, 1024),
        timeout{},
        addr_group_catch_all{0},
        connections_per_host{0},
        connections_per_frontend{0},
        request_buffer_size{0},
        response_buffer_size{0},
        family{0} {}

  DownstreamConfig(const DownstreamConfig &) = delete;
  DownstreamConfig(DownstreamConfig &&) = delete;
  DownstreamConfig &operator=(const DownstreamConfig &) = delete;
  DownstreamConfig &operator=(DownstreamConfig &&) = delete;

  // Allocator to allocate memory for Downstream configuration.  Since
  // we may swap around DownstreamConfig in arbitrary times with API
  // calls, we should use their own allocator instead of per Config
  // allocator.
  BlockAllocator balloc;
  struct {
    ev_tstamp read;
    ev_tstamp write;
    ev_tstamp idle_read;
    // The maximum backoff while checking health check for offline
    // backend or while detaching failed backend from load balancing
    // group temporarily.
    ev_tstamp max_backoff;
  } timeout;
  RouterConfig router;
  std::vector<DownstreamAddrGroupConfig> addr_groups;
  // The index of catch-all group in downstream_addr_groups.
  size_t addr_group_catch_all;
  size_t connections_per_host;
  size_t connections_per_frontend;
  size_t request_buffer_size;
  size_t response_buffer_size;
  // Address family of backend connection.  One of either AF_INET,
  // AF_INET6 or AF_UNSPEC.  This is ignored if backend connection
  // is made via Unix domain socket.
  int family;
};

struct ConnectionConfig {
  struct {
    struct {
      ev_tstamp sleep;
    } timeout;
    // address of frontend acceptors
    std::vector<UpstreamAddr> addrs;
    int backlog;
    // TCP fastopen.  If this is positive, it is passed to
    // setsockopt() along with TCP_FASTOPEN.
    int fastopen;
  } listener;

  struct {
    struct {
      ev_tstamp http2_read;
      ev_tstamp read;
      ev_tstamp write;
    } timeout;
    struct {
      RateLimitConfig read;
      RateLimitConfig write;
    } ratelimit;
    size_t worker_connections;
    bool accept_proxy_protocol;
  } upstream;

  std::shared_ptr<DownstreamConfig> downstream;
};

struct APIConfig {
  // Maximum request body size for one API request
  size_t max_request_body;
  // true if at least one of UpstreamAddr has api enabled
  bool enabled;
};

struct Config {
  Config()
      : balloc(4096, 4096),
        downstream_http_proxy{},
        http{},
        http2{},
        tls{},
        logging{},
        conn{},
        api{},
        num_worker{0},
        padding{0},
        rlimit_nofile{0},
        uid{0},
        gid{0},
        pid{0},
        verbose{false},
        daemon{false},
        http2_proxy{false},
        ev_loop_flags{0} {}
  ~Config();

  Config(Config &&) = delete;
  Config(const Config &&) = delete;
  Config &operator=(Config &&) = delete;
  Config &operator=(const Config &&) = delete;

  // Allocator to allocate memory for this object except for
  // DownstreamConfig.  Currently, it is used to allocate memory for
  // strings.
  BlockAllocator balloc;
  HttpProxy downstream_http_proxy;
  HttpConfig http;
  Http2Config http2;
  TLSConfig tls;
  LoggingConfig logging;
  ConnectionConfig conn;
  APIConfig api;
  StringRef pid_file;
  StringRef conf_path;
  StringRef user;
  StringRef mruby_file;
  size_t num_worker;
  size_t padding;
  size_t rlimit_nofile;
  uid_t uid;
  gid_t gid;
  pid_t pid;
  bool verbose;
  bool daemon;
  bool http2_proxy;
  // flags passed to ev_default_loop() and ev_loop_new()
  int ev_loop_flags;
};

const Config *get_config();
Config *mod_config();
// Replaces the current config with given |new_config|.  The old config is
// returned.
std::unique_ptr<Config> replace_config(std::unique_ptr<Config> new_config);
void create_config();

// generated by gennghttpxfun.py
enum {
  SHRPX_OPTID_ACCEPT_PROXY_PROTOCOL,
  SHRPX_OPTID_ACCESSLOG_FILE,
  SHRPX_OPTID_ACCESSLOG_FORMAT,
  SHRPX_OPTID_ACCESSLOG_SYSLOG,
  SHRPX_OPTID_ADD_FORWARDED,
  SHRPX_OPTID_ADD_REQUEST_HEADER,
  SHRPX_OPTID_ADD_RESPONSE_HEADER,
  SHRPX_OPTID_ADD_X_FORWARDED_FOR,
  SHRPX_OPTID_ALTSVC,
  SHRPX_OPTID_API_MAX_REQUEST_BODY,
  SHRPX_OPTID_BACKEND,
  SHRPX_OPTID_BACKEND_ADDRESS_FAMILY,
  SHRPX_OPTID_BACKEND_CONNECTIONS_PER_FRONTEND,
  SHRPX_OPTID_BACKEND_CONNECTIONS_PER_HOST,
  SHRPX_OPTID_BACKEND_HTTP_PROXY_URI,
  SHRPX_OPTID_BACKEND_HTTP1_CONNECTIONS_PER_FRONTEND,
  SHRPX_OPTID_BACKEND_HTTP1_CONNECTIONS_PER_HOST,
  SHRPX_OPTID_BACKEND_HTTP1_TLS,
  SHRPX_OPTID_BACKEND_HTTP2_CONNECTION_WINDOW_BITS,
  SHRPX_OPTID_BACKEND_HTTP2_CONNECTION_WINDOW_SIZE,
  SHRPX_OPTID_BACKEND_HTTP2_CONNECTIONS_PER_WORKER,
  SHRPX_OPTID_BACKEND_HTTP2_DECODER_DYNAMIC_TABLE_SIZE,
  SHRPX_OPTID_BACKEND_HTTP2_ENCODER_DYNAMIC_TABLE_SIZE,
  SHRPX_OPTID_BACKEND_HTTP2_MAX_CONCURRENT_STREAMS,
  SHRPX_OPTID_BACKEND_HTTP2_SETTINGS_TIMEOUT,
  SHRPX_OPTID_BACKEND_HTTP2_WINDOW_BITS,
  SHRPX_OPTID_BACKEND_HTTP2_WINDOW_SIZE,
  SHRPX_OPTID_BACKEND_IPV4,
  SHRPX_OPTID_BACKEND_IPV6,
  SHRPX_OPTID_BACKEND_KEEP_ALIVE_TIMEOUT,
  SHRPX_OPTID_BACKEND_MAX_BACKOFF,
  SHRPX_OPTID_BACKEND_NO_TLS,
  SHRPX_OPTID_BACKEND_READ_TIMEOUT,
  SHRPX_OPTID_BACKEND_REQUEST_BUFFER,
  SHRPX_OPTID_BACKEND_RESPONSE_BUFFER,
  SHRPX_OPTID_BACKEND_TLS,
  SHRPX_OPTID_BACKEND_TLS_SNI_FIELD,
  SHRPX_OPTID_BACKEND_WRITE_TIMEOUT,
  SHRPX_OPTID_BACKLOG,
  SHRPX_OPTID_CACERT,
  SHRPX_OPTID_CERTIFICATE_FILE,
  SHRPX_OPTID_CIPHERS,
  SHRPX_OPTID_CLIENT,
  SHRPX_OPTID_CLIENT_CERT_FILE,
  SHRPX_OPTID_CLIENT_PRIVATE_KEY_FILE,
  SHRPX_OPTID_CLIENT_PROXY,
  SHRPX_OPTID_CONF,
  SHRPX_OPTID_DAEMON,
  SHRPX_OPTID_DH_PARAM_FILE,
  SHRPX_OPTID_ECDH_CURVES,
  SHRPX_OPTID_ERROR_PAGE,
  SHRPX_OPTID_ERRORLOG_FILE,
  SHRPX_OPTID_ERRORLOG_SYSLOG,
  SHRPX_OPTID_FASTOPEN,
  SHRPX_OPTID_FETCH_OCSP_RESPONSE_FILE,
  SHRPX_OPTID_FORWARDED_BY,
  SHRPX_OPTID_FORWARDED_FOR,
  SHRPX_OPTID_FRONTEND,
  SHRPX_OPTID_FRONTEND_FRAME_DEBUG,
  SHRPX_OPTID_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS,
  SHRPX_OPTID_FRONTEND_HTTP2_CONNECTION_WINDOW_SIZE,
  SHRPX_OPTID_FRONTEND_HTTP2_DECODER_DYNAMIC_TABLE_SIZE,
  SHRPX_OPTID_FRONTEND_HTTP2_DUMP_REQUEST_HEADER,
  SHRPX_OPTID_FRONTEND_HTTP2_DUMP_RESPONSE_HEADER,
  SHRPX_OPTID_FRONTEND_HTTP2_ENCODER_DYNAMIC_TABLE_SIZE,
  SHRPX_OPTID_FRONTEND_HTTP2_MAX_CONCURRENT_STREAMS,
  SHRPX_OPTID_FRONTEND_HTTP2_OPTIMIZE_WINDOW_SIZE,
  SHRPX_OPTID_FRONTEND_HTTP2_OPTIMIZE_WRITE_BUFFER_SIZE,
  SHRPX_OPTID_FRONTEND_HTTP2_READ_TIMEOUT,
  SHRPX_OPTID_FRONTEND_HTTP2_SETTINGS_TIMEOUT,
  SHRPX_OPTID_FRONTEND_HTTP2_WINDOW_BITS,
  SHRPX_OPTID_FRONTEND_HTTP2_WINDOW_SIZE,
  SHRPX_OPTID_FRONTEND_NO_TLS,
  SHRPX_OPTID_FRONTEND_READ_TIMEOUT,
  SHRPX_OPTID_FRONTEND_WRITE_TIMEOUT,
  SHRPX_OPTID_HEADER_FIELD_BUFFER,
  SHRPX_OPTID_HOST_REWRITE,
  SHRPX_OPTID_HTTP2_BRIDGE,
  SHRPX_OPTID_HTTP2_MAX_CONCURRENT_STREAMS,
  SHRPX_OPTID_HTTP2_NO_COOKIE_CRUMBLING,
  SHRPX_OPTID_HTTP2_PROXY,
  SHRPX_OPTID_INCLUDE,
  SHRPX_OPTID_INSECURE,
  SHRPX_OPTID_LISTENER_DISABLE_TIMEOUT,
  SHRPX_OPTID_LOG_LEVEL,
  SHRPX_OPTID_MAX_HEADER_FIELDS,
  SHRPX_OPTID_MAX_REQUEST_HEADER_FIELDS,
  SHRPX_OPTID_MAX_RESPONSE_HEADER_FIELDS,
  SHRPX_OPTID_MRUBY_FILE,
  SHRPX_OPTID_NO_HOST_REWRITE,
  SHRPX_OPTID_NO_HTTP2_CIPHER_BLACK_LIST,
  SHRPX_OPTID_NO_KQUEUE,
  SHRPX_OPTID_NO_LOCATION_REWRITE,
  SHRPX_OPTID_NO_OCSP,
  SHRPX_OPTID_NO_SERVER_PUSH,
  SHRPX_OPTID_NO_SERVER_REWRITE,
  SHRPX_OPTID_NO_VIA,
  SHRPX_OPTID_NPN_LIST,
  SHRPX_OPTID_OCSP_UPDATE_INTERVAL,
  SHRPX_OPTID_PADDING,
  SHRPX_OPTID_PID_FILE,
  SHRPX_OPTID_PRIVATE_KEY_FILE,
  SHRPX_OPTID_PRIVATE_KEY_PASSWD_FILE,
  SHRPX_OPTID_READ_BURST,
  SHRPX_OPTID_READ_RATE,
  SHRPX_OPTID_REQUEST_HEADER_FIELD_BUFFER,
  SHRPX_OPTID_RESPONSE_HEADER_FIELD_BUFFER,
  SHRPX_OPTID_RLIMIT_NOFILE,
  SHRPX_OPTID_SERVER_NAME,
  SHRPX_OPTID_STREAM_READ_TIMEOUT,
  SHRPX_OPTID_STREAM_WRITE_TIMEOUT,
  SHRPX_OPTID_STRIP_INCOMING_FORWARDED,
  SHRPX_OPTID_STRIP_INCOMING_X_FORWARDED_FOR,
  SHRPX_OPTID_SUBCERT,
  SHRPX_OPTID_SYSLOG_FACILITY,
  SHRPX_OPTID_TLS_DYN_REC_IDLE_TIMEOUT,
  SHRPX_OPTID_TLS_DYN_REC_WARMUP_THRESHOLD,
  SHRPX_OPTID_TLS_PROTO_LIST,
  SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED,
  SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_ADDRESS_FAMILY,
  SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_CERT_FILE,
  SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_PRIVATE_KEY_FILE,
  SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_TLS,
  SHRPX_OPTID_TLS_TICKET_KEY_CIPHER,
  SHRPX_OPTID_TLS_TICKET_KEY_FILE,
  SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED,
  SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_ADDRESS_FAMILY,
  SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_CERT_FILE,
  SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_INTERVAL,
  SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_MAX_FAIL,
  SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_MAX_RETRY,
  SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_PRIVATE_KEY_FILE,
  SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_TLS,
  SHRPX_OPTID_USER,
  SHRPX_OPTID_VERIFY_CLIENT,
  SHRPX_OPTID_VERIFY_CLIENT_CACERT,
  SHRPX_OPTID_WORKER_FRONTEND_CONNECTIONS,
  SHRPX_OPTID_WORKER_READ_BURST,
  SHRPX_OPTID_WORKER_READ_RATE,
  SHRPX_OPTID_WORKER_WRITE_BURST,
  SHRPX_OPTID_WORKER_WRITE_RATE,
  SHRPX_OPTID_WORKERS,
  SHRPX_OPTID_WRITE_BURST,
  SHRPX_OPTID_WRITE_RATE,
  SHRPX_OPTID_MAXIDX,
};

// Looks up token for given option name |name| of length |namelen|.
int option_lookup_token(const char *name, size_t namelen);

// Parses option name |opt| and value |optarg|.  The results are
// stored into the object pointed by |config|. This function returns 0
// if it succeeds, or -1.  The |included_set| contains the all paths
// already included while processing this configuration, to avoid loop
// in --include option.
int parse_config(Config *config, const StringRef &opt, const StringRef &optarg,
                 std::set<StringRef> &included_set);

// Similar to parse_config() above, but additional |optid| which
// should be the return value of option_lookup_token(opt).
int parse_config(Config *config, int optid, const StringRef &opt,
                 const StringRef &optarg, std::set<StringRef> &included_set);

// Loads configurations from |filename| and stores them in |config|.
// This function returns 0 if it succeeds, or -1.  See parse_config()
// for |include_set|.
int load_config(Config *config, const char *filename,
                std::set<StringRef> &include_set);

// Parses header field in |optarg|.  We expect header field is formed
// like "NAME: VALUE".  We require that NAME is non empty string.  ":"
// is allowed at the start of the NAME, but NAME == ":" is not
// allowed.  This function returns pair of NAME and VALUE.
HeaderRefs::value_type parse_header(BlockAllocator &balloc,
                                    const StringRef &optarg);

std::vector<LogFragment> parse_log_format(BlockAllocator &balloc,
                                          const StringRef &optarg);

// Returns string for syslog |facility|.
StringRef str_syslog_facility(int facility);

// Returns integer value of syslog |facility| string.
int int_syslog_facility(const StringRef &strfacility);

FILE *open_file_for_write(const char *filename);

// Reads TLS ticket key file in |files| and returns TicketKey which
// stores read key data.  The given |cipher| and |hmac| determine the
// expected file size.  This function returns TicketKey if it
// succeeds, or nullptr.
std::unique_ptr<TicketKeys>
read_tls_ticket_key_file(const std::vector<StringRef> &files,
                         const EVP_CIPHER *cipher, const EVP_MD *hmac);

// Returns string representation of |proto|.
StringRef strproto(shrpx_proto proto);

int configure_downstream_group(Config *config, bool http2_proxy,
                               bool numeric_addr_only,
                               const TLSConfig &tlsconf);

int resolve_hostname(Address *addr, const char *hostname, uint16_t port,
                     int family, int additional_flags = 0);

} // namespace shrpx

#endif // SHRPX_CONFIG_H
