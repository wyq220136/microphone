#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#define NOMINMAX
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
using TickType_t=uint32_t;
using UBaseType_t=unsigned;
using BaseType_t=int;
using TaskHandle_t=void*;
using QueueHandle_t=void*;
using esp_err_t=int;
constexpr int ESP_OK=0, pdTRUE=1, pdPASS=1;
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_INTERNAL=2, MALLOC_CAP_8BIT=4;
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) assert(x)
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
inline int64_t esp_timer_get_time() {
 return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline TickType_t xTaskGetTickCount(){return esp_timer_get_time()/1000;}
inline void vTaskDelay(TickType_t t){Sleep(t);}
inline void delay(unsigned t){vTaskDelay(t);}
inline int esp_task_wdt_status(void*){return ESP_OK;}
inline int esp_task_wdt_add(void*){return ESP_OK;}
inline int esp_task_wdt_reset(){return ESP_OK;}
inline bool esp_ptr_internal(const void*){return true;}
inline void* heap_caps_malloc(size_t n,unsigned){return std::malloc(n);}
inline void* heap_caps_aligned_alloc(size_t,size_t n,unsigned){return std::malloc(n);}
inline void* heap_caps_calloc(size_t n,size_t z,unsigned){return std::calloc(n,z);}
inline void heap_caps_free(void*p){std::free(p);}
inline size_t heap_caps_get_free_size(unsigned){return 1024*1024;}
inline unsigned uxTaskGetStackHighWaterMark(void*){return 4096;}
inline BaseType_t xTaskCreatePinnedToCore(void(*f)(void*),const char*,uint32_t,void*p,unsigned,void**out,int){
 struct Start {void(*fn)(void*);void*arg;};
 auto*start=new Start{f,p};
 HANDLE h=CreateThread(nullptr,0,[](LPVOID raw)->DWORD{auto*s=static_cast<Start*>(raw);auto fn=s->fn;void*arg=s->arg;delete s;fn(arg);return 0;},start,0,nullptr);
 if(!h)return 0;*out=h;CloseHandle(h);return pdPASS;
}
struct FakeQueue {size_t depth,item;std::deque<std::vector<unsigned char>> data;CRITICAL_SECTION m;CONDITION_VARIABLE c;FakeQueue(){InitializeCriticalSection(&m);InitializeConditionVariable(&c);}};
inline QueueHandle_t xQueueCreate(size_t d,size_t s){auto*q=new FakeQueue;q->depth=d;q->item=s;return q;}
inline int xQueueSend(QueueHandle_t h,const void*p,TickType_t wait){
 auto&q=*static_cast<FakeQueue*>(h);EnterCriticalSection(&q.m);
 while(q.data.size()>=q.depth){if(!SleepConditionVariableCS(&q.c,&q.m,wait)){LeaveCriticalSection(&q.m);return 0;}}
 q.data.emplace_back(static_cast<const unsigned char*>(p),static_cast<const unsigned char*>(p)+q.item);WakeAllConditionVariable(&q.c);LeaveCriticalSection(&q.m);return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t h,void*p,TickType_t wait){
 auto&q=*static_cast<FakeQueue*>(h);EnterCriticalSection(&q.m);
 while(q.data.empty()){if(!SleepConditionVariableCS(&q.c,&q.m,wait)){LeaveCriticalSection(&q.m);return 0;}}
 std::memcpy(p,q.data.front().data(),q.item);q.data.pop_front();WakeAllConditionVariable(&q.c);LeaveCriticalSection(&q.m);return pdTRUE;
}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t h){auto&q=*static_cast<FakeQueue*>(h);EnterCriticalSection(&q.m);auto n=q.data.size();LeaveCriticalSection(&q.m);return n;}
inline unsigned uxQueueSpacesAvailable(QueueHandle_t h){auto&q=*static_cast<FakeQueue*>(h);EnterCriticalSection(&q.m);auto n=q.depth-q.data.size();LeaveCriticalSection(&q.m);return n;}
struct FakeSerial {template<class...A>void printf(const char*,A...){} void println(const char*){}};
inline FakeSerial Serial;
