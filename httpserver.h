#include "components.h"

// Unix Socket Includes
#include <sys/socket.h>
#include <sys/un.h>

#define MAX_WRITES 10000

class HttpServer;
static void http_read(uv_stream_t* tcp, ssize_t nread, const uv_buf_t* buf);
static void http_on_connect(uv_stream_t* handle, int status);
static void async_callback(uv_async_t *handle);
static void on_write_end(uv_write_t* response, int status);

static inline void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  buf->base = (char*)malloc(suggested_size);
  buf->len = suggested_size;
}

static inline void free_async_handle(uv_async_t* handle){
  handle->data = NULL; // nullify the data to avoid side effects
  uv_unref((uv_handle_t*)handle);
  uv_close((uv_handle_t*)handle, NULL); // close async handle immediately
}

// Integrated Http Request and Response
typedef struct _HttpData {
  uv_buf_t resBuf;
  uv_async_t async;
  uv_tcp_t handle;

  string request_url;
  ostringstream request_body;
  string request_method;

  int response_status;
  bool complete;
  void* server;

  map<const string, const string>* response_header;

  _HttpData(){
    complete = false;
    response_header = new map<const string, const string>;
    resBuf = {.base = NULL, .len = 0};
  };

  ~_HttpData(){
    response_header->clear();
    delete response_header;
    free(resBuf.base);
  };

  void setResponseHeader (const string key, const string val) {
    response_header->insert({ key, val });
  }

  void setResponseStatus(int status){
    response_status = status;
  }

  void sendResponse(string str){
    if(!complete) return;
    ostringstream ss;
    int len = str.length();

    ss << "HTTP/1.1 " << response_status << " OK" << CRLF;

    for (auto &header : *response_header) {
      ss << header.first << ": " << header.second << CRLF;
    }
    ss << CRLF;
     
    bool isChunked = response_header->count("Transfer-Encoding") 
      && (*response_header)["Transfer-Encoding"] == "chunked";

    if (isChunked) {
      ss << std::hex << len 
         << std::dec << CRLF << str << CRLF;
    }
    else {
      ss << str;
    }

    if (isChunked) {
      ss << "0" << CRLF << CRLF;
    }

    resBuf.base = CharCopy(ss.str().c_str()),
    resBuf.len = strlen(resBuf.base);
    async.data = this;
    uv_async_send(&async);
  }
} HttpData;


namespace parser{
  
  // called after the url has been parsed.
  int on_url(http_parser* parser, const char* at, size_t len){
    HttpData* wrapper = static_cast<HttpData*>(parser->data);
    if (at && wrapper) { wrapper->request_url = string(at, len); }
    return 0;
  };

  // called when there are either fields or values in the request.
  int on_header_field(http_parser* parser, const char* at, size_t length){
    return 0;
  };

  // called when header value is given
  int on_header_value(http_parser* parser, const char* at, size_t length){
    return 0;
  };

  // called once all fields and values have been parsed.
  int on_headers_complete(http_parser* parser){
    HttpData* wrapper = static_cast<HttpData*>(parser->data);
    wrapper->request_method = string(http_method_str((enum http_method) parser->method));
    return 0;
  };

  // called when there is a body for the request.
  int on_body(http_parser* parser, const char* at, size_t len){
    HttpData* wrapper = static_cast<HttpData*>(parser->data);
    if (at && wrapper && (int) len > -1) {
      wrapper->request_body << at;
    }
    return 0;
  };

  // called after all other events.
  int on_message_complete(http_parser* parser){
    HttpData* wrapper = static_cast<HttpData*>(parser->data);
    wrapper->complete = true;
    return 0;
  };

  http_parser_settings* get_settings(){
    http_parser_settings *settings = (http_parser_settings*)malloc(sizeof(http_parser_settings));
    http_parser_settings_init(settings);
    settings->on_url = on_url;
    settings->on_header_field = on_header_field;
    settings->on_header_value = on_header_value;
    settings->on_headers_complete = on_headers_complete;
    settings->on_body = on_body;
    settings->on_message_complete = on_message_complete;
    return settings;
  }

}; // namespace parser


