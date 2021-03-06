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
#include "shrpx_ssl.h"

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif // HAVE_SYS_SOCKET_H
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif // HAVE_NETDB_H
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/types.h>

#include <vector>
#include <string>
#include <iomanip>

#include <iostream>

#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <openssl/dh.h>

#include <nghttp2/nghttp2.h>

#ifdef HAVE_SPDYLAY
#include <spdylay/spdylay.h>
#endif // HAVE_SPDYLAY

#include "shrpx_log.h"
#include "shrpx_client_handler.h"
#include "shrpx_config.h"
#include "shrpx_worker.h"
#include "shrpx_downstream_connection_pool.h"
#include "shrpx_http2_session.h"
#include "shrpx_memcached_request.h"
#include "shrpx_memcached_dispatcher.h"
#include "shrpx_connection_handler.h"
#include "util.h"
#include "ssl.h"
#include "template.h"
#include "ssl_compat.h"

using namespace nghttp2;

namespace shrpx {

namespace ssl {

#if !OPENSSL_1_1_API
namespace {
const unsigned char *ASN1_STRING_get0_data(ASN1_STRING *x) {
  return ASN1_STRING_data(x);
}
} // namespace
#endif // !OPENSSL_1_1_API

namespace {
int next_proto_cb(SSL *s, const unsigned char **data, unsigned int *len,
                  void *arg) {
  auto &prefs = get_config()->tls.alpn_prefs;
  *data = prefs.data();
  *len = prefs.size();
  return SSL_TLSEXT_ERR_OK;
}
} // namespace

namespace {
int verify_callback(int preverify_ok, X509_STORE_CTX *ctx) {
  if (!preverify_ok) {
    int err = X509_STORE_CTX_get_error(ctx);
    int depth = X509_STORE_CTX_get_error_depth(ctx);
    LOG(ERROR) << "client certificate verify error:num=" << err << ":"
               << X509_verify_cert_error_string(err) << ":depth=" << depth;
  }
  return preverify_ok;
}
} // namespace

int set_alpn_prefs(std::vector<unsigned char> &out,
                   const std::vector<StringRef> &protos) {
  size_t len = 0;

  for (const auto &proto : protos) {
    if (proto.size() > 255) {
      LOG(FATAL) << "Too long ALPN identifier: " << proto.size();
      return -1;
    }

    len += 1 + proto.size();
  }

  if (len > (1 << 16) - 1) {
    LOG(FATAL) << "Too long ALPN identifier list: " << len;
    return -1;
  }

  out.resize(len);
  auto ptr = out.data();

  for (const auto &proto : protos) {
    *ptr++ = proto.size();
    ptr = std::copy(std::begin(proto), std::end(proto), ptr);
  }

  return 0;
}

namespace {
int ssl_pem_passwd_cb(char *buf, int size, int rwflag, void *user_data) {
  auto config = static_cast<Config *>(user_data);
  auto len = static_cast<int>(config->tls.private_key_passwd.size());
  if (size < len + 1) {
    LOG(ERROR) << "ssl_pem_passwd_cb: buf is too small " << size;
    return 0;
  }
  // Copy string including last '\0'.
  memcpy(buf, config->tls.private_key_passwd.c_str(), len + 1);
  return len;
}
} // namespace

namespace {
int servername_callback(SSL *ssl, int *al, void *arg) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  auto handler = static_cast<ClientHandler *>(conn->data);
  auto worker = handler->get_worker();

  auto rawhost = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (rawhost == nullptr) {
    return SSL_TLSEXT_ERR_OK;
  }

  auto len = strlen(rawhost);
  // NI_MAXHOST includes terminal NULL.
  if (len == 0 || len + 1 > NI_MAXHOST) {
    return SSL_TLSEXT_ERR_OK;
  }

  std::array<uint8_t, NI_MAXHOST> buf;

  auto end_buf = std::copy_n(rawhost, len, std::begin(buf));

  util::inp_strlower(std::begin(buf), end_buf);

  auto hostname = StringRef{std::begin(buf), end_buf};

  handler->set_tls_sni(hostname);

  auto cert_tree = worker->get_cert_lookup_tree();
  if (!cert_tree) {
    return SSL_TLSEXT_ERR_OK;
  }

  auto idx = cert_tree->lookup(hostname);
  if (idx == -1) {
    return SSL_TLSEXT_ERR_OK;
  }

  auto conn_handler = worker->get_connection_handler();
  auto ssl_ctx = conn_handler->get_ssl_ctx(idx);

  SSL_set_SSL_CTX(ssl, ssl_ctx);

  return SSL_TLSEXT_ERR_OK;
}
} // namespace

#ifndef OPENSSL_IS_BORINGSSL
namespace {
std::shared_ptr<std::vector<uint8_t>>
get_ocsp_data(TLSContextData *tls_ctx_data) {
#ifdef HAVE_ATOMIC_STD_SHARED_PTR
  return std::atomic_load_explicit(&tls_ctx_data->ocsp_data,
                                   std::memory_order_acquire);
#else  // !HAVE_ATOMIC_STD_SHARED_PTR
  std::lock_guard<std::mutex> g(tls_ctx_data->mu);
  return tls_ctx_data->ocsp_data;
#endif // !HAVE_ATOMIC_STD_SHARED_PTR
}
} // namespace

namespace {
int ocsp_resp_cb(SSL *ssl, void *arg) {
  auto ssl_ctx = SSL_get_SSL_CTX(ssl);
  auto tls_ctx_data =
      static_cast<TLSContextData *>(SSL_CTX_get_app_data(ssl_ctx));

  auto data = get_ocsp_data(tls_ctx_data);

  if (!data) {
    return SSL_TLSEXT_ERR_OK;
  }

  auto buf =
      static_cast<uint8_t *>(CRYPTO_malloc(data->size(), __FILE__, __LINE__));

  if (!buf) {
    return SSL_TLSEXT_ERR_OK;
  }

  std::copy(std::begin(*data), std::end(*data), buf);

  SSL_set_tlsext_status_ocsp_resp(ssl, buf, data->size());

  return SSL_TLSEXT_ERR_OK;
}
} // namespace
#endif // OPENSSL_IS_BORINGSSL

constexpr auto MEMCACHED_SESSION_CACHE_KEY_PREFIX =
    StringRef::from_lit("nghttpx:tls-session-cache:");

namespace {
int tls_session_new_cb(SSL *ssl, SSL_SESSION *session) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  auto handler = static_cast<ClientHandler *>(conn->data);
  auto worker = handler->get_worker();
  auto dispatcher = worker->get_session_cache_memcached_dispatcher();
  auto &balloc = handler->get_block_allocator();

