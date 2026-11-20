// Run the production pool/session/task code against timed host queues/threads.
#include "audio_pipeline.h"
#include "gtcrn_engine.h"
#include <iostream>
#include <string>

std::atomic<unsigned> inferenceDelay{0};
std::atomic<bool> failFirst{false}, processStarted{false};
bool available = true, realtime = true;
int32_t previous[256];
size_t previousSize = 0;

I2SSampler::~I2SSampler() = default;
bool I2SSampler::begin() { ready_ = true; return true; }
void I2SSampler::end() { ready_ = false; }
bool I2SSampler::readSamplesBounded(int32_t *dst,size_t count,size_t capacity) {
  assert(ready_ && count <= capacity);
  vTaskDelay(16);
  std::fill_n(dst, count, 1000000 * 256);
  return true;
}
GtcrnEngine::~GtcrnEngine() = default;
bool GtcrnEngine::begin() { return available; }
bool GtcrnEngine::benchmark() { return realtime; }
void GtcrnEngine::reset() { previousSize = 0; }
bool GtcrnEngine::process(const int32_t *in,size_t n,int32_t *out,size_t &produced) {
  const bool fail = failFirst.exchange(false);
  processStarted = true;
  vTaskDelay(inferenceDelay.load());
  if (fail) return false;
  produced = previousSize;
  std::copy_n(previous, produced, out);
  if (n) std::copy_n(in,n,previous);
  previousSize=n;
  return true;
}
bool GtcrnEngine::transform(float*,bool){return true;}
bool GtcrnEngine::infer(const float*,float*){return true;}
void GtcrnEngine::resetModel(){reset();}

struct Result {size_t samples=0,raw=0,enhanced=0;};
Result record(AudioPipeline &pipeline,uint32_t session,unsigned blocks,bool stop=true) {
  assert(pipeline.start(session));
  AudioProcessor reference;
  reference.reset();
  Result result;
  bool requested=false;
  const auto deadline=esp_timer_get_time()+5000000;
  while(esp_timer_get_time()<deadline) {
    AudioPipeline::Output out;
    while(pipeline.poll(out)) {
      assert(out.session==session);
      assert(out.firstSample==result.samples);
      int32_t input[256], expected[256];
      std::fill_n(input,256,1000000*256);
      reference.convertToPcm24(input,256,expected,256);
      assert(out.count==256);
      for(size_t i=0;i<out.count;++i)assert(out.samples[i]==expected[i]);
      result.samples+=out.count;
      (out.mode==AudioPipeline::Mode::RAW ? result.raw : result.enhanced)++;
      pipeline.release(out);
    }
    if(result.samples>=blocks*256&&!requested) {
      if(!stop) {pipeline.abort();return result;}
      pipeline.requestStop();requested=true;
    }
    if(requested&&pipeline.drained())return result;
    vTaskDelay(1);
  }
  assert(false && "pipeline drain timed out");
  return result;
}

int main(int argc,char **argv) {
  const std::string scenario=argc>1?argv[1]:"normal";
  if(scenario=="missing")available=false;
  if(scenario=="preflight")realtime=false;
  if(scenario=="slow")inferenceDelay=80;
  auto *pipeline=new AudioPipeline;
  assert(pipeline->begin());
  if(scenario=="stale_failure") {
    inferenceDelay=40;failFirst=true;
    assert(pipeline->start(1));
    const auto deadline=esp_timer_get_time()+1000000;
    while(!processStarted.load()&&esp_timer_get_time()<deadline) {
      AudioPipeline::Output out;
      if(pipeline->poll(out))pipeline->release(out);
      vTaskDelay(1);
    }
    assert(processStarted.load());
    pipeline->abort();inferenceDelay=0;
    const auto r=record(*pipeline,2,16);
    assert(r.samples>=16*256);
  } else if(scenario=="immediate_stop") {
    for (unsigned i=1;i<=10;++i) {
      assert(pipeline->start(i));
      pipeline->requestStop();
      const auto deadline=esp_timer_get_time()+1000000;
      while(!pipeline->drained()&&esp_timer_get_time()<deadline) {
        AudioPipeline::Output out;
        while(pipeline->poll(out))pipeline->release(out);
        vTaskDelay(1);
      }
      assert(esp_timer_get_time()<deadline);
    }
  } else if(scenario=="abort") {
    inferenceDelay=20;
    record(*pipeline,1,3,false);
    inferenceDelay=0;
    const auto r=record(*pipeline,2,12);
    assert(r.enhanced>0);
  } else if(scenario=="pool") {
    assert(pipeline->start(1));
    vTaskDelay(600);
    size_t next=0;
    const auto deadline=esp_timer_get_time()+2000000;
    while(!pipeline->drained()&&esp_timer_get_time()<deadline) {
      AudioPipeline::Output out;
      while(pipeline->poll(out)) {
        assert(out.firstSample==next);next+=out.count;pipeline->release(out);
      }
      vTaskDelay(1);
    }
    assert(next>0 && next<=8*256);
  } else {
    const auto r=record(*pipeline,1,16);
    if(scenario=="normal")assert(r.raw==0 && r.enhanced>=16);
    else assert(r.raw>0);
    if(scenario=="slow") {
      inferenceDelay=0;
      const auto next=record(*pipeline,2,12);
      assert(next.enhanced>0);
    }
  }
  std::cout<<"PASS "<<scenario<<std::endl;
  // Firmware tasks deliberately run forever; the test owns this process.
  std::_Exit(0);
}
