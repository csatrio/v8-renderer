// pragma once is a non-standard but widely supported preprocessor directive,
// designed to cause the current source file to be included only once in a single compilation
#pragma once 
#include "common_functions.h"

// Blocking Queue with locks
template <typename T>
class LockingQueue : public std::queue<T> {
 public:
  LockingQueue(){guard = new mutex;}
  virtual ~LockingQueue(){delete guard;}

  void push(T const& _data) {
    {
      std::lock_guard<std::mutex> lock(*guard);
      queue<T>::push(_data);
    }
    signal.notify_all();
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(*guard);
    return queue<T>::empty();
  }

  bool tryPop(T& _value) {
    std::lock_guard<std::mutex> lock(*guard);
    if (queue<T>::empty()) {
      return false;
    }

    _value = this->front();
    this->pop();
    return true;
  }

  void waitAndPop(T& _value) {
    std::unique_lock<std::mutex> lock(*guard);
    while (queue<T>::empty()) {
      signal.wait(lock);
    }

    _value = this->front();
    this->pop();
  }

  bool tryWaitAndPop(T& _value, int _milli) {
    std::unique_lock<std::mutex> lock(*guard);
    while (queue<T>::empty()) {
      signal.wait_for(lock, std::chrono::milliseconds(_milli));
      return false;
    }

    _value = this->front();
    this->pop();
    return true;
  }

 private:
  mutable std::mutex* guard;
  std::condition_variable signal;
};

// Synchronized Queue with locks
template<typename T> 
class TQueue : public std::queue<T>{
public:
  TQueue(){guard = new mutex;}
  virtual ~TQueue(){delete guard;}

  void push(T const& _data){
    std::lock_guard<std::mutex> lock(*guard);
    queue<T>::push(_data);
    _count++;
  }

  bool empty() const{
    std::lock_guard<std::mutex> lock(*guard);
    return queue<T>::empty();
  }

  int count(){
    std::lock_guard<std::mutex> lock(*guard);
    return _count;
  }

  T& take(){
    std::lock_guard<std::mutex> lock(*guard);
    T& _value = this->front();
    this->pop();
    _count --;
    return _value;
  }

private:
  mutable std::mutex* guard;
  int _count;
};


// round robin counter
class RoundRobin{
  private:
    int robin;
    int limit;
    int tmp;
  
  public:
  RoundRobin(int _limit){
    robin = 0;
    limit = _limit;
  }
  RoundRobin(){robin = 0;};
  virtual ~RoundRobin(){};

  void set_limit(int _limit){
    limit = _limit;
  }

  int get(){
    if(robin == limit) robin = 0;
    tmp = robin;
    robin ++;
    return tmp;
  }
};


// Output Printer With Function Callback
class OutputPrinter {
  private:
    bool first;
    const char* name;
    function<void(const char*)>* callback;
  
  public:
    OutputPrinter(const char* _name){
      name = _name;
      first = true;
      callback = NULL;
    };

    OutputPrinter(){
      OutputPrinter("default");
    };

    ~OutputPrinter(){};

    void setCallback(function<void(const char*)>* _callback){
      this->callback = _callback;
    }

    void Print(const FunctionCallbackInfo<Value>& args) {
      //if(callback == NULL) {cout<<"Callback not set !"<<endl;return;}
      Isolate* isolate = args.GetIsolate();
      HandleScope handle_scope(isolate);

      first = true;
      int length = args.Length();
      for (int i = 0; i < length; i++) {
        if (first) {
          first = false;
        } else {
          (*callback)(" ");
        }
        String::Utf8Value str(isolate, args[i]);
        const char* src = ToCString(str);
        (*callback)(src);
      }
      (*callback)("\n");
    }

};


// synchronizer for wait and notify
typedef struct _synchronizer {
  mutable std::mutex guard;
  std::condition_variable signal;

  _synchronizer(){};
  ~_synchronizer(){};

  void wait(){
    std::unique_lock<std::mutex> lock(guard);
    signal.wait(lock);
  }

  void wait(int _milli){
    std::unique_lock<std::mutex> lock(guard);
    signal.wait_for(lock, std::chrono::milliseconds(_milli));
  }

  void notify(){
    std::lock_guard<std::mutex> lock(guard);
    signal.notify_one();
  }

  void notify_all(){
    std::lock_guard<std::mutex> lock(guard);
    signal.notify_all();
  }

} synchronizer;