  const unsigned char *id;
  unsigned int idlen;

  id = SSL_SESSION_get_id(session, &idlen);

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Memached: cache session, id=" << util::format_hex(id, idlen);
  }

  auto req = make_unique<MemcachedRequest>();
  req->op = MEMCACHED_OP_ADD;
  req->key = MEMCACHED_SESSION_CACHE_KEY_PREFIX.str();
  req->key +=
      util::format_hex(balloc, StringRef{id, static_cast<size_t>(idlen)});

  auto sessionlen = i2d_SSL_SESSION(session, nullptr);
  req->value.resize(sessionlen);
  auto buf = &req->value[0];
  i2d_SSL_SESSION(session, &buf);
  req->expiry = 12_h;
  req->cb = [](MemcachedRequest *req, MemcachedResult res) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Memcached: session cache done.  key=" << req->key
                << ", status_code=" << res.status_code << ", value="
                << std::string(std::begin(res.value), std::end(res.value));
    }
    if (res.status_code != 0) {
      LOG(WARN) << "Memcached: failed to cache session key=" << req->key
                << ", status_code=" << res.status_code << ", value="
                << std::string(std::begin(res.value), std::end(res.value));
    }
  };
  assert(!req->canceled);

  dispatcher->add_request(std::move(req));

  return 0;
}
} // namespace

namespace {
SSL_SESSION *tls_session_get_cb(SSL *ssl,
#if OPENSSL_1_1_API
                                const unsigned char *id,
#else  // !OPENSSL_1_1_API
                                unsigned char *id,
#endif // !OPENSSL_1_1_API
                                int idlen, int *copy) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  auto handler = static_cast<ClientHandler *>(conn->data);
  auto worker = handler->get_worker();
  auto dispatcher = worker->get_session_cache_memcached_dispatcher();
  auto &balloc = handler->get_block_allocator();

  if (conn->tls.cached_session) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Memcached: found cached session, id="
                << util::format_hex(id, idlen);
    }

    // This is required, without this, memory leak occurs.
    *copy = 0;

    auto session = conn->tls.cached_session;
    conn->tls.cached_session = nullptr;
    return session;
  }

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Memcached: get cached session, id="
              << util::format_hex(id, idlen);
  }

  auto req = make_unique<MemcachedRequest>();
  req->op = MEMCACHED_OP_GET;
  req->key = MEMCACHED_SESSION_CACHE_KEY_PREFIX.str();
  req->key +=
      util::format_hex(balloc, StringRef{id, static_cast<size_t>(idlen)});
  req->cb = [conn](MemcachedRequest *, MemcachedResult res) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Memcached: returned status code " << res.status_code;
    }

    // We might stop reading, so start it again
    conn->rlimit.startw();
    ev_timer_again(conn->loop, &conn->rt);

    conn->wlimit.startw();
    ev_timer_again(conn->loop, &conn->wt);

    conn->tls.cached_session_lookup_req = nullptr;
    if (res.status_code != 0) {
      conn->tls.handshake_state = TLS_CONN_CANCEL_SESSION_CACHE;
      return;
    }

    const uint8_t *p = res.value.data();

    auto session = d2i_SSL_SESSION(nullptr, &p, res.value.size());
    if (!session) {
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "cannot materialize session";
      }
      conn->tls.handshake_state = TLS_CONN_CANCEL_SESSION_CACHE;
      return;
    }

    conn->tls.cached_session = session;
    conn->tls.handshake_state = TLS_CONN_GOT_SESSION_CACHE;
  };

  conn->tls.handshake_state = TLS_CONN_WAIT_FOR_SESSION_CACHE;
  conn->tls.cached_session_lookup_req = req.get();

  dispatcher->add_request(std::move(req));

  return nullptr;
}
} // namespace

namespace {
int ticket_key_cb(SSL *ssl, unsigned char *key_name, unsigned char *iv,
                  EVP_CIPHER_CTX *ctx, HMAC_CTX *hctx, int enc) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  auto handler = static_cast<ClientHandler *>(conn->data);
  auto worker = handler->get_worker();
  auto ticket_keys = worker->get_ticket_keys();

  if (!ticket_keys) {
    // No ticket keys available.
    return -1;
  }

  auto &keys = ticket_keys->keys;
  assert(!keys.empty());

  if (enc) {
    if (RAND_bytes(iv, EVP_MAX_IV_LENGTH) == 0) {
      if (LOG_ENABLED(INFO)) {
        CLOG(INFO, handler) << "session ticket key: RAND_bytes failed";
      }
      return -1;
    }

    auto &key = keys[0];

    if (LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "encrypt session ticket key: "
                          << util::format_hex(key.data.name);
    }

    std::copy(std::begin(key.data.name), std::end(key.data.name), key_name);

    EVP_EncryptInit_ex(ctx, get_config()->tls.ticket.cipher, nullptr,
                       key.data.enc_key.data(), iv);
    HMAC_Init_ex(hctx, key.data.hmac_key.data(), key.hmac_keylen, key.hmac,
                 nullptr);
    return 1;
  }

  size_t i;
  for (i = 0; i < keys.size(); ++i) {
    auto &key = keys[i];
    if (std::equal(std::begin(key.data.name), std::end(key.data.name),
                   key_name)) {
      break;
    }
  }

  if (i == keys.size()) {
    if (LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "session ticket key "
                          << util::format_hex(key_name, 16) << " not found";
    }
    return 0;
  }

