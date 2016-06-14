// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_PROFILER_CPU_PROFILER_H_
#define V8_PROFILER_CPU_PROFILER_H_

#include "src/allocation.h"
#include "src/base/atomic-utils.h"
#include "src/base/atomicops.h"
#include "src/base/platform/time.h"
#include "src/compiler.h"
#include "src/libsampler/v8-sampler.h"
#include "src/locked-queue.h"
#include "src/profiler/circular-queue.h"
#include "src/profiler/tick-sample.h"

namespace v8 {
namespace internal {

// Forward declarations.
class CodeEntry;
class CodeMap;
class CpuProfile;
class CpuProfilesCollection;
class ProfileGenerator;

#define CODE_EVENTS_TYPE_LIST(V)                         \
  V(CODE_CREATION, CodeCreateEventRecord)                \
  V(CODE_MOVE, CodeMoveEventRecord)                      \
  V(CODE_DISABLE_OPT, CodeDisableOptEventRecord)         \
  V(CODE_DEOPT, CodeDeoptEventRecord)                    \
  V(REPORT_BUILTIN, ReportBuiltinEventRecord)


class CodeEventRecord {
 public:
#define DECLARE_TYPE(type, ignore) type,
  enum Type {
    NONE = 0,
    CODE_EVENTS_TYPE_LIST(DECLARE_TYPE)
    NUMBER_OF_TYPES
  };
#undef DECLARE_TYPE

  Type type;
  mutable unsigned order;
};


class CodeCreateEventRecord : public CodeEventRecord {
 public:
  Address start;
  CodeEntry* entry;
  unsigned size;

  INLINE(void UpdateCodeMap(CodeMap* code_map));
};


class CodeMoveEventRecord : public CodeEventRecord {
 public:
  Address from;
  Address to;

  INLINE(void UpdateCodeMap(CodeMap* code_map));
};


class CodeDisableOptEventRecord : public CodeEventRecord {
 public:
  Address start;
  const char* bailout_reason;

  INLINE(void UpdateCodeMap(CodeMap* code_map));
};


class CodeDeoptEventRecord : public CodeEventRecord {
 public:
  Address start;
  const char* deopt_reason;
  SourcePosition position;
  int deopt_id;

  INLINE(void UpdateCodeMap(CodeMap* code_map));
};


class ReportBuiltinEventRecord : public CodeEventRecord {
 public:
  Address start;
  Builtins::Name builtin_id;

  INLINE(void UpdateCodeMap(CodeMap* code_map));
};


class TickSampleEventRecord {
 public:
  // The parameterless constructor is used when we dequeue data from
  // the ticks buffer.
  TickSampleEventRecord() { }
  explicit TickSampleEventRecord(unsigned order) : order(order) { }

  unsigned order;
  TickSample sample;
};


class CodeEventsContainer {
 public:
  explicit CodeEventsContainer(
      CodeEventRecord::Type type = CodeEventRecord::NONE) {
    generic.type = type;
  }
  union  {
    CodeEventRecord generic;
#define DECLARE_CLASS(ignore, type) type type##_;
    CODE_EVENTS_TYPE_LIST(DECLARE_CLASS)
#undef DECLARE_TYPE
  };
};


// This class implements both the profile events processor thread and
// methods called by event producers: VM and stack sampler threads.
class ProfilerEventsProcessor : public base::Thread {
 public:
  ProfilerEventsProcessor(ProfileGenerator* generator,
                          sampler::Sampler* sampler,
                          base::TimeDelta period);
  virtual ~ProfilerEventsProcessor();

  // Thread control.
  virtual void Run();
  void StopSynchronously();
  INLINE(bool running()) { return !!base::NoBarrier_Load(&running_); }
  void Enqueue(const CodeEventsContainer& event);

  // Puts current stack into tick sample events buffer.
  void AddCurrentStack(Isolate* isolate, bool update_stats = false);
  void AddDeoptStack(Isolate* isolate, Address from, int fp_to_sp_delta);

  // Tick sample events are filled directly in the buffer of the circular
  // queue (because the structure is of fixed width, but usually not all
  // stack frame entries are filled.) This method returns a pointer to the
  // next record of the buffer.
  inline TickSample* StartTickSample();
  inline void FinishTickSample();

  // SamplingCircularQueue has stricter alignment requirements than a normal new
  // can fulfil, so we need to provide our own new/delete here.
  void* operator new(size_t size);
  void operator delete(void* ptr);

 private:
  // Called from events processing thread (Run() method.)
  bool ProcessCodeEvent();

  enum SampleProcessingResult {
    OneSampleProcessed,
    FoundSampleForNextCodeEvent,
    NoSamplesInQueue
  };
  SampleProcessingResult ProcessOneSample();