// this is required to avoid segfault when closing handle and deleting wrapper struct
static void free_handle(uv_handle_t* handle){
  HttpData* wrapper = reinterpret_cast<HttpData*>(handle->data);
  handle->data = NULL;
  delete wrapper;
}


// Non Blocking HTTP Server with deferred write capability
// Has asynchronous callback performed by eventloop itself
// Therefore response can be written safely from multiple threads
class HttpServer{
  private:
    uv_loop_t* UV_LOOP;
    uv_tcp_t socket_;
    function<void(HttpData*)> callback;

  public:
    cache_map cache;
    cacheable cache_url;
    http_parser_settings* settings;
    http_parser* parser;

    HttpServer(function<void(HttpData*)> _callback){
      callback = _callback;
      settings = parser::get_settings();
      http_parser *parser = (http_parser*)malloc(sizeof(http_parser));
    };
    ~HttpServer(){
      delete settings;
      free(parser);
    };

    uv_loop_t* loop(){
      return UV_LOOP;
    }

    // forward call to callback
    void send_to_lambda(HttpData* data){
      callback(data);
    }

    int listen (const char* ip, int port) {
      int status = 0;

      #ifdef _WIN32
        SYSTEM_INFO sysinfo;
        GetSystemInfo( &sysinfo );
        int cores = sysinfo.dwNumberOfProcessors;
      #else
        int cores = sysconf(_SC_NPROCESSORS_ONLN);
        char* cores_string = str_format("%d", cores);
      #endif

      printf("Number of detected cores :  %s\n", cores_string);

      #ifdef _WIN32
        SetEnvironmentVariable("UV_THREADPOOL_SIZE", cores_string);
      #else
        setenv("UV_THREADPOOL_SIZE", cores_string, 1);
      #endif

      struct sockaddr_in address;

      //UV_LOOP = uv_default_loop();
      UV_LOOP = uv_loop_new();
      uv_tcp_init(UV_LOOP, &socket_);

      status = uv_ip4_addr(ip, port, &address);
      ASSERT_STATUS(status, "Resolve Address");

      status = uv_tcp_bind(&socket_, (const struct sockaddr*) &address, 0);
      ASSERT_STATUS(status, "Bind");

      status = uv_listen((uv_stream_t*) &socket_, MAX_WRITES, http_on_connect);
      socket_.data = this;

      ASSERT_STATUS(status, "Listen");

      // init loop
      uv_run(UV_LOOP, UV_RUN_DEFAULT);
      return 0;
    }
};

// async http write
static void async_callback(uv_async_t *handle){
  HttpData* wrapper = static_cast<HttpData*>(handle->data);
  free_async_handle(handle);

  if(wrapper->complete){ // on successful read, write processed data
    HttpServer* server = static_cast<HttpServer*>(wrapper->server);
    // add to cache
    if(enable_cache && server->cache_url.is_cache(wrapper->request_url) ) {
      server->cache.add(wrapper->request_url, wrapper->resBuf.base, cache_timeout);
    }
    uv_write_t *_response = (uv_write_t *) malloc(sizeof(uv_write_t));
    uv_write(_response, (uv_stream_t *) &wrapper->handle, &wrapper->resBuf, 1, on_write_end);
  }
  else { // on read error close the handle
    uv_close((uv_handle_t*) &wrapper->handle, free_handle); // async, so need callback
    println("Close handle on error");
  }
};

// after write cleanup
static void on_write_end(uv_write_t* response, int status) {
  uv_handle_t* handle = (uv_handle_t*)response->handle;
  response->handle = NULL;
  uv_unref(handle);
  uv_close(handle, free_handle);  // async, so need callback
  free(response);
};