  if (LOG_ENABLED(INFO)) {
    CLOG(INFO, handler) << "decrypt session ticket key: "
                        << util::format_hex(key_name, 16);
  }

  auto &key = keys[i];
  HMAC_Init_ex(hctx, key.data.hmac_key.data(), key.hmac_keylen, key.hmac,
               nullptr);
  EVP_DecryptInit_ex(ctx, key.cipher, nullptr, key.data.enc_key.data(), iv);

  return i == 0 ? 1 : 2;
}
} // namespace

namespace {
void info_callback(const SSL *ssl, int where, int ret) {
  // To mitigate possible DOS attack using lots of renegotiations, we
  // disable renegotiation. Since OpenSSL does not provide an easy way
  // to disable it, we check that renegotiation is started in this
  // callback.
  if (where & SSL_CB_HANDSHAKE_START) {
    auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
    if (conn && conn->tls.initial_handshake_done) {
      auto handler = static_cast<ClientHandler *>(conn->data);
      if (LOG_ENABLED(INFO)) {
        CLOG(INFO, handler) << "TLS renegotiation started";
      }
      handler->start_immediate_shutdown();
    }
  }
}
} // namespace

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
namespace {
int alpn_select_proto_cb(SSL *ssl, const unsigned char **out,
                         unsigned char *outlen, const unsigned char *in,
                         unsigned int inlen, void *arg) {
  // We assume that get_config()->npn_list contains ALPN protocol
  // identifier sorted by preference order.  So we just break when we
  // found the first overlap.
  for (const auto &target_proto_id : get_config()->tls.npn_list) {
    for (auto p = in, end = in + inlen; p < end;) {
      auto proto_id = p + 1;
      auto proto_len = *p;

      if (proto_id + proto_len <= end &&
          util::streq(target_proto_id, StringRef{proto_id, proto_len})) {

        *out = reinterpret_cast<const unsigned char *>(proto_id);
        *outlen = proto_len;

        return SSL_TLSEXT_ERR_OK;
      }

      p += 1 + proto_len;
    }
  }

  return SSL_TLSEXT_ERR_NOACK;
}
} // namespace
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L

struct TLSProtocol {
  StringRef name;
  long int mask;
};

constexpr TLSProtocol TLS_PROTOS[] = {
    TLSProtocol{StringRef::from_lit("TLSv1.2"), SSL_OP_NO_TLSv1_2},
    TLSProtocol{StringRef::from_lit("TLSv1.1"), SSL_OP_NO_TLSv1_1},
    TLSProtocol{StringRef::from_lit("TLSv1.0"), SSL_OP_NO_TLSv1}};

long int create_tls_proto_mask(const std::vector<StringRef> &tls_proto_list) {
  long int res = 0;

  for (auto &supported : TLS_PROTOS) {
    auto ok = false;
    for (auto &name : tls_proto_list) {
      if (util::strieq(supported.name, name)) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      res |= supported.mask;
    }
  }
  return res;
}

SSL_CTX *create_ssl_context(const char *private_key_file, const char *cert_file
#ifdef HAVE_NEVERBLEED
                            ,
                            neverbleed_t *nb
#endif // HAVE_NEVERBLEED
                            ) {
  auto ssl_ctx = SSL_CTX_new(SSLv23_server_method());
  if (!ssl_ctx) {
    LOG(FATAL) << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }

  constexpr auto ssl_opts =
      (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) | SSL_OP_NO_SSLv2 |
      SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION |
      SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION | SSL_OP_SINGLE_ECDH_USE |
      SSL_OP_SINGLE_DH_USE | SSL_OP_CIPHER_SERVER_PREFERENCE;

  auto config = mod_config();
  auto &tlsconf = config->tls;

  SSL_CTX_set_options(ssl_ctx, ssl_opts | tlsconf.tls_proto_mask);

  const unsigned char sid_ctx[] = "shrpx";
  SSL_CTX_set_session_id_context(ssl_ctx, sid_ctx, sizeof(sid_ctx) - 1);
  SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);

  if (!tlsconf.session_cache.memcached.host.empty()) {
    SSL_CTX_sess_set_new_cb(ssl_ctx, tls_session_new_cb);
    SSL_CTX_sess_set_get_cb(ssl_ctx, tls_session_get_cb);
  }

  SSL_CTX_set_timeout(ssl_ctx, tlsconf.session_timeout.count());

  const char *ciphers;
  if (!tlsconf.ciphers.empty()) {
    ciphers = tlsconf.ciphers.c_str();
  } else {
    ciphers = nghttp2::ssl::DEFAULT_CIPHER_LIST;
  }

  if (SSL_CTX_set_cipher_list(ssl_ctx, ciphers) == 0) {
    LOG(FATAL) << "SSL_CTX_set_cipher_list " << ciphers
               << " failed: " << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }

#ifndef OPENSSL_NO_EC
#if !LIBRESSL_IN_USE && OPENSSL_VERSION_NUMBER >= 0x10002000L
  if (SSL_CTX_set1_curves_list(ssl_ctx, tlsconf.ecdh_curves.c_str()) != 1) {
    LOG(FATAL) << "SSL_CTX_set1_curves_list " << tlsconf.ecdh_curves
               << " failed";
    DIE();
  }
#if !OPENSSL_1_1_API
  // It looks like we need this function call for OpenSSL 1.0.2.  This
  // function was deprecated in OpenSSL 1.1.0.
  SSL_CTX_set_ecdh_auto(ssl_ctx, 1);
#endif // !OPENSSL_1_1_API
#else  // LIBRESSL_IN_USE || OPENSSL_VERSION_NUBMER < 0x10002000L
  // Use P-256, which is sufficiently secure at the time of this
  // writing.
  auto ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  if (ecdh == nullptr) {
    LOG(FATAL) << "EC_KEY_new_by_curv_name failed: "
               << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }
  SSL_CTX_set_tmp_ecdh(ssl_ctx, ecdh);
  EC_KEY_free(ecdh);