  ProfileGenerator* generator_;
  sampler::Sampler* sampler_;
  base::Atomic32 running_;
  const base::TimeDelta period_;  // Samples & code events processing period.
  LockedQueue<CodeEventsContainer> events_buffer_;
  static const size_t kTickSampleBufferSize = 1 * MB;
  static const size_t kTickSampleQueueLength =
      kTickSampleBufferSize / sizeof(TickSampleEventRecord);
  SamplingCircularQueue<TickSampleEventRecord,
                        kTickSampleQueueLength> ticks_buffer_;
  LockedQueue<TickSampleEventRecord> ticks_from_vm_buffer_;
  base::AtomicNumber<unsigned> last_code_event_id_;
  unsigned last_processed_code_event_id_;
};

#define PROFILE(IsolateGetter, Call)                                       \
  do {                                                                     \
    Isolate* the_isolate = (IsolateGetter);                                \
    v8::internal::Logger* logger = the_isolate->logger();                  \
    if (logger->is_logging_code_events() || the_isolate->is_profiling()) { \
      logger->Call;                                                        \
    }                                                                      \
  } while (false)

class CpuProfiler : public CodeEventListener {
 public:
  explicit CpuProfiler(Isolate* isolate);

  CpuProfiler(Isolate* isolate,
              CpuProfilesCollection* test_collection,
              ProfileGenerator* test_generator,
              ProfilerEventsProcessor* test_processor);

  ~CpuProfiler() override;

  void set_sampling_interval(base::TimeDelta value);
  void CollectSample();
  void StartProfiling(const char* title, bool record_samples = false);
  void StartProfiling(String* title, bool record_samples);
  CpuProfile* StopProfiling(const char* title);
  CpuProfile* StopProfiling(String* title);
  int GetProfilesCount();
  CpuProfile* GetProfile(int index);
  void DeleteAllProfiles();
  void DeleteProfile(CpuProfile* profile);

  // Invoked from stack sampler (thread or signal handler.)
  inline TickSample* StartTickSample();
  inline void FinishTickSample();

  // Must be called via PROFILE macro, otherwise will crash when
  // profiling is not enabled.
  void CallbackEvent(Name* name, Address entry_point) override;
  void CodeCreateEvent(Logger::LogEventsAndTags tag, AbstractCode* code,
                       const char* comment) override;
  void CodeCreateEvent(Logger::LogEventsAndTags tag, AbstractCode* code,
                       Name* name) override;
  void CodeCreateEvent(Logger::LogEventsAndTags tag, AbstractCode* code,
                       SharedFunctionInfo* shared, Name* script_name) override;
  void CodeCreateEvent(Logger::LogEventsAndTags tag, AbstractCode* code,
                       SharedFunctionInfo* shared, Name* script_name, int line,
                       int column) override;
  void CodeCreateEvent(Logger::LogEventsAndTags tag, AbstractCode* code,
                       int args_count) override;
  void CodeMovingGCEvent() override {}
  void CodeMoveEvent(AbstractCode* from, Address to) override;
  void CodeDisableOptEvent(AbstractCode* code,
                           SharedFunctionInfo* shared) override;
  void CodeDeoptEvent(Code* code, Address pc, int fp_to_sp_delta);
  void GetterCallbackEvent(Name* name, Address entry_point) override;
  void RegExpCodeCreateEvent(AbstractCode* code, String* source) override;
  void SetterCallbackEvent(Name* name, Address entry_point) override;
  void SharedFunctionInfoMoveEvent(Address from, Address to) override {}

  bool is_profiling() const { return is_profiling_; }

  ProfileGenerator* generator() const { return generator_; }
  ProfilerEventsProcessor* processor() const { return processor_; }
  Isolate* isolate() const { return isolate_; }

 private:
  void StartProcessorIfNotStarted();
  void StopProcessorIfLastProfile(const char* title);
  void StopProcessor();
  void ResetProfiles();
  void LogBuiltins();
  void RecordInliningInfo(CodeEntry* entry, AbstractCode* abstract_code);
  void RecordDeoptInlinedFrames(CodeEntry* entry, AbstractCode* abstract_code);
  Name* InferScriptName(Name* name, SharedFunctionInfo* info);

  Isolate* isolate_;
  base::TimeDelta sampling_interval_;
  CpuProfilesCollection* profiles_;
  ProfileGenerator* generator_;
  ProfilerEventsProcessor* processor_;
  bool saved_is_logging_;
  bool is_profiling_;

  DISALLOW_COPY_AND_ASSIGN(CpuProfiler);
};

}  // namespace internal
}  // namespace v8


#endif  // V8_PROFILER_CPU_PROFILER_H_
