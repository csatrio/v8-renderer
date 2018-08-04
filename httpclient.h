#include "httpserver.h"
#include <forward_list>

#define READ_BUFFER 65536
#define WRITE_BUFFER 65536

using namespace std;

class HttpClient;
static void* execute_thread(void* p);
static void httpclient_on_resolved(uv_getaddrinfo_t* req, int status, struct addrinfo* res);
static void httpclient_read(uv_stream_t* tcp, ssize_t nread, const uv_buf_t* buf);
static void httpclient_on_connect(uv_connect_t* req, int status);
static void httpclient_write(uv_write_t* response, int status);
static void httpclient_close(uv_handle_t* req);

static inline void free_async_handle(uv_async_t* handle){
  uv_close((uv_handle_t*)handle, NULL); // close async handle immediately
}
static void async_hold(uv_async_t *handle){free_async_handle(handle);}

// allocate buffer from read_buffer_pool
static void static_allocator(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf){
  static char* base = (char*)malloc(READ_BUFFER);
  memset(base, 0, READ_BUFFER);
  buf->base = base;
  buf->len = READ_BUFFER;
}

typedef struct http_header{
  const char* key;
  const char* value;
}http_header;

static void free_header(forward_list<http_header> headers){
  for(http_header h : headers){
    free((char*)h.key);
    free((char*)h.value);
  }  
  headers.clear();
}

// Integrated Http Request and Response
typedef struct HttpClient {
    uv_getaddrinfo_t addr_req;
    uv_loop_t* loop;
    uv_tcp_t handle;
    const char* url;
    const char* host;
    const char* port;
    char* method;
    char* response;
    unsigned int status;
    forward_list<http_header> response_header;
    function<void(HttpClient*)> callback;
    
  HttpClient(uv_loop_t* _loop, const char* _host, const char* _port, function<void(HttpClient*)> _callback){
    loop = _loop;
    host = _host;
    port = _port;
    callback = _callback;
    method = NULL;
    response = NULL;
  };

  ~HttpClient(){
    free_header(response_header);
    free(method);
    free(response);
  };

  void send(const char* _url){
    send(_url, host, port);
  }

  void send(const char* _url, const char* _host, const char* _port){
    static struct addrinfo ai;
    static bool init = false;
    if(!init){
      ai.ai_family = PF_INET;
      ai.ai_socktype = SOCK_STREAM;
      ai.ai_protocol = IPPROTO_TCP;
      ai.ai_flags = 0;
      init = true;
    }
    addr_req.data = this;
    url = _url;
    host = _host;
    port = _port;
    uv_getaddrinfo(loop, &addr_req, httpclient_on_resolved, host, port, &ai);
  }
  
} HttpClient;

namespace clientparser{
  
  // called after the url has been parsed.
  static inline int on_status(http_parser* parser, const char* at, size_t length){
    HttpClient* wrapper = static_cast<HttpClient*>(parser->data);
    wrapper->status = parser->status_code;
    return 0;
  };

  // called when there are either fields or values in the request.
  static inline int on_header_field(http_parser* parser, const char* at, size_t length){
    //cout<<"Header field : "<<string(at,length)<<endl;
    HttpClient* wrapper = static_cast<HttpClient*>(parser->data);
    char* tmp = CharCopy(at, length);
    wrapper->response = tmp;
    return 0;
  };

  // called when header value is given
  static inline int on_header_value(http_parser* parser, const char* at, size_t length){
    //cout<<"Header value : "<<string(at,length)<<endl;
    HttpClient* wrapper = static_cast<HttpClient*>(parser->data);
    char* tmp = CharCopy(at, length);
    wrapper->response_header.push_front({wrapper->response, tmp});
    wrapper->response = NULL;
    return 0;
  };

  // called when there is a body for the request.
  static inline int on_body(http_parser* parser, const char* at, size_t length){
    HttpClient* wrapper = static_cast<HttpClient*>(parser->data);
    if (at && wrapper && (int) length > -1) {
      wrapper->response = CharCopy(at, length);
    }
    return 0;
  };

  http_parser_settings* get_settings(){
    http_parser_settings *settings = (http_parser_settings*)malloc(sizeof(http_parser_settings));
    http_parser_settings_init(settings);
    settings->on_status = on_status;
    settings->on_header_field = on_header_field;
    settings->on_header_value = on_header_value;
    settings->on_body = on_body;
    return settings;
  }

}; // namespace clientparser

typedef struct loop_holder{
  uv_loop_t* loop;
  uv_async_t holder;
}loop_holder;

static void* execute_thread(void* p){
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  println("executing uv loop on another thread");
  loop_holder* holder = (loop_holder*) p;
  holder->loop = uv_loop_new();
  uv_async_init(holder->loop, &holder->holder, async_hold);
  uv_run(holder->loop, UV_RUN_DEFAULT);
}

static void httpclient_on_resolved (uv_getaddrinfo_t* req, int status, struct addrinfo* res){
  HttpClient* client = (HttpClient*) req->data;
  char addr[17] = { '\0' };

  uv_ip4_name((struct sockaddr_in*) res->ai_addr, addr, 16);
  uv_freeaddrinfo(res);

  struct sockaddr_in dest;
  uv_ip4_addr(addr, atoi(client->port), &dest);

  client->handle.data = client;

  uv_tcp_init(client->loop, &client->handle);

  uv_connect_t* connect = (uv_connect_t*)malloc(sizeof(uv_connect_t));
  connect->data = client;
  uv_tcp_connect(
    connect,
    &client->handle,
    (const struct sockaddr*) &dest,
    httpclient_on_connect);
}

// called once a connection is made.
static void httpclient_on_connect(uv_connect_t* req, int status){
  static uv_buf_t reqbuf = {.base = NULL, .len = 0};
  static ostringstream buf;
  HttpClient* client = (HttpClient*) req->data;
  buf.str("");
  buf.clear();

  buf << (client->method == NULL ? "GET" : client->method) << " " << client->url << " HTTP/1.1" << CRLF;
  buf << "Connection: keep-alive" << CRLF;
  buf << CRLF;

  string tmp = buf.str();

  reqbuf.base = (char*)tmp.c_str();
  reqbuf.len = tmp.length();

  uv_write_t *write_req = (uv_write_t *) malloc(sizeof(uv_write_t));
  write_req->data = client;
  uv_write(write_req, (uv_stream_t*)&client->handle, &reqbuf, 1, httpclient_write);

  free(req);
}

// called on every read
static void httpclient_read(uv_stream_t* tcp, ssize_t nread, const uv_buf_t* buf) {
  static http_parser_settings* settings = clientparser::get_settings();
  http_parser parser;
  http_parser_init(&parser, HTTP_RESPONSE);
  HttpClient* client = (HttpClient*) tcp->data;
  parser.data = client;
  http_parser_execute(&parser, settings, buf->base, nread);
  client->callback(client);
  uv_close((uv_handle_t*)&client->handle, httpclient_close);
}

// after write cleanup
static void httpclient_write(uv_write_t* req, int status) {
  HttpClient* client = (HttpClient*)req->data;
  uv_read_start((uv_stream_t*) &client->handle, static_allocator, httpclient_read);
  free(req);
};

static void httpclient_close(uv_handle_t* req){
  HttpClient* client = (HttpClient*)req->data;
  uv_unref(req);
  delete client;
}