#endif // LIBRESSL_IN_USE || OPENSSL_VERSION_NUBMER < 0x10002000L
#endif // OPENSSL_NO_EC

  if (!tlsconf.dh_param_file.empty()) {
    // Read DH parameters from file
    auto bio = BIO_new_file(tlsconf.dh_param_file.c_str(), "r");
    if (bio == nullptr) {
      LOG(FATAL) << "BIO_new_file() failed: "
                 << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
    auto dh = PEM_read_bio_DHparams(bio, nullptr, nullptr, nullptr);
    if (dh == nullptr) {
      LOG(FATAL) << "PEM_read_bio_DHparams() failed: "
                 << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
    SSL_CTX_set_tmp_dh(ssl_ctx, dh);
    DH_free(dh);
    BIO_free(bio);
  }

  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
  if (!tlsconf.private_key_passwd.empty()) {
    SSL_CTX_set_default_passwd_cb(ssl_ctx, ssl_pem_passwd_cb);
    SSL_CTX_set_default_passwd_cb_userdata(ssl_ctx, config);
  }

#ifndef HAVE_NEVERBLEED
  if (SSL_CTX_use_PrivateKey_file(ssl_ctx, private_key_file,
                                  SSL_FILETYPE_PEM) != 1) {
    LOG(FATAL) << "SSL_CTX_use_PrivateKey_file failed: "
               << ERR_error_string(ERR_get_error(), nullptr);
  }
#else  // HAVE_NEVERBLEED
  std::array<char, NEVERBLEED_ERRBUF_SIZE> errbuf;
  if (neverbleed_load_private_key_file(nb, ssl_ctx, private_key_file,
                                       errbuf.data()) != 1) {
    LOG(FATAL) << "neverbleed_load_private_key_file failed: " << errbuf.data();
    DIE();
  }
#endif // HAVE_NEVERBLEED

  if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file) != 1) {
    LOG(FATAL) << "SSL_CTX_use_certificate_file failed: "
               << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }
  if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
    LOG(FATAL) << "SSL_CTX_check_private_key failed: "
               << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }
  if (tlsconf.client_verify.enabled) {
    if (!tlsconf.client_verify.cacert.empty()) {
      if (SSL_CTX_load_verify_locations(
              ssl_ctx, tlsconf.client_verify.cacert.c_str(), nullptr) != 1) {

        LOG(FATAL) << "Could not load trusted ca certificates from "
                   << tlsconf.client_verify.cacert << ": "
                   << ERR_error_string(ERR_get_error(), nullptr);
        DIE();
      }
      // It is heard that SSL_CTX_load_verify_locations() may leave
      // error even though it returns success. See
      // http://forum.nginx.org/read.php?29,242540
      ERR_clear_error();
      auto list = SSL_load_client_CA_file(tlsconf.client_verify.cacert.c_str());
      if (!list) {
        LOG(FATAL) << "Could not load ca certificates from "
                   << tlsconf.client_verify.cacert << ": "
                   << ERR_error_string(ERR_get_error(), nullptr);
        DIE();
      }
      SSL_CTX_set_client_CA_list(ssl_ctx, list);
    }
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE |
                                    SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       verify_callback);
  }
  SSL_CTX_set_tlsext_servername_callback(ssl_ctx, servername_callback);
  SSL_CTX_set_tlsext_ticket_key_cb(ssl_ctx, ticket_key_cb);
#ifndef OPENSSL_IS_BORINGSSL
  SSL_CTX_set_tlsext_status_cb(ssl_ctx, ocsp_resp_cb);
#endif // OPENSSL_IS_BORINGSSL
  SSL_CTX_set_info_callback(ssl_ctx, info_callback);

  // NPN advertisement
  SSL_CTX_set_next_protos_advertised_cb(ssl_ctx, next_proto_cb, nullptr);
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  // ALPN selection callback
  SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_select_proto_cb, nullptr);
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L

  auto tls_ctx_data = new TLSContextData();
  tls_ctx_data->cert_file = cert_file;

  SSL_CTX_set_app_data(ssl_ctx, tls_ctx_data);

  return ssl_ctx;
}

namespace {
int select_h2_next_proto_cb(SSL *ssl, unsigned char **out,
                            unsigned char *outlen, const unsigned char *in,
                            unsigned int inlen, void *arg) {
  if (!util::select_h2(const_cast<const unsigned char **>(out), outlen, in,
                       inlen)) {
    return SSL_TLSEXT_ERR_NOACK;
  }

  return SSL_TLSEXT_ERR_OK;
}
} // namespace

namespace {
int select_h1_next_proto_cb(SSL *ssl, unsigned char **out,
                            unsigned char *outlen, const unsigned char *in,
                            unsigned int inlen, void *arg) {
  auto end = in + inlen;
  for (; in < end;) {
    if (util::streq(NGHTTP2_H1_1_ALPN, StringRef{in, in + (in[0] + 1)})) {
      *out = const_cast<unsigned char *>(in) + 1;
      *outlen = in[0];
      return SSL_TLSEXT_ERR_OK;
    }
    in += in[0] + 1;
  }

  return SSL_TLSEXT_ERR_NOACK;
}
} // namespace

namespace {
int select_next_proto_cb(SSL *ssl, unsigned char **out, unsigned char *outlen,
                         const unsigned char *in, unsigned int inlen,
                         void *arg) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  switch (conn->proto) {
  case PROTO_HTTP1:
    return select_h1_next_proto_cb(ssl, out, outlen, in, inlen, arg);
  case PROTO_HTTP2:
    return select_h2_next_proto_cb(ssl, out, outlen, in, inlen, arg);
  default:
    return SSL_TLSEXT_ERR_NOACK;
  }
}
} // namespace

