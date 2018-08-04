// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "engine.h"
#include <exception>

using namespace std; 

static void on_pipe_connect(uv_connect_t* connect, int status);
static void on_ipc_write(uv_write_t* req, int status);
static void on_ipc_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf);
static void async_pipe_write(uv_async_t *handle);
static void check_pending_queue (uv_timer_t* timer, int status);

typedef HttpData* balancer_job;

typedef struct job_binder{
  HttpData* data;
  uv_pipe_t* pipe;
  uv_buf_t url;
}job_binder;

typedef struct BalancerWorker{
    uv_loop_t* loop;
    uv_pipe_t pipe;
    const char* socket_path;
    job_binder* current_job;
    uv_async_t async_write;
    mutable std::mutex* guard;

    BalancerWorker(const char* _socket_path){
      current_job = NULL;
      guard = new mutex;
    };
    ~BalancerWorker(){};

    bool process(balancer_job job){
      std::lock_guard<std::mutex> lock(*guard);
      if(current_job != NULL) return false;
      job_binder* binder = new job_binder;
      binder->data = job;
      binder->pipe = &pipe;
      async_write.data = binder;
      current_job = binder;
      uv_async_send(&async_write);
      return true;
    }

    bool isWorking(){
      std::lock_guard<std::mutex> lock(*guard);
      return current_job !=NULL;
    }

    void reset(){
      std::lock_guard<std::mutex> lock(*guard);
      current_job = NULL;
    }

} BalancerWorker;


class Balancer : public Thread{
  private:
    uv_loop_t* UV_LOOP;
    uv_async_t holder;
    uv_timer_t checker;
    vector<const char*>* sockets;
    vector<BalancerWorker*> workers;
    TQueue<balancer_job> pending;
    int worker_count;
    synchronizer* sync;
    RoundRobin robin;
    mutable std::mutex* guard;

  public:
    Balancer(vector<const char*>* _sockets){
      sockets = _sockets;
      sync = new synchronizer();
      guard = new mutex;
      robin.set_limit(_sockets->size());
    };
    ~Balancer(){
      delete sync;
      delete guard;
    };

    // use lock coz it might be called either from the event loop
    // or from the http server thread
    void load_balance(balancer_job job){
      std::lock_guard<std::mutex> lock(*guard);
      // if only single worker, why load balance??
      if(worker_count == 1){
        if(!workers[0]->process(job)){
          pending.push(job);
        }
      }

      // use round robin 'skip-if-busy' algorithm
      else {
        int x;
        bool is_process = false;
        for(int i=0; i<worker_count; i++){
          x = robin.get();
          is_process = workers[x]->process(job);
          if(is_process) break;
        }
        if(!is_process) pending.push(job);
      }

      // int pending_count = pending->count();
      // printf("Pending count : %d\n", pending_count);
    }

    TQueue<balancer_job>* get_pending(){
      return &pending;
    }

    void startup(){
      this->start_detached();
    }

    void wait_startup(){
      sync->wait(10000);
    }

    void run () {
      UV_LOOP = uv_loop_new();

      // UNIX-SOCKET process connection
      for(auto socket_path : *sockets){
        BalancerWorker* worker = new BalancerWorker(socket_path);
        worker->loop = UV_LOOP;
        uv_status("Pipe Initialization", uv_pipe_init(UV_LOOP, &worker->pipe, 0));
        uv_connect_t connect;
        worker->pipe.data = worker;
        uv_status("Pipe Open", uv_pipe_open(&worker->pipe, socket(PF_UNIX, SOCK_STREAM, 0)));
        connect.data = (void*)socket_path;
        uv_pipe_connect(&connect, &worker->pipe, socket_path, on_pipe_connect);
        uv_async_init(UV_LOOP, &worker->async_write, async_pipe_write);
        workers.insert(workers.end(), worker);
        worker_count++;
      }
      // Initialize timer to check pending queue
      uv_timer_init(UV_LOOP, &checker);
      checker.data = this;
      uv_timer_start(&checker, (uv_timer_cb) check_pending_queue, 4000, 250);
      // init loop
      sync->notify_all();
      println("Balancer Started");
      uv_run(UV_LOOP, UV_RUN_DEFAULT);
    }
};

// timer to check pending queue that is left when renderer process is busy
static void check_pending_queue (uv_timer_t* timer, int status) {
  Balancer* bal = static_cast<Balancer*>(timer->data);
  TQueue<balancer_job>* pending = bal->get_pending();
  int count = pending->count();
  if(count == 0) return; // return if no pending
  for(int i=0; i<count; i++){
    bal->load_balance(pending->take());
  }
}

// called upon new unix socket connection
static void on_pipe_connect(uv_connect_t* connect, int status){
  printf("Connected to %s\n", (const char*)connect->data);
  uv_status("CONNECT", status);
}

static void async_pipe_write(uv_async_t *handle){
  job_binder* binder = (job_binder*)(handle->data);
  binder->url = {
    .base = CharCopy(binder->data->request_url.c_str()), 
    .len = binder->data->request_url.length()
  };
  uv_write_t *_write = (uv_write_t *) malloc(sizeof(uv_write_t));
  _write->data = binder;
  uv_write(_write, (uv_stream_t *) binder->pipe, &binder->url, 1, on_ipc_write);
}

static void on_ipc_write(uv_write_t* req, int status){
  job_binder* binder = (job_binder*)(req->data);
  if(binder==NULL) println("BINDER IS NULL HERE");
  // no free here, it will be done after reading data from child process
  if (status < 0) {
    fprintf(stderr, "IPC Write error %s\n", uv_err_name(status));
    uv_close((uv_handle_t*) binder->pipe, NULL);
  }
  else {
    uv_read_start((uv_stream_t*)binder->pipe, alloc_buffer, on_ipc_read);
  }
  req->data = NULL;
  free(req);
}

static void on_ipc_read(uv_stream_t* pipe, ssize_t nread, const uv_buf_t* buf) {
  if (nread > 0) {
    BalancerWorker* w = (BalancerWorker*) pipe->data;
    job_binder* binder = w->current_job;
    if(binder == NULL) println("NO BINDER!!");
    HttpData* data = binder->data;
    data->sendResponse(buf->base);
    free(binder->url.base);
    delete binder;
    w->reset();
  } else {
    if (nread != UV_EOF)
      fprintf(stderr, "IPC Handle re-Read error %s\n", uv_err_name(nread));
    uv_close((uv_handle_t*)pipe, NULL);
  }
  free(buf->base);
};

static void http_server_test_case(){
  HttpServer server([](HttpData* req){
    req->setResponseStatus(200);
    req->setResponseHeader("Connection", "keep-alive");
    req->setResponseHeader("Transfer-Encoding", "chunked");
    req->setResponseHeader("Content-Type", "text/html");
    req->sendResponse(req->request_url);
  });
  server.listen("0.0.0.0",8000);  
}


// main function
int main(int argc, char* argv[]) {
  vector<const char*>* sockets = new vector<const char*>;
  int pid;
  
  for(int i=0; i<num_process; i++){
    char* socket_path = str_format("/tmp/v8_process%d.sock", i);
    unlink(socket_path);
    pid = fork();
    if(pid == 0){ // forked process
      engineProcess(argv[0], socket_path);
    } else { // main process
      sockets->insert(sockets->end(), socket_path);
    }
  } 

  if(pid!=0){
    println("starting http server");
    sleep(4);
    static Balancer bal = Balancer(sockets);
    bal.startup();
    bal.wait_startup();

    HttpServer server([](HttpData* req){
      req->setResponseStatus(200);
      req->setResponseHeader("Connection", "keep-alive");
      req->setResponseHeader("Transfer-Encoding", "chunked");

      if(req->request_url == "/favicon.ico"){
        req->setResponseHeader("Content-Type", "image/vnd.microsoft.icon");
        req->sendResponse(" ");
      }else{
        req->setResponseHeader("Content-Type", "text/html");
        bal.load_balance(req);
      }
      
    });
    server.cache_url.add("/page1","/page2","/itemgrid");
    server.listen("0.0.0.0", 8000);
  }
  
  return 0;
}