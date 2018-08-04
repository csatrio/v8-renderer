// pragma once is a non-standard but widely supported preprocessor directive,
// designed to cause the current source file to be included only once in a single compilation
#pragma once 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>

#include <iostream>
#include <algorithm>
#include <cstring>
#include <regex>
#include <curl/curl.h>

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <signal.h>

#include <cstdlib>
#include <cstdarg>
#include <pthread.h>

// For Execution Info and Error Handling
#include <execinfo.h>
#include <errno.h>
#include <unistd.h>

// This one is for syscall
#include <sys/types.h>
#include <sys/syscall.h>

// This one is for parameter
#include "parameters.h"

#define NAME_LENGTH 255
#define FORMAT_BUFFER 1024
#define print_thread_name(msg){printf("%s%ld\n",msg,pthread_self());}

using namespace std;

// prototype
class BaseThread;
class Thread;
static void* execute_thread(void* p);
// static void signal_handler(int sig, siginfo_t *info, void* _context);
// static void main_signal_mask();
// static void install_signal_handler();
// static void set_signal_mask();
// end protoype


// format output char with variable args
static char* f_buf = (char*)malloc(FORMAT_BUFFER);
static char* str_format(char* format, ...){
  va_list args;
  va_start(args, format);
  int len = vsnprintf(f_buf, FORMAT_BUFFER, format, args);
  char* ret = (char*)malloc(len + 1);
  memcpy(ret, f_buf, len);
  ret[len] = '\0';
  va_end(args);
  return ret;
}

class BaseThread{
  public:
    BaseThread(){
      static int __thread_number = 0;
      int number = (__thread_number++);
      this->name = str_format("Thread-%d", number);
      this->_thread_number = number;
      this->is_started = false;
      this->is_cancel = false;
      this->is_detach = false;
      this->is_join = false;
      this->_cancel_state = true;
      pthread_attr_init(&this->attr);
    };

    int threadNumber(){
      return this->_thread_number;
    }

    const char* getName(){
      return this->name;
    }

    ~BaseThread(){};
    
  protected:
    const char* name;
    int _thread_number;
    bool is_started;
    bool is_cancel;
    bool is_detach;
    bool is_join;
    bool _cancel_state;
    pthread_attr_t attr;
};


class Thread : public BaseThread{
  public:
    Thread(function<void()> _lambda_run){
      this->lambda_run = _lambda_run;
    }

    Thread(){};
    ~Thread(){};

    void setName(const char* _name){
      this->name = _name;
      if(this->is_started) pthread_setname_np(this->handle, this->name);
    }

    void set_stack_size_kb(size_t size){
      pthread_attr_setstacksize(&this->attr, 1024*size);
    }

    int start(){
      if(this->is_started||this->is_cancel) return -1;
      int status =  pthread_create(&this->handle, &this->attr, execute_thread, (void*)this);
      status = pthread_setname_np(this->handle, this->name);
      this->is_started = true;
      return status;
    }
    
    int start_detached(){
      int status =  this->start();
      if(status == -1) return -1;
      this->is_detach = true;
      return pthread_detach(this->handle);
    }

    pthread_t getHandle(){
      return this->handle;
    }

    int join(){
      if(!this->is_started||this->is_join) return -1;
      this->is_join = true;
      this->is_detach = false;
      return pthread_join(this->handle, NULL);
    }

    int detach(){
      if(!this->is_started||this->is_detach) return -1;
      this->is_detach = true;
      this->is_join = false;
      return pthread_detach(this->handle);
    }

    int cancel(){
      if(!this->is_started||this->is_cancel) return -1;
      this->is_cancel = true;
      return pthread_cancel(this->handle);
    }

    void enable_cancel(){
      this->_cancel_state = true;
      pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }

    void disable_cancel(){
      this->_cancel_state = false;
      pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    }

    bool cancellable(){
      return this->_cancel_state;
    }

    int signal(int signal){
      if(!this->is_started) return -1;
      return pthread_kill(this->handle, signal);
    }

    virtual void run(){
      if(this->lambda_run == NULL){
        printf("Run function for thread : %s is not set !\n", this->getName());
      }
      lambda_run();
    }

    private:
      pthread_t handle;
      function<void()> lambda_run = NULL;
};


// //set signal mask for main thread
// static void main_signal_mask(){
//   sigset_t block;
//   /* Block SIGQUIT, SIGUSR1, SIGUSR2; other threads created by main()
//       will inherit a copy of the signal mask. */
//   sigemptyset(&block);
//   sigaddset(&block, SIGQUIT);
//   sigaddset(&block, SIGUSR1);
//   sigaddset(&block, SIGUSR2);
//   pthread_sigmask(SIG_BLOCK, &block, NULL);
// }


// // set signal mask on all thread
// static void set_signal_mask(){
//   sigset_t unblock;
//   sigemptyset(&unblock);
//   /* Unblock all signals on child thread, some signals are blocked on main thread only*/
//   for(int i=1; i<=31; i++){
//     sigaddset(&unblock, i);
//   }
//   pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
// }


// // part of threading system signal trampoline
// // signal handler function that will be entered by thread that received signal
// static void signal_handler(int sig, siginfo_t *info, void* _context) {
//   pthread_t self = pthread_self();
//   //ucontext_t* context = static_cast<ucontext_t*>(_context);
//   char* thread_name = new char[NAME_LENGTH];
//   int length = pthread_getname_np(self, thread_name, NAME_LENGTH);
//   thread_name[length] = '\0';
//   printf("Inside signal handler for %s, receiving signal : %d\n", thread_name, sig);
// }


// static void install_signal_handler(){
//   struct sigaction actions;
//   actions.sa_flags = 0;
//   sigemptyset (&actions.sa_mask);
//   memset(&actions,0,sizeof(actions));
//   actions.sa_sigaction = signal_handler;
//   for(int i=1; i<=31; i++){
//     switch(i){
//       case SIGSEGV: break;
//       case SIGHUP: break;
//       case SIGABRT: break;
//       case SIGQUIT: break;
//       case SIGINT: break;
//       case SIGKILL: break;
//       default : sigaction(i, &actions, NULL); break;
//     }
//   }
//   sigaction(_NSIG, &actions, NULL);
//   sigaction(SIGRTMIN, &actions, NULL);
//   sigaction(SIGRTMAX, &actions, NULL);
// }


// execute thread when it's start method is invoked
static void* execute_thread(void* p){
  //set_signal_mask();
  //install_signal_handler();
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  Thread* t = reinterpret_cast<Thread*>(p);
  t->run();
}