SSL_CTX *create_ssl_client_context(
#ifdef HAVE_NEVERBLEED
    neverbleed_t *nb,
#endif // HAVE_NEVERBLEED
    const StringRef &cacert, const StringRef &cert_file,
    const StringRef &private_key_file,
    int (*next_proto_select_cb)(SSL *s, unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg)) {
  auto ssl_ctx = SSL_CTX_new(SSLv23_client_method());
  if (!ssl_ctx) {
    LOG(FATAL) << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }

  constexpr auto ssl_opts = (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                            SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                            SSL_OP_NO_COMPRESSION |
                            SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION;

  auto &tlsconf = get_config()->tls;

  SSL_CTX_set_options(ssl_ctx, ssl_opts | tlsconf.tls_proto_mask);

  const char *ciphers;
  if (!tlsconf.ciphers.empty()) {
    ciphers = tlsconf.ciphers.c_str();
  } else {
    ciphers = nghttp2::ssl::DEFAULT_CIPHER_LIST;
  }
  if (SSL_CTX_set_cipher_list(ssl_ctx, ciphers) == 0) {
    LOG(FATAL) << "SSL_CTX_set_cipher_list " << ciphers
               << " failed: " << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }

  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);

  if (SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
    LOG(WARN) << "Could not load system trusted ca certificates: "
              << ERR_error_string(ERR_get_error(), nullptr);
  }

  if (!cacert.empty()) {
    if (SSL_CTX_load_verify_locations(ssl_ctx, cacert.c_str(), nullptr) != 1) {

      LOG(FATAL) << "Could not load trusted ca certificates from " << cacert
                 << ": " << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
  }

  if (!cert_file.empty()) {
    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file.c_str()) != 1) {

      LOG(FATAL) << "Could not load client certificate from " << cert_file
                 << ": " << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
  }

  if (!private_key_file.empty()) {
#ifndef HAVE_NEVERBLEED
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, private_key_file.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
      LOG(FATAL) << "Could not load client private key from "
                 << private_key_file << ": "
                 << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
#else  // HAVE_NEVERBLEED
    std::array<char, NEVERBLEED_ERRBUF_SIZE> errbuf;
    if (neverbleed_load_private_key_file(nb, ssl_ctx, private_key_file.c_str(),
                                         errbuf.data()) != 1) {
      LOG(FATAL) << "neverbleed_load_private_key_file: could not load client "
                    "private key from " << private_key_file << ": "
                 << errbuf.data();
      DIE();
    }
#endif // HAVE_NEVERBLEED
  }

  // NPN selection callback.  This is required to set SSL_CTX because
  // OpenSSL does not offer SSL_set_next_proto_select_cb.
  SSL_CTX_set_next_proto_select_cb(ssl_ctx, next_proto_select_cb, nullptr);

  return ssl_ctx;
}

SSL *create_ssl(SSL_CTX *ssl_ctx) {
  auto ssl = SSL_new(ssl_ctx);
  if (!ssl) {
    LOG(ERROR) << "SSL_new() failed: " << ERR_error_string(ERR_get_error(),
                                                           nullptr);
    return nullptr;
  }

  return ssl;
}

ClientHandler *accept_connection(Worker *worker, int fd, sockaddr *addr,
                                 int addrlen, const UpstreamAddr *faddr) {
  std::array<char, NI_MAXHOST> host;
  std::array<char, NI_MAXSERV> service;
  int rv;

  if (addr->sa_family == AF_UNIX) {
    std::copy_n("localhost", sizeof("localhost"), std::begin(host));
    service[0] = '\0';
  } else {
    rv = getnameinfo(addr, addrlen, host.data(), host.size(), service.data(),
                     service.size(), NI_NUMERICHOST | NI_NUMERICSERV);
    if (rv != 0) {
      LOG(ERROR) << "getnameinfo() failed: " << gai_strerror(rv);

      return nullptr;
    }

    rv = util::make_socket_nodelay(fd);
    if (rv == -1) {
      LOG(WARN) << "Setting option TCP_NODELAY failed: errno=" << errno;
    }
  }
  SSL *ssl = nullptr;
  if (faddr->tls) {
    auto ssl_ctx = worker->get_sv_ssl_ctx();

    assert(ssl_ctx);

    ssl = create_ssl(ssl_ctx);
    if (!ssl) {
      return nullptr;
    }
    // Disable TLS session ticket if we don't have working ticket
    // keys.
    if (!worker->get_ticket_keys()) {
      SSL_set_options(ssl, SSL_OP_NO_TICKET);
    }
  }

  return new ClientHandler(worker, fd, ssl, StringRef{host.data()},
                           StringRef{service.data()}, addr->sa_family, faddr);
}

bool tls_hostname_match(const StringRef &pattern, const StringRef &hostname) {
  auto ptWildcard = std::find(std::begin(pattern), std::end(pattern), '*');
  if (ptWildcard == std::end(pattern)) {
    return util::strieq(pattern, hostname);
  }

  auto ptLeftLabelEnd = std::find(std::begin(pattern), std::end(pattern), '.');
  auto wildcardEnabled = true;
  // Do case-insensitive match. At least 2 dots are required to enable
  // wildcard match. Also wildcard must be in the left-most label.
  // Don't attempt to match a presented identifier where the wildcard
  // character is embedded within an A-label.
  if (ptLeftLabelEnd == std::end(pattern) ||
      std::find(ptLeftLabelEnd + 1, std::end(pattern), '.') ==
          std::end(pattern) ||
      ptLeftLabelEnd < ptWildcard || util::istarts_with_l(pattern, "xn--")) {
    wildcardEnabled = false;
  }

  if (!wildcardEnabled) {
    return util::strieq(pattern, hostname);
  }

  auto hnLeftLabelEnd =
      std::find(std::begin(hostname), std::end(hostname), '.');
  if (hnLeftLabelEnd == std::end(hostname) ||
      !util::strieq(StringRef{ptLeftLabelEnd, std::end(pattern)},
                    StringRef{hnLeftLabelEnd, std::end(hostname)})) {
    return false;
  }
  // Perform wildcard match. Here '*' must match at least one
  // character.
  if (hnLeftLabelEnd - std::begin(hostname) <
      ptLeftLabelEnd - std::begin(pattern)) {
    return false;
  }
  return util::istarts_with(StringRef{std::begin(hostname), hnLeftLabelEnd},
                            StringRef{std::begin(pattern), ptWildcard}) &&
         util::iends_with(StringRef{std::begin(hostname), hnLeftLabelEnd},
                          StringRef{ptWildcard + 1, ptLeftLabelEnd});
}

