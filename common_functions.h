// pragma once is a non-standard but widely supported preprocessor directive,
// designed to cause the current source file to be included only once in a single compilation
#pragma once 
#include "threads.h"

// V8 Includes
#include "include/libplatform/libplatform.h"
#include "include/v8.h"
#include "uv.h"
#include "http_parser.h"

using namespace std;
using namespace v8;


#define ASSERT_STATUS(status, msg) \
  if (status != 0) { \
    std::cerr << msg << ": " << uv_err_name(status); \
    /*exit(1);*/ \
  }
// Simple error handling functions
#define handle_error_en(en, msg) \
        do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)
// Simple print macro
#define println(msg) printf("%s\n", msg)
//#define array_length(arr)({int ret = sizeof(arr)/sizeof(*arr); ret;})
#define uv_status(msg, status){ASSERT_STATUS(status,msg);}

static const string CRLF = "\r\n";
static const string empty_string = string("");
static const char* read_error = "Read Error";
static const int char_size = sizeof(char);

// Return current time since epoch in milliseconds
static inline long millis(){
  return std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::system_clock::now().time_since_epoch()).count();
}

// Copy character array
static inline char* CharCopy(const char* x, int len){
  char* buf = (char*)malloc(len);
  memcpy(buf, x, len);
  buf[len] = '\0';
  return buf;
}

static inline char* CharCopy(const char* x, int* length){
  return CharCopy(x, (*length = strlen(x)));
}

static inline char* CharCopy(const char* x){
  return CharCopy(x, strlen(x));
}

// Extracts a C string from a V8 Utf8Value.
static inline const char* ToCString(const String::Utf8Value &value) {
  return *value ? *value : "<string conversion failed>";
}

static inline Local<String> CreateString(Isolate* isolate, const char* chr) {
  return String::NewFromUtf8(isolate, chr, NewStringType::kNormal).ToLocalChecked();
}

static inline Local<String> CreateString(Isolate* isolate, std::string str) {
  return String::NewFromUtf8(isolate, str.c_str(), NewStringType::kNormal).ToLocalChecked();
}

// Reads a file into a char.
static char* ReadFile(const char* name) {
  FILE* file = fopen(name, "rb");
  if (file == NULL) return new char[0];

  fseek(file, 0, SEEK_END);
  size_t size = ftell(file);
  rewind(file);

  char* chars = new char[size + 1];
  chars[size] = '\0';
  for (size_t i = 0; i < size;) {
    i += fread(&chars[i], 1, size - i, file);
    if (ferror(file)) {
      fclose(file);
      return chars;
    }
  }
  fclose(file);
  return chars;
}




