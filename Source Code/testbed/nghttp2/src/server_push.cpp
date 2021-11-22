#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <nghttp2/asio_http2_server.h>

using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;

std::string bitrate;

int segment_duration = 500; // 1000ms
auto avail_seg = std::make_shared<int>();
auto server_seg = std::make_shared<int>();
auto client_seg = std::make_shared<int>();
const int MAX_SEGMENTS = 2001;

bool on_periodic_mode = true;
bool on_pushing_in_periodic_mode = false;

std::chrono::time_point<std::chrono::system_clock> start, end;

void init_new_connection() {
  *avail_seg = 0;
  *server_seg = 0;

  on_periodic_mode = true;
  on_pushing_in_periodic_mode = false;

  start = std::chrono::system_clock::now();
}

int get_time() {
  end = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

void print_new_seg(int cur_seg) {
  if (on_periodic_mode)
    std::cout << std::endl << "Push seg #" << cur_seg << " bitrate " << bitrate;
  else 
    std::cout << std::endl << "Response seg #" << cur_seg << " bitrate " << bitrate;

  std::cout << " at time: " << get_time() << "ms" << std::endl;
}

void push_remaining_files(const response *res) {
  if (*server_seg + 1 >= MAX_SEGMENTS) {
    res->write_head(200);
    res->end(file_generator("push.html"));
    return;
  } 

  print_new_seg(*server_seg+1);
  boost::system::error_code ec;

  auto push = res->push(ec, "GET", "/seg_"+std::to_string(*server_seg+1)+"_rate_"+bitrate);

  push->on_close([res](uint32_t error_code) {
    if (!error_code) {                    // if CANCEL is not sent
      *server_seg = *server_seg + 1;
      std::cout << "Sent seg #" << *server_seg << " bitrate " << bitrate 
                        << " at time: " << get_time() << "ms" << std::endl;
    }

    if (error_code == NGHTTP2_CANCEL) {   // if receving Cancel req (code = 0x08)
      *server_seg = *client_seg;
      std::cout << "Resend seg #" << *server_seg+1 << " bitrate " << bitrate
                        << " at time: " << get_time() << "ms" << std::endl;
    } 

    if (*server_seg == *avail_seg) {
      on_periodic_mode = true;
      on_pushing_in_periodic_mode = false;
    } else {
      on_periodic_mode = false;
      on_pushing_in_periodic_mode = true;
      push_remaining_files(res);
    }

  });

  push->write_head(200);
  push->end(file_generator("./real_cbr/"+std::to_string(segment_duration)+"ms/"+bitrate));
  on_pushing_in_periodic_mode = true;
}

void push_file(
    std::shared_ptr<boost::asio::basic_deadline_timer<boost::posix_time::ptime>> timer, 
    const response *res, std::shared_ptr<bool> closed, boost::system::error_code &ec) {
  if (ec || *closed) {
    return;
  }

  // Hung_comment: Cai nay chi la dia chi thoi nhe!
  timer->expires_at(timer->expires_at() + boost::posix_time::milliseconds(segment_duration));
  timer->async_wait(boost::bind(push_file, timer, res, closed, ec));

  *avail_seg = *avail_seg + 1;          // generate a new segment every segment duration

  // if the server are pushing another segment
  if (on_pushing_in_periodic_mode) { return; }

  // push all remaining segments, 
  // during this duration, on_pushing_in_periodic_mode = true
  push_remaining_files(res);
}

int main(int argc, char *argv[]) {
  boost::system::error_code ec;
  boost::asio::ssl::context tls(boost::asio::ssl::context::sslv23);

//  tls.use_private_key_file("server.key", boost::asio::ssl::context::pem);
//  tls.use_certificate_chain_file("server.crt");
//  configure_tls_context_easy(ec, tls);

  http2 server;
  server.num_threads(100);

  server.handle("/file/", [](const request &req, const response &res) {
    init_new_connection();

    std::vector<std::string> strs;
    boost::split(strs, req.uri().path, boost::is_any_of("/"));
    bitrate = strs[2];
  
    boost::system::error_code ec;

    auto timer = std::make_shared<boost::asio::deadline_timer>(res.io_service(), boost::posix_time::milliseconds(segment_duration));
    auto closed = std::make_shared<bool>();

    res.on_close([timer, closed](uint32_t error_code) {
      timer->cancel();
      *closed = true;
    });

    timer->async_wait(boost::bind(push_file, timer, &res, closed, ec));
  });

  server.handle("/change/", [](const request &req, const response &res) {
    std::vector<std::string> strs;
    boost::split(strs, req.uri().path, boost::is_any_of("/"));
    bitrate = strs[2];

    std::cout << "[CONT] segment #" << *server_seg+1 << std::endl;

    res.write_head(200);
    res.end("Responded by "+bitrate);
  });

  server.handle("/req/", [](const request &req, const response &res) {
    std::vector<std::string> strs;
    boost::split(strs, req.uri().path, boost::is_any_of("/"));
    bitrate = strs[2];

    *client_seg = std::stoi(strs[3]);
    // Now, we did not consider the case that the server sent out the cancel segment,
    // when RTT is changed
    std::cout << "[SUCC] segment #" << *client_seg << std::endl;

    res.write_head(200);
    res.end("Responded by "+bitrate);
  });

/*
  std::string style_css = "h1 { color: red; }";
  server.handle("/push.html", [&style_css](const request &req, const response &res) {
    boost::system::error_code ec;

    res.write_head(200);
    res.end(file_generator("push.html"));

    auto push = res.push(ec, "GET", "/style.css");
    push->write_head(200);
    push->end(style_css);

    auto push2 = res.push(ec, "GET", "/push.png");
    push2->write_head(200);
    push2->end(file_generator("push.png"));
  });
*/


  if (server.listen_and_serve(ec, "192.168.9.76", "3002")) {
    std::cerr << "error: " << ec.message() << std::endl;
  }

/*
  if (server.listen_and_serve(ec, tls, "192.168.9.76", "3002")) {
    std::cerr << "error: " << ec.message() << std::endl;
  }
*/
}