namespace {
// if return value is not empty, StringRef.c_str() must be freed using
// OPENSSL_free().
StringRef get_common_name(X509 *cert) {
  auto subjectname = X509_get_subject_name(cert);
  if (!subjectname) {
    LOG(WARN) << "Could not get X509 name object from the certificate.";
    return StringRef{};
  }
  int lastpos = -1;
  for (;;) {
    lastpos = X509_NAME_get_index_by_NID(subjectname, NID_commonName, lastpos);
    if (lastpos == -1) {
      break;
    }
    auto entry = X509_NAME_get_entry(subjectname, lastpos);

    unsigned char *p;
    auto plen = ASN1_STRING_to_UTF8(&p, X509_NAME_ENTRY_get_data(entry));
    if (plen < 0) {
      continue;
    }
    if (std::find(p, p + plen, '\0') != p + plen) {
      // Embedded NULL is not permitted.
      continue;
    }
    if (plen == 0) {
      LOG(WARN) << "X509 name is empty";
      OPENSSL_free(p);
      continue;
    }

    return StringRef{p, static_cast<size_t>(plen)};
  }
  return StringRef{};
}
} // namespace

namespace {
int verify_numeric_hostname(X509 *cert, const StringRef &hostname,
                            const Address *addr) {
  const void *saddr;
  switch (addr->su.storage.ss_family) {
  case AF_INET:
    saddr = &addr->su.in.sin_addr;
    break;
  case AF_INET6:
    saddr = &addr->su.in6.sin6_addr;
    break;
  default:
    return -1;
  }

  auto altnames = static_cast<GENERAL_NAMES *>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (altnames) {
    auto altnames_deleter = defer(GENERAL_NAMES_free, altnames);
    size_t n = sk_GENERAL_NAME_num(altnames);
    auto ip_found = false;
    for (size_t i = 0; i < n; ++i) {
      auto altname = sk_GENERAL_NAME_value(altnames, i);
      if (altname->type != GEN_IPADD) {
        continue;
      }

      auto ip_addr = altname->d.iPAddress->data;
      if (!ip_addr) {
        continue;
      }
      size_t ip_addrlen = altname->d.iPAddress->length;

      ip_found = true;
      if (addr->len == ip_addrlen && memcmp(saddr, ip_addr, ip_addrlen) == 0) {
        return 0;
      }
    }

    if (ip_found) {
      return -1;
    }
  }

  auto cn = get_common_name(cert);
  if (cn.empty()) {
    return -1;
  }

  // cn is not NULL terminated
  auto rv = util::streq(hostname, cn);
  OPENSSL_free(const_cast<char *>(cn.c_str()));

  if (rv) {
    return 0;
  }

  return -1;
}
} // namespace

namespace {
int verify_hostname(X509 *cert, const StringRef &hostname,
                    const Address *addr) {
  if (util::numeric_host(hostname.c_str())) {
    return verify_numeric_hostname(cert, hostname, addr);
  }

  auto altnames = static_cast<GENERAL_NAMES *>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (altnames) {
    auto dns_found = false;
    auto altnames_deleter = defer(GENERAL_NAMES_free, altnames);
    size_t n = sk_GENERAL_NAME_num(altnames);
    for (size_t i = 0; i < n; ++i) {
      auto altname = sk_GENERAL_NAME_value(altnames, i);
      if (altname->type != GEN_DNS) {
        continue;
      }

      auto name = ASN1_STRING_get0_data(altname->d.ia5);
      if (!name) {
        continue;
      }

      auto len = ASN1_STRING_length(altname->d.ia5);
      if (len == 0) {
        continue;
      }
      if (std::find(name, name + len, '\0') != name + len) {
        // Embedded NULL is not permitted.
        continue;
      }

      if (name[len - 1] == '.') {
        --len;
        if (len == 0) {
          continue;
        }
      }

      dns_found = true;

      if (tls_hostname_match(StringRef{name, static_cast<size_t>(len)},
                             hostname)) {
        return 0;
      }
    }

    // RFC 6125, section 6.4.4. says that client MUST not seek a match
    // for CN if a dns dNSName is found.
    if (dns_found) {
      return -1;
    }
  }

  auto cn = get_common_name(cert);
  if (cn.empty()) {
    return -1;
  }

  if (cn[cn.size() - 1] == '.') {
    if (cn.size() == 1) {
      OPENSSL_free(const_cast<char *>(cn.c_str()));

      return -1;
    }
    cn = StringRef{cn.c_str(), cn.size() - 1};
  }

  auto rv = tls_hostname_match(cn, hostname);
  OPENSSL_free(const_cast<char *>(cn.c_str()));

  return rv ? 0 : -1;
}
} // namespace

int check_cert(SSL *ssl, const Address *addr, const StringRef &host) {
  auto cert = SSL_get_peer_certificate(ssl);
  if (!cert) {
    LOG(ERROR) << "No certificate found";
    return -1;
  }
  auto cert_deleter = defer(X509_free, cert);
  auto verify_res = SSL_get_verify_result(ssl);
  if (verify_res != X509_V_OK) {
    LOG(ERROR) << "Certificate verification failed: "
               << X509_verify_cert_error_string(verify_res);
    return -1;
  }

  if (verify_hostname(cert, host, addr) != 0) {
    LOG(ERROR) << "Certificate verification failed: hostname does not match";
    return -1;
  }
  return 0;
}

int check_cert(SSL *ssl, const DownstreamAddr *addr) {
  auto hostname =
      addr->sni.empty() ? StringRef{addr->host} : StringRef{addr->sni};
  return check_cert(ssl, &addr->addr, hostname);
}

CertLookupTree::CertLookupTree() {}

void CertLookupTree::add_cert(const StringRef &hostname, size_t idx) {
  std::array<uint8_t, NI_MAXHOST> buf;

  // NI_MAXHOST includes terminal NULL byte
  if (hostname.empty() || hostname.size() + 1 > buf.size()) {
    return;
  }

  auto wildcard_it = std::find(std::begin(hostname), std::end(hostname), '*');
  if (wildcard_it != std::end(hostname) &&
      wildcard_it + 1 != std::end(hostname)) {
    auto wildcard_prefix = StringRef{std::begin(hostname), wildcard_it};
    auto wildcard_suffix = StringRef{wildcard_it + 1, std::end(hostname)};

    auto rev_suffix = StringRef{std::begin(buf),
                                std::reverse_copy(std::begin(wildcard_suffix),
                                                  std::end(wildcard_suffix),
                                                  std::begin(buf))};

    WildcardPattern *wpat;

    if (!rev_wildcard_router_.add_route(rev_suffix,
                                        wildcard_patterns_.size())) {
      auto wcidx = rev_wildcard_router_.match(rev_suffix);

      assert(wcidx != -1);

      wpat = &wildcard_patterns_[wcidx];
    } else {
      wildcard_patterns_.emplace_back();
      wpat = &wildcard_patterns_.back();
    }

    auto rev_prefix = StringRef{std::begin(buf),
                                std::reverse_copy(std::begin(wildcard_prefix),
                                                  std::end(wildcard_prefix),
                                                  std::begin(buf))};

    wpat->rev_prefix.emplace_back(rev_prefix, idx);

    return;
  }

  router_.add_route(hostname, idx);
}

ssize_t CertLookupTree::lookup(const StringRef &hostname) {
  std::array<uint8_t, NI_MAXHOST> buf;

  // NI_MAXHOST includes terminal NULL byte
  if (hostname.empty() || hostname.size() + 1 > buf.size()) {
    return -1;
  }

  // Always prefer exact match
  auto idx = router_.match(hostname);
  if (idx != -1) {
    return idx;
  }

  if (wildcard_patterns_.empty()) {
    return -1;
  }

  ssize_t best_idx = -1;
  size_t best_prefixlen = 0;
  const RNode *last_node = nullptr;

  auto rev_host = StringRef{
      std::begin(buf), std::reverse_copy(std::begin(hostname),
                                         std::end(hostname), std::begin(buf))};

  for (;;) {
    size_t nread = 0;

    auto wcidx =
        rev_wildcard_router_.match_prefix(&nread, &last_node, rev_host);
    if (wcidx == -1) {
      return best_idx;
    }

    // '*' must match at least one byte
    if (nread == rev_host.size()) {
      return best_idx;
    }

    rev_host = StringRef{std::begin(rev_host) + nread, std::end(rev_host)};

    auto rev_prefix = StringRef{std::begin(rev_host) + 1, std::end(rev_host)};

    auto &wpat = wildcard_patterns_[wcidx];
    for (auto &wprefix : wpat.rev_prefix) {
      if (!util::ends_with(rev_prefix, wprefix.prefix)) {
        continue;
      }

      auto prefixlen =
          wprefix.prefix.size() +
          (reinterpret_cast<const uint8_t *>(&rev_host[0]) - &buf[0]);

      // Breaking a tie with longer suffix
      if (prefixlen < best_prefixlen) {
        continue;
      }

      best_idx = wprefix.idx;
      best_prefixlen = prefixlen;
    }
  }
}

void CertLookupTree::dump() const {
  std::cerr << "exact:" << std::endl;
  router_.dump();
  std::cerr << "wildcard suffix (reversed):" << std::endl;
  rev_wildcard_router_.dump();
}

int cert_lookup_tree_add_cert_from_x509(CertLookupTree *lt, size_t idx,
                                        X509 *cert) {
  std::array<uint8_t, NI_MAXHOST> buf;

  auto altnames = static_cast<GENERAL_NAMES *>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (altnames) {
    auto altnames_deleter = defer(GENERAL_NAMES_free, altnames);
    size_t n = sk_GENERAL_NAME_num(altnames);
    auto dns_found = false;
    for (size_t i = 0; i < n; ++i) {
      auto altname = sk_GENERAL_NAME_value(altnames, i);
      if (altname->type != GEN_DNS) {
        continue;
      }

      auto name = ASN1_STRING_get0_data(altname->d.ia5);
      if (!name) {
        continue;
      }

      auto len = ASN1_STRING_length(altname->d.ia5);
      if (len == 0) {
        continue;
      }
      if (std::find(name, name + len, '\0') != name + len) {
        // Embedded NULL is not permitted.
        continue;
      }

      if (name[len - 1] == '.') {
        --len;
        if (len == 0) {
          continue;
        }
      }

      dns_found = true;

      if (static_cast<size_t>(len) + 1 > buf.size()) {
        continue;
      }

      auto end_buf = std::copy_n(name, len, std::begin(buf));
      util::inp_strlower(std::begin(buf), end_buf);

      lt->add_cert(StringRef{std::begin(buf), end_buf}, idx);
    }

    // Don't bother CN if we have dNSName.
    if (dns_found) {
      return 0;
    }
  }

  auto cn = get_common_name(cert);
  if (cn.empty()) {
    return 0;
  }

  if (cn[cn.size() - 1] == '.') {
    if (cn.size() == 1) {
      OPENSSL_free(const_cast<char *>(cn.c_str()));

      return 0;
    }

    cn = StringRef{cn.c_str(), cn.size() - 1};
  }

  auto end_buf = std::copy(std::begin(cn), std::end(cn), std::begin(buf));

  OPENSSL_free(const_cast<char *>(cn.c_str()));

  util::inp_strlower(std::begin(buf), end_buf);

  lt->add_cert(StringRef{std::begin(buf), end_buf}, idx);

  return 0;
}

bool in_proto_list(const std::vector<StringRef> &protos,
                   const StringRef &needle) {
  for (auto &proto : protos) {
    if (util::streq(proto, needle)) {
      return true;
    }
  }
  return false;
}