// Atomic Long Wrapper
struct AtomicLong{
  atomic_long x;
  AtomicLong(long _millis){
    x.store(_millis);
  }
  AtomicLong(){};
  ~AtomicLong(){};
  void set(long l){x.store(l);}
  void set_millis(){x.store(millis());}
  long get(){return x.load();}
};

typedef struct AtomicInt{
  atomic_int x;
  AtomicInt(int _init){
    x.store(_init);
  }
  AtomicInt(){};
  ~AtomicInt(){};
  void set(int l){x.store(l);}
  void increment(){x.store(x.load() + 1);}
  void decrement(){x.store(x.load() - 1);}
  int get(){return x.load();}
  int load(){return x.load();}
}AtomicInt;


// Append char to buffer fast with malloc + memcpy
typedef struct stringbuffer{
  char* buf;
  int len;
  int length;
  int max_length;

  stringbuffer(int size){
    buf = (char*)malloc(size);
    length = 0;
    len = 0;
    max_length = size;
  }

  ~stringbuffer(){
    free(buf);
  }

  void add(const char* c){
    len = strlen(c);
    memcpy(buf + length, c, len);
    length += len;
  }

  stringbuffer* adds(const char* c){
    add(c);
    return this;
  }

  void reset(){
    len = 0;
    length = 0;
  }

  char* str(){
    buf[length] = '\0';
    return buf;
  }

  char* str_cpy(){
    char* tmp = (char*)malloc(length + 1);
    memcpy(tmp, buf, length);
    tmp[length] = '\0';
    return tmp;
  }

} stringbuffer;


// Store global variable here to ease creation of new thread
typedef struct __v8_globals{
  shared_ptr<Platform> platform;
  string script_template;
  string css;
  char* startup_location[];
} _v8_globals;


// Cache Entry Struct
typedef struct cache_entry_t {
  long start;
  long timeout;
  string data;

  cache_entry_t(const string& _data, long _timeout){
    start = millis(); 
    timeout = _timeout;
    data = _data;
  };

  ~cache_entry_t(){
    println("Destroyed cache entry");
  };

  bool isExpired(){
    return (millis() - start) > timeout;
  }

  bool isExpired(int _timeout){
    return (millis() - start) > _timeout;
  }
  
} cache_entry_t;


typedef unique_ptr<cache_entry_t> cache_entry;
typedef map<const string, cache_entry> cache_storage;
typedef map<const string, cache_entry>::iterator cache_iterator;
typedef pair<const string, cache_entry> cache_pair;
typedef struct cache_map {
  cache_storage* _map;

  cache_map(){
    _map = new cache_storage();
  }

  ~cache_map(){
    _map->clear();
    delete _map;
  }

  string add(const string &key, const string &value, const long timeout){
    _map->insert(cache_pair(
        key,
        make_unique<cache_entry_t>(value, timeout)
        ));
    return value;
  }

  int count(const string &key){
    return _map->count(key);
  }

  string* get(const string &key){
    cache_iterator it = _map->find(key);
    string* data;
    // if found
    if (it!=_map->end()){
      cache_entry &entry = it->second;
      if(entry->isExpired()){
        _map->erase(key);
        return NULL;
      } 
      else {
        return &entry->data;
      } 
    } 
    // if not found
    return NULL;
  }

} cache_map;


// struct to check url that can be cached
// this one is lockless because content is unmodifiable
typedef pair<const string, const bool>  url_pair;
typedef struct cacheable{
  map<const string, const bool>* _map;

  cacheable(){
    _map = new map<const string, const bool>();
  };

  ~cacheable(){
    _map->clear();
    delete _map;
  };

  void add(const char* arg, ...) {
    va_list arguments;

    for (va_start(arguments, arg); arg != NULL; arg = va_arg(arguments, const char *)) {
        //printf("%s:%d\n",arg, strlen(arg));
        _map->insert({arg, true});
    }

    va_end(arguments);
  }

  bool is_cache(const string &key){
    return ( !(_map->find(key) == _map->end()) )? true : false;
  }

} cacheable;

