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
#include "shrpx_config.h"

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif // HAVE_PWD_H
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif // HAVE_NETDB_H
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif // HAVE_SYSLOG_H
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif // HAVE_FCNTL_H
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif // HAVE_UNISTD_H

#include <cstring>
#include <cerrno>
#include <limits>
#include <fstream>

#include <nghttp2/nghttp2.h>

#include "http-parser/http_parser.h"

#include "shrpx_log.h"
#include "shrpx_ssl.h"
#include "shrpx_http.h"
#include "util.h"
#include "base64.h"
#include "ssl_compat.h"

namespace shrpx {

namespace {
std::unique_ptr<Config> config;
} // namespace

constexpr auto SHRPX_UNIX_PATH_PREFIX = StringRef::from_lit("unix:");

const Config *get_config() { return config.get(); }

Config *mod_config() { return config.get(); }

std::unique_ptr<Config> replace_config(std::unique_ptr<Config> another) {
  config.swap(another);
  return another;
}

void create_config() { config = make_unique<Config>(); }

Config::~Config() {
  auto &upstreamconf = http2.upstream;

  nghttp2_option_del(upstreamconf.option);
  nghttp2_option_del(upstreamconf.alt_mode_option);
  nghttp2_session_callbacks_del(upstreamconf.callbacks);

  auto &downstreamconf = http2.downstream;

  nghttp2_option_del(downstreamconf.option);
  nghttp2_session_callbacks_del(downstreamconf.callbacks);

  auto &dumpconf = http2.upstream.debug.dump;

  if (dumpconf.request_header) {
    fclose(dumpconf.request_header);
  }

  if (dumpconf.response_header) {
    fclose(dumpconf.response_header);
  }
}

TicketKeys::~TicketKeys() {
  /* Erase keys from memory */
  for (auto &key : keys) {
    memset(&key, 0, sizeof(key));
  }
}

namespace {
int split_host_port(char *host, size_t hostlen, uint16_t *port_ptr,
                    const StringRef &hostport, const StringRef &opt) {
  // host and port in |hostport| is separated by single ','.
  auto sep = std::find(std::begin(hostport), std::end(hostport), ',');
  if (sep == std::end(hostport)) {
    LOG(ERROR) << opt << ": Invalid host, port: " << hostport;
    return -1;
  }
  size_t len = sep - std::begin(hostport);
  if (hostlen < len + 1) {
    LOG(ERROR) << opt << ": Hostname too long: " << hostport;
    return -1;
  }
  std::copy(std::begin(hostport), sep, host);
  host[len] = '\0';

  auto portstr = StringRef{sep + 1, std::end(hostport)};
  auto d = util::parse_uint(portstr);
  if (1 <= d && d <= std::numeric_limits<uint16_t>::max()) {
    *port_ptr = d;
    return 0;
  }

  LOG(ERROR) << opt << ": Port is invalid: " << portstr;
  return -1;
}
} // namespace

namespace {
bool is_secure(const StringRef &filename) {
  struct stat buf;
  int rv = stat(filename.c_str(), &buf);
  if (rv == 0) {
    if ((buf.st_mode & S_IRWXU) && !(buf.st_mode & S_IRWXG) &&
        !(buf.st_mode & S_IRWXO)) {
      return true;
    }
  }

  return false;
}
} // namespace

std::unique_ptr<TicketKeys>
read_tls_ticket_key_file(const std::vector<StringRef> &files,
                         const EVP_CIPHER *cipher, const EVP_MD *hmac) {
  auto ticket_keys = make_unique<TicketKeys>();
  auto &keys = ticket_keys->keys;
  keys.resize(files.size());
  auto enc_keylen = EVP_CIPHER_key_length(cipher);
  auto hmac_keylen = EVP_MD_size(hmac);
  if (cipher == EVP_aes_128_cbc()) {
    // backward compatibility, as a legacy of using same file format
    // with nginx and apache.
    hmac_keylen = 16;
  }
  auto expectedlen = keys[0].data.name.size() + enc_keylen + hmac_keylen;
  char buf[256];
  assert(sizeof(buf) >= expectedlen);

  size_t i = 0;
  for (auto &file : files) {
    struct stat fst {};

    if (stat(file.c_str(), &fst) == -1) {
      auto error = errno;
      LOG(ERROR) << "tls-ticket-key-file: could not stat file " << file
                 << ", errno=" << error;
      return nullptr;
    }

    if (static_cast<size_t>(fst.st_size) != expectedlen) {
      LOG(ERROR) << "tls-ticket-key-file: the expected file size is "
                 << expectedlen << ", the actual file size is " << fst.st_size;
      return nullptr;
    }

    std::ifstream f(file.c_str());
    if (!f) {
      LOG(ERROR) << "tls-ticket-key-file: could not open file " << file;
      return nullptr;
    }

    f.read(buf, expectedlen);
    if (static_cast<size_t>(f.gcount()) != expectedlen) {
      LOG(ERROR) << "tls-ticket-key-file: want to read " << expectedlen
                 << " bytes but only read " << f.gcount() << " bytes from "
                 << file;
      return nullptr;
    }

    auto &key = keys[i++];
    key.cipher = cipher;
    key.hmac = hmac;
    key.hmac_keylen = hmac_keylen;

    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "enc_keylen=" << enc_keylen
                << ", hmac_keylen=" << key.hmac_keylen;
    }

    auto p = buf;
    std::copy_n(p, key.data.name.size(), std::begin(key.data.name));
    p += key.data.name.size();
    std::copy_n(p, enc_keylen, std::begin(key.data.enc_key));
    p += enc_keylen;
    std::copy_n(p, hmac_keylen, std::begin(key.data.hmac_key));

    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "session ticket key: " << util::format_hex(key.data.name);
    }
  }
  return ticket_keys;
}

FILE *open_file_for_write(const char *filename) {
#if defined O_CLOEXEC
  auto fd = open(filename, O_WRONLY | O_CLOEXEC | O_CREAT | O_TRUNC,
                 S_IRUSR | S_IWUSR);
#else
  auto fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

  // We get race condition if execve is called at the same time.
  if (fd != -1) {
    util::make_socket_closeonexec(fd);
  }
#endif
  if (fd == -1) {
    LOG(ERROR) << "Failed to open " << filename
               << " for writing. Cause: " << strerror(errno);
    return nullptr;
  }
  auto f = fdopen(fd, "wb");
  if (f == nullptr) {
    LOG(ERROR) << "Failed to open " << filename
               << " for writing. Cause: " << strerror(errno);
    return nullptr;
  }

  return f;
}

namespace {
// Read passwd from |filename|
std::string read_passwd_from_file(const StringRef &opt,
                                  const StringRef &filename) {
  std::string line;

  if (!is_secure(filename)) {
    LOG(ERROR) << opt << ": Private key passwd file " << filename
               << " has insecure mode.";
    return line;
  }

  std::ifstream in(filename.c_str(), std::ios::binary);
  if (!in) {
    LOG(ERROR) << opt << ": Could not open key passwd file " << filename;
    return line;
  }

  std::getline(in, line);
  return line;
}
} // namespace

HeaderRefs::value_type parse_header(BlockAllocator &balloc,
                                    const StringRef &optarg) {
  auto colon = std::find(std::begin(optarg), std::end(optarg), ':');

  if (colon == std::end(optarg) || colon == std::begin(optarg)) {
    return {};
  }

  auto value = colon + 1;
  for (; *value == '\t' || *value == ' '; ++value)
    ;

  auto name_iov =
      make_byte_ref(balloc, std::distance(std::begin(optarg), colon) + 1);
  auto p = name_iov.base;
  p = std::copy(std::begin(optarg), colon, p);
  util::inp_strlower(name_iov.base, p);
  *p = '\0';

  auto nv =
      HeaderRef(StringRef{name_iov.base, p},
                make_string_ref(balloc, StringRef{value, std::end(optarg)}));

  if (!nghttp2_check_header_name(nv.name.byte(), nv.name.size()) ||
      !nghttp2_check_header_value(nv.value.byte(), nv.value.size())) {
    return {};
  }

  return nv;
}

template <typename T>
int parse_uint(T *dest, const StringRef &opt, const StringRef &optarg) {
  auto val = util::parse_uint(optarg);
  if (val == -1) {
    LOG(ERROR) << opt << ": bad value.  Specify an integer >= 0.";
    return -1;
  }

  *dest = val;

  return 0;
}

namespace {
template <typename T>
int parse_uint_with_unit(T *dest, const StringRef &opt,
                         const StringRef &optarg) {
  auto n = util::parse_uint_with_unit(optarg);
  if (n == -1) {
    LOG(ERROR) << opt << ": bad value: '" << optarg << "'";
    return -1;
  }

  if (static_cast<uint64_t>(std::numeric_limits<T>::max()) <
      static_cast<uint64_t>(n)) {
    LOG(ERROR) << opt
               << ": too large.  The value should be less than or equal to "
               << std::numeric_limits<T>::max();
    return -1;
  }

  *dest = n;

  return 0;
}
} // namespace

// Parses |optarg| as signed integer.  This requires |optarg| to be
// NULL-terminated string.
template <typename T>
int parse_int(T *dest, const StringRef &opt, const char *optarg) {
  char *end = nullptr;

  errno = 0;

  auto val = strtol(optarg, &end, 10);

  if (!optarg[0] || errno != 0 || *end) {
    LOG(ERROR) << opt << ": bad value.  Specify an integer.";
    return -1;
  }

  *dest = val;

  return 0;
}

namespace {
// generated by gennghttpxfun.py
LogFragmentType log_var_lookup_token(const char *name, size_t namelen) {
  switch (namelen) {
  case 3:
    switch (name[2]) {
    case 'd':
      if (util::strieq_l("pi", name, 2)) {
        return SHRPX_LOGF_PID;
      }
      break;
    }
    break;
  case 4:
    switch (name[3]) {
    case 'n':
      if (util::strieq_l("alp", name, 3)) {
        return SHRPX_LOGF_ALPN;
      }
      break;
    }
    break;
  case 6:
    switch (name[5]) {
    case 's':
      if (util::strieq_l("statu", name, 5)) {
        return SHRPX_LOGF_STATUS;
      }
      break;
    }
    break;
  case 7:
    switch (name[6]) {
    case 't':
      if (util::strieq_l("reques", name, 6)) {
        return SHRPX_LOGF_REQUEST;
      }
      break;
    }
    break;
  case 10:
    switch (name[9]) {
    case 'l':
      if (util::strieq_l("time_loca", name, 9)) {
        return SHRPX_LOGF_TIME_LOCAL;
      }
      break;
    case 'r':
      if (util::strieq_l("ssl_ciphe", name, 9)) {
        return SHRPX_LOGF_SSL_CIPHER;
      }
      break;
    }
    break;
  case 11:
    switch (name[10]) {
    case 'r':
      if (util::strieq_l("remote_add", name, 10)) {
        return SHRPX_LOGF_REMOTE_ADDR;
      }
      break;
    case 't':
      if (util::strieq_l("remote_por", name, 10)) {
        return SHRPX_LOGF_REMOTE_PORT;
      }
      if (util::strieq_l("server_por", name, 10)) {
        return SHRPX_LOGF_SERVER_PORT;
      }
      break;
    }
    break;
  case 12:
    switch (name[11]) {
    case '1':
      if (util::strieq_l("time_iso860", name, 11)) {
        return SHRPX_LOGF_TIME_ISO8601;
      }
      break;
    case 'e':
      if (util::strieq_l("request_tim", name, 11)) {
        return SHRPX_LOGF_REQUEST_TIME;
      }
      break;
    case 'l':
      if (util::strieq_l("ssl_protoco", name, 11)) {
        return SHRPX_LOGF_SSL_PROTOCOL;
      }
      break;
    case 't':
      if (util::strieq_l("backend_hos", name, 11)) {
        return SHRPX_LOGF_BACKEND_HOST;
      }
      if (util::strieq_l("backend_por", name, 11)) {
        return SHRPX_LOGF_BACKEND_PORT;
      }
      break;
    }
    break;
  case 14:
    switch (name[13]) {
    case 'd':
      if (util::strieq_l("ssl_session_i", name, 13)) {
        return SHRPX_LOGF_SSL_SESSION_ID;
      }
      break;
    }
    break;
  case 15:
    switch (name[14]) {
    case 't':
      if (util::strieq_l("body_bytes_sen", name, 14)) {
        return SHRPX_LOGF_BODY_BYTES_SENT;
      }
      break;
    }
    break;
  case 18:
    switch (name[17]) {
    case 'd':
      if (util::strieq_l("ssl_session_reuse", name, 17)) {
        return SHRPX_LOGF_SSL_SESSION_REUSED;
      }
      break;
    }
    break;
  }
  return SHRPX_LOGF_NONE;
}
} // namespace

namespace {
bool var_token(char c) {
  return util::is_alpha(c) || util::is_digit(c) || c == '_';
}
} // namespace