// called on every read
static void http_read(uv_stream_t* tcp, ssize_t nread, const uv_buf_t* buf) {
  HttpData* wrapper = static_cast<HttpData*>(tcp->data);
  HttpServer* server = static_cast<HttpServer*>(wrapper->server);

  // ON NO ERROR
  if (nread >= 0) {
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = wrapper;
    ssize_t parsed = http_parser_execute(&parser, server->settings, buf->base, nread);
    parser.data = NULL;

    // close handle on parse error
    if (parsed < nread) {
      println("Parse Error : closing handle");
      uv_close((uv_handle_t*) &wrapper->handle, free_handle);
    }
    
    // cache hit
    if(enable_cache){
      string* from_cache = server->cache.get(wrapper->request_url);
      if(from_cache){
        wrapper->resBuf.base = CharCopy(from_cache->c_str()); 
        wrapper->resBuf.len = from_cache->length();
        wrapper->server = NULL;
        uv_write_t *_response = (uv_write_t *) malloc(sizeof(uv_write_t));
        uv_write(_response, (uv_stream_t *) &wrapper->handle, &wrapper->resBuf, 1, on_write_end);
      }
    }

    // cache miss
    else {
      uv_async_init(server->loop(), &wrapper->async, async_callback); // create async handler
      server->send_to_lambda(wrapper); // send request wrapper to server lambda
    }
  } 
  
  // ON ERROR
  else {
    // @TODO - debug error : if (nread != UV_EOF) {}
    // close handle
    println("Read Error!!");
    uv_close((uv_handle_t*) &wrapper->handle, free_handle);
  }

  // free request buffer data
  free(buf->base);
}

// called once a connection is made.
static void http_on_connect(uv_stream_t* handle, int status){
  HttpServer* server = static_cast<HttpServer*>(handle->data);
  HttpData *wrapper = new HttpData();

  // pass pointer to server on wrapper handle
  wrapper->server = server;

  // init tcp handle
  uv_tcp_init(server->loop(), &wrapper->handle);

  // client reference for handle data on requests
  wrapper->handle.data = wrapper;

  // accept connection passing in refernce to the client handle
  uv_accept(handle, (uv_stream_t*) &wrapper->handle);

  // allocate memory and attempt to read.
  uv_read_start((uv_stream_t*) &wrapper->handle, alloc_buffer, http_read);
}


