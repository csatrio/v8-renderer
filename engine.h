// pragma once is a non-standard but widely supported preprocessor directive,
// designed to cause the current source file to be included only once in a single compilation
#pragma once 
#include "httpclient.h"
//#include "icon.h"

using namespace std; 
using namespace v8;

typedef std::function<void(const v8::FunctionCallbackInfo<v8::Value>&)> v8_callback_ptr;


// Prototypes
static void ReportException(Isolate* isolate, TryCatch* try_catch);
//static void TerminateExecution(v8::Isolate *isolate);
static inline bool ExecuteString(Isolate* isolate, Local<String> source,
                   Local<String> _name,
                   bool report_exceptions);
static Local<v8::Context> CreateContext(
    Isolate* isolate,
    function<void(const FunctionCallbackInfo<Value>&)> methods[]);
static inline void SetTimeout(const FunctionCallbackInfo<Value> &info);
static inline size_t WriteCallback(char *contents, size_t size, size_t nmemb, void *userp);
static void HttpGet(const FunctionCallbackInfo<Value>& info);
static inline void js_callback(const FunctionCallbackInfo<Value>& info);
static std::string LoadScript();
static void engineProcess(const char* startup_location, const char* socket_addr);
int startEngine(char* argv[]);
// End Prototypes


// Modify job and job queue type here
typedef HttpData* job_type;
typedef LockingQueue<job_type> jobqueue_type;
typedef LockingQueue<job_type>* jobqueue_pointer;


// V8 Engine Process
static void engineProcess(const char* startup_location, const char* socket_addr){
  printf("Startup Location Argument: %s\n", startup_location);
  static const char* process_name = str_format("V8 Process: %s", socket_addr);

  // Initialize V8.
  curl_global_init(CURL_GLOBAL_ALL);
  V8::InitializeICUDefaultLocation(startup_location);
  V8::InitializeExternalStartupData(startup_location);
  std::unique_ptr<v8::Platform> _platform = platform::NewDefaultPlatform(num_v8_internal_threads);
  V8::InitializePlatform(_platform.get());
  V8::Initialize();
  
  Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = ArrayBuffer::Allocator::NewDefaultAllocator();
  static v8::Platform* platform = _platform.get();
  static Isolate* isolate = Isolate::New(create_params);

  static string script_template = LoadScript();
  static string css = string(ReadFile("/home/csatrio/Desktop/css.config"));

  static OutputPrinter renderer = OutputPrinter(str_format("RENDERER::%s", process_name));
  static OutputPrinter logger = OutputPrinter(str_format("LOGGER::%s", process_name));
  static stringbuffer render_buffer(1024*1024); // 1MB render buffer
  static stringbuffer script_buffer(100*1024); // 100KB script buffer


  function<void(const char*)> loggerCb = [](const char* data){
    //cout<<data;
  };

  function<void(const char*)> renderCb = [](const char* data){
    render_buffer.add(data);
  };

  logger.setCallback(&loggerCb);
  renderer.setCallback(&renderCb);

  // Bind callback from static proxy to our class instance
  function<void(const FunctionCallbackInfo<Value>&)> methods[]{
    bind(&OutputPrinter::Print, renderer, std::placeholders::_1),
    bind(&OutputPrinter::Print, logger, std::placeholders::_1)
  };

  // Isolate Block Scope Function.
  {  
    Isolate::Scope isolate_scope(isolate);

    // Create a stack-allocated handle scope.
    HandleScope handle_scope(isolate);

    // Create a new context.
    Local<v8::Context> context = CreateContext(isolate, methods);
    
    // Enter the context for compiling and running the hello world script.
    v8::Context::Scope context_scope(context);

    // Initialize startup of javascript
    static Local<String> threadName = CreateString(isolate, process_name);
    ExecuteString(isolate, CreateString(isolate, script_template), threadName, true);
    while (v8::platform::PumpMessageLoop(platform, isolate)) continue;

    static auto render = [](const char* url)->char*{
      render_buffer.reset();
      script_buffer.reset();
      render_buffer.add("<html><head></head><body>");

      script_buffer.adds("currentRoute = '")->adds(url)->adds("';");
      script_buffer.add("renderVueComponentToString(server.createApp(), (err, res) => {print(res);});");

      ExecuteString(isolate, CreateString(isolate, script_buffer.str()), threadName, true);
      while (v8::platform::PumpMessageLoop(platform, isolate)) continue;

      render_buffer.adds("</body>")->adds(css.c_str())->add("</html>");
      return render_buffer.str();
    };

    // Start Worker Thread Execution Loop
    printf("Starting IPC Server %s\n", socket_addr);
    unlink(socket_addr); // unlink first to avoid name collision
    ipc::IpcServer ipc_server([](ipc::ipc_call* ipc){
      ipc->send(render(ipc->req.base));
    });
    // End Worker Thread Execution Loop
    ipc_server.listen(socket_addr);
    
  }// End Isolate Block Scope Function
  isolate->Dispose();
  curl_global_cleanup();
  V8::Dispose();
  V8::ShutdownPlatform();
}