std::vector<LogFragment> parse_log_format(BlockAllocator &balloc,
                                          const StringRef &optarg) {
  auto literal_start = std::begin(optarg);
  auto p = literal_start;
  auto eop = std::end(optarg);

  auto res = std::vector<LogFragment>();

  for (; p != eop;) {
    if (*p != '$') {
      ++p;
      continue;
    }

    auto var_start = p;

    ++p;

    const char *var_name;
    size_t var_namelen;
    if (p != eop && *p == '{') {
      var_name = ++p;
      for (; p != eop && var_token(*p); ++p)
        ;

      if (p == eop || *p != '}') {
        LOG(WARN) << "Missing '}' after " << StringRef{var_start, p};
        continue;
      }

      var_namelen = p - var_name;
      ++p;
    } else {
      var_name = p;
      for (; p != eop && var_token(*p); ++p)
        ;

      var_namelen = p - var_name;
    }

    const char *value = nullptr;

    auto type = log_var_lookup_token(var_name, var_namelen);

    if (type == SHRPX_LOGF_NONE) {
      if (util::istarts_with_l(StringRef{var_name, var_namelen}, "http_")) {
        if (util::streq_l("host", StringRef{var_name + str_size("http_"),
                                            var_namelen - str_size("http_")})) {
          // Special handling of host header field.  We will use
          // :authority header field if host header is missing.  This
          // is a typical case in HTTP/2.
          type = SHRPX_LOGF_AUTHORITY;
        } else {
          type = SHRPX_LOGF_HTTP;
          value = var_name + str_size("http_");
        }
      } else {
        LOG(WARN) << "Unrecognized log format variable: "
                  << StringRef{var_name, var_namelen};
        continue;
      }
    }

    if (literal_start < var_start) {
      res.emplace_back(
          SHRPX_LOGF_LITERAL,
          make_string_ref(balloc, StringRef{literal_start, var_start}));
    }

    literal_start = p;

    if (value == nullptr) {
      res.emplace_back(type);
      continue;
    }

    {
      auto iov = make_byte_ref(
          balloc, std::distance(value, var_name + var_namelen) + 1);
      auto p = iov.base;
      p = std::copy(value, var_name + var_namelen, p);
      for (auto cp = iov.base; cp != p; ++cp) {
        if (*cp == '_') {
          *cp = '-';
        }
      }
      *p = '\0';
      res.emplace_back(type, StringRef{iov.base, p});
    }
  }

  if (literal_start != eop) {
    res.emplace_back(SHRPX_LOGF_LITERAL,
                     make_string_ref(balloc, StringRef{literal_start, eop}));
  }

  return res;
}

namespace {
int parse_address_family(int *dest, const StringRef &opt,
                         const StringRef &optarg) {
  if (util::strieq_l("auto", optarg)) {
    *dest = AF_UNSPEC;
    return 0;
  }
  if (util::strieq_l("IPv4", optarg)) {
    *dest = AF_INET;
    return 0;
  }
  if (util::strieq_l("IPv6", optarg)) {
    *dest = AF_INET6;
    return 0;
  }

  LOG(ERROR) << opt << ": bad value: '" << optarg << "'";
  return -1;
}
} // namespace

namespace {
int parse_duration(ev_tstamp *dest, const StringRef &opt,
                   const StringRef &optarg) {
  auto t = util::parse_duration_with_unit(optarg);
  if (t == std::numeric_limits<double>::infinity()) {
    LOG(ERROR) << opt << ": bad value: '" << optarg << "'";
    return -1;
  }

  *dest = t;

  return 0;
}
} // namespace

struct MemcachedConnectionParams {
  bool tls;
};

namespace {
// Parses memcached connection configuration parameter |src_params|,
// and stores parsed results into |out|.  This function returns 0 if
// it succeeds, or -1.
int parse_memcached_connection_params(MemcachedConnectionParams &out,
                                      const StringRef &src_params,
                                      const StringRef &opt) {
  auto last = std::end(src_params);
  for (auto first = std::begin(src_params); first != last;) {
    auto end = std::find(first, last, ';');
    auto param = StringRef{first, end};

    if (util::strieq_l("tls", param)) {
      out.tls = true;
    } else if (util::strieq_l("no-tls", param)) {
      out.tls = false;
    } else if (!param.empty()) {
      LOG(ERROR) << opt << ": " << param << ": unknown keyword";
      return -1;
    }

    if (end == last) {
      break;
    }

    first = end + 1;
  }

  return 0;
}
} // namespace

struct UpstreamParams {
  int alt_mode;
  bool tls;
};

namespace {
// Parses upstream configuration parameter |src_params|, and stores
// parsed results into |out|.  This function returns 0 if it succeeds,
// or -1.
int parse_upstream_params(UpstreamParams &out, const StringRef &src_params) {
  auto last = std::end(src_params);
  for (auto first = std::begin(src_params); first != last;) {
    auto end = std::find(first, last, ';');
    auto param = StringRef{first, end};

    if (util::strieq_l("tls", param)) {
      out.tls = true;
    } else if (util::strieq_l("no-tls", param)) {
      out.tls = false;
    } else if (util::strieq_l("api", param)) {
      if (out.alt_mode && out.alt_mode != ALTMODE_API) {
        LOG(ERROR) << "frontend: api and healthmon are mutually exclusive";
        return -1;
      }
      out.alt_mode = ALTMODE_API;
    } else if (util::strieq_l("healthmon", param)) {
      if (out.alt_mode && out.alt_mode != ALTMODE_HEALTHMON) {
        LOG(ERROR) << "frontend: api and healthmon are mutually exclusive";
        return -1;
      }
      out.alt_mode = ALTMODE_HEALTHMON;
    } else if (!param.empty()) {
      LOG(ERROR) << "frontend: " << param << ": unknown keyword";
      return -1;
    }

    if (end == last) {
      break;
    }

    first = end + 1;
  }

  return 0;
}
} // namespace

struct DownstreamParams {
  StringRef sni;
  size_t fall;
  size_t rise;
  shrpx_proto proto;
  shrpx_session_affinity affinity;
  bool tls;
};

namespace {
// Parses downstream configuration parameter |src_params|, and stores
// parsed results into |out|.  This function returns 0 if it succeeds,
// or -1.
int parse_downstream_params(DownstreamParams &out,
                            const StringRef &src_params) {
  auto last = std::end(src_params);
  for (auto first = std::begin(src_params); first != last;) {
    auto end = std::find(first, last, ';');
    auto param = StringRef{first, end};

    if (util::istarts_with_l(param, "proto=")) {
      auto protostr = StringRef{first + str_size("proto="), end};
      if (protostr.empty()) {
        LOG(ERROR) << "backend: proto: protocol is empty";
        return -1;
      }

      if (util::streq_l("h2", std::begin(protostr), protostr.size())) {
        out.proto = PROTO_HTTP2;
      } else if (util::streq_l("http/1.1", std::begin(protostr),
                               protostr.size())) {
        out.proto = PROTO_HTTP1;
      } else {
        LOG(ERROR) << "backend: proto: unknown protocol " << protostr;
        return -1;
      }
    } else if (util::istarts_with_l(param, "fall=")) {
      auto valstr = StringRef{first + str_size("fall="), end};
      if (valstr.empty()) {
        LOG(ERROR) << "backend: fall: non-negative integer is expected";
        return -1;
      }

      auto n = util::parse_uint(valstr);
      if (n == -1) {
        LOG(ERROR) << "backend: fall: non-negative integer is expected";
        return -1;
      }

      out.fall = n;
    } else if (util::istarts_with_l(param, "rise=")) {
      auto valstr = StringRef{first + str_size("rise="), end};
      if (valstr.empty()) {
        LOG(ERROR) << "backend: rise: non-negative integer is expected";
        return -1;
      }

      auto n = util::parse_uint(valstr);
      if (n == -1) {
        LOG(ERROR) << "backend: rise: non-negative integer is expected";
        return -1;
      }

      out.rise = n;
    } else if (util::strieq_l("tls", param)) {
      out.tls = true;
    } else if (util::strieq_l("no-tls", param)) {
      out.tls = false;
    } else if (util::istarts_with_l(param, "sni=")) {
      out.sni = StringRef{first + str_size("sni="), end};
    } else if (util::istarts_with_l(param, "affinity=")) {
      auto valstr = StringRef{first + str_size("affinity="), end};
      if (util::strieq_l("none", valstr)) {
        out.affinity = AFFINITY_NONE;
      } else if (util::strieq_l("ip", valstr)) {
        out.affinity = AFFINITY_IP;
      } else {
        LOG(ERROR) << "backend: affinity: value must be either none or ip";
        return -1;
      }
    } else if (!param.empty()) {
      LOG(ERROR) << "backend: " << param << ": unknown keyword";
      return -1;
    }

    if (end == last) {
      break;
    }

    first = end + 1;
  }

  return 0;
}
} // namespace

namespace {
// Parses host-path mapping patterns in |src_pattern|, and stores
// mappings in config.  We will store each host-path pattern found in
// |src| with |addr|.  |addr| will be copied accordingly.  Also we
// make a group based on the pattern.  The "/" pattern is considered
// as catch-all.  We also parse protocol specified in |src_proto|.
//
// This function returns 0 if it succeeds, or -1.
int parse_mapping(Config *config, DownstreamAddrConfig &addr,
                  const StringRef &src_pattern, const StringRef &src_params) {
  // This returns at least 1 element (it could be empty string).  We
  // will append '/' to all patterns, so it becomes catch-all pattern.
  auto mapping = util::split_str(src_pattern, ':');
  assert(!mapping.empty());
  auto &downstreamconf = *config->conn.downstream;
  auto &addr_groups = downstreamconf.addr_groups;

  DownstreamParams params{};
  params.proto = PROTO_HTTP1;

  if (parse_downstream_params(params, src_params) != 0) {
    return -1;
  }

  addr.fall = params.fall;
  addr.rise = params.rise;
  addr.proto = params.proto;
  addr.tls = params.tls;
  addr.sni = make_string_ref(downstreamconf.balloc, params.sni);

  auto &routerconf = downstreamconf.router;
  auto &router = routerconf.router;
  auto &rw_router = routerconf.rev_wildcard_router;
  auto &wildcard_patterns = routerconf.wildcard_patterns;

  for (const auto &raw_pattern : mapping) {
    auto done = false;
    StringRef pattern;
    auto slash = std::find(std::begin(raw_pattern), std::end(raw_pattern), '/');
    if (slash == std::end(raw_pattern)) {
      // This effectively makes empty pattern to "/".  2 for '/' and
      // terminal NULL character.
      auto iov = make_byte_ref(downstreamconf.balloc, raw_pattern.size() + 2);
      auto p = iov.base;
      p = std::copy(std::begin(raw_pattern), std::end(raw_pattern), p);
      util::inp_strlower(iov.base, p);
      *p++ = '/';
      *p = '\0';
      pattern = StringRef{iov.base, p};
    } else {
      auto path = http2::normalize_path(downstreamconf.balloc,
                                        StringRef{slash, std::end(raw_pattern)},
                                        StringRef{});
      auto iov = make_byte_ref(downstreamconf.balloc,
                               std::distance(std::begin(raw_pattern), slash) +
                                   path.size() + 1);
      auto p = iov.base;
      p = std::copy(std::begin(raw_pattern), slash, p);
      util::inp_strlower(iov.base, p);
      p = std::copy(std::begin(path), std::end(path), p);
      *p = '\0';
      pattern = StringRef{iov.base, p};
    }
    for (auto &g : addr_groups) {
      if (g.pattern == pattern) {
        // Last value wins if we have multiple different affinity
        // value under one group.
        if (params.affinity != AFFINITY_NONE) {
          g.affinity = params.affinity;
        }
        g.addrs.push_back(addr);
        done = true;
        break;
      }
    }
    if (done) {
      continue;
    }

    auto idx = addr_groups.size();
    addr_groups.emplace_back(pattern);
    auto &g = addr_groups.back();
    g.addrs.push_back(addr);
    g.affinity = params.affinity;

    if (pattern[0] == '*') {
      // wildcard pattern
      auto path_first =
          std::find(std::begin(g.pattern), std::end(g.pattern), '/');

      auto host = StringRef{std::begin(g.pattern) + 1, path_first};
      auto path = StringRef{path_first, std::end(g.pattern)};

      auto it = std::find_if(
          std::begin(wildcard_patterns), std::end(wildcard_patterns),
          [&host](const WildcardPattern &wp) { return wp.host == host; });

      if (it == std::end(wildcard_patterns)) {
        wildcard_patterns.emplace_back(host);

        auto &router = wildcard_patterns.back().router;
        router.add_route(path, idx);

        auto iov = make_byte_ref(downstreamconf.balloc, host.size() + 1);
        auto p = iov.base;
        p = std::reverse_copy(std::begin(host), std::end(host), p);
        *p = '\0';
        auto rev_host = StringRef{iov.base, p};

        rw_router.add_route(rev_host, wildcard_patterns.size() - 1);
      } else {
        (*it).router.add_route(path, idx);
      }

      continue;
    }

    router.add_route(g.pattern, idx);
  }
  return 0;
}
} // namespace

namespace {
int parse_forwarded_node_type(const StringRef &optarg) {
  if (util::strieq_l("obfuscated", optarg)) {
    return FORWARDED_NODE_OBFUSCATED;
  }

  if (util::strieq_l("ip", optarg)) {
    return FORWARDED_NODE_IP;
  }

  if (optarg.size() < 2 || optarg[0] != '_') {
    return -1;
  }

  if (std::find_if_not(std::begin(optarg), std::end(optarg), [](char c) {
        return util::is_alpha(c) || util::is_digit(c) || c == '.' || c == '_' ||
               c == '-';
      }) != std::end(optarg)) {
    return -1;
  }

  return FORWARDED_NODE_OBFUSCATED;
}
} // namespace

namespace {
int parse_error_page(std::vector<ErrorPage> &error_pages, const StringRef &opt,
                     const StringRef &optarg) {
  auto eq = std::find(std::begin(optarg), std::end(optarg), '=');
  if (eq == std::end(optarg) || eq + 1 == std::end(optarg)) {
    LOG(ERROR) << opt << ": bad value: '" << optarg << "'";
    return -1;
  }

  auto codestr = StringRef{std::begin(optarg), eq};
  unsigned int code;

  if (codestr == StringRef::from_lit("*")) {
    code = 0;
  } else {
    auto n = util::parse_uint(codestr);

    if (n == -1 || n < 400 || n > 599) {
      LOG(ERROR) << opt << ": bad code: '" << codestr << "'";
      return -1;
    }

    code = static_cast<unsigned int>(n);
  }

  auto path = StringRef{eq + 1, std::end(optarg)};

  std::vector<uint8_t> content;
  auto fd = open(path.c_str(), O_RDONLY);
  if (fd == -1) {
    auto error = errno;
    LOG(ERROR) << opt << ": " << optarg << ": " << strerror(error);
    return -1;
  }

  auto fd_closer = defer(close, fd);

  std::array<uint8_t, 4096> buf;
  for (;;) {
    auto n = read(fd, buf.data(), buf.size());
    if (n == -1) {
      auto error = errno;
      LOG(ERROR) << opt << ": " << optarg << ": " << strerror(error);
      return -1;
    }
    if (n == 0) {
      break;
    }
    content.insert(std::end(content), std::begin(buf), std::begin(buf) + n);
  }

  error_pages.push_back(ErrorPage{std::move(content), code});

  return 0;
}
} // namespace

// generated by gennghttpxfun.py
int option_lookup_token(const char *name, size_t namelen) {
  switch (namelen) {
  case 4:
    switch (name[3]) {
    case 'f':
      if (util::strieq_l("con", name, 3)) {
        return SHRPX_OPTID_CONF;
      }
      break;
    case 'r':
      if (util::strieq_l("use", name, 3)) {
        return SHRPX_OPTID_USER;
      }
      break;
    }
    break;
  case 6:
    switch (name[5]) {
    case 'a':
      if (util::strieq_l("no-vi", name, 5)) {
        return SHRPX_OPTID_NO_VIA;
      }
      break;
    case 'c':
      if (util::strieq_l("altsv", name, 5)) {
        return SHRPX_OPTID_ALTSVC;
      }
      break;
    case 'n':
      if (util::strieq_l("daemo", name, 5)) {
        return SHRPX_OPTID_DAEMON;
      }
      break;
    case 't':
      if (util::strieq_l("cacer", name, 5)) {
        return SHRPX_OPTID_CACERT;
      }
      if (util::strieq_l("clien", name, 5)) {
        return SHRPX_OPTID_CLIENT;
      }
      break;
    }
    break;
  case 7:
    switch (name[6]) {
    case 'd':
      if (util::strieq_l("backen", name, 6)) {
        return SHRPX_OPTID_BACKEND;
      }
      break;
    case 'e':
      if (util::strieq_l("includ", name, 6)) {
        return SHRPX_OPTID_INCLUDE;
      }
      break;
    case 'g':
      if (util::strieq_l("backlo", name, 6)) {
        return SHRPX_OPTID_BACKLOG;
      }
      if (util::strieq_l("paddin", name, 6)) {
        return SHRPX_OPTID_PADDING;
      }
      break;
    case 'p':
      if (util::strieq_l("no-ocs", name, 6)) {
        return SHRPX_OPTID_NO_OCSP;
      }
      break;
    case 's':
      if (util::strieq_l("cipher", name, 6)) {
        return SHRPX_OPTID_CIPHERS;
      }
      if (util::strieq_l("worker", name, 6)) {
        return SHRPX_OPTID_WORKERS;
      }
      break;
    case 't':
      if (util::strieq_l("subcer", name, 6)) {
        return SHRPX_OPTID_SUBCERT;
      }
      break;
    }
    break;
  case 8:
    switch (name[7]) {
    case 'd':
      if (util::strieq_l("fronten", name, 7)) {
        return SHRPX_OPTID_FRONTEND;
      }
      break;
    case 'e':
      if (util::strieq_l("insecur", name, 7)) {
        return SHRPX_OPTID_INSECURE;
      }
      if (util::strieq_l("pid-fil", name, 7)) {
        return SHRPX_OPTID_PID_FILE;
      }
      break;
    case 'n':
      if (util::strieq_l("fastope", name, 7)) {
        return SHRPX_OPTID_FASTOPEN;
      }
      break;
    case 't':
      if (util::strieq_l("npn-lis", name, 7)) {
        return SHRPX_OPTID_NPN_LIST;
      }
      break;
    }
    break;
  case 9:
    switch (name[8]) {
    case 'e':
      if (util::strieq_l("no-kqueu", name, 8)) {
        return SHRPX_OPTID_NO_KQUEUE;
      }
      if (util::strieq_l("read-rat", name, 8)) {
        return SHRPX_OPTID_READ_RATE;
      }
      break;
    case 'l':
      if (util::strieq_l("log-leve", name, 8)) {
        return SHRPX_OPTID_LOG_LEVEL;
      }
      break;
    }
    break;
  case 10:
    switch (name[9]) {
    case 'e':
      if (util::strieq_l("error-pag", name, 9)) {
        return SHRPX_OPTID_ERROR_PAGE;
      }
      if (util::strieq_l("mruby-fil", name, 9)) {
        return SHRPX_OPTID_MRUBY_FILE;
      }
      if (util::strieq_l("write-rat", name, 9)) {
        return SHRPX_OPTID_WRITE_RATE;
      }
      break;
    case 't':
      if (util::strieq_l("read-burs", name, 9)) {
        return SHRPX_OPTID_READ_BURST;
      }
      break;
    }
    break;
  case 11:
    switch (name[10]) {
    case 'e':
      if (util::strieq_l("server-nam", name, 10)) {
        return SHRPX_OPTID_SERVER_NAME;
      }
      break;
    case 's':
      if (util::strieq_l("backend-tl", name, 10)) {
        return SHRPX_OPTID_BACKEND_TLS;
      }
      if (util::strieq_l("ecdh-curve", name, 10)) {
        return SHRPX_OPTID_ECDH_CURVES;
      }
      break;
    case 't':
      if (util::strieq_l("write-burs", name, 10)) {
        return SHRPX_OPTID_WRITE_BURST;
      }
      break;
    case 'y':
      if (util::strieq_l("http2-prox", name, 10)) {
        return SHRPX_OPTID_HTTP2_PROXY;
      }
      break;
    }
    break;
  case 12:
    switch (name[11]) {
    case '4':
      if (util::strieq_l("backend-ipv", name, 11)) {
        return SHRPX_OPTID_BACKEND_IPV4;
      }
      break;
    case '6':
      if (util::strieq_l("backend-ipv", name, 11)) {
        return SHRPX_OPTID_BACKEND_IPV6;
      }
      break;
    case 'e':
      if (util::strieq_l("host-rewrit", name, 11)) {
        return SHRPX_OPTID_HOST_REWRITE;
      }
      if (util::strieq_l("http2-bridg", name, 11)) {
        return SHRPX_OPTID_HTTP2_BRIDGE;
      }
      break;
    case 'y':
      if (util::strieq_l("client-prox", name, 11)) {
        return SHRPX_OPTID_CLIENT_PROXY;
      }
      if (util::strieq_l("forwarded-b", name, 11)) {
        return SHRPX_OPTID_FORWARDED_BY;
      }
      break;
    }
    break;
  case 13:
    switch (name[12]) {
    case 'd':
      if (util::strieq_l("add-forwarde", name, 12)) {
        return SHRPX_OPTID_ADD_FORWARDED;
      }
      break;
    case 'e':
      if (util::strieq_l("dh-param-fil", name, 12)) {
        return SHRPX_OPTID_DH_PARAM_FILE;
      }
      if (util::strieq_l("errorlog-fil", name, 12)) {
        return SHRPX_OPTID_ERRORLOG_FILE;
      }
      if (util::strieq_l("rlimit-nofil", name, 12)) {
        return SHRPX_OPTID_RLIMIT_NOFILE;
      }
      break;
    case 'r':
      if (util::strieq_l("forwarded-fo", name, 12)) {
        return SHRPX_OPTID_FORWARDED_FOR;
      }
      break;
    case 't':
      if (util::strieq_l("verify-clien", name, 12)) {
        return SHRPX_OPTID_VERIFY_CLIENT;
      }
      break;
    }
    break;
  case 14:
    switch (name[13]) {
    case 'e':
      if (util::strieq_l("accesslog-fil", name, 13)) {
        return SHRPX_OPTID_ACCESSLOG_FILE;
      }
      break;
    case 'h':
      if (util::strieq_l("no-server-pus", name, 13)) {
        return SHRPX_OPTID_NO_SERVER_PUSH;
      }
      break;
    case 's':
      if (util::strieq_l("backend-no-tl", name, 13)) {
        return SHRPX_OPTID_BACKEND_NO_TLS;
      }
      break;
    case 't':
      if (util::strieq_l("tls-proto-lis", name, 13)) {
        return SHRPX_OPTID_TLS_PROTO_LIST;
      }
      break;
    }
    break;
  case 15:
    switch (name[14]) {
    case 'e':
      if (util::strieq_l("no-host-rewrit", name, 14)) {
        return SHRPX_OPTID_NO_HOST_REWRITE;
      }
      break;
    case 'g':
      if (util::strieq_l("errorlog-syslo", name, 14)) {
        return SHRPX_OPTID_ERRORLOG_SYSLOG;
      }
      break;
    case 's':
      if (util::strieq_l("frontend-no-tl", name, 14)) {
        return SHRPX_OPTID_FRONTEND_NO_TLS;
      }
      break;
    case 'y':
      if (util::strieq_l("syslog-facilit", name, 14)) {
        return SHRPX_OPTID_SYSLOG_FACILITY;
      }
      break;
    }
    break;
  case 16:
    switch (name[15]) {
    case 'e':
      if (util::strieq_l("certificate-fil", name, 15)) {
        return SHRPX_OPTID_CERTIFICATE_FILE;
      }
      if (util::strieq_l("client-cert-fil", name, 15)) {
        return SHRPX_OPTID_CLIENT_CERT_FILE;
      }
      if (util::strieq_l("private-key-fil", name, 15)) {
        return SHRPX_OPTID_PRIVATE_KEY_FILE;
      }
      if (util::strieq_l("worker-read-rat", name, 15)) {
        return SHRPX_OPTID_WORKER_READ_RATE;
      }
      break;
    case 'g':
      if (util::strieq_l("accesslog-syslo", name, 15)) {
        return SHRPX_OPTID_ACCESSLOG_SYSLOG;
      }
      break;
    case 't':
      if (util::strieq_l("accesslog-forma", name, 15)) {
        return SHRPX_OPTID_ACCESSLOG_FORMAT;
      }
      break;
    }
    break;
  case 17:
    switch (name[16]) {
    case 'e':
      if (util::strieq_l("no-server-rewrit", name, 16)) {
        return SHRPX_OPTID_NO_SERVER_REWRITE;
      }
      if (util::strieq_l("worker-write-rat", name, 16)) {
        return SHRPX_OPTID_WORKER_WRITE_RATE;
      }
      break;
    case 's':
      if (util::strieq_l("backend-http1-tl", name, 16)) {
        return SHRPX_OPTID_BACKEND_HTTP1_TLS;
      }
      if (util::strieq_l("max-header-field", name, 16)) {
        return SHRPX_OPTID_MAX_HEADER_FIELDS;
      }
      break;
    case 't':
      if (util::strieq_l("worker-read-burs", name, 16)) {
        return SHRPX_OPTID_WORKER_READ_BURST;
      }
      break;
    }
    break;
  case 18:
    switch (name[17]) {
    case 'r':
      if (util::strieq_l("add-request-heade", name, 17)) {
        return SHRPX_OPTID_ADD_REQUEST_HEADER;
      }
      break;
    case 't':
      if (util::strieq_l("worker-write-burs", name, 17)) {
        return SHRPX_OPTID_WORKER_WRITE_BURST;
      }
      break;
    }
    break;
  case 19:
    switch (name[18]) {
    case 'e':
      if (util::strieq_l("no-location-rewrit", name, 18)) {
        return SHRPX_OPTID_NO_LOCATION_REWRITE;
      }
      if (util::strieq_l("tls-ticket-key-fil", name, 18)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_FILE;
      }
      break;
    case 'f':
      if (util::strieq_l("backend-max-backof", name, 18)) {
        return SHRPX_OPTID_BACKEND_MAX_BACKOFF;
      }
      break;
    case 'r':
      if (util::strieq_l("add-response-heade", name, 18)) {
        return SHRPX_OPTID_ADD_RESPONSE_HEADER;
      }
      if (util::strieq_l("add-x-forwarded-fo", name, 18)) {
        return SHRPX_OPTID_ADD_X_FORWARDED_FOR;
      }
      if (util::strieq_l("header-field-buffe", name, 18)) {
        return SHRPX_OPTID_HEADER_FIELD_BUFFER;
      }
      break;
    case 't':
      if (util::strieq_l("stream-read-timeou", name, 18)) {
        return SHRPX_OPTID_STREAM_READ_TIMEOUT;
      }
      break;
    }
    break;
  case 20:
    switch (name[19]) {
    case 'g':
      if (util::strieq_l("frontend-frame-debu", name, 19)) {
        return SHRPX_OPTID_FRONTEND_FRAME_DEBUG;
      }
      break;
    case 'l':
      if (util::strieq_l("ocsp-update-interva", name, 19)) {
        return SHRPX_OPTID_OCSP_UPDATE_INTERVAL;
      }
      break;
    case 't':
      if (util::strieq_l("backend-read-timeou", name, 19)) {
        return SHRPX_OPTID_BACKEND_READ_TIMEOUT;
      }
      if (util::strieq_l("stream-write-timeou", name, 19)) {
        return SHRPX_OPTID_STREAM_WRITE_TIMEOUT;
      }
      if (util::strieq_l("verify-client-cacer", name, 19)) {
        return SHRPX_OPTID_VERIFY_CLIENT_CACERT;
      }
      break;
    case 'y':
      if (util::strieq_l("api-max-request-bod", name, 19)) {
        return SHRPX_OPTID_API_MAX_REQUEST_BODY;
      }
      break;
    }
    break;
  case 21:
    switch (name[20]) {
    case 'd':
      if (util::strieq_l("backend-tls-sni-fiel", name, 20)) {
        return SHRPX_OPTID_BACKEND_TLS_SNI_FIELD;
      }
      break;
    case 'l':
      if (util::strieq_l("accept-proxy-protoco", name, 20)) {
        return SHRPX_OPTID_ACCEPT_PROXY_PROTOCOL;
      }
      break;
    case 'r':
      if (util::strieq_l("tls-ticket-key-ciphe", name, 20)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_CIPHER;
      }
      break;
    case 't':
      if (util::strieq_l("backend-write-timeou", name, 20)) {
        return SHRPX_OPTID_BACKEND_WRITE_TIMEOUT;
      }
      if (util::strieq_l("frontend-read-timeou", name, 20)) {
        return SHRPX_OPTID_FRONTEND_READ_TIMEOUT;
      }
      break;
    }
    break;
  case 22:
    switch (name[21]) {
    case 'i':
      if (util::strieq_l("backend-http-proxy-ur", name, 21)) {
        return SHRPX_OPTID_BACKEND_HTTP_PROXY_URI;
      }
      break;
    case 'r':
      if (util::strieq_l("backend-request-buffe", name, 21)) {
        return SHRPX_OPTID_BACKEND_REQUEST_BUFFER;
      }
      break;
    case 't':
      if (util::strieq_l("frontend-write-timeou", name, 21)) {
        return SHRPX_OPTID_FRONTEND_WRITE_TIMEOUT;
      }
      break;
    case 'y':
      if (util::strieq_l("backend-address-famil", name, 21)) {
        return SHRPX_OPTID_BACKEND_ADDRESS_FAMILY;
      }
      break;
    }
    break;
  case 23:
    switch (name[22]) {
    case 'e':
      if (util::strieq_l("client-private-key-fil", name, 22)) {
        return SHRPX_OPTID_CLIENT_PRIVATE_KEY_FILE;
      }
      if (util::strieq_l("private-key-passwd-fil", name, 22)) {
        return SHRPX_OPTID_PRIVATE_KEY_PASSWD_FILE;
      }
      break;
    case 'r':
      if (util::strieq_l("backend-response-buffe", name, 22)) {
        return SHRPX_OPTID_BACKEND_RESPONSE_BUFFER;
      }
      break;
    }
    break;
  case 24:
    switch (name[23]) {
    case 'd':
      if (util::strieq_l("strip-incoming-forwarde", name, 23)) {
        return SHRPX_OPTID_STRIP_INCOMING_FORWARDED;
      }
      if (util::strieq_l("tls-ticket-key-memcache", name, 23)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED;
      }
      break;
    case 'e':
      if (util::strieq_l("fetch-ocsp-response-fil", name, 23)) {
        return SHRPX_OPTID_FETCH_OCSP_RESPONSE_FILE;
      }
      break;
    case 't':
      if (util::strieq_l("listener-disable-timeou", name, 23)) {
        return SHRPX_OPTID_LISTENER_DISABLE_TIMEOUT;
      }
      if (util::strieq_l("tls-dyn-rec-idle-timeou", name, 23)) {
        return SHRPX_OPTID_TLS_DYN_REC_IDLE_TIMEOUT;
      }
      break;
    }
    break;
  case 25:
    switch (name[24]) {
    case 'e':
      if (util::strieq_l("backend-http2-window-siz", name, 24)) {
        return SHRPX_OPTID_BACKEND_HTTP2_WINDOW_SIZE;
      }
      break;
    case 'g':
      if (util::strieq_l("http2-no-cookie-crumblin", name, 24)) {
        return SHRPX_OPTID_HTTP2_NO_COOKIE_CRUMBLING;
      }
      break;
    case 's':
      if (util::strieq_l("backend-http2-window-bit", name, 24)) {
        return SHRPX_OPTID_BACKEND_HTTP2_WINDOW_BITS;
      }
      if (util::strieq_l("max-request-header-field", name, 24)) {
        return SHRPX_OPTID_MAX_REQUEST_HEADER_FIELDS;
      }
      break;
    }
    break;
  case 26:
    switch (name[25]) {
    case 'e':
      if (util::strieq_l("frontend-http2-window-siz", name, 25)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_WINDOW_SIZE;
      }
      break;
    case 's':
      if (util::strieq_l("frontend-http2-window-bit", name, 25)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_WINDOW_BITS;
      }
      if (util::strieq_l("max-response-header-field", name, 25)) {
        return SHRPX_OPTID_MAX_RESPONSE_HEADER_FIELDS;
      }
      break;
    case 't':
      if (util::strieq_l("backend-keep-alive-timeou", name, 25)) {
        return SHRPX_OPTID_BACKEND_KEEP_ALIVE_TIMEOUT;
      }
      if (util::strieq_l("no-http2-cipher-black-lis", name, 25)) {
        return SHRPX_OPTID_NO_HTTP2_CIPHER_BLACK_LIST;
      }
      break;
    }
    break;
  case 27:
    switch (name[26]) {
    case 'd':
      if (util::strieq_l("tls-session-cache-memcache", name, 26)) {
        return SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED;
      }
      break;
    case 'r':
      if (util::strieq_l("request-header-field-buffe", name, 26)) {
        return SHRPX_OPTID_REQUEST_HEADER_FIELD_BUFFER;
      }
      break;
    case 's':
      if (util::strieq_l("worker-frontend-connection", name, 26)) {
        return SHRPX_OPTID_WORKER_FRONTEND_CONNECTIONS;
      }
      break;
    case 't':
      if (util::strieq_l("frontend-http2-read-timeou", name, 26)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_READ_TIMEOUT;
      }
      break;
    }
    break;
  case 28:
    switch (name[27]) {
    case 'd':
      if (util::strieq_l("tls-dyn-rec-warmup-threshol", name, 27)) {
        return SHRPX_OPTID_TLS_DYN_REC_WARMUP_THRESHOLD;
      }
      break;
    case 'r':
      if (util::strieq_l("response-header-field-buffe", name, 27)) {
        return SHRPX_OPTID_RESPONSE_HEADER_FIELD_BUFFER;
      }
      break;
    case 's':
      if (util::strieq_l("http2-max-concurrent-stream", name, 27)) {
        return SHRPX_OPTID_HTTP2_MAX_CONCURRENT_STREAMS;
      }
      if (util::strieq_l("tls-ticket-key-memcached-tl", name, 27)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_TLS;
      }
      break;
    case 't':
      if (util::strieq_l("backend-connections-per-hos", name, 27)) {
        return SHRPX_OPTID_BACKEND_CONNECTIONS_PER_HOST;
      }
      break;
    }
    break;
  case 30:
    switch (name[29]) {
    case 'r':
      if (util::strieq_l("strip-incoming-x-forwarded-fo", name, 29)) {
        return SHRPX_OPTID_STRIP_INCOMING_X_FORWARDED_FOR;
      }
      break;
    case 't':
      if (util::strieq_l("backend-http2-settings-timeou", name, 29)) {
        return SHRPX_OPTID_BACKEND_HTTP2_SETTINGS_TIMEOUT;
      }
      break;
    }
    break;
  case 31:
    switch (name[30]) {
    case 's':
      if (util::strieq_l("tls-session-cache-memcached-tl", name, 30)) {
        return SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_TLS;
      }
      break;
    case 't':
      if (util::strieq_l("frontend-http2-settings-timeou", name, 30)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_SETTINGS_TIMEOUT;
      }
      break;
    }
    break;
  case 32:
    switch (name[31]) {
    case 'd':
      if (util::strieq_l("backend-connections-per-fronten", name, 31)) {
        return SHRPX_OPTID_BACKEND_CONNECTIONS_PER_FRONTEND;
      }
      break;
    }
    break;
  case 33:
    switch (name[32]) {
    case 'l':
      if (util::strieq_l("tls-ticket-key-memcached-interva", name, 32)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_INTERVAL;
      }
      if (util::strieq_l("tls-ticket-key-memcached-max-fai", name, 32)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_MAX_FAIL;
      }
      break;
    }
    break;
  case 34:
    switch (name[33]) {
    case 'e':
      if (util::strieq_l("tls-ticket-key-memcached-cert-fil", name, 33)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_CERT_FILE;
      }
      break;
    case 'r':
      if (util::strieq_l("frontend-http2-dump-request-heade", name, 33)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_DUMP_REQUEST_HEADER;
      }
      break;
    case 't':
      if (util::strieq_l("backend-http1-connections-per-hos", name, 33)) {
        return SHRPX_OPTID_BACKEND_HTTP1_CONNECTIONS_PER_HOST;
      }
      break;
    case 'y':
      if (util::strieq_l("tls-ticket-key-memcached-max-retr", name, 33)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_MAX_RETRY;
      }
      break;
    }
    break;
  case 35:
    switch (name[34]) {
    case 'e':
      if (util::strieq_l("frontend-http2-optimize-window-siz", name, 34)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_OPTIMIZE_WINDOW_SIZE;
      }
      break;
    case 'r':
      if (util::strieq_l("frontend-http2-dump-response-heade", name, 34)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_DUMP_RESPONSE_HEADER;
      }
      break;
    }
    break;
  case 36:
    switch (name[35]) {
    case 'e':
      if (util::strieq_l("backend-http2-connection-window-siz", name, 35)) {
        return SHRPX_OPTID_BACKEND_HTTP2_CONNECTION_WINDOW_SIZE;
      }
      break;
    case 'r':
      if (util::strieq_l("backend-http2-connections-per-worke", name, 35)) {
        return SHRPX_OPTID_BACKEND_HTTP2_CONNECTIONS_PER_WORKER;
      }
      break;
    case 's':
      if (util::strieq_l("backend-http2-connection-window-bit", name, 35)) {
        return SHRPX_OPTID_BACKEND_HTTP2_CONNECTION_WINDOW_BITS;
      }
      if (util::strieq_l("backend-http2-max-concurrent-stream", name, 35)) {
        return SHRPX_OPTID_BACKEND_HTTP2_MAX_CONCURRENT_STREAMS;
      }
      break;
    }
    break;
  case 37:
    switch (name[36]) {
    case 'e':
      if (util::strieq_l("frontend-http2-connection-window-siz", name, 36)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_CONNECTION_WINDOW_SIZE;
      }
      if (util::strieq_l("tls-session-cache-memcached-cert-fil", name, 36)) {
        return SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_CERT_FILE;
      }
      break;
    case 's':
      if (util::strieq_l("frontend-http2-connection-window-bit", name, 36)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS;
      }
      if (util::strieq_l("frontend-http2-max-concurrent-stream", name, 36)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_MAX_CONCURRENT_STREAMS;
      }
      break;
    }
    break;
  case 38:
    switch (name[37]) {
    case 'd':
      if (util::strieq_l("backend-http1-connections-per-fronten", name, 37)) {
        return SHRPX_OPTID_BACKEND_HTTP1_CONNECTIONS_PER_FRONTEND;
      }
      break;
    }
    break;
  case 39:
    switch (name[38]) {
    case 'y':
      if (util::strieq_l("tls-ticket-key-memcached-address-famil", name, 38)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_ADDRESS_FAMILY;
      }
      break;
    }
    break;
  case 40:
    switch (name[39]) {
    case 'e':
      if (util::strieq_l("backend-http2-decoder-dynamic-table-siz", name, 39)) {
        return SHRPX_OPTID_BACKEND_HTTP2_DECODER_DYNAMIC_TABLE_SIZE;
      }
      if (util::strieq_l("backend-http2-encoder-dynamic-table-siz", name, 39)) {
        return SHRPX_OPTID_BACKEND_HTTP2_ENCODER_DYNAMIC_TABLE_SIZE;
      }
      break;
    }
    break;
  case 41:
    switch (name[40]) {
    case 'e':
      if (util::strieq_l("frontend-http2-decoder-dynamic-table-siz", name,
                         40)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_DECODER_DYNAMIC_TABLE_SIZE;
      }
      if (util::strieq_l("frontend-http2-encoder-dynamic-table-siz", name,
                         40)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_ENCODER_DYNAMIC_TABLE_SIZE;
      }
      if (util::strieq_l("frontend-http2-optimize-write-buffer-siz", name,
                         40)) {
        return SHRPX_OPTID_FRONTEND_HTTP2_OPTIMIZE_WRITE_BUFFER_SIZE;
      }
      if (util::strieq_l("tls-ticket-key-memcached-private-key-fil", name,
                         40)) {
        return SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_PRIVATE_KEY_FILE;
      }
      break;
    }
    break;
  case 42:
    switch (name[41]) {
    case 'y':
      if (util::strieq_l("tls-session-cache-memcached-address-famil", name,
                         41)) {
        return SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_ADDRESS_FAMILY;
      }
      break;
    }
    break;
  case 44:
    switch (name[43]) {
    case 'e':
      if (util::strieq_l("tls-session-cache-memcached-private-key-fil", name,
                         43)) {
        return SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_PRIVATE_KEY_FILE;
      }
      break;
    }
    break;
  }
  return -1;
}

int parse_config(Config *config, const StringRef &opt, const StringRef &optarg,
                 std::set<StringRef> &included_set) {
  auto optid = option_lookup_token(opt.c_str(), opt.size());
  return parse_config(config, optid, opt, optarg, included_set);
}

int parse_config(Config *config, int optid, const StringRef &opt,
                 const StringRef &optarg, std::set<StringRef> &included_set) {
  char host[NI_MAXHOST];
  uint16_t port;

  switch (optid) {
  case SHRPX_OPTID_BACKEND: {
    auto &downstreamconf = *config->conn.downstream;
    auto addr_end = std::find(std::begin(optarg), std::end(optarg), ';');

    DownstreamAddrConfig addr{};
    if (util::istarts_with(optarg, SHRPX_UNIX_PATH_PREFIX)) {
      auto path = std::begin(optarg) + SHRPX_UNIX_PATH_PREFIX.size();
      addr.host =
          make_string_ref(downstreamconf.balloc, StringRef{path, addr_end});
      addr.host_unix = true;
    } else {
      if (split_host_port(host, sizeof(host), &port,
                          StringRef{std::begin(optarg), addr_end}, opt) == -1) {
        return -1;
      }

      addr.host = make_string_ref(downstreamconf.balloc, StringRef{host});
      addr.port = port;
    }

    auto mapping = addr_end == std::end(optarg) ? addr_end : addr_end + 1;
    auto mapping_end = std::find(mapping, std::end(optarg), ';');

    auto params =
        mapping_end == std::end(optarg) ? mapping_end : mapping_end + 1;

    if (parse_mapping(config, addr, StringRef{mapping, mapping_end},
                      StringRef{params, std::end(optarg)}) != 0) {
      return -1;
    }

    return 0;
  }
  case SHRPX_OPTID_FRONTEND: {
    auto &listenerconf = config->conn.listener;
    auto &apiconf = config->api;

    auto addr_end = std::find(std::begin(optarg), std::end(optarg), ';');
    auto src_params = StringRef{addr_end, std::end(optarg)};

    UpstreamParams params{};
    params.tls = true;

    if (parse_upstream_params(params, src_params) != 0) {
      return -1;
    }

    UpstreamAddr addr{};
    addr.fd = -1;
    addr.tls = params.tls;
    addr.alt_mode = params.alt_mode;

    if (addr.alt_mode == ALTMODE_API) {
      apiconf.enabled = true;
    }

    if (util::istarts_with(optarg, SHRPX_UNIX_PATH_PREFIX)) {
      auto path = std::begin(optarg) + SHRPX_UNIX_PATH_PREFIX.size();
      addr.host = make_string_ref(config->balloc, StringRef{path, addr_end});
      addr.host_unix = true;

      listenerconf.addrs.push_back(std::move(addr));

      return 0;
    }

    if (split_host_port(host, sizeof(host), &port,
                        StringRef{std::begin(optarg), addr_end}, opt) == -1) {
      return -1;
    }

    addr.host = make_string_ref(config->balloc, StringRef{host});
    addr.port = port;

    if (util::numeric_host(host, AF_INET)) {
      addr.family = AF_INET;
      listenerconf.addrs.push_back(std::move(addr));
      return 0;
    }

    if (util::numeric_host(host, AF_INET6)) {
      addr.family = AF_INET6;
      listenerconf.addrs.push_back(std::move(addr));
      return 0;
    }

    addr.family = AF_INET;
    listenerconf.addrs.push_back(addr);

    addr.family = AF_INET6;
    listenerconf.addrs.push_back(std::move(addr));

    return 0;
  }
  case SHRPX_OPTID_WORKERS:
#ifdef NOTHREADS
    LOG(WARN) << "Threading disabled at build time, no threads created.";
    return 0;
#else  // !NOTHREADS
    return parse_uint(&config->num_worker, opt, optarg);
#endif // !NOTHREADS
  case SHRPX_OPTID_HTTP2_MAX_CONCURRENT_STREAMS: {
    LOG(WARN) << opt << ": deprecated. Use "
              << SHRPX_OPT_FRONTEND_HTTP2_MAX_CONCURRENT_STREAMS << " and "
              << SHRPX_OPT_BACKEND_HTTP2_MAX_CONCURRENT_STREAMS << " instead.";
    size_t n;
    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }
    auto &http2conf = config->http2;
    http2conf.upstream.max_concurrent_streams = n;
    http2conf.downstream.max_concurrent_streams = n;

    return 0;
  }
  case SHRPX_OPTID_LOG_LEVEL:
    if (Log::set_severity_level_by_name(optarg) == -1) {
      LOG(ERROR) << opt << ": Invalid severity level: " << optarg;
      return -1;
    }

    return 0;
  case SHRPX_OPTID_DAEMON:
    config->daemon = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_HTTP2_PROXY:
    config->http2_proxy = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_HTTP2_BRIDGE:
    LOG(ERROR) << opt
               << ": deprecated.  Use backend=<addr>,<port>;;proto=h2;tls";
    return -1;
  case SHRPX_OPTID_CLIENT_PROXY:
    LOG(ERROR)
        << opt
        << ": deprecated.  Use http2-proxy, frontend=<addr>,<port>;no-tls "
           "and backend=<addr>,<port>;;proto=h2;tls";
    return -1;
  case SHRPX_OPTID_ADD_X_FORWARDED_FOR:
    config->http.xff.add = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_STRIP_INCOMING_X_FORWARDED_FOR:
    config->http.xff.strip_incoming = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_NO_VIA:
    config->http.no_via = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_READ_TIMEOUT:
    return parse_duration(&config->conn.upstream.timeout.http2_read, opt,
                          optarg);
  case SHRPX_OPTID_FRONTEND_READ_TIMEOUT:
    return parse_duration(&config->conn.upstream.timeout.read, opt, optarg);
  case SHRPX_OPTID_FRONTEND_WRITE_TIMEOUT:
    return parse_duration(&config->conn.upstream.timeout.write, opt, optarg);
  case SHRPX_OPTID_BACKEND_READ_TIMEOUT:
    return parse_duration(&config->conn.downstream->timeout.read, opt, optarg);
  case SHRPX_OPTID_BACKEND_WRITE_TIMEOUT:
    return parse_duration(&config->conn.downstream->timeout.write, opt, optarg);
  case SHRPX_OPTID_STREAM_READ_TIMEOUT:
    return parse_duration(&config->http2.timeout.stream_read, opt, optarg);
  case SHRPX_OPTID_STREAM_WRITE_TIMEOUT:
    return parse_duration(&config->http2.timeout.stream_write, opt, optarg);
  case SHRPX_OPTID_ACCESSLOG_FILE:
    config->logging.access.file = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_ACCESSLOG_SYSLOG:
    config->logging.access.syslog = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_ACCESSLOG_FORMAT:
    config->logging.access.format = parse_log_format(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_ERRORLOG_FILE:
    config->logging.error.file = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_ERRORLOG_SYSLOG:
    config->logging.error.syslog = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_FASTOPEN: {
    int n;
    if (parse_int(&n, opt, optarg.c_str()) != 0) {
      return -1;
    }

    if (n < 0) {
      LOG(ERROR) << opt << ": " << optarg << " is not allowed";
      return -1;
    }

    config->conn.listener.fastopen = n;

    return 0;
  }
  case SHRPX_OPTID_BACKEND_KEEP_ALIVE_TIMEOUT:
    return parse_duration(&config->conn.downstream->timeout.idle_read, opt,
                          optarg);
  case SHRPX_OPTID_FRONTEND_HTTP2_WINDOW_BITS:
  case SHRPX_OPTID_BACKEND_HTTP2_WINDOW_BITS: {
    LOG(WARN) << opt << ": deprecated.  Use "
              << (optid == SHRPX_OPTID_FRONTEND_HTTP2_WINDOW_BITS
                      ? SHRPX_OPT_FRONTEND_HTTP2_WINDOW_SIZE
                      : SHRPX_OPT_BACKEND_HTTP2_WINDOW_SIZE);
    int32_t *resp;

    if (optid == SHRPX_OPTID_FRONTEND_HTTP2_WINDOW_BITS) {
      resp = &config->http2.upstream.window_size;
    } else {
      resp = &config->http2.downstream.window_size;
    }

    errno = 0;

    int n;

    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n >= 31) {
      LOG(ERROR) << opt
                 << ": specify the integer in the range [0, 30], inclusive";
      return -1;
    }

    // Make 16 bits to the HTTP/2 default 64KiB - 1.  This is the same
    // behaviour of previous code.  For SPDY, we adjust this value in
    // SpdyUpstream to look like the SPDY default.
    *resp = (1 << n) - 1;

    return 0;
  }
  case SHRPX_OPTID_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS:
  case SHRPX_OPTID_BACKEND_HTTP2_CONNECTION_WINDOW_BITS: {
    LOG(WARN) << opt << ": deprecated.  Use "
              << (optid == SHRPX_OPTID_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS
                      ? SHRPX_OPT_FRONTEND_HTTP2_CONNECTION_WINDOW_SIZE
                      : SHRPX_OPT_BACKEND_HTTP2_CONNECTION_WINDOW_SIZE);
    int32_t *resp;

    if (optid == SHRPX_OPTID_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS) {
      resp = &config->http2.upstream.connection_window_size;
    } else {
      resp = &config->http2.downstream.connection_window_size;
    }

    errno = 0;

    int n;

    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n < 16 || n >= 31) {
      LOG(ERROR) << opt
                 << ": specify the integer in the range [16, 30], inclusive";
      return -1;
    }

    *resp = (1 << n) - 1;

    return 0;
  }
  case SHRPX_OPTID_FRONTEND_NO_TLS:
    LOG(WARN) << opt << ": deprecated.  Use no-tls keyword in "
              << SHRPX_OPT_FRONTEND;
    return 0;
  case SHRPX_OPTID_BACKEND_NO_TLS:
    LOG(WARN) << opt << ": deprecated.  backend connection is not encrypted by "
                        "default.  See also " << SHRPX_OPT_BACKEND_TLS;
    return 0;
  case SHRPX_OPTID_BACKEND_TLS_SNI_FIELD:
    LOG(WARN) << opt << ": deprecated.  Use sni keyword in --backend option.  "
                        "For now, all sni values of all backends are "
                        "overridden by the given value " << optarg;
    config->tls.backend_sni_name = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_PID_FILE:
    config->pid_file = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_USER: {
    auto pwd = getpwnam(optarg.c_str());
    if (!pwd) {
      LOG(ERROR) << opt << ": failed to get uid from " << optarg << ": "
                 << strerror(errno);
      return -1;
    }
    config->user = make_string_ref(config->balloc, StringRef{pwd->pw_name});
    config->uid = pwd->pw_uid;
    config->gid = pwd->pw_gid;

    return 0;
  }
  case SHRPX_OPTID_PRIVATE_KEY_FILE:
    config->tls.private_key_file = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_PRIVATE_KEY_PASSWD_FILE: {
    auto passwd = read_passwd_from_file(opt, optarg);
    if (passwd.empty()) {
      LOG(ERROR) << opt << ": Couldn't read key file's passwd from " << optarg;
      return -1;
    }
    config->tls.private_key_passwd =
        make_string_ref(config->balloc, StringRef{passwd});

    return 0;
  }
  case SHRPX_OPTID_CERTIFICATE_FILE:
    config->tls.cert_file = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_DH_PARAM_FILE:
    config->tls.dh_param_file = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_SUBCERT: {
    // Private Key file and certificate file separated by ':'.
    auto sp = std::find(std::begin(optarg), std::end(optarg), ':');
    if (sp == std::end(optarg)) {
      LOG(ERROR) << opt << ": missing ':' in " << optarg;
      return -1;
    }

    auto private_key_file = StringRef{std::begin(optarg), sp};

    if (private_key_file.empty()) {
      LOG(ERROR) << opt << ": missing private key file: " << optarg;
      return -1;
    }

    auto cert_file = StringRef{sp + 1, std::end(optarg)};

    if (cert_file.empty()) {
      LOG(ERROR) << opt << ": missing certificate file: " << optarg;
      return -1;
    }

    config->tls.subcerts.emplace_back(
        make_string_ref(config->balloc, private_key_file),
        make_string_ref(config->balloc, cert_file));

    return 0;
  }
  case SHRPX_OPTID_SYSLOG_FACILITY: {
    int facility = int_syslog_facility(optarg);
    if (facility == -1) {
      LOG(ERROR) << opt << ": Unknown syslog facility: " << optarg;
      return -1;
    }
    config->logging.syslog_facility = facility;

    return 0;
  }
  case SHRPX_OPTID_BACKLOG: {
    int n;
    if (parse_int(&n, opt, optarg.c_str()) != 0) {
      return -1;
    }

    if (n < -1) {
      LOG(ERROR) << opt << ": " << optarg << " is not allowed";

      return -1;
    }

    config->conn.listener.backlog = n;

    return 0;
  }
  case SHRPX_OPTID_CIPHERS:
    config->tls.ciphers = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_CLIENT:
    LOG(ERROR) << opt << ": deprecated.  Use frontend=<addr>,<port>;no-tls, "
                         "backend=<addr>,<port>;;proto=h2;tls";
    return -1;
  case SHRPX_OPTID_INSECURE:
    config->tls.insecure = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_CACERT:
    config->tls.cacert = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_BACKEND_IPV4:
    LOG(WARN) << opt
              << ": deprecated.  Use backend-address-family=IPv4 instead.";

    config->conn.downstream->family = AF_INET;

    return 0;
  case SHRPX_OPTID_BACKEND_IPV6:
    LOG(WARN) << opt
              << ": deprecated.  Use backend-address-family=IPv6 instead.";

    config->conn.downstream->family = AF_INET6;

    return 0;
  case SHRPX_OPTID_BACKEND_HTTP_PROXY_URI: {
    auto &proxy = config->downstream_http_proxy;
    // Reset here so that multiple option occurrence does not merge
    // the results.
    proxy = {};
    // parse URI and get hostname, port and optionally userinfo.
    http_parser_url u{};
    int rv = http_parser_parse_url(optarg.c_str(), optarg.size(), 0, &u);
    if (rv == 0) {
      if (u.field_set & UF_USERINFO) {
        auto uf = util::get_uri_field(optarg.c_str(), u, UF_USERINFO);
        // Surprisingly, u.field_set & UF_USERINFO is nonzero even if
        // userinfo component is empty string.
        if (!uf.empty()) {
          proxy.userinfo = util::percent_decode(config->balloc, uf);
        }
      }
      if (u.field_set & UF_HOST) {
        proxy.host = make_string_ref(
            config->balloc, util::get_uri_field(optarg.c_str(), u, UF_HOST));
      } else {
        LOG(ERROR) << opt << ": no hostname specified";
        return -1;
      }
      if (u.field_set & UF_PORT) {
        proxy.port = u.port;
      } else {
        LOG(ERROR) << opt << ": no port specified";
        return -1;
      }
    } else {
      LOG(ERROR) << opt << ": parse error";
      return -1;
    }

    return 0;
  }
  case SHRPX_OPTID_READ_RATE:
    return parse_uint_with_unit(&config->conn.upstream.ratelimit.read.rate, opt,
                                optarg);
  case SHRPX_OPTID_READ_BURST:
    return parse_uint_with_unit(&config->conn.upstream.ratelimit.read.burst,
                                opt, optarg);
  case SHRPX_OPTID_WRITE_RATE:
    return parse_uint_with_unit(&config->conn.upstream.ratelimit.write.rate,
                                opt, optarg);
  case SHRPX_OPTID_WRITE_BURST:
    return parse_uint_with_unit(&config->conn.upstream.ratelimit.write.burst,
                                opt, optarg);
  case SHRPX_OPTID_WORKER_READ_RATE:
    LOG(WARN) << opt << ": not implemented yet";
    return 0;
  case SHRPX_OPTID_WORKER_READ_BURST:
    LOG(WARN) << opt << ": not implemented yet";
    return 0;
  case SHRPX_OPTID_WORKER_WRITE_RATE:
    LOG(WARN) << opt << ": not implemented yet";
    return 0;
  case SHRPX_OPTID_WORKER_WRITE_BURST:
    LOG(WARN) << opt << ": not implemented yet";
    return 0;
  case SHRPX_OPTID_NPN_LIST: {
    auto list = util::split_str(optarg, ',');
    config->tls.npn_list.resize(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
      config->tls.npn_list[i] = make_string_ref(config->balloc, list[i]);
    }

    return 0;
  }
  case SHRPX_OPTID_TLS_PROTO_LIST: {
    auto list = util::split_str(optarg, ',');
    config->tls.tls_proto_list.resize(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
      config->tls.tls_proto_list[i] = make_string_ref(config->balloc, list[i]);
    }

    return 0;
  }
  case SHRPX_OPTID_VERIFY_CLIENT:
    config->tls.client_verify.enabled = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_VERIFY_CLIENT_CACERT:
    config->tls.client_verify.cacert = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_CLIENT_PRIVATE_KEY_FILE:
    config->tls.client.private_key_file =
        make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_CLIENT_CERT_FILE:
    config->tls.client.cert_file = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_DUMP_REQUEST_HEADER:
    config->http2.upstream.debug.dump.request_header_file =
        make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_DUMP_RESPONSE_HEADER:
    config->http2.upstream.debug.dump.response_header_file =
        make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_HTTP2_NO_COOKIE_CRUMBLING:
    config->http2.no_cookie_crumbling = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_FRONTEND_FRAME_DEBUG:
    config->http2.upstream.debug.frame_debug = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_PADDING:
    return parse_uint(&config->padding, opt, optarg);
  case SHRPX_OPTID_ALTSVC: {
    auto tokens = util::split_str(optarg, ',');

    if (tokens.size() < 2) {
      // Requires at least protocol_id and port
      LOG(ERROR) << opt << ": too few parameters: " << optarg;
      return -1;
    }

    if (tokens.size() > 4) {
      // We only need protocol_id, port, host and origin
      LOG(ERROR) << opt << ": too many parameters: " << optarg;
      return -1;
    }

    int port;

    if (parse_uint(&port, opt, tokens[1]) != 0) {
      return -1;
    }

    if (port < 1 ||
        port > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
      LOG(ERROR) << opt << ": port is invalid: " << tokens[1];
      return -1;
    }

    AltSvc altsvc{};

    altsvc.protocol_id = make_string_ref(config->balloc, tokens[0]);

    altsvc.port = port;
    altsvc.service = make_string_ref(config->balloc, tokens[1]);

    if (tokens.size() > 2) {
      altsvc.host = make_string_ref(config->balloc, tokens[2]);

      if (tokens.size() > 3) {
        altsvc.origin = make_string_ref(config->balloc, tokens[3]);
      }
    }

    config->http.altsvcs.push_back(std::move(altsvc));

    return 0;
  }
  case SHRPX_OPTID_ADD_REQUEST_HEADER:
  case SHRPX_OPTID_ADD_RESPONSE_HEADER: {
    auto p = parse_header(config->balloc, optarg);
    if (p.name.empty()) {
      LOG(ERROR) << opt << ": invalid header field: " << optarg;
      return -1;
    }
    if (optid == SHRPX_OPTID_ADD_REQUEST_HEADER) {
      config->http.add_request_headers.push_back(std::move(p));
    } else {
      config->http.add_response_headers.push_back(std::move(p));
    }
    return 0;
  }
  case SHRPX_OPTID_WORKER_FRONTEND_CONNECTIONS:
    return parse_uint(&config->conn.upstream.worker_connections, opt, optarg);
  case SHRPX_OPTID_NO_LOCATION_REWRITE:
    config->http.no_location_rewrite = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_NO_HOST_REWRITE:
    LOG(WARN) << SHRPX_OPT_NO_HOST_REWRITE
              << ": deprecated.  :authority and host header fields are NOT "
                 "altered by default.  To rewrite these headers, use "
                 "--host-rewrite option.";

    return 0;
  case SHRPX_OPTID_BACKEND_HTTP1_CONNECTIONS_PER_HOST:
    LOG(WARN) << opt
              << ": deprecated.  Use backend-connections-per-host instead.";
  // fall through
  case SHRPX_OPTID_BACKEND_CONNECTIONS_PER_HOST: {
    int n;

    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n == 0) {
      LOG(ERROR) << opt << ": specify an integer strictly more than 0";

      return -1;
    }

    config->conn.downstream->connections_per_host = n;

    return 0;
  }
  case SHRPX_OPTID_BACKEND_HTTP1_CONNECTIONS_PER_FRONTEND:
    LOG(WARN) << opt << ": deprecated.  Use "
              << SHRPX_OPT_BACKEND_CONNECTIONS_PER_FRONTEND << " instead.";
  // fall through
  case SHRPX_OPTID_BACKEND_CONNECTIONS_PER_FRONTEND:
    return parse_uint(&config->conn.downstream->connections_per_frontend, opt,
                      optarg);
  case SHRPX_OPTID_LISTENER_DISABLE_TIMEOUT:
    return parse_duration(&config->conn.listener.timeout.sleep, opt, optarg);
  case SHRPX_OPTID_TLS_TICKET_KEY_FILE:
    config->tls.ticket.files.emplace_back(
        make_string_ref(config->balloc, optarg));
    return 0;
  case SHRPX_OPTID_RLIMIT_NOFILE: {
    int n;

    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n < 0) {
      LOG(ERROR) << opt << ": specify the integer more than or equal to 0";

      return -1;
    }

    config->rlimit_nofile = n;

    return 0;
  }
  case SHRPX_OPTID_BACKEND_REQUEST_BUFFER:
  case SHRPX_OPTID_BACKEND_RESPONSE_BUFFER: {
    size_t n;
    if (parse_uint_with_unit(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n == 0) {
      LOG(ERROR) << opt << ": specify an integer strictly more than 0";

      return -1;
    }

    if (optid == SHRPX_OPTID_BACKEND_REQUEST_BUFFER) {
      config->conn.downstream->request_buffer_size = n;
    } else {
      config->conn.downstream->response_buffer_size = n;
    }

    return 0;
  }

  case SHRPX_OPTID_NO_SERVER_PUSH:
    config->http2.no_server_push = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_BACKEND_HTTP2_CONNECTIONS_PER_WORKER:
    LOG(WARN) << opt << ": deprecated.";
    return 0;
  case SHRPX_OPTID_FETCH_OCSP_RESPONSE_FILE:
    config->tls.ocsp.fetch_ocsp_response_file =
        make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_OCSP_UPDATE_INTERVAL:
    return parse_duration(&config->tls.ocsp.update_interval, opt, optarg);
  case SHRPX_OPTID_NO_OCSP:
    config->tls.ocsp.disabled = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_HEADER_FIELD_BUFFER:
    LOG(WARN) << opt
              << ": deprecated.  Use request-header-field-buffer instead.";
  // fall through
  case SHRPX_OPTID_REQUEST_HEADER_FIELD_BUFFER:
    return parse_uint_with_unit(&config->http.request_header_field_buffer, opt,
                                optarg);
  case SHRPX_OPTID_MAX_HEADER_FIELDS:
    LOG(WARN) << opt << ": deprecated.  Use max-request-header-fields instead.";
  // fall through
  case SHRPX_OPTID_MAX_REQUEST_HEADER_FIELDS:
    return parse_uint(&config->http.max_request_header_fields, opt, optarg);
  case SHRPX_OPTID_RESPONSE_HEADER_FIELD_BUFFER:
    return parse_uint_with_unit(&config->http.response_header_field_buffer, opt,
                                optarg);
  case SHRPX_OPTID_MAX_RESPONSE_HEADER_FIELDS:
    return parse_uint(&config->http.max_response_header_fields, opt, optarg);
  case SHRPX_OPTID_INCLUDE: {
    if (included_set.count(optarg)) {
      LOG(ERROR) << opt << ": " << optarg << " has already been included";
      return -1;
    }

    included_set.insert(optarg);
    auto rv = load_config(config, optarg.c_str(), included_set);
    included_set.erase(optarg);

    if (rv != 0) {
      return -1;
    }

    return 0;
  }
  case SHRPX_OPTID_TLS_TICKET_KEY_CIPHER:
    if (util::strieq_l("aes-128-cbc", optarg)) {
      config->tls.ticket.cipher = EVP_aes_128_cbc();
    } else if (util::strieq_l("aes-256-cbc", optarg)) {
      config->tls.ticket.cipher = EVP_aes_256_cbc();
    } else {
      LOG(ERROR) << opt
                 << ": unsupported cipher for ticket encryption: " << optarg;
      return -1;
    }
    config->tls.ticket.cipher_given = true;

    return 0;
  case SHRPX_OPTID_HOST_REWRITE:
    config->http.no_host_rewrite = !util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED:
  case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED: {
    auto addr_end = std::find(std::begin(optarg), std::end(optarg), ';');
    auto src_params = StringRef{addr_end, std::end(optarg)};

    MemcachedConnectionParams params{};
    if (parse_memcached_connection_params(params, src_params, StringRef{opt}) !=
        0) {
      return -1;
    }

    if (split_host_port(host, sizeof(host), &port,
                        StringRef{std::begin(optarg), addr_end}, opt) == -1) {
      return -1;
    }

    switch (optid) {
    case SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED: {
      auto &memcachedconf = config->tls.session_cache.memcached;
      memcachedconf.host = make_string_ref(config->balloc, StringRef{host});
      memcachedconf.port = port;
      memcachedconf.tls = params.tls;
      break;
    }
    case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED: {
      auto &memcachedconf = config->tls.ticket.memcached;
      memcachedconf.host = make_string_ref(config->balloc, StringRef{host});
      memcachedconf.port = port;
      memcachedconf.tls = params.tls;
      break;
    }
    };

    return 0;
  }
  case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_INTERVAL:
    return parse_duration(&config->tls.ticket.memcached.interval, opt, optarg);
  case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_MAX_RETRY: {
    int n;
    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n > 30) {
      LOG(ERROR) << opt << ": must be smaller than or equal to 30";
      return -1;
    }

    config->tls.ticket.memcached.max_retry = n;
    return 0;
  }
  case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_MAX_FAIL:
    return parse_uint(&config->tls.ticket.memcached.max_fail, opt, optarg);
  case SHRPX_OPTID_TLS_DYN_REC_WARMUP_THRESHOLD: {
    size_t n;
    if (parse_uint_with_unit(&n, opt, optarg) != 0) {
      return -1;
    }

    config->tls.dyn_rec.warmup_threshold = n;

    return 0;
  }

  case SHRPX_OPTID_TLS_DYN_REC_IDLE_TIMEOUT:
    return parse_duration(&config->tls.dyn_rec.idle_timeout, opt, optarg);

  case SHRPX_OPTID_MRUBY_FILE:
#ifdef HAVE_MRUBY
    config->mruby_file = make_string_ref(config->balloc, optarg);
#else  // !HAVE_MRUBY
    LOG(WARN) << opt
              << ": ignored because mruby support is disabled at build time.";
#endif // !HAVE_MRUBY
    return 0;
  case SHRPX_OPTID_ACCEPT_PROXY_PROTOCOL:
    config->conn.upstream.accept_proxy_protocol = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_ADD_FORWARDED: {
    auto &fwdconf = config->http.forwarded;
    fwdconf.params = FORWARDED_NONE;
    for (const auto &param : util::split_str(optarg, ',')) {
      if (util::strieq_l("by", param)) {
        fwdconf.params |= FORWARDED_BY;
        continue;
      }
      if (util::strieq_l("for", param)) {
        fwdconf.params |= FORWARDED_FOR;
        continue;
      }
      if (util::strieq_l("host", param)) {
        fwdconf.params |= FORWARDED_HOST;
        continue;
      }
      if (util::strieq_l("proto", param)) {
        fwdconf.params |= FORWARDED_PROTO;
        continue;
      }

      LOG(ERROR) << opt << ": unknown parameter " << optarg;

      return -1;
    }

    return 0;
  }
  case SHRPX_OPTID_STRIP_INCOMING_FORWARDED:
    config->http.forwarded.strip_incoming = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_FORWARDED_BY:
  case SHRPX_OPTID_FORWARDED_FOR: {
    auto type = parse_forwarded_node_type(optarg);

    if (type == -1 ||
        (optid == SHRPX_OPTID_FORWARDED_FOR && optarg[0] == '_')) {
      LOG(ERROR) << opt << ": unknown node type or illegal obfuscated string "
                 << optarg;
      return -1;
    }

    auto &fwdconf = config->http.forwarded;

    switch (optid) {
    case SHRPX_OPTID_FORWARDED_BY:
      fwdconf.by_node_type = static_cast<shrpx_forwarded_node_type>(type);
      if (optarg[0] == '_') {
        fwdconf.by_obfuscated = make_string_ref(config->balloc, optarg);
      } else {
        fwdconf.by_obfuscated = StringRef::from_lit("");
      }
      break;
    case SHRPX_OPTID_FORWARDED_FOR:
      fwdconf.for_node_type = static_cast<shrpx_forwarded_node_type>(type);
      break;
    }

    return 0;
  }
  case SHRPX_OPTID_NO_HTTP2_CIPHER_BLACK_LIST:
    config->tls.no_http2_cipher_black_list = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_BACKEND_HTTP1_TLS:
  case SHRPX_OPTID_BACKEND_TLS:
    LOG(WARN) << opt << ": deprecated.  Use tls keyword in "
              << SHRPX_OPT_BACKEND << " instead.";
    return 0;
  case SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_TLS:
    LOG(WARN) << opt << ": deprecated.  Use tls keyword in "
              << SHRPX_OPT_TLS_SESSION_CACHE_MEMCACHED;
    return 0;
  case SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_CERT_FILE:
    config->tls.session_cache.memcached.cert_file =
        make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_PRIVATE_KEY_FILE:
    config->tls.session_cache.memcached.private_key_file =
        make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_TLS:
    LOG(WARN) << opt << ": deprecated.  Use tls keyword in "
              << SHRPX_OPT_TLS_TICKET_KEY_MEMCACHED;
    return 0;
  case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_CERT_FILE:
    config->tls.ticket.memcached.cert_file =
        make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_PRIVATE_KEY_FILE:
    config->tls.ticket.memcached.private_key_file =
        make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_TLS_TICKET_KEY_MEMCACHED_ADDRESS_FAMILY:
    return parse_address_family(&config->tls.ticket.memcached.family, opt,
                                optarg);
  case SHRPX_OPTID_TLS_SESSION_CACHE_MEMCACHED_ADDRESS_FAMILY:
    return parse_address_family(&config->tls.session_cache.memcached.family,
                                opt, optarg);
  case SHRPX_OPTID_BACKEND_ADDRESS_FAMILY:
    return parse_address_family(&config->conn.downstream->family, opt, optarg);
  case SHRPX_OPTID_FRONTEND_HTTP2_MAX_CONCURRENT_STREAMS:
    return parse_uint(&config->http2.upstream.max_concurrent_streams, opt,
                      optarg);
  case SHRPX_OPTID_BACKEND_HTTP2_MAX_CONCURRENT_STREAMS:
    return parse_uint(&config->http2.downstream.max_concurrent_streams, opt,
                      optarg);
  case SHRPX_OPTID_ERROR_PAGE:
    return parse_error_page(config->http.error_pages, opt, optarg);
  case SHRPX_OPTID_NO_KQUEUE:
    if ((ev_supported_backends() & EVBACKEND_KQUEUE) == 0) {
      LOG(WARN) << opt << ": kqueue is not supported on this platform";
      return 0;
    }

    config->ev_loop_flags = ev_recommended_backends() & ~EVBACKEND_KQUEUE;

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_SETTINGS_TIMEOUT:
    return parse_duration(&config->http2.upstream.timeout.settings, opt,
                          optarg);
  case SHRPX_OPTID_BACKEND_HTTP2_SETTINGS_TIMEOUT:
    return parse_duration(&config->http2.downstream.timeout.settings, opt,
                          optarg);
  case SHRPX_OPTID_API_MAX_REQUEST_BODY:
    return parse_uint_with_unit(&config->api.max_request_body, opt, optarg);
  case SHRPX_OPTID_BACKEND_MAX_BACKOFF:
    return parse_duration(&config->conn.downstream->timeout.max_backoff, opt,
                          optarg);
  case SHRPX_OPTID_SERVER_NAME:
    config->http.server_name = make_string_ref(config->balloc, optarg);

    return 0;
  case SHRPX_OPTID_NO_SERVER_REWRITE:
    config->http.no_server_rewrite = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_OPTIMIZE_WRITE_BUFFER_SIZE:
    config->http2.upstream.optimize_write_buffer_size =
        util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_OPTIMIZE_WINDOW_SIZE:
    config->http2.upstream.optimize_window_size = util::strieq_l("yes", optarg);

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_WINDOW_SIZE:
    if (parse_uint_with_unit(&config->http2.upstream.window_size, opt,
                             optarg) != 0) {
      return -1;
    }

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_CONNECTION_WINDOW_SIZE:
    if (parse_uint_with_unit(&config->http2.upstream.connection_window_size,
                             opt, optarg) != 0) {
      return -1;
    }

    return 0;
  case SHRPX_OPTID_BACKEND_HTTP2_WINDOW_SIZE:
    if (parse_uint_with_unit(&config->http2.downstream.window_size, opt,
                             optarg) != 0) {
      return -1;
    }

    return 0;
  case SHRPX_OPTID_BACKEND_HTTP2_CONNECTION_WINDOW_SIZE:
    if (parse_uint_with_unit(&config->http2.downstream.connection_window_size,
                             opt, optarg) != 0) {
      return -1;
    }

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_ENCODER_DYNAMIC_TABLE_SIZE:
    if (parse_uint_with_unit(&config->http2.upstream.encoder_dynamic_table_size,
                             opt, optarg) != 0) {
      return -1;
    }

    nghttp2_option_set_max_deflate_dynamic_table_size(
        config->http2.upstream.option,
        config->http2.upstream.encoder_dynamic_table_size);
    nghttp2_option_set_max_deflate_dynamic_table_size(
        config->http2.upstream.alt_mode_option,
        config->http2.upstream.encoder_dynamic_table_size);

    return 0;
  case SHRPX_OPTID_FRONTEND_HTTP2_DECODER_DYNAMIC_TABLE_SIZE:
    return parse_uint_with_unit(
        &config->http2.upstream.decoder_dynamic_table_size, opt, optarg);
  case SHRPX_OPTID_BACKEND_HTTP2_ENCODER_DYNAMIC_TABLE_SIZE:
    if (parse_uint_with_unit(
            &config->http2.downstream.encoder_dynamic_table_size, opt,
            optarg) != 0) {
      return -1;
    }

    nghttp2_option_set_max_deflate_dynamic_table_size(
        config->http2.downstream.option,
        config->http2.downstream.encoder_dynamic_table_size);

    return 0;
  case SHRPX_OPTID_BACKEND_HTTP2_DECODER_DYNAMIC_TABLE_SIZE:
    return parse_uint_with_unit(
        &config->http2.downstream.decoder_dynamic_table_size, opt, optarg);
  case SHRPX_OPTID_ECDH_CURVES:
#if !LIBRESSL_IN_USE && OPENSSL_VERSION_NUMBER >= 0x10002000L
    config->tls.ecdh_curves = make_string_ref(config->balloc, optarg);
#else  // !(!LIBRESSL_IN_USE && OPENSSL_VERSION_NUMBER >= 0x10002000L)
    LOG(WARN) << opt << ": This option requires OpenSSL >= 1.0.2";
#endif // !(!LIBRESSL_IN_USE && OPENSSL_VERSION_NUMBER >= 0x10002000L)
    return 0;
  case SHRPX_OPTID_CONF:
    LOG(WARN) << "conf: ignored";

    return 0;
  }

  LOG(ERROR) << "Unknown option: " << opt;

  return -1;
}

int load_config(Config *config, const char *filename,
                std::set<StringRef> &include_set) {
  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    LOG(ERROR) << "Could not open config file " << filename;
    return -1;
  }
  std::string line;
  int linenum = 0;
  while (std::getline(in, line)) {
    ++linenum;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto eq = std::find(std::begin(line), std::end(line), '=');
    if (eq == std::end(line)) {
      LOG(ERROR) << "Bad configuration format in " << filename << " at line "
                 << linenum;
      return -1;
    }
    *eq = '\0';

    if (parse_config(config, StringRef{std::begin(line), eq},
                     StringRef{eq + 1, std::end(line)}, include_set) != 0) {
      return -1;
    }
  }
  return 0;
}

StringRef str_syslog_facility(int facility) {
  switch (facility) {
  case (LOG_AUTH):
    return StringRef::from_lit("auth");
#ifdef LOG_AUTHPRIV
  case (LOG_AUTHPRIV):
    return StringRef::from_lit("authpriv");
#endif // LOG_AUTHPRIV
  case (LOG_CRON):
    return StringRef::from_lit("cron");
  case (LOG_DAEMON):
    return StringRef::from_lit("daemon");
#ifdef LOG_FTP
  case (LOG_FTP):
    return StringRef::from_lit("ftp");
#endif // LOG_FTP
  case (LOG_KERN):
    return StringRef::from_lit("kern");
  case (LOG_LOCAL0):
    return StringRef::from_lit("local0");
  case (LOG_LOCAL1):
    return StringRef::from_lit("local1");
  case (LOG_LOCAL2):
    return StringRef::from_lit("local2");
  case (LOG_LOCAL3):
    return StringRef::from_lit("local3");
  case (LOG_LOCAL4):
    return StringRef::from_lit("local4");
  case (LOG_LOCAL5):
    return StringRef::from_lit("local5");
  case (LOG_LOCAL6):
    return StringRef::from_lit("local6");
  case (LOG_LOCAL7):
    return StringRef::from_lit("local7");
  case (LOG_LPR):
    return StringRef::from_lit("lpr");
  case (LOG_MAIL):
    return StringRef::from_lit("mail");
  case (LOG_SYSLOG):
    return StringRef::from_lit("syslog");
  case (LOG_USER):
    return StringRef::from_lit("user");
  case (LOG_UUCP):
    return StringRef::from_lit("uucp");
  default:
    return StringRef::from_lit("(unknown)");
  }
}

int int_syslog_facility(const StringRef &strfacility) {
  if (util::strieq_l("auth", strfacility)) {
    return LOG_AUTH;
  }

#ifdef LOG_AUTHPRIV
  if (util::strieq_l("authpriv", strfacility)) {
    return LOG_AUTHPRIV;
  }
#endif // LOG_AUTHPRIV

  if (util::strieq_l("cron", strfacility)) {
    return LOG_CRON;
  }

  if (util::strieq_l("daemon", strfacility)) {
    return LOG_DAEMON;
  }

#ifdef LOG_FTP
  if (util::strieq_l("ftp", strfacility)) {
    return LOG_FTP;
  }
#endif // LOG_FTP

  if (util::strieq_l("kern", strfacility)) {
    return LOG_KERN;
  }

  if (util::strieq_l("local0", strfacility)) {
    return LOG_LOCAL0;
  }

  if (util::strieq_l("local1", strfacility)) {
    return LOG_LOCAL1;
  }

  if (util::strieq_l("local2", strfacility)) {
    return LOG_LOCAL2;
  }

  if (util::strieq_l("local3", strfacility)) {
    return LOG_LOCAL3;
  }

  if (util::strieq_l("local4", strfacility)) {
    return LOG_LOCAL4;
  }

  if (util::strieq_l("local5", strfacility)) {
    return LOG_LOCAL5;
  }

  if (util::strieq_l("local6", strfacility)) {
    return LOG_LOCAL6;
  }

  if (util::strieq_l("local7", strfacility)) {
    return LOG_LOCAL7;
  }

  if (util::strieq_l("lpr", strfacility)) {
    return LOG_LPR;
  }

  if (util::strieq_l("mail", strfacility)) {
    return LOG_MAIL;
  }

  if (util::strieq_l("news", strfacility)) {
    return LOG_NEWS;
  }

  if (util::strieq_l("syslog", strfacility)) {
    return LOG_SYSLOG;
  }

  if (util::strieq_l("user", strfacility)) {
    return LOG_USER;
  }

  if (util::strieq_l("uucp", strfacility)) {
    return LOG_UUCP;
  }

  return -1;
}

StringRef strproto(shrpx_proto proto) {
  switch (proto) {
  case PROTO_NONE:
    return StringRef::from_lit("none");
  case PROTO_HTTP1:
    return StringRef::from_lit("http/1.1");
  case PROTO_HTTP2:
    return StringRef::from_lit("h2");
  case PROTO_MEMCACHED:
    return StringRef::from_lit("memcached");
  }

  // gcc needs this.
  assert(0);
}

namespace {
// Consistent hashing method described in
// https://github.com/RJ/ketama.  Generate 160 32-bit hashes per |s|,
// which is usually backend address.  The each hash is associated to
// index of backend address.  When all hashes for every backend
// address are calculated, sort it in ascending order of hash.  To
// choose the index, compute 32-bit hash based on client IP address,
// and do lower bound search in the array. The returned index is the
// backend to use.
int compute_affinity_hash(std::vector<AffinityHash> &res, size_t idx,
                          const StringRef &s) {
  int rv;
  std::array<uint8_t, 32> buf;

  for (auto i = 0; i < 20; ++i) {
    auto t = s.str();
    t += i;

    rv = util::sha256(buf.data(), StringRef{t});
    if (rv != 0) {
      return -1;
    }

    for (int i = 0; i < 8; ++i) {
      auto h = (static_cast<uint32_t>(buf[4 * i]) << 24) |
               (static_cast<uint32_t>(buf[4 * i + 1]) << 16) |
               (static_cast<uint32_t>(buf[4 * i + 2]) << 8) |
               static_cast<uint32_t>(buf[4 * i + 3]);

      res.emplace_back(idx, h);
    }
  }

  return 0;
}
} // namespace

// Configures the following member in |config|:
// conn.downstream_router, conn.downstream.addr_groups,
// conn.downstream.addr_group_catch_all.
int configure_downstream_group(Config *config, bool http2_proxy,
                               bool numeric_addr_only,
                               const TLSConfig &tlsconf) {
  int rv;

  auto &downstreamconf = *config->conn.downstream;
  auto &addr_groups = downstreamconf.addr_groups;
  auto &routerconf = downstreamconf.router;
  auto &router = routerconf.router;

  if (addr_groups.empty()) {
    DownstreamAddrConfig addr{};
    addr.host = StringRef::from_lit(DEFAULT_DOWNSTREAM_HOST);
    addr.port = DEFAULT_DOWNSTREAM_PORT;
    addr.proto = PROTO_HTTP1;

    DownstreamAddrGroupConfig g(StringRef::from_lit("/"));
    g.addrs.push_back(std::move(addr));
    router.add_route(g.pattern, addr_groups.size());
    addr_groups.push_back(std::move(g));
  } else if (http2_proxy) {
    // We don't support host mapping in these cases.  Move all
    // non-catch-all patterns to catch-all pattern.
    DownstreamAddrGroupConfig catch_all(StringRef::from_lit("/"));
    for (auto &g : addr_groups) {
      std::move(std::begin(g.addrs), std::end(g.addrs),
                std::back_inserter(catch_all.addrs));
    }
    std::vector<DownstreamAddrGroupConfig>().swap(addr_groups);
    // maybe not necessary?
    routerconf = RouterConfig{};
    router.add_route(catch_all.pattern, addr_groups.size());
    addr_groups.push_back(std::move(catch_all));
  }

  // backward compatibility: override all SNI fields with the option
  // value --backend-tls-sni-field
  if (!tlsconf.backend_sni_name.empty()) {
    auto &sni = tlsconf.backend_sni_name;
    for (auto &addr_group : addr_groups) {
      for (auto &addr : addr_group.addrs) {
        addr.sni = StringRef{sni};
      }
    }
  }

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Resolving backend address";
  }

  ssize_t catch_all_group = -1;
  for (size_t i = 0; i < addr_groups.size(); ++i) {
    auto &g = addr_groups[i];
    if (g.pattern == StringRef::from_lit("/")) {
      catch_all_group = i;
    }
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Host-path pattern: group " << i << ": '" << g.pattern
                << "'";
      for (auto &addr : g.addrs) {
        LOG(INFO) << "group " << i << " -> " << addr.host.c_str()
                  << (addr.host_unix ? "" : ":" + util::utos(addr.port))
                  << ", proto=" << strproto(addr.proto)
                  << (addr.tls ? ", tls" : "");
      }
    }
  }

  if (catch_all_group == -1) {
    LOG(FATAL) << "backend: No catch-all backend address is configured";
    return -1;
  }

  downstreamconf.addr_group_catch_all = catch_all_group;

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Catch-all pattern is group " << catch_all_group;
  }

  auto resolve_flags = numeric_addr_only ? AI_NUMERICHOST | AI_NUMERICSERV : 0;

  for (auto &g : addr_groups) {
    for (auto &addr : g.addrs) {

      if (addr.host_unix) {
        // for AF_UNIX socket, we use "localhost" as host for backend
        // hostport.  This is used as Host header field to backend and
        // not going to be passed to any syscalls.
        addr.hostport = StringRef::from_lit("localhost");

        auto path = addr.host.c_str();
        auto pathlen = addr.host.size();

        if (pathlen + 1 > sizeof(addr.addr.su.un.sun_path)) {
          LOG(FATAL) << "UNIX domain socket path " << path << " is too long > "
                     << sizeof(addr.addr.su.un.sun_path);
          return -1;
        }

        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "Use UNIX domain socket path " << path
                    << " for backend connection";
        }

        addr.addr.su.un.sun_family = AF_UNIX;
        // copy path including terminal NULL
        std::copy_n(path, pathlen + 1, addr.addr.su.un.sun_path);
        addr.addr.len = sizeof(addr.addr.su.un);

        continue;
      }

      addr.hostport =
          util::make_http_hostport(downstreamconf.balloc, addr.host, addr.port);

      auto hostport =
          util::make_hostport(downstreamconf.balloc, addr.host, addr.port);

      if (resolve_hostname(&addr.addr, addr.host.c_str(), addr.port,
                           downstreamconf.family, resolve_flags) == -1) {
        LOG(FATAL) << "Resolving backend address failed: " << hostport;
        return -1;
      }

      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "Resolved backend address: " << hostport << " -> "
                  << util::to_numeric_addr(&addr.addr);
      }
    }

    if (g.affinity == AFFINITY_IP) {
      size_t idx = 0;
      for (auto &addr : g.addrs) {
        auto p = reinterpret_cast<uint8_t *>(&addr.addr.su);
        rv = compute_affinity_hash(g.affinity_hash, idx,
                                   StringRef{p, addr.addr.len});
        if (rv != 0) {
          return -1;
        }

        ++idx;
      }

      std::sort(std::begin(g.affinity_hash), std::end(g.affinity_hash),
                [](const AffinityHash &lhs, const AffinityHash &rhs) {
                  return lhs.hash < rhs.hash;
                });
    }
  }

  return 0;
}

int resolve_hostname(Address *addr, const char *hostname, uint16_t port,
                     int family, int additional_flags) {
  int rv;

  auto service = util::utos(port);

  addrinfo hints{};
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags |= additional_flags;
#ifdef AI_ADDRCONFIG
  hints.ai_flags |= AI_ADDRCONFIG;
#endif // AI_ADDRCONFIG
  addrinfo *res;

  rv = getaddrinfo(hostname, service.c_str(), &hints, &res);
  if (rv != 0) {
    LOG(FATAL) << "Unable to resolve address for " << hostname << ": "
               << gai_strerror(rv);
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  char host[NI_MAXHOST];
  rv = getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof(host), nullptr,
                   0, NI_NUMERICHOST);
  if (rv != 0) {
    LOG(FATAL) << "Address resolution for " << hostname
               << " failed: " << gai_strerror(rv);

    return -1;
  }

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Address resolution for " << hostname
              << " succeeded: " << host;
  }

  memcpy(&addr->su, res->ai_addr, res->ai_addrlen);
  addr->len = res->ai_addrlen;

  return 0;
}

} // namespace shrpx