bool upstream_tls_enabled() {
  const auto &faddrs = get_config()->conn.listener.addrs;
  return std::any_of(std::begin(faddrs), std::end(faddrs),
                     [](const UpstreamAddr &faddr) { return faddr.tls; });
}

X509 *load_certificate(const char *filename) {
  auto bio = BIO_new(BIO_s_file());
  if (!bio) {
    fprintf(stderr, "BIO_new() failed\n");
    return nullptr;
  }
  auto bio_deleter = defer(BIO_vfree, bio);
  if (!BIO_read_filename(bio, filename)) {
    fprintf(stderr, "Could not read certificate file '%s'\n", filename);
    return nullptr;
  }
  auto cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  if (!cert) {
    fprintf(stderr, "Could not read X509 structure from file '%s'\n", filename);
    return nullptr;
  }

  return cert;
}

SSL_CTX *setup_server_ssl_context(std::vector<SSL_CTX *> &all_ssl_ctx,
                                  CertLookupTree *cert_tree
#ifdef HAVE_NEVERBLEED
                                  ,
                                  neverbleed_t *nb
#endif // HAVE_NEVERBLEED
                                  ) {
  if (!upstream_tls_enabled()) {
    return nullptr;
  }

  auto &tlsconf = get_config()->tls;

  auto ssl_ctx = ssl::create_ssl_context(tlsconf.private_key_file.c_str(),
                                         tlsconf.cert_file.c_str()
#ifdef HAVE_NEVERBLEED
                                             ,
                                         nb
#endif // HAVE_NEVERBLEED
                                         );

  all_ssl_ctx.push_back(ssl_ctx);

  if (tlsconf.subcerts.empty()) {
    return ssl_ctx;
  }

  if (!cert_tree) {
    LOG(WARN) << "We have multiple additional certificates (--subcert), but "
                 "cert_tree is not given.  SNI may not work.";
    return ssl_ctx;
  }

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10002000L
  auto cert = SSL_CTX_get0_certificate(ssl_ctx);
#else  // defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER <
  // 0x10002000L
  auto cert = load_certificate(tlsconf.cert_file.c_str());
  auto cert_deleter = defer(X509_free, cert);
#endif // defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER <
       // 0x10002000L

  if (ssl::cert_lookup_tree_add_cert_from_x509(
          cert_tree, all_ssl_ctx.size() - 1, cert) == -1) {
    LOG(FATAL) << "Failed to add default certificate.";
    DIE();
  }

  for (auto &keycert : tlsconf.subcerts) {
    auto &priv_key_file = keycert.first;
    auto &cert_file = keycert.second;

    auto ssl_ctx =
        ssl::create_ssl_context(priv_key_file.c_str(), cert_file.c_str()
#ifdef HAVE_NEVERBLEED
                                                           ,
                                nb
#endif // HAVE_NEVERBLEED
                                );
    all_ssl_ctx.push_back(ssl_ctx);

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10002000L
    auto cert = SSL_CTX_get0_certificate(ssl_ctx);
#else  // defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER <
    // 0x10002000L
    auto cert = load_certificate(cert_file.c_str());
    auto cert_deleter = defer(X509_free, cert);
#endif // defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER <
       // 0x10002000L

    if (ssl::cert_lookup_tree_add_cert_from_x509(
            cert_tree, all_ssl_ctx.size() - 1, cert) == -1) {
      LOG(FATAL) << "Failed to add sub certificate.";
      DIE();
    }
  }

  return ssl_ctx;
}

SSL_CTX *setup_downstream_client_ssl_context(
#ifdef HAVE_NEVERBLEED
    neverbleed_t *nb
#endif // HAVE_NEVERBLEED
    ) {
  auto &tlsconf = get_config()->tls;

  return ssl::create_ssl_client_context(
#ifdef HAVE_NEVERBLEED
      nb,
#endif // HAVE_NEVERBLEED
      tlsconf.cacert, tlsconf.client.cert_file, tlsconf.client.private_key_file,
      select_next_proto_cb);
}

void setup_downstream_http2_alpn(SSL *ssl) {
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  // ALPN advertisement
  auto alpn = util::get_default_alpn();
  SSL_set_alpn_protos(ssl, alpn.data(), alpn.size());
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L
}

void setup_downstream_http1_alpn(SSL *ssl) {
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  // ALPN advertisement
  SSL_set_alpn_protos(ssl, NGHTTP2_H1_1_ALPN.byte(), NGHTTP2_H1_1_ALPN.size());
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L
}

std::unique_ptr<CertLookupTree> create_cert_lookup_tree() {
  if (!upstream_tls_enabled() || get_config()->tls.subcerts.empty()) {
    return nullptr;
  }
  return make_unique<CertLookupTree>();
}

namespace {
std::vector<uint8_t> serialize_ssl_session(SSL_SESSION *session) {
  auto len = i2d_SSL_SESSION(session, nullptr);
  auto buf = std::vector<uint8_t>(len);
  auto p = buf.data();
  i2d_SSL_SESSION(session, &p);

  return buf;
}
} // namespace

void try_cache_tls_session(TLSSessionCache &cache, const Address &addr,
                           SSL_SESSION *session, ev_tstamp t) {
  if (cache.last_updated + 1_min > t) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Cache for addr=" << util::to_numeric_addr(&addr)
                << " is still host.  Not updating.";
    }
    return;
  }

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Update cache entry for SSL_SESSION=" << session
              << ", addr=" << util::to_numeric_addr(&addr)
              << ", timestamp=" << std::fixed << std::setprecision(6) << t;
  }

  cache.session_data = serialize_ssl_session(session);
  cache.last_updated = t;
}

SSL_SESSION *reuse_tls_session(const TLSSessionCache &cache) {
  if (cache.session_data.empty()) {
    return nullptr;
  }

  auto p = cache.session_data.data();
  return d2i_SSL_SESSION(nullptr, &p, cache.session_data.size());
}

} // namespace ssl

} // namespace shrpx