// Report exception that caught during execution
static void ReportException(Isolate* isolate, TryCatch* try_catch) {
  HandleScope handle_scope(isolate);
  String::Utf8Value exception(isolate, try_catch->Exception());
  const char* exception_string = ToCString(exception);
  Local<Message> message = try_catch->Message();
  if (message.IsEmpty()) {
    // V8 didn't provide any extra information about this error; just
    // print the exception.
    fprintf(stderr, "%s\n", exception_string);
  } else {
    // Print (filename):(line number): (message).
    String::Utf8Value filename(isolate,message->GetScriptOrigin().ResourceName());
    Local<v8::Context> context(isolate->GetCurrentContext());
    const char* filename_string = ToCString(filename);
    int linenum = message->GetLineNumber(context).FromJust();
    fprintf(stderr, "%s:%i: %s\n", filename_string, linenum, exception_string);
    // Print line of source code.
    String::Utf8Value sourceline(isolate, message->GetSourceLine(context).ToLocalChecked());
    const char* sourceline_string = ToCString(sourceline);
    fprintf(stderr, "%s\n", sourceline_string);
    // Print wavy underline (GetUnderline is deprecated).
    int start = message->GetStartColumn(context).FromJust();
    for (int i = 0; i < start; i++) {
      fprintf(stderr, " ");
    }
    int end = message->GetEndColumn(context).FromJust();
    for (int i = start; i < end; i++) {
      fprintf(stderr, "^");
    }
    fprintf(stderr, "\n");
    Local<Value> stack_trace_string;
    if (try_catch->StackTrace(context).ToLocal(&stack_trace_string) &&
        stack_trace_string->IsString() &&
        Local<String>::Cast(stack_trace_string)->Length() > 0) {
      String::Utf8Value stack_trace(isolate, stack_trace_string);
      const char* stack_trace_string = ToCString(stack_trace);
      fprintf(stderr, "%s\n", stack_trace_string);
    }
  }
}


// Executes a string within the current v8 context.
static inline bool ExecuteString(Isolate* isolate, Local<String> source,
                   Local<String> name,
                   bool report_exceptions) {
  HandleScope handle_scope(isolate);
  TryCatch try_catch(isolate);
  ScriptOrigin origin(name);
  Local<v8::Context> context(isolate->GetCurrentContext());
  Local<Script> script;
  if (!Script::Compile(context, source, &origin).ToLocal(&script)) {
    // Print errors that happened during compilation.
    if (report_exceptions)
      ReportException(isolate, &try_catch);
    return false;
  } else {
    Local<Value> result;
    if (!script->Run(context).ToLocal(&result)) {
      assert(try_catch.HasCaught());
      // Print errors that happened during execution.
      if (report_exceptions)
        ReportException(isolate, &try_catch);
      return false;
    } else {
      return true;
    }
  }
}


// Terminate Javascript Execution on an isolate
// static void TerminateExecution(v8::Isolate *isolate)
// {
// 	if(isolate->IsExecutionTerminating()) {
// 		/* Execution already terminating, needn't trigger it again and
// 		 * especially must not execute the spinning loop (which would cause
// 		 * crashes in V8 itself, at least with 4.2 and 4.3 version lines). */
// 		return;
// 	}

// 	/* Unfortunately just calling TerminateExecution on the isolate is not
// 	 * enough, since v8 just marks the thread as "to be aborted" and doesn't
// 	 * immediately do so.  Hence we enter an endless loop after signalling
// 	 * termination, so we definitely don't execute JS code after the exit()
// 	 * statement. */
// 	/* Don't forget the locker if you use 1 isolate for multiple threads
//      * v8::Locker locker(isolate);
// 	 * v8::Isolate::Scope isolate_scope(isolate);*/
// 	v8::HandleScope handle_scope(isolate);
// 	v8::Local<v8::String> source = CreateString(isolate, "for(;;);");
// 	v8::Local<v8::Script> script = v8::Script::Compile(source);
// 	isolate->TerminateExecution();
// 	script->Run();
// }


// Javascript Set Timeout
static inline void SetTimeout(const FunctionCallbackInfo<Value> &info) {
  HandleScope scope(info.GetIsolate());  // To prevent memory leak, use handlescope
  Handle<Function> function = Handle<Function>::Cast(info[0]);
  Local<Value> rcv = info.This();
  Local<Value> args[] = {};
  function->Call(rcv, 0, args);
}


// Write Callback from Curl
static inline size_t WriteCallback(char *contents, size_t size, size_t nmemb, void *userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}


// Http Get using Curl
static void HttpGet(const FunctionCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  HandleScope scope(isolate);  // To prevent memory leak, use handlescope
  
  CURL* easyhandle = curl_easy_init();
  std::string readBuffer;
  String::Utf8Value str(isolate, info[0]);
  const char* url = ToCString(str);
  curl_easy_setopt(easyhandle, CURLOPT_URL, url);
  curl_easy_setopt(easyhandle, CURLOPT_VERBOSE, 0L); //1 on, 0 off
  curl_easy_setopt(easyhandle, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(easyhandle, CURLOPT_WRITEDATA, &readBuffer);
  curl_easy_perform(easyhandle);

  info.GetReturnValue().Set(CreateString(isolate, readBuffer));
  curl_easy_cleanup(easyhandle); // cleanup curl stuff
}


// Static proxy callback for non static methods, unpack function pointer callback then call the corresponding function
static inline void js_callback(const FunctionCallbackInfo<Value>& info){
  function<void(const FunctionCallbackInfo<Value>&)>* method_ptr = 
    static_cast<function<void(const FunctionCallbackInfo<Value>&)>*> (External::Cast(*info.Data())->Value());
  (*method_ptr)(info);
}


// Creates a new execution environment containing the built-in functions.
static Local<v8::Context> CreateContext(Isolate* isolate, function<void(const FunctionCallbackInfo<Value>&)> methods[]) {
  // Create a template for the global object.
  Local<ObjectTemplate> global = ObjectTemplate::New(isolate);
  
  // Bind the global 'print' function to the C++ Print callback.
  //cout<<"adress of print pointer : "<<&methods[0]<<endl;
  Local<FunctionTemplate> print_template = 
    FunctionTemplate::New(isolate, js_callback, External::New((isolate), &methods[0]));
  global->Set(CreateString(isolate, "print"), print_template);

  // Bind the global 'Log' function to the C++ Log callback.
  //cout<<"adress of logger pointer : "<<&methods[1]<<endl;
  Local<FunctionTemplate> logger_template = 
    FunctionTemplate::New(isolate, js_callback, External::New((isolate), &methods[1]));
  global->Set(CreateString(isolate, "Log"), logger_template);

  // Bind the global 'alert' function to the C++ Log callback.
  global->Set(CreateString(isolate, "alert"), logger_template);

  // Bind the global 'setTimeout' function to the C++ SetTimeout callback.
  global->Set(CreateString(isolate, "setTimeout"),FunctionTemplate::New(isolate, SetTimeout));

  // Bind the global 'httpGet' function to the C++ HttpGet callback.
  global->Set(CreateString(isolate, "httpGet"),FunctionTemplate::New(isolate, HttpGet));
  //cout<<"returning from context create"<<endl;
  return v8::Context::New(isolate, NULL, global);
}


// Initialize bulk script here
static std::string LoadScript(){
  std::string buf;
  const char* initVar =
      "var process = { env: { VUE_ENV:'server', NODE_ENV:'production' }}; "
      "this.global = { process: process };"
      "var webpackJsonp_name_ = null;";

  buf.append(initVar);
  buf.append(ReadFile("/var/www/html/assets/webpack/manifest.js"));
  buf = regex_replace(buf, regex("\\window.webpackJsonp_name_"), "webpackJsonp_name_");
  buf.append(";");
  buf.append(ReadFile("/var/www/html/assets/webpack/vendor.js"));
  buf.append(";");
  buf.append(ReadFile("/var/www/html/assets/webpack/promise_polyfill.js"));
  buf.append(";");
  buf.append(ReadFile("/var/www/html/assets/webpack/basic.min.js"));
  buf.append(";");

  buf.append(";var export_server = function(){");
  buf.append(ReadFile("/var/www/html/assets/webpack/server.js"));
  buf.append("; return server;};");
  buf.append("const console = {log: Log, err:Log};");
  buf.append("export_renderer(); var server = export_server();");
  buf.append("var currentRoute = '/';export_renderer(); var server = export_server();");

  std::replace(buf.begin(), buf.end(), '\n',' ');  // remove any linebreaks from script
  return buf;
}