// for inter process communication
namespace ipc {
  static void free_ipc_handle(uv_handle_t* handle);
  static void re_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf);
  static void on_write(uv_write_t* req, int status);
  static void async_write(uv_async_t* handle);
  static void on_read(uv_stream_t* client, ssize_t nread,const uv_buf_t* buf);
  static void on_new_client(uv_stream_t* server, int status);


  typedef struct ipc_call{
    uv_buf_t req;
    uv_buf_t res;
    uv_async_t async_write;
    uv_pipe_t handle;
    void* callback;
    void* server;
    bool writeable;

    ipc_call(){
      req = {.base = NULL, .len = 0};
      res = {.base = NULL, .len = 0};
      writeable = false;
    }

    ~ipc_call(){
      free(req.base);
      free(res.base);
    }

    void send(string str){
      res.base = CharCopy(str.c_str());
      res.len = str.length();
      async_write.data = this;
      writeable = true;
      uv_async_send(&async_write);
    }

    void free_req(){
      free(req.base);
      req.base = NULL;
    }

    void free_res(){
      free(res.base);
      res.base = NULL;
    }

  } ipc_call;


  typedef function<void(ipc_call*)> ipc_callback;
  void sync_write(ipc_call* ipc);


  static void free_ipc_handle(uv_handle_t* handle){
    ipc_call* ipc = static_cast<ipc_call*>(handle->data);
    handle->data = NULL;
    delete ipc;
  }

  static void re_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) {
    ipc_call* ipc = static_cast<ipc_call*>(client->data);
    if (nread > 0) {
      ipc->free_req();
      ipc->req.base = CharCopy(buf->base, nread);
      ipc->req.len = nread;
      ipc_callback* _callback = static_cast<ipc_callback*>(ipc->callback);
      (*_callback)(ipc);
    } else {
      if (nread != UV_EOF)
        fprintf(stderr, "IPC Handle re-Read error %s\n", uv_err_name(nread));
      uv_close((uv_handle_t*)client, free_ipc_handle);
    }
    free(buf->base);
  };

  static void on_write(uv_write_t* req, int status) {
    ipc_call* ipc = reinterpret_cast<ipc_call*>(req->data);
    //printf("on write => status: %d, uv_status: %s\n",status, status == 0 ? "OK" : uv_err_name(status));
    req->data = NULL;
    if (status < 0) {
      fprintf(stderr, "IPC Write error %s\n", uv_err_name(status));
      uv_close((uv_handle_t*) &ipc->handle, free_ipc_handle); // async, so need callback
    }
    else {
      ipc->writeable = false;
      ipc->free_res();
      uv_read_start((uv_stream_t*)&ipc->handle, alloc_buffer, re_read);
    }
    free(req);
  }

  static void async_write(uv_async_t* handle){
    ipc_call* ipc = static_cast<ipc_call*>(handle->data);
    if(ipc->writeable){ // on successful read, write processed data
      uv_write_t *_write = (uv_write_t *) malloc(sizeof(uv_write_t));
      _write->data = ipc;
      uv_write(_write, (uv_stream_t *) &ipc->handle, &ipc->res, 1, on_write);
    }
    else { // on read error close the handle
      free_async_handle(handle);
      uv_close((uv_handle_t*) &ipc->handle, free_ipc_handle); // async, so need callback
      println("IPC close handle on error");
    }
  }

  
  class IpcServer{
    private:
      ipc_callback callback;
      uv_loop_t* loop;
    public:
      IpcServer(ipc_callback _callback){
        callback = _callback;
      }
      ~IpcServer(){}

      uv_loop_t* get_loop(){
        return loop;
      }

      ipc_callback* get_callback(){
        return &callback;
      }
      
      int listen(const char* socket_path){
        loop = uv_loop_new();
        uv_pipe_t server;
        uv_pipe_init(loop, &server, 0);
        uv_status("IPC Server Bind", uv_pipe_bind(&server, socket_path));
        server.data = this;
        uv_status("IPC Server Listen", uv_listen((uv_stream_t*)&server, MAX_WRITES, on_new_client));
        return uv_run(loop, UV_RUN_DEFAULT);
      }
  };

  static void on_new_client(uv_stream_t* server, int status){
    IpcServer* s = (IpcServer*)server->data;
    ipc_call* ipc = new ipc_call();
    uv_pipe_init(s->get_loop(), &ipc->handle, 0);
    ipc->handle.data = ipc;
    ipc->callback = s->get_callback();
    if (uv_accept(server, (uv_stream_t*)&ipc->handle) == 0) {
      uv_read_start((uv_stream_t*)&ipc->handle, alloc_buffer, on_read);
    } else {
      uv_close((uv_handle_t*)&ipc->handle, free_ipc_handle);
    }
  }

  static void on_read(uv_stream_t* client, ssize_t nread,const uv_buf_t* buf){
    if (nread > 0) {
      ipc_call* ipc = static_cast<ipc_call*>(client->data);
      IpcServer* server = static_cast<IpcServer*>(ipc->callback);
      ipc->req.base = CharCopy(buf->base, nread);
      ipc->req.len = nread;
      uv_async_init(server->get_loop(), &ipc->async_write, async_write); // create async handler
      ipc_callback* _callback = static_cast<ipc_callback*>(ipc->callback);
      (*_callback)(ipc);
    } else {
      if (nread != UV_EOF)
        fprintf(stderr, "IPC Server Read error %s\n", uv_err_name(nread));
      uv_close((uv_handle_t*)client, free_ipc_handle);
    }
    free(buf->base);
  }

}