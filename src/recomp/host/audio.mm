// audio.mm - host_audio_play and friends, on AVAudioEngine.
//
// The shims hand out one flat channel numbering for DirectSound secondary
// buffers and QMixer channels alike (dx_alloc_audio_channel), so this file
// sees a small set of integers, each of which owns an AVAudioPlayerNode
// connected to the engine's main mixer.
//
// Three things about DirectSound decide the shape of this file:
//
//   * A buffer has a play cursor that survives a Stop. GetCurrentPosition
//     after a Stop reports where it stopped, and a Play afterwards resumes
//     there. So a channel keeps its own byte cursor rather than asking the
//     node, which knows nothing once it is stopped.
//   * Pan is an attenuation of one channel, not a position (see audio.h). It
//     is applied to the samples, which is also what lets it be exact.
//   * SetFrequency takes effect immediately, mid-sound. AVAudioPlayerNode has
//     no rate control, so a rate change re-schedules the rest of the buffer at
//     the new rate from the cursor it had reached. The sound continues; it
//     just continues faster.
//
// Everything the guest points at is copied on the way in: `pcm` is guest
// memory that is only valid for the duration of the call, and a channel may
// be re-scheduled long afterwards.
//
// LOCK ORDER, which is the thing this file gets wrong most easily.
//
// AVFoundation drains a node's completion handlers from inside [node stop], on
// its own queue, synchronously. The first live run deadlocked on exactly that:
// host_audio_play held the channel mutex, called [node stop] to interrupt the
// previous sound, and Stop waited for the completion queue, which was waiting
// for the mutex. The guest's audio thread never returned, so it never gave the
// cooperative baton back, so the main thread never rendered and the intro was
// blank.
//
// Two rules, and both are checked rather than remembered:
//
//   * No node or engine method is called while the channel-data lock is held.
//     Everything a call needs - the buffers, the volume, the generation - is
//     prepared under the lock, the lock is dropped, and only then does the
//     node hear about it. audio_check_unlocked() counts any breach.
//   * The completion handler takes no lock at all. It stores the generation
//     that finished into a lock-free array and returns; the guest side
//     reconciles the next time it asks whether a channel is still playing.
//
// A generation still distinguishes a completion from the sound that replaced
// it: a stale one names a playback that is already over and is ignored.
#include "audio.h"
#include "audio_capture.h"
#include <stdarg.h>
#include "../dx/host_api.h"
// The arithmetic lives in audio_math.cpp; this file is the engine.

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The engine.
// ---------------------------------------------------------------------------
// How many samples the clipper had to take the top off, and the worst one it
// saw. Both are written from the render thread and read from anywhere, so both
// are relaxed atomics; neither is used for anything but the report.
std::atomic<uint64_t> g_clipped_samples{0};
std::atomic<uint32_t> g_worst_over_bits{0}; // a float, bit for bit

// --- the clipper node -------------------------------------------------------
//
// A memoryless saturator between the mixer and the output, sample by sample,
// with no state and no gain to ride.
//
// WHY IT IS NOT A LIMITER ANY MORE
//
// The original has no dynamics processing anywhere: QMixer sums its voices at
// the gains the game asked for into a 16-bit buffer, and a sum that will not
// fit saturates there. So the level model here has to be the original's, and a
// limiter is not it. Apple's peak limiter, even configured to act only above
// full scale with its attack and decay at the unit's floor, still pulls the
// whole mix down for a millisecond after every overshoot: a live run measured
// 5.3 dB of gain reduction with four voices and the headless capture counted
// thirteen to twenty-five gain steps in forty seconds. A listener heard that
// as everything sounding squashed and dull.
//
// WHY IT HARD-CLIPS, WHICH IS NOT LAZINESS
//
// The ruling asked for a soft clipper that acts only above 0 dBFS. Those two
// cannot both hold. A curve that is the identity up to full scale arrives at
// 1.0 with slope 1; if it is to stay at or below 1.0 above that, its slope
// must drop to zero there. There is no soft knee to be had on the far side of
// a ceiling that the curve reaches with slope 1 - softening it means bending
// the curve BELOW full scale, which is exactly what "acts only above 0 dBFS"
// forbids, and which would also make the game quieter than it is. So the map
// is the identity below full scale and flat above it, which is the same map
// the original's 16-bit sum performs.
//
// WHAT IT IS ACTUALLY FOR
//
// Nothing in this file can make a voice exceed full scale: the decode maps
// 16-bit to [-1, 0.99997], the pan gains are at most one and the volume gain
// is an attenuation. Sample-rate conversion can, and does. The game's sounds
// are 11025 and 22050 Hz and the device runs at 44100 or 48000, so every voice
// is resampled, and interpolating a band-limited signal that is already near
// full scale overshoots it - measured at 1.093 on this graph with one voice
// playing. That is distortion in the output of a mix that was correct going
// in, and this node is what takes it off.
//
// THE NODE STAYS EVEN IF THE CLIPPER CANNOT BE BUILT
//
// Removing the effect node from between the mixer and the output entirely was
// tried, and it crashed the guest at EIP 00564d99 in four runs out of four.
// The cause was never found and the crash is not in this file, so the graph
// keeps a node there either way: if registering or instantiating this unit
// fails, ensure_engine falls back to the peak limiter it used to use.
API_AVAILABLE(macos(10.13))
@interface PopClipperAU : AUAudioUnit
@end

API_AVAILABLE(macos(10.13))
@implementation PopClipperAU {
    AUAudioUnitBus *_inBus;
    AUAudioUnitBus *_outBus;
    AUAudioUnitBusArray *_inBusses;
    AUAudioUnitBusArray *_outBusses;
    // Only used if the host hands the render block an output buffer list with
    // no storage in it, which is a thing an AudioUnit host is allowed to do.
    // Owned here so the render block can capture a plain pointer and touch no
    // Objective-C object at all while it runs.
    std::vector<float> *_scratch;
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)description
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError *__autoreleasing *)outError {
    self = [super initWithComponentDescription:description options:options error:outError];
    if (!self)
        return nil;
    // The rate here is only what the bus is born with; the engine sets the
    // real one when it connects the node.
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
                                                                           channels:2];
    _inBus = [[AUAudioUnitBus alloc] initWithFormat:format error:outError];
    _outBus = [[AUAudioUnitBus alloc] initWithFormat:format error:outError];
    if (!_inBus || !_outBus)
        return nil;
    _inBus.maximumChannelCount = 8;
    _outBus.maximumChannelCount = 8;
    _inBusses = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                       busType:AUAudioUnitBusTypeInput
                                                        busses:@[ _inBus ]];
    _outBusses = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                        busType:AUAudioUnitBusTypeOutput
                                                         busses:@[ _outBus ]];
    self.maximumFramesToRender = 4096;
    _scratch = new std::vector<float>();
    return self;
}

- (void)dealloc {
    delete _scratch;
}

- (AUAudioUnitBusArray *)inputBusses {
    return _inBusses;
}
- (AUAudioUnitBusArray *)outputBusses {
    return _outBusses;
}

- (BOOL)allocateRenderResourcesAndReturnError:(NSError *__autoreleasing *)outError {
    if (![super allocateRenderResourcesAndReturnError:outError])
        return NO;
    // Room for every channel of the widest block the host may ask for, so the
    // render block never allocates.
    _scratch->assign((size_t)self.maximumFramesToRender * 8, 0.0f);
    return YES;
}

- (AUInternalRenderBlock)internalRenderBlock {
    std::vector<float> *scratch = _scratch;
    const AUAudioFrameCount most = self.maximumFramesToRender;
    return ^AUAudioUnitStatus(AudioUnitRenderActionFlags *actionFlags,
                              const AudioTimeStamp *timestamp, AVAudioFrameCount frameCount,
                              NSInteger outputBusNumber, AudioBufferList *outputData,
                              const AURenderEvent *events, AURenderPullInputBlock pullInput) {
      (void)actionFlags;
      (void)outputBusNumber;
      (void)events;
      if (!pullInput)
          return kAudioUnitErr_NoConnection;
      if (frameCount > most)
          return kAudioUnitErr_TooManyFramesToProcess;
      // Give the input somewhere to land if the host did not.
      float *spare = scratch->empty() ? nullptr : scratch->data();
      for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i) {
          if (outputData->mBuffers[i].mData)
              continue;
          if (!spare || (size_t)(i + 1) * frameCount > scratch->size())
              return kAudioUnitErr_InvalidPropertyValue;
          outputData->mBuffers[i].mData = spare + (size_t)i * frameCount;
          outputData->mBuffers[i].mDataByteSize = frameCount * sizeof(float);
      }
      AudioUnitRenderActionFlags pulled = 0;
      AUAudioUnitStatus err = pullInput(&pulled, timestamp, frameCount, 0, outputData);
      if (err)
          return err;

      uint64_t clipped = 0;
      float worst = 0.0f;
      for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i) {
          float *p = (float *)outputData->mBuffers[i].mData;
          if (!p)
              continue;
          UInt32 n = outputData->mBuffers[i].mDataByteSize / sizeof(float);
          if (n > frameCount * outputData->mBuffers[i].mNumberChannels)
              n = frameCount * outputData->mBuffers[i].mNumberChannels;
          for (UInt32 f = 0; f < n; ++f) {
              float v = p[f];
              float mag = v < 0.0f ? -v : v;
              if (mag <= 1.0f)
                  continue; // the identity, and most samples
              if (mag > worst)
                  worst = mag;
              ++clipped;
              p[f] = v < 0.0f ? -1.0f : 1.0f;
          }
      }
      if (clipped) {
          g_clipped_samples.fetch_add(clipped, std::memory_order_relaxed);
          uint32_t bits;
          memcpy(&bits, &worst, sizeof bits);
          uint32_t seen = g_worst_over_bits.load(std::memory_order_relaxed);
          for (;;) {
              float had;
              memcpy(&had, &seen, sizeof had);
              if (had >= worst)
                  break;
              if (g_worst_over_bits.compare_exchange_weak(seen, bits, std::memory_order_relaxed))
                  break;
          }
      }
      return noErr;
    };
}
@end

// The four-character codes are this project's own; nothing outside the process
// ever sees them, because the subclass is registered in-process and looked up
// by exactly this description.
static AudioComponentDescription clipper_description(void) {
    AudioComponentDescription desc;
    memset(&desc, 0, sizeof desc);
    desc.componentType = kAudioUnitType_Effect;
    desc.componentSubType = 'pclp';
    desc.componentManufacturer = 'Popm';
    return desc;
}

// nil if the unit could not be registered or instantiated, which leaves the
// caller to put the old limiter in the same place.
static AVAudioUnitEffect *make_clipper(void) {
    if (@available(macOS 10.13, *)) {
        AudioComponentDescription desc = clipper_description();
        static bool registered = false;
        if (!registered) {
            [AUAudioUnit registerSubclass:[PopClipperAU class]
                   asComponentDescription:desc
                                     name:@"Pop Metal clipper"
                                  version:1];
            registered = true;
        }
        return [[AVAudioUnitEffect alloc] initWithAudioComponentDescription:desc];
    }
    return nil;
}

namespace {

// --- the trace, stamped with the capture's own clock ------------------------
//
// POP_HOST_TRACE_AUDIO=1 prints every play, conversion, append and completion
// against how much audio had been rendered when it happened. That is the same
// number the capture's gap list is in, so a hole in the wave and the lines
// around it are the same instant rather than two clocks to reconcile.
bool trace_on() {
    static int on = -1;
    if (on < 0)
        on = getenv("POP_HOST_TRACE_AUDIO") ? 1 : 0;
    return on != 0;
}

void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void trace(const char *fmt, ...) {
    if (!trace_on())
        return;
    printf("[audio %8.3f] ", host_capture_now());
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

struct Channel {
    AVAudioPlayerNode *node = nil;
    AVAudioFormat *format = nil; // what the node is connected with

    std::vector<uint8_t> pcm;    // the whole buffer, copied
    int bits = 16, channels = 2; // the source format
    uint32_t base_rate = 22050;  // the rate the buffer was made with
    uint32_t rate = 0;           // the rate it is playing at now
    // True once SetFrequency has retuned this channel away from its buffer's
    // own rate. Without it a channel could not tell "nobody has asked for a
    // rate" from "somebody asked for exactly the default", and a first Play of
    // a 44100 Hz buffer would inherit a 22050 Hz default and run at half speed.
    bool rate_overridden = false;
    bool loop = false;

    int32_t volume_mb = 0, pan_mb = 0;
    uint32_t start_offset = 0; // where the current scheduling began
    uint32_t cursor = 0;       // retained when not playing
    // When this playback started, on the monotonic clock. The cursor is
    // modelled from it whenever the node is not reporting progress of its own:
    // a DirectSound play cursor moves from the moment Play was called, and the
    // guest's video player polls it and sleeps until it does.
    double started = 0.0;
    uint64_t generation = 0;
    bool playing = false;
    // Whether the last thing submitted was silence, so a refill that turns a
    // silent loop into a sounding one can be reported.
    bool silent = true;
    // True once this channel became a stream. A streamed channel has no total
    // length to run out of, so its cursor is not modelled against one.
    bool streaming = false;
    // When it became one, and how far into the sound it was at that moment.
    // Together they are a cursor that advances at the buffer's own sample rate
    // and never goes backwards - across a re-schedule, an underrun or a refill
    // - which is what a ring's writer paces itself against.
    double stream_started = 0.0;
    uint32_t stream_base = 0;
    // The re-issued sound's remaining length at the moment of conversion.
    // Read by host_audio_voice_remaining_bytes and nothing else; see the note
    // on host_audio_queued_bytes for why the two answers differ.
    uint32_t stream_head = 0;

    // A ring played in place: one buffer, scheduled once, looping for ever,
    // whose samples the guest's writes overwrite where they lie. See
    // host_audio_write.
    AVAudioPCMBuffer *ring = nil;
    bool ring_mode = false;
    uint32_t ring_head_frames = 0; // the one-shot played before the loop
    uint32_t ring_from_frame = 0;  // where in the ring that one-shot started
    // The last sample of the last run written, and where it ended, so the next
    // run can be checked for joining it. A source that does not join itself is
    // a click nothing on this side can remove.
    float ring_tail_l = 0.0f, ring_tail_r = 0.0f;
    uint32_t ring_tail_at = 0xffffffffu;
    uint32_t ring_writes = 0, ring_breaks = 0;
    uint32_t ring_wraps = 0, ring_breaks_at_wrap = 0, ring_unchecked = 0;
    float ring_worst_break = 0.0f;
    // The same measurement taken inside each run, which is the control. A
    // boundary rate on its own has no scale: 26 of 213 reads as a fault until
    // it is put beside the rate the audio steps at anyway.
    uint64_t ring_inside_steps = 0, ring_inside_pairs = 0;
    // What the clock counted while there was nothing to play. A stream that
    // ran dry and was fed late did not play during the gap, so the clock has
    // to be told to skip it or everything after would be counted as played.
    uint64_t stream_skew = 0;
};

double now_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// --- the clock a play cursor runs on ----------------------------------------
//
// Not the wall clock, when the engine is being rendered by hand.
//
// A cursor has to run at the speed the audio is actually being produced. On a
// device that is real time and the two are the same thing. Rendered offline it
// is however fast the host chooses to render, and the headless host renders in
// step with the guest's clock rather than the wall's - so the two came apart
// there by a factor of nine, and every stream cursor ran that much slow. The
// symptom was a refill gate that waited four seconds to fire, which looked
// exactly like the late-completion bug it had just replaced.
//
// So the cursor counts what has been rendered whenever anything is rendering
// by hand, and wall-clock seconds otherwise.
std::atomic<bool> g_manual_render{false};
std::atomic<uint64_t> g_rendered_frames{0};
double g_render_rate = 48000.0;

double audio_clock() {
    if (g_manual_render.load(std::memory_order_acquire) && g_render_rate > 0.0)
        return (double)g_rendered_frames.load(std::memory_order_acquire) / g_render_rate;
    return now_seconds();
}

AVAudioEngine *g_engine = nil;
AVAudioUnitEffect *g_limiter = nil;
std::map<int32_t, Channel> *g_channels = nullptr;
bool g_disabled = false;
bool g_failed = false;
const HostAudioNodeOps *g_ops = nullptr;

// The outer lock. It orders the node calls against each other and is never
// taken by the completion handler, so it cannot take part in the inversion.
std::mutex g_api_mutex;
// The inner lock, over the channel table and the plain data in it. No node or
// engine call may be made while it is held.
std::mutex g_data_mutex;

// Which generation has finished, per channel, written by the completion
// handler with no lock at all. A flat array rather than a lookup into the
// channel table, because the handler must not touch a container the guest
// thread may be inserting into.
const int32_t MAX_AUDIO_CHANNELS = 256;
std::atomic<uint64_t> g_finished[MAX_AUDIO_CHANNELS];
// Bytes appended to a channel, and bytes of those that have played. The first
// is written by the guest thread under the data lock, the second by the
// completion handler with no lock at all, and the difference is what is still
// queued. Two counters rather than one because only one writer touches each.
std::atomic<uint64_t> g_queued_total[MAX_AUDIO_CHANNELS];
std::atomic<uint64_t> g_queued_played[MAX_AUDIO_CHANNELS];
std::atomic<uint32_t> g_lock_violations{0};
// Set from the configuration-change notification, which arrives on some other
// queue; cleared by the next scheduling on the run thread.
std::atomic<bool> g_restart_wanted{false};

// Depth rather than a flag, so a nested guard cannot clear it early.
thread_local int t_data_depth = 0;

struct DataLock {
    std::lock_guard<std::mutex> held;
    DataLock() : held(g_data_mutex) {
        ++t_data_depth;
    }
    ~DataLock() {
        --t_data_depth;
    }
};

void log_once(const char *message) {
    static std::map<std::string, bool> seen;
    if (seen[message])
        return;
    seen[message] = true;
    fprintf(stderr, "[host] %s\n", message);
}

// The loudest sample in a decoded buffer. It is the one measurement that tells
// "the guest handed us silence" apart from "the pipeline swallowed it", and
// those two have entirely different causes.
float buffer_peak(AVAudioPCMBuffer *buffer) {
    if (!buffer || !buffer.floatChannelData)
        return 0.0f;
    float peak = 0.0f;
    for (AVAudioChannelCount c = 0; c < buffer.format.channelCount; ++c) {
        const float *p = buffer.floatChannelData[c];
        for (AVAudioFrameCount i = 0; i < buffer.frameLength; ++i) {
            float v = p[i] < 0 ? -p[i] : p[i];
            if (v > peak)
                peak = v;
        }
    }
    return peak;
}

// Called immediately before every node or engine call. If the data lock is
// held here, the call can re-enter through a completion handler and the
// process is one stop away from the deadlock the live run hit.
// This checks one lock, this file's own. It cannot see a lock taken in another
// host file and held across a call into here, which is the same inversion from
// the other side: midi.mm attaches its synth to this engine, and a mutex of its
// own held across that would establish a lock order - synth before audio - that
// deadlocks the first time anything here calls a MIDI function under the audio
// lock. That file has no mutex at all, deliberately, and says why.
//
// So the invariant, for whoever adds the next thing to this file: NOTHING HERE
// MAY CALL A host_midi_ FUNCTION WHILE HOLDING g_api_mutex OR THE DATA LOCK.
// Today nothing here calls one at all. The natural place for that to change is
// a stop-everything or an engine restart wanting to silence the synth too, and
// it would hang on this mutex and take every guest thread that touches sound
// with it, looking nothing like a MIDI fault from anywhere. Read the synth's
// state first, drop the lock, then call it - which is the same rule as for the
// nodes, for the same reason.
void audio_check_unlocked(const char *what) {
    if (t_data_depth == 0)
        return;
    g_lock_violations.fetch_add(1, std::memory_order_relaxed);
    fprintf(stderr,
            "[host] audio: %s was called while the channel lock was held; "
            "AVFoundation drains completion handlers from inside stop and "
            "this is how that deadlocks\n",
            what);
    assert(t_data_depth == 0 && "no node call may hold the audio channel lock");
}

// Callers hold the data lock.
// Mixer -> clipper -> output, all at the rate the output actually runs at.
//
// The format matters more than it looks. Left to negotiate, the edges can put
// a sample-rate conversion AFTER the clipper, and a conversion applied to a
// hard-clipped signal rings past the corner it was just given: an offline
// render measured 1.0054 coming out of a graph that had been clipped to 1.0.
// Pinning both edges to the output's own format moves the conversion to the
// mixer's side, where the clipper can still see what it does.
void connect_clipper_locked(void) {
    if (!g_engine || !g_limiter)
        return;
    // The format the OUTPUT will actually run at, which is not the same thing
    // as the format its input bus currently reports.
    //
    // In manual rendering the engine renders at manualRenderingFormat, and the
    // input bus can still be carrying whatever the audio device was last set
    // to. Asking the input bus therefore pinned this graph at 24000 Hz while
    // the render ran at 48000, which put a sample-rate conversion AFTER the
    // clipper - and a conversion applied to a signal that has just been given
    // a hard corner rings past it. The node's own output measured exactly
    // 1.0000 and the rendered buffer measured 1.0196.
    //
    // It is also where the test's flakiness came from: which rate the device
    // happens to be in when the process starts decides whether that trailing
    // conversion exists at all, so the same build measured 1.0000 on one run
    // and 1.0196 on the next with nothing changed.
    //
    // Only the offline branch is changed here. On a real device the input bus
    // is the format the output node expects and there is nothing stale about
    // it, that is what shipped, and it cannot be tested without opening the
    // audio hardware - so it is left exactly as it was rather than changed on
    // the strength of an argument.
    // isInManualRenderingMode FIRST. manualRenderingMode is only meaningful
    // while the engine is in manual rendering, and Offline is zero, so asking
    // it alone is asking an unspecified value whether it is the default one.
    // If it ever answered Offline outside manual rendering, manualRenderingFormat
    // would hand back a zero-rate format, the guard below would turn it into
    // nil, and both edges would be negotiated - which is the arrangement that
    // puts a conversion after the clipper. Measured on this macOS the engine
    // reports Realtime both when fresh and after disableManualRenderingMode, so
    // that path does not open here; the guard costs nothing and does not depend
    // on it staying that way.
    const bool manual = g_engine.isInManualRenderingMode &&
                        g_engine.manualRenderingMode == AVAudioEngineManualRenderingModeOffline;
    AVAudioFormat *out =
        manual ? g_engine.manualRenderingFormat : [g_engine.outputNode inputFormatForBus:0];
    if (out && out.sampleRate <= 0.0)
        out = nil;
    [g_engine disconnectNodeOutput:g_engine.mainMixerNode];
    [g_engine disconnectNodeOutput:g_limiter];
    [g_engine connect:g_engine.mainMixerNode to:g_limiter format:out];
    [g_engine connect:g_limiter to:g_engine.outputNode format:out];
}

// Lazily create the audio graph and channel table, or the test stand-ins when installed.
// Callers serialize graph setup; a channel table alone does not imply a running engine.
bool ensure_engine() {
    // With stand-in node calls installed there is nothing to open: the table
    // exists on its own so the state machine can be driven without a device.
    if (g_ops) {
        if (!g_channels)
            g_channels = new std::map<int32_t, Channel>();
        return true;
    }
    // The table alone is not enough to answer yes. A test that drove the state
    // machine with stand-ins leaves the table behind it, and a later caller
    // that wants the engine itself - the MIDI synth does - would otherwise be
    // told there was one when there never had been.
    if (g_channels && g_engine)
        return true;
    if (g_disabled || g_failed)
        return false;
    if (getenv("POP_HOST_NO_AUDIO")) {
        g_disabled = true;
        return false;
    }
    if (!g_channels)
        g_channels = new std::map<int32_t, Channel>();
    if (g_engine)
        return true;
    g_engine = [[AVAudioEngine alloc] init];
    // A clipper between the mixer and the output. What it is and why it is
    // not a limiter is written above PopClipperAU; the short of it is that the
    // original sums at the gains the game asked for and saturates, so this
    // does too, sample by sample, with nothing to ride the gain.
    {
        g_limiter = make_clipper();
        if (g_limiter) {
            [g_engine attachNode:g_limiter];
            connect_clipper_locked();
            log_once("audio: the mix is summed at the game's own gains and "
                     "saturated sample by sample above full scale, as the "
                     "original clips; nothing below full scale is touched");
        } else {
            // The fallback, only so the graph keeps a node between the mixer
            // and the output: removing that node crashed the guest at EIP
            // 00564d99 in four runs out of four. Configured to act only above
            // full scale and to recover in a millisecond, which is as near
            // sample-wise as this unit goes.
            AudioComponentDescription desc;
            memset(&desc, 0, sizeof desc);
            desc.componentType = kAudioUnitType_Effect;
            desc.componentSubType = kAudioUnitSubType_PeakLimiter;
            desc.componentManufacturer = kAudioUnitManufacturer_Apple;
            g_limiter = [[AVAudioUnitEffect alloc] initWithAudioComponentDescription:desc];
            if (g_limiter) {
                AudioUnitSetParameter(g_limiter.audioUnit, kLimiterParam_AttackTime,
                                      kAudioUnitScope_Global, 0, 0.001f, 0);
                AudioUnitSetParameter(g_limiter.audioUnit, kLimiterParam_DecayTime,
                                      kAudioUnitScope_Global, 0, 0.001f, 0);
                AudioUnitSetParameter(g_limiter.audioUnit, kLimiterParam_PreGain,
                                      kAudioUnitScope_Global, 0, 0.0f, 0);
                [g_engine attachNode:g_limiter];
                connect_clipper_locked();
                log_once("audio: the clipper could not be registered; falling "
                         "back to a peak limiter that acts only above full "
                         "scale");
            }
        }
    }
    // Touching the main mixer is what makes the engine build its output graph.
    (void)g_engine.mainMixerNode;
    // Not started here. An engine is started once its graph exists, which is
    // the order Apple's own examples use and the order that matters: starting
    // one whose only connection is the mixer to the output, and attaching the
    // player afterwards, is the arrangement least likely to render anything.
    // ensure_running() starts it from perform(), after the node is attached
    // and connected and before the first play.
    //
    // An engine stops itself when the audio hardware changes underneath it -
    // headphones in, a device switched, a sample rate changed - and nothing
    // restarts it. That is silence from that moment on with nothing said.
    [[NSNotificationCenter defaultCenter]
        addObserverForName:AVAudioEngineConfigurationChangeNotification
                    object:g_engine
                     queue:nil
                usingBlock:^(NSNotification *note) {
                  (void)note;
                  g_restart_wanted.store(true, std::memory_order_release);
                  fprintf(stderr, "[host] the audio hardware changed; the engine will be "
                                  "restarted before the next sound\n");
                }];

    return true;
}

// Called with no data lock held, before anything is scheduled. An engine that
// has stopped plays nothing and says nothing, so this is the difference
// between a run that goes quiet and a run that says why.
void ensure_running() {
    if (!g_engine)
        return;
    audio_check_unlocked("restarting the engine");
    bool wanted = g_restart_wanted.exchange(false, std::memory_order_acq_rel);
    if (!wanted && g_engine.isRunning)
        return;
    NSError *error = nil;
    if ([g_engine startAndReturnError:&error]) {
        static bool told = false;
        if (!told || wanted) {
            told = true;
            AVAudioFormat *out = [g_engine.outputNode outputFormatForBus:0];
            AVAudioFormat *mix = [g_engine.mainMixerNode outputFormatForBus:0];
            printf("[host] audio engine running: %s, output %.0f Hz %u ch, "
                   "mixer %.0f Hz %u ch, mixer volume %.2f%s\n",
                   g_engine.isRunning ? "yes" : "no", out.sampleRate, (unsigned)out.channelCount,
                   mix.sampleRate, (unsigned)mix.channelCount, g_engine.mainMixerNode.outputVolume,
                   wanted ? " (restarted)" : "");
            fflush(stdout);
        }
        return;
    }
    fprintf(stderr, "[host] the audio engine would not start (%s); this run is silent\n",
            error ? error.localizedDescription.UTF8String : "unknown error");
    log_once("audio: the engine is not running, so nothing will be heard");
}

Channel *channel_for(int32_t id, bool create) {
    if (id < 0 || id >= MAX_AUDIO_CHANNELS) {
        log_once("audio: a channel number outside the range this host tracks was used; "
                 "it is ignored");
        return nullptr;
    }
    if (!ensure_engine())
        return nullptr;
    auto it = g_channels->find(id);
    if (it != g_channels->end()) {
        // A channel that was created while stand-in node calls were installed
        // has no node, and it would never get one: every later Play would find
        // the channel, plan against it and schedule into nothing. Only a test
        // can reach this, and a test that reached it heard silence with
        // everything else saying the sound was playing.
        if (!g_ops && !it->second.node) {
            it->second.node = [[AVAudioPlayerNode alloc] init];
            [g_engine attachNode:it->second.node];
            it->second.format = nil;
        }
        return &it->second;
    }
    if (!create)
        return nullptr;
    Channel c;
    if (!g_ops) {
        c.node = [[AVAudioPlayerNode alloc] init];
        [g_engine attachNode:c.node];
    }
    (*g_channels)[id] = c;
    return &(*g_channels)[id];
}

// A completion that named this channel's current playback means a one-shot
// ran out. Callers hold the data lock.
void reconcile_locked(int32_t id, Channel &ch) {
    if (id < 0 || id >= MAX_AUDIO_CHANNELS)
        return;
    uint64_t finished = g_finished[id].load(std::memory_order_acquire);
    // A stream has no end to run out of. The buffer that converted the ring is
    // a one-shot, so a completion arrives for it while appended buffers are
    // still scheduled behind it; retiring the channel there refuses every later
    // chunk and stops the sound a third of a second in. The same reasoning
    // exempts a stream from the end-of-sound check in audio_advance.
    if (!ch.playing || ch.loop || ch.streaming || finished != ch.generation)
        return;
    ch.playing = false;
    ch.cursor = (uint32_t)ch.pcm.size();
}

uint32_t align_down(uint32_t bytes, uint32_t frame) {
    return frame ? bytes - (bytes % frame) : bytes;
}

// A stereo float buffer holding the channel's PCM from `from` to `to`, with the
// pan gains already applied. Stereo whatever the source is, so the pan can
// attenuate one side of a mono sound too. Allocation only: no node is touched,
// so this is safe under the data lock.
AVAudioPCMBuffer *make_buffer(const Channel &ch, uint32_t from, uint32_t to,
                              AVAudioFormat *format) {
    uint32_t frame_bytes = host_audio_frame_bytes(ch.bits, ch.channels);
    if (!format || !frame_bytes || to <= from || to > ch.pcm.size())
        return nil;
    uint32_t frames = (to - from) / frame_bytes;
    if (!frames)
        return nil;
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format
                                                             frameCapacity:frames];
    if (!buffer)
        return nil;
    float *left = buffer.floatChannelData[0];
    float *right = buffer.floatChannelData[1];
    uint32_t written = host_audio_decode_pcm(ch.pcm.data() + from, to - from, ch.bits, ch.channels,
                                             left, right, frames);
    float lg = 1.0f, rg = 1.0f;
    host_audio_pan_gains(ch.pan_mb, &lg, &rg);
    for (uint32_t i = 0; i < written; ++i) {
        left[i] *= lg;
        right[i] *= rg;
    }
    buffer.frameLength = written;
    return written ? buffer : nil;
}

// Everything one scheduling needs, gathered under the data lock and carried
// out without it.
struct Job {
    bool valid = false;
    int32_t id = 0;
    uint64_t generation = 0;
    AVAudioPlayerNode *node = nil;
    AVAudioFormat *connect = nil; // non-nil: reconnect before scheduling
    AVAudioPCMBuffer *head = nil;
    AVAudioPCMBuffer *whole = nil; // non-nil: loop this after the head
    bool head_loops = false;
    float volume = 1.0f;
    uint32_t from = 0, to = 0; // for the stand-in ops
    bool loop = false;
};

// Prepares a scheduling from `from` bytes. Callers hold the data lock; nothing
// here touches a node.
Job plan_locked(int32_t id, Channel &ch, uint32_t from) {
    Job job;
    uint32_t frame_bytes = host_audio_frame_bytes(ch.bits, ch.channels);
    uint32_t total = (uint32_t)ch.pcm.size();
    if (!frame_bytes || !total)
        return job;
    from = align_down(from < total ? from : 0, frame_bytes);

    uint32_t rate = ch.rate ? ch.rate : ch.base_rate;
    AVAudioFormat *format = nil;
    if (!g_ops) {
        format = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                  sampleRate:(double)rate
                                                    channels:2
                                                 interleaved:NO];
        if (!format)
            return job;
        // The node has to be connected with the same format its buffers carry,
        // or the engine either refuses the buffer or plays it at the graph's
        // rate. The connection is remade only when the format changes.
        if (!ch.format || ![ch.format isEqual:format])
            job.connect = format;
    }

    ++ch.generation;
    ch.start_offset = from;
    ch.cursor = from;
    ch.started = audio_clock();
    ch.playing = true;

    job.valid = true;
    job.id = id;
    job.generation = ch.generation;
    job.node = ch.node;
    job.volume = host_audio_gain_from_millibels(ch.volume_mb);
    job.from = from;
    job.to = total;
    job.loop = ch.loop;

    if (!g_ops) {
        job.head = make_buffer(ch, from, total, format);
        if (!job.head) {
            ch.playing = false;
            job.valid = false;
            return job;
        }
        // A loop that began part-way through plays the tail once and then the
        // whole buffer for ever: the loop point is the start of the buffer, not
        // the offset the guest happened to start at.
        if (ch.loop && from != 0)
            job.whole = make_buffer(ch, 0, total, format);
        else
            job.head_loops = ch.loop;
        if (job.connect)
            ch.format = format;
    }
    return job;
}

// The node calls. None of these may run with the data lock held.
void perform(const Job &job) {
    if (!job.valid)
        return;
    audio_check_unlocked("scheduling a buffer");
    if (g_ops) {
        if (g_ops->stop)
            g_ops->stop(job.id, job.generation);
        if (g_ops->schedule)
            g_ops->schedule(job.id, job.generation, job.from, job.to, job.loop ? 1 : 0);
        if (g_ops->play)
            g_ops->play(job.id, job.generation);
        return;
    }
    if (!job.node)
        return;
    if (job.connect) {
        [job.node stop];
        if (job.node.engine)
            [g_engine disconnectNodeOutput:job.node];
        [g_engine connect:job.node to:g_engine.mainMixerNode format:job.connect];
    }
    // After the graph exists, before anything is played into it.
    ensure_running();
    [job.node stop];
    job.node.volume = job.volume;
    job.node.pan = 0.0f; // the pan is in the samples
    int32_t id = job.id;
    uint64_t generation = job.generation;
    AVAudioPlayerNodeBufferOptions head_options =
        job.head_loops ? (AVAudioPlayerNodeBufferLoops | AVAudioPlayerNodeBufferInterrupts)
                       : AVAudioPlayerNodeBufferInterrupts;
    if (job.whole) {
        [job.node scheduleBuffer:job.head atTime:nil options:head_options completionHandler:nil];
        [job.node scheduleBuffer:job.whole
                          atTime:nil
                         options:AVAudioPlayerNodeBufferLoops
               completionHandler:nil];
    } else {
        [job.node scheduleBuffer:job.head
                            atTime:nil
                           options:head_options
            completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
                 completionHandler:^(AVAudioPlayerNodeCompletionCallbackType type) {
                   (void)type;
                   // Runs on AVFoundation's completion queue, which it drains from
                   // inside [node stop]. It must not take a lock, and it does not.
                   static bool first = true;
                   if (first) {
                       first = false;
                       fprintf(stderr, "[host] first audio completion fired\n");
                   }
                   host_audio_completed(id, generation);
                 }];
    }
    [job.node play];

    // Once, for the first sound of the run: everything that decides whether it
    // is audible, in one line, so a silent run says which link is broken.
    static bool told = false;
    if (!told) {
        told = true;
        AVAudioFormat *connected = [job.node outputFormatForBus:0];
        printf("[host] first buffer: channel %d, %.0f Hz %u ch, %u frames, "
               "peak %.3f, node volume %.2f, node playing %s, engine running %s, "
               "connected %.0f Hz %u ch\n",
               job.id, job.head.format.sampleRate, (unsigned)job.head.format.channelCount,
               (unsigned)job.head.frameLength, buffer_peak(job.head), job.node.volume,
               job.node.isPlaying ? "yes" : "no", g_engine.isRunning ? "yes" : "no",
               connected.sampleRate, (unsigned)connected.channelCount);
        fflush(stdout);
    }
}

// The node's own idea of where it has reached, read without the data lock.
bool node_sample_time(int32_t id, AVAudioPlayerNode *node, uint64_t *out) {
    audio_check_unlocked("reading the player time");
    int64_t frames = 0;
    if (g_ops) {
        if (!g_ops->sample_time || !g_ops->sample_time(id, &frames))
            return false;
    } else {
        if (!node)
            return false;
        if (!node.isPlaying)
            return false;
        AVAudioTime *node_time = node.lastRenderTime;
        // playerTimeForNodeTime: does not return nil for a time it cannot use: it
        // raises. Its precondition is that the node time carries a valid sample
        // time or a valid host time, and a graph that has not rendered a block yet
        // - which is every graph in manual rendering mode until the first render -
        // hands back one that carries neither. Asking anyway terminated the
        // headless run on its first GetCurrentPosition.
        if (!node_time || !(node_time.sampleTimeValid || node_time.hostTimeValid))
            return false;
        AVAudioTime *player_time = [node playerTimeForNodeTime:node_time];
        if (!player_time || !player_time.sampleTimeValid)
            return false;
        frames = player_time.sampleTime;
    }
    // lastRenderTime can precede this playback's start. The player timeline
    // is signed (observed -472 frames just after play); wrapping it to uint64
    // makes a fresh sound look finished and forces another stop/play cycle.
    *out = frames > 0 ? (uint64_t)frames : 0;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

extern "C" void host_audio_set_node_ops(const HostAudioNodeOps *ops) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    DataLock held;
    g_ops = ops;
}

extern "C" void host_audio_queue_completed(int32_t id, uint32_t bytes) {
    // No lock, for the same reason as host_audio_completed: this runs on the
    // completion queue that [node stop] drains.
    if (id < 0 || id >= MAX_AUDIO_CHANNELS)
        return;
    g_queued_played[id].fetch_add(bytes, std::memory_order_release);
}

// A cursor that advances at the sample rate and never goes backwards. Callers
// hold the data lock.
uint32_t stream_cursor_locked(const Channel &ch) {
    if (!ch.streaming)
        return ch.cursor;
    uint32_t rate = ch.rate ? ch.rate : ch.base_rate;
    uint32_t frame = host_audio_frame_bytes(ch.bits, ch.channels);
    if (!rate || !frame)
        return ch.stream_base;
    double elapsed = audio_clock() - ch.stream_started;
    if (elapsed < 0.0)
        elapsed = 0.0;
    double bytes = elapsed * (double)rate * (double)frame;
    if (bytes > 4.0e18)
        bytes = 4.0e18;
    uint64_t played = (uint64_t)bytes;
    played -= played % frame;
    return (uint32_t)((uint64_t)ch.stream_base + played);
}

// Of everything appended to this stream, how much has been played. This is a
// clock and not a tally of completions, and that is the whole point.
//
// AVFoundation delivers a buffer's completion when it feels like it: measured
// on the intro, between eleven milliseconds and nine hundred after the audio
// actually ran out. A caller that refills when its queue empties, which is
// what a streaming caller does, therefore refilled that late - and the silence
// in between is exactly the hole the capture found, one every 1.4 seconds for
// the whole run. Counted from the clock the answer is right the moment it
// changes, and the refill can come before the sound stops rather than after.
uint64_t stream_played_locked(const Channel &ch) {
    uint32_t cursor = stream_cursor_locked(ch);
    uint64_t consumed = cursor > ch.stream_base ? (uint64_t)(cursor - ch.stream_base) : 0;
    // The clock counts everything the channel has sounded since it became a
    // stream, and this is asked only about what was appended. The remainder of
    // the converting lap is in that number too, so this over-counts by up to
    // one lap at the start.
    //
    // That is on purpose, and the direction matters. Reporting less played
    // than really has been makes the queue look fuller than it is, and a
    // caller that refills when its queue runs low then refills LATE, which is
    // silence. Reporting more only makes it refill sooner, which costs a
    // buffer's memory. The first version subtracted the lap to be exact and
    // starved the music by a whole buffer: it kept saying 61440 bytes were
    // still to play while the channel was already silent.
    return consumed > ch.stream_skew ? consumed - ch.stream_skew : 0;
}

extern "C" uint32_t host_audio_played_bytes(int32_t id) {
    std::lock_guard<std::mutex> api(g_api_mutex);

    // A ring played in place answers from the node and not from the clock.
    //
    // This is the number the guest computes its next write offset from. For a
    // queue it only has to pace the caller, and a clock does that. For a ring
    // it decides WHERE the bytes go, so it has to be where the output actually
    // is: a cursor a few milliseconds out puts every write a few milliseconds
    // out, and then the runs do not join each other. Measured before this: 26
    // of 213 runs arrived not joining the one before, and 28 clicks in the
    // capture to match.
    AVAudioPlayerNode *node = nil;
    uint32_t from_frame = 0, frame_bytes = 0;
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        if (!channel)
            return 0;
        if (!channel->ring_mode || g_ops)
            return stream_cursor_locked(*channel);
        node = channel->node;
        from_frame = channel->ring_from_frame;
        frame_bytes = host_audio_frame_bytes(channel->bits, channel->channels);
    }
    uint64_t frames = 0;
    if (node && frame_bytes && node_sample_time(id, node, &frames))
        return (uint32_t)((from_frame + frames) * frame_bytes);
    DataLock held;
    Channel *channel = channel_for(id, false);
    return channel ? stream_cursor_locked(*channel) : 0;
}

// Convert an existing channel into an appendable stream and return its starting offset.
// Preserve the schedule of a live one-shot; a loop must first relinquish its loop schedule.
extern "C" int32_t host_audio_stream(int32_t id) {
    if (id < 0 || id >= MAX_AUDIO_CHANNELS)
        return -1;
    std::lock_guard<std::mutex> api(g_api_mutex);

    AVAudioPlayerNode *node = nil;
    bool need_position = false;
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        if (!channel || channel->pcm.empty())
            return -1;
        reconcile_locked(id, *channel);
        if (channel->streaming)
            return (int32_t)stream_cursor_locked(*channel);
        node = channel->node;
        need_position = channel->playing;
    }

    // Where it has reached, read without the lock as every node call must be.
    uint64_t sample_time = 0;
    bool have_time = need_position && node_sample_time(id, node, &sample_time);

    Job job;
    uint32_t from = 0;
    bool continuing_one_shot = false;
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        if (!channel)
            return -1;
        uint32_t total = (uint32_t)channel->pcm.size();
        from = channel->cursor;
        if (have_time)
            from =
                host_audio_position_bytes(sample_time, channel->start_offset, total, channel->bits,
                                          channel->channels, channel->loop ? 1 : 0);
        continuing_one_shot = channel->playing && !channel->loop && from < total;
        if (from >= total)
            from = 0;

        // A live one-shot is already appendable. Preserve its node schedule,
        // generation and sample-time origin: stopping and playing it again
        // blocks the guest for an audio I/O cycle and can interrupt its onset.
        // A loop still needs its remaining lap re-issued without loop flags;
        // a stopped or completed voice still needs to be started.
        channel->loop = false;
        channel->streaming = true;
        channel->stream_started = audio_clock();
        // The cursor carries on from where the loop had reached rather than
        // restarting at zero, so a caller pacing its refills against it sees
        // one continuously advancing number across the conversion.
        channel->stream_base = from;
        channel->stream_head = total > from ? total - from : 0;
        channel->stream_skew = 0;
        g_queued_total[id].store(0, std::memory_order_release);
        g_queued_played[id].store(0, std::memory_order_release);
        if (continuing_one_shot)
            channel->cursor = from;
        else
            job = plan_locked(id, *channel, from);
    }
    trace("stream  ch %d  continuing from byte %u%s", id, from,
          continuing_one_shot ? " without restarting the one-shot" : " after rescheduling");
    perform(job);
    printf("[host] audio channel %d is a stream now, continuing from byte %u\n", id, from);
    fflush(stdout);
    return (int32_t)from;
}

// What the queue said about itself, kept so a run can check the answer against
// the audio it actually held.
//
// The failure this exists to catch does not look like a wrong number. It looks
// like a refill gate skipping: the queue reports depth, the caller sees no
// reason to send more, and the sound stops while everything says it is fine.
// impl-audio found it by hand - thirty seconds of "plenty queued" over a run
// that had been handed nine and three quarter seconds of audio - and that
// arithmetic is the check.
struct QueueHealth {
    double reporting = 0.0;  // seconds spent answering "not empty"
    double last = -1.0;      // when it was last asked
    uint64_t appended = 0;   // bytes ever handed over
    uint32_t rate = 0;       // bytes a second of that channel's audio
    uint32_t violations = 0; // answers larger than could possibly be left
};
QueueHealth g_queue_health[MAX_AUDIO_CHANNELS];

// What is STILL TO PLAY OF WHAT THE CALLER APPENDED - not of what the host is
// holding altogether, and the difference is measured rather than assumed.
//
// A play resets this to zero, so a caller that has just handed over a
// 1.4-second sound is told nothing is outstanding thirty milliseconds later,
// and QMixer's refill gate then refills at once: 635 times in one live run,
// every one logging "queued 0" for a sound that had barely started. Adding the
// re-issued sound to the answer looks like the fix and is not - measured on
// the intro, it took the run from no dropouts to thirty-one silent gaps,
// because the gate waits out the whole re-issued lap before refilling at all.
// Which of the two numbers the gate should read is a decision for both sides
// of it, so this one keeps the meaning its caller was written against.
// Report queued PCM bytes from the playback clock for live streams. Completion callbacks
// can arrive late, so their tally is used only where a real stream clock is unavailable.
extern "C" uint32_t host_audio_queued_bytes(int32_t id) {
    if (id < 0 || id >= MAX_AUDIO_CHANNELS)
        return 0;
    uint64_t appended = g_queued_total[id].load(std::memory_order_acquire);
    uint64_t played = g_queued_played[id].load(std::memory_order_acquire);

    // A stream answers from the clock. See stream_played_locked: a completion
    // arrives long after the audio it belongs to has been heard, and a caller
    // that refills when this reaches zero refilled that late, once every 1.4
    // seconds for a whole run. With stand-in node calls installed there is no
    // clock to read - a test drives the completions itself - so the tally is
    // still the answer there.
    if (!g_ops) {
        std::lock_guard<std::mutex> api(g_api_mutex);
        DataLock held;
        Channel *channel = channel_for(id, false);

        // The clock alone, not the larger of the two. A completion is the late
        // number - it arrived between eleven milliseconds and nine hundred
        // after the audio it belongs to had been heard - and taking whichever
        // is larger lets the late one win whenever it is ahead, which it is
        // for as long as it takes the clock to overtake it. Measured on the
        // intro that was the first thirty-one seconds: one buffer in flight,
        // a refill only once the previous had finished, and a hole every lap.
        if (channel && channel->streaming)
            played = stream_played_locked(*channel);
    }

    // The time spent reporting depth, charged to the answer just given. Held
    // under the data lock because that is what guards the health record.
    {
        DataLock held;
        QueueHealth &h = g_queue_health[id];
        double now = audio_clock();
        if (h.last >= 0.0 && now > h.last && appended > played)
            h.reporting += now - h.last;
        h.last = now;
        h.appended = appended;
        // An answer bigger than everything handed over minus everything the
        // clock says has played is not possible. If one appears, the accounting
        // has gone back to trusting something other than the clock.
        if (appended > played && (appended - played) > appended)
            ++h.violations;
    }

    if (played >= appended) {
        // Nothing outstanding. The store pulls the played count back to the
        // appended one so a completion that reported more than was ever
        // appended - a duplicate, or one belonging to a sound already replaced
        // - does not leave the accounting skewed for the rest of the run. A
        // streaming caller reads this to decide when to send more, and a
        // permanently wrong answer there is a stream that starves or floods.
        uint64_t counted = g_queued_played[id].load(std::memory_order_acquire);
        if (counted > appended)
            g_queued_played[id].store(appended, std::memory_order_release);
        return 0;
    }
    return (uint32_t)(appended - played);
}

// A ring played where it lies, which is what a DirectSound streaming buffer
// actually is.
//
// The game's streamed sounds are a ring the guest rewrites as it plays. On
// hardware the play cursor runs through that memory continuously and the guest
// writes ahead of it; there is no stopping and no boundary, and a lap costs
// nothing. Handing each written run to the host as a buffer to schedule behind
// the last one is not the same shape, and the difference is audible: the guest
// can only write a full ring once the cursor has come the whole way round, so
// the run it writes always arrives just AFTER the previous one has finished
// playing. The capture measured the result exactly - twenty-five milliseconds
// of silence every 1.425 seconds, for the whole run, which is one lap of the
// 61440-byte ring the intro music plays.
//
// So this writes into the samples that are already sounding. One buffer,
// scheduled once, looping for ever, its floats overwritten in place where the
// guest's bytes land. After the first call nothing here touches a node again:
// no stop, no schedule, no completion, and therefore no seam of any kind. It
// is a race with the render thread, as it is on hardware, and it is the same
// race hardware has: the writer stays ahead of the cursor or it does not.
//
// `offset` is where in the ring the bytes go. Returns the bytes taken, or 0
// for a channel that has never been played.
// Apply an in-place PCM write to a playing channel and return the accepted byte count.
// Sample alignment and ring bounds are enforced; node operations occur outside DataLock.
extern "C" int32_t host_audio_write(int32_t id, const void *pcm, uint32_t offset, uint32_t bytes) {
    if (!pcm || !bytes || id < 0 || id >= MAX_AUDIO_CHANNELS)
        return 0;
    std::lock_guard<std::mutex> api(g_api_mutex);

    // Where the node has actually reached, read before the data lock is taken
    // because no node call may be made under it. The clock is a good enough
    // cursor for a caller pacing its writes and not for this: the fade has to
    // land on the sample the output is about to produce, and being a few
    // milliseconds out puts it somewhere that does not matter while the step
    // stays where it did.
    AVAudioPlayerNode *head_node = nil;
    uint32_t head_len = 0, head_from = 0, ring_frames = 0;
    {
        DataLock held;
        Channel *c0 = channel_for(id, false);
        if (c0 && c0->ring_mode && c0->ring) {
            head_node = c0->node;
            head_len = c0->ring_head_frames;
            head_from = c0->ring_from_frame;
            ring_frames = (uint32_t)c0->ring.frameLength;
        }
    }
    uint64_t node_frames = 0;
    bool have_head = head_node && ring_frames && node_sample_time(id, head_node, &node_frames);

    Job job;
    bool schedule_ring = false;
    AVAudioPlayerNode *node = nil;
    AVAudioPCMBuffer *ring = nil;
    AVAudioPCMBuffer *head = nil;
    AVAudioFormat *connect = nil;
    uint32_t accepted = 0;
    float volume = 1.0f;
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        if (!channel || channel->pcm.empty())
            return 0;
        uint32_t total = (uint32_t)channel->pcm.size();
        uint32_t frame = host_audio_frame_bytes(channel->bits, channel->channels);
        if (!frame || offset >= total)
            return 0;
        // Aligned to a sample frame and clipped to the ring. A run that would
        // cross the end is the caller's to split, because the two halves are
        // two runs in play order and only the caller knows that.
        offset -= offset % frame;
        accepted = bytes - (bytes % frame);
        if (offset + accepted > total)
            accepted = total - offset;
        if (!accepted)
            return 0;

        // The authoritative copy, so a later Play starts from what is there now.
        memcpy(channel->pcm.data() + offset, pcm, accepted);

        if (g_ops) {
            channel->streaming = true;
            channel->ring_mode = true;
            return (int32_t)accepted;
        }

        uint32_t rate = channel->rate ? channel->rate : channel->base_rate;
        if (!channel->ring_mode) {
            AVAudioFormat *format =
                [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                 sampleRate:(double)rate
                                                   channels:2
                                                interleaved:NO];
            if (!format)
                return 0;
            if (!channel->format || ![channel->format isEqual:format])
                connect = format;
            channel->format = format;

            // Where it has reached, so the ring starts sounding from there
            // rather than jumping to its beginning.
            uint32_t from = channel->cursor;
            if (channel->streaming)
                from = stream_cursor_locked(*channel) % total;
            from -= from % frame;

            channel->ring = make_buffer(*channel, 0, total, format);
            if (!channel->ring)
                return 0;
            channel->ring_from_frame = from / frame;
            channel->ring_head_frames = from ? (total - from) / frame : 0;
            // The rest of the current lap, played once before the loop takes
            // over at the top. Its bytes are the ones already written, so
            // nothing is stale about it.
            if (from)
                head = make_buffer(*channel, from, total, format);

            ++channel->generation;
            channel->ring_mode = true;
            channel->streaming = true;
            channel->loop = false;
            channel->playing = true;
            channel->start_offset = from;
            channel->cursor = from;
            channel->stream_started = audio_clock();
            channel->stream_base = from;
            channel->stream_head = 0; // a ring is written where it lies
            channel->stream_skew = 0;
            g_queued_total[id].store(0, std::memory_order_release);
            g_queued_played[id].store(0, std::memory_order_release);
            schedule_ring = true;
            node = channel->node;
            ring = channel->ring;
            volume = host_audio_gain_from_millibels(channel->volume_mb);
        } else {
            // The ordinary case, and the whole point: samples straight into
            // the buffer the node is looping. No node call, no seam.
            ring = channel->ring;
            if (!ring)
                return 0;
            uint32_t at = offset / frame;
            uint32_t frames = accepted / frame;
            if (at + frames > ring.frameLength)
                return 0;
            float *left = ring.floatChannelData[0] + at;
            float *right = ring.floatChannelData[1] + at;

            // Where the samples change is a step, and a step of any size is a
            // click. It is at the start of the written run when the writer is
            // ahead of the play head, and at the play head itself when the run
            // has landed on top of it - which happens whenever the writer and
            // the cursor drift close, and did, every forty milliseconds for a
            // second and a half at the start of the movie.
            //
            // The old samples there are kept, and the new ones fade in over
            // two milliseconds. That is shorter than anything a person hears
            // as a change in the sound and long enough that the step is gone.
            uint32_t rate = channel->rate ? channel->rate : channel->base_rate;
            uint32_t fade = rate / 500; // 2 ms
            if (fade > frames)
                fade = frames;
            // The node's own position, in frames from the start of the ring.
            // Before the one-shot has finished the output is still inside it,
            // which is a different place in the ring.
            uint32_t head;
            if (have_head) {
                head = node_frames < head_len ? (uint32_t)((head_from + node_frames) % ring_frames)
                                              : (uint32_t)((node_frames - head_len) % ring_frames);
            } else {
                head = stream_cursor_locked(*channel) % total / frame;
            }
            // Only when the run has landed on top of the play head. When the
            // writer is ahead - which is the normal case - the samples at the
            // start of the run are stale data from the previous lap, and
            // blending the new content into them would MANUFACTURE the step
            // this is here to remove. The join that matters then is between
            // the end of the previous run and the start of this one, and that
            // one is the source's to make.
            bool on_the_head = head >= at && head < at + frames;
            uint32_t from_frame = on_the_head ? head - at : 0;
            if (!on_the_head)
                fade = 0;
            if (from_frame + fade > frames)
                fade = frames - from_frame;
            std::vector<float> old_l(fade), old_r(fade);
            for (uint32_t i = 0; i < fade; ++i) {
                old_l[i] = left[from_frame + i];
                old_r[i] = right[from_frame + i];
            }

            uint32_t written = host_audio_decode_pcm(pcm, accepted, channel->bits,
                                                     channel->channels, left, right, frames);
            float lg = 1.0f, rg = 1.0f;
            host_audio_pan_gains(channel->pan_mb, &lg, &rg);
            for (uint32_t i = 0; i < written; ++i) {
                left[i] *= lg;
                right[i] *= rg;
            }
            // Does this run join the last one? If the source hands over runs
            // that do not meet at their edges, every boundary is a click and
            // it is in the data rather than in the scheduling.
            ++channel->ring_writes;
            if (!at)
                ++channel->ring_wraps;
            if (channel->ring_tail_at != at) {
                // Not the continuation of the last run. Nothing to compare.
                if (channel->ring_tail_at != 0xffffffffu)
                    ++channel->ring_unchecked;
            } else if (written) {
                float dl = left[0] - channel->ring_tail_l;
                float dr = right[0] - channel->ring_tail_r;
                if (dl < 0)
                    dl = -dl;
                if (dr < 0)
                    dr = -dr;
                float d = dl > dr ? dl : dr;
                if (d > 0.25f) {
                    ++channel->ring_breaks;
                    if (!at)
                        ++channel->ring_breaks_at_wrap;
                    if (d > channel->ring_worst_break)
                        channel->ring_worst_break = d;
                }
            }
            if (written) {
                channel->ring_tail_l = left[written - 1];
                channel->ring_tail_r = right[written - 1];
                channel->ring_tail_at = (at + written) % ring.frameLength;
            }

            // What this run does to itself, sample by sample, measured the
            // same way as its join. impl-audio's control: a decoder that reset
            // its predictor every chunk would make boundaries far worse than
            // interiors, and these came out the same, so the steps are the
            // content. Four-bit ADPCM at 22 kHz is coarse and sounds it.
            for (uint32_t i = 1; i < written; ++i) {
                float dl = left[i] - left[i - 1];
                float dr = right[i] - right[i - 1];
                if (dl < 0)
                    dl = -dl;
                if (dr < 0)
                    dr = -dr;
                ++channel->ring_inside_pairs;
                if (dl > 0.25f || dr > 0.25f)
                    ++channel->ring_inside_steps;
            }

            for (uint32_t i = 0; i < fade && from_frame + i < written; ++i) {
                float w = (float)(i + 1) / (float)fade;
                left[from_frame + i] = old_l[i] * (1.0f - w) + left[from_frame + i] * w;
                right[from_frame + i] = old_r[i] * (1.0f - w) + right[from_frame + i] * w;
            }
            if (from_frame)
                log_once("audio: a write landed on the samples being played, so "
                         "the join is faded over two milliseconds rather than "
                         "stepped; the guest is writing close behind the cursor");
            return (int32_t)accepted;
        }
    }

    if (schedule_ring) {
        audio_check_unlocked("starting a ring");
        if (node) {
            if (connect) {
                [node stop];
                if (node.engine)
                    [g_engine disconnectNodeOutput:node];
                [g_engine connect:node to:g_engine.mainMixerNode format:connect];
            }
            ensure_running();
            [node stop];
            node.volume = volume;
            node.pan = 0.0f;
            if (head)
                [node scheduleBuffer:head
                               atTime:nil
                              options:AVAudioPlayerNodeBufferInterrupts
                    completionHandler:nil];
            [node scheduleBuffer:ring
                           atTime:nil
                          options:head ? AVAudioPlayerNodeBufferLoops
                                       : (AVAudioPlayerNodeBufferLoops |
                                          AVAudioPlayerNodeBufferInterrupts)
                completionHandler:nil];
            [node play];
        }
        trace("ring    ch %d  playing its %u-byte ring in place from now on", id,
              (unsigned)accepted);
    }
    return (int32_t)accepted;
}

// Seconds each channel spent claiming a queue, against the seconds of audio it
// was ever handed. A channel that claims depth for much longer than it holds is
// the failure above, and the ratio is the whole of the diagnosis.
extern "C" void host_audio_queue_report(void *file) {
    FILE *out = file ? (FILE *)file : stdout;
    std::lock_guard<std::mutex> api(g_api_mutex);
    DataLock held;
    if (g_channels) {
        for (const auto &entry : *g_channels) {
            const Channel &c = entry.second;
            if (!c.ring_writes)
                continue;
            fprintf(out,
                    "audio ring:         channel %d took %u writes in place, "
                    "%u of them at the wrap, %u not the continuation of the "
                    "one before\n",
                    entry.first, c.ring_writes, c.ring_wraps, c.ring_unchecked);
            double joins =
                c.ring_writes > c.ring_unchecked ? (double)(c.ring_writes - c.ring_unchecked) : 1.0;
            double at_join = 100.0 * (double)c.ring_breaks / joins;
            double inside = c.ring_inside_pairs
                                ? 100.0 * (double)c.ring_inside_steps / (double)c.ring_inside_pairs
                                : 0.0;
            fprintf(out,
                    "                    %u did not join what they continue "
                    "(%u of those at the wrap), worst step %.3f of full "
                    "scale\n",
                    c.ring_breaks, c.ring_breaks_at_wrap, (double)c.ring_worst_break);
            // The two rates side by side. Alike means the seams are as good as
            // the audio gets; a boundary rate well above the interior one is a
            // seam the host or the guest is making.
            fprintf(out,
                    "                    %.2f%% of joins step over a quarter "
                    "of full scale, against %.2f%% of the pairs inside the "
                    "runs%s\n",
                    at_join, inside,
                    inside > 0.0 && at_join > inside * 2.0
                        ? "  <-- the seams are worse than the content"
                        : "  (the content, not the seams)");
        }
    }
    for (int32_t id = 0; id < MAX_AUDIO_CHANNELS; ++id) {
        QueueHealth &h = g_queue_health[id];
        if (!h.appended)
            continue;
        uint32_t rate = h.rate;
        if (!rate && g_channels) {
            auto it = g_channels->find(id);
            if (it != g_channels->end()) {
                uint32_t r = it->second.rate ? it->second.rate : it->second.base_rate;
                rate = r * host_audio_frame_bytes(it->second.bits, it->second.channels);
            }
        }
        double holds = rate ? (double)h.appended / (double)rate : 0.0;
        bool wrong = holds > 0.0 && h.reporting > holds * 1.25;
        fprintf(out,
                "audio queue:        channel %d claimed a queue for %.1fs "
                "against %.1fs of audio handed over%s\n",
                id, h.reporting, holds, wrong ? "  <-- claiming depth it does not hold" : "");
        if (h.violations)
            fprintf(out,
                    "                    %u answers larger than anything "
                    "left could be\n",
                    h.violations);
    }
}

// How much of what this voice is playing is still to play - the sound itself
// included, whether it arrived with a Play, was re-issued by a conversion, or
// replaced what was there before.
//
// The other question from host_audio_queued_bytes, deliberately. That one
// answers "how much of what YOU appended is still to play", which is the
// DirectSound streaming contract and what a ring's writer needs; a Play resets
// it because a Play is a new sound rather than a continuation. A caller pacing
// refills against a VOICE - one named channel at a time, which is how QMixer
// works - wants this one, or it is told nothing is outstanding thirty
// milliseconds into a second-and-a-half sound and refills at once, every time.
extern "C" uint32_t host_audio_voice_remaining_bytes(int32_t id) {
    if (id < 0 || id >= MAX_AUDIO_CHANNELS)
        return 0;
    std::lock_guard<std::mutex> api(g_api_mutex);
    DataLock held;
    Channel *channel = channel_for(id, false);
    if (!channel || !channel->playing)
        return 0;
    const uint64_t appended = g_queued_total[id].load(std::memory_order_acquire);
    if (channel->streaming) {
        const uint64_t holding = (uint64_t)channel->stream_head + appended;
        const uint64_t played = g_ops ? g_queued_played[id].load(std::memory_order_acquire)
                                      : stream_played_locked(*channel);
        return holding > played ? (uint32_t)(holding - played) : 0;
    }
    const uint32_t total = (uint32_t)channel->pcm.size();
    const uint32_t at = channel->cursor < total ? channel->cursor : total;
    return total - at;
}

// Append PCM in the channel format established by Play and return accepted bytes.
// A looping channel must be converted to a stream before it can accept a continuation.
extern "C" int32_t host_audio_queue(int32_t id, const void *pcm, uint32_t bytes) {
    if (!pcm || !bytes || id < 0 || id >= MAX_AUDIO_CHANNELS)
        return 0;
    std::lock_guard<std::mutex> api(g_api_mutex);

    AVAudioPlayerNode *node = nil;
    AVAudioPCMBuffer *buffer = nil;
    uint32_t accepted = 0;
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        // A continuation needs something to continue. Without a Play first
        // there is no format to append in, and guessing one would be inventing
        // the sound's rate.
        if (!channel || channel->pcm.empty())
            return 0;
        // A buffer scheduled to loop plays for ever, so anything queued behind
        // it is never reached. host_audio_stream converts the loop first, at
        // the cursor, and the append goes behind that.
        if (channel->loop) {
            log_once("audio: a queue arrived for a looping channel, where a buffer "
                     "behind the loop would never be reached; call "
                     "host_audio_stream first to continue it as a stream");
            return 0;
        }
        // A stream that has momentarily run dry is still a stream. Refusing
        // here would send the caller back to re-submitting the whole sound
        // every refill, which is what stopping and rescheduling a player node
        // twenty-five times a second sounds like: silence. The append is taken
        // and the node started again from where it stopped, which is a gap of
        // exactly the time nothing was ready - and that gap is worth saying
        // out loud, because it means the writer is not keeping up.
        if (!channel->playing) {
            if (!channel->streaming)
                return 0;
            log_once("audio: a stream ran dry before its next chunk arrived; the "
                     "gap is the time nothing was ready to play");
            channel->playing = true;
        }
        uint32_t frame = host_audio_frame_bytes(channel->bits, channel->channels);
        if (!frame || bytes < frame)
            return 0;
        accepted = bytes - (bytes % frame);
        channel->streaming = true;
        node = channel->node;

        // If the clock says everything appended has already been played, this
        // chunk arrived after the stream ran dry: nothing was playing during
        // the wait, so the clock counted time the audio did not. Carrying that
        // forward would report every later chunk as played before it was. The
        // skew absorbs it, and the queue is empty at exactly this instant,
        // which is the truth.
        if (!g_ops) {
            uint64_t already = g_queued_total[id].load(std::memory_order_acquire);
            uint64_t by_clock = stream_played_locked(*channel);
            if (by_clock > already)
                channel->stream_skew += by_clock - already;
        }

        if (!g_ops) {
            uint32_t rate = channel->rate ? channel->rate : channel->base_rate;
            AVAudioFormat *format =
                [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                 sampleRate:(double)rate
                                                   channels:2
                                                interleaved:NO];
            if (!format)
                return 0;
            // The append is built from the caller's memory directly: the
            // channel's own pcm is the sound that started, and a stream's later
            // chunks are not part of it.
            uint32_t frames = accepted / frame;
            buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:frames];
            if (!buffer)
                return 0;
            uint32_t written = host_audio_decode_pcm(pcm, accepted, channel->bits,
                                                     channel->channels, buffer.floatChannelData[0],
                                                     buffer.floatChannelData[1], frames);
            float lg = 1.0f, rg = 1.0f;
            host_audio_pan_gains(channel->pan_mb, &lg, &rg);
            for (uint32_t i = 0; i < written; ++i) {
                buffer.floatChannelData[0][i] *= lg;
                buffer.floatChannelData[1][i] *= rg;
            }
            buffer.frameLength = written;
            if (!written)
                return 0;
        }
    }

    g_queued_total[id].fetch_add(accepted, std::memory_order_release);
    {
        DataLock held;
        Channel *c2 = channel_for(id, false);
        if (c2) {
            uint32_t r = c2->rate ? c2->rate : c2->base_rate;
            g_queue_health[id].rate = r * host_audio_frame_bytes(c2->bits, c2->channels);
        }
    }
    {
        uint64_t total = g_queued_total[id].load(std::memory_order_acquire);
        uint64_t played = g_queued_played[id].load(std::memory_order_acquire);
        uint64_t by_clock = 0;
        {
            DataLock held;
            Channel *c2 = channel_for(id, false);
            if (c2 && c2->streaming)
                by_clock = stream_played_locked(*c2);
        }
        trace("queue   ch %d  +%u bytes, %llu appended, %llu played by the clock, "
              "%llu still to play",
              id, accepted, (unsigned long long)total, (unsigned long long)by_clock,
              (unsigned long long)(total > by_clock ? total - by_clock : 0));
    }

    // No stop, no play, no interrupt: the buffer goes behind whatever is still
    // scheduled and the render position never restarts. That is the whole
    // difference between a queue and a new sound.
    audio_check_unlocked("queueing a buffer");
    if (g_ops) {
        if (g_ops->queue)
            g_ops->queue(id, accepted);
        return (int32_t)accepted;
    }
    if (!node)
        return 0;
    ensure_running();
    uint32_t counted = accepted;
    [node scheduleBuffer:buffer
                        atTime:nil
                       options:0
        completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
             completionHandler:^(AVAudioPlayerNodeCompletionCallbackType type) {
               (void)type;
               host_audio_queue_completed(id, counted);
               trace("done    ch %d  a queued buffer of %u bytes finished", id, counted);
             }];
    // A node that has run dry stopped itself, and starting it again is a seam.
    // Saying so is better than a silent gap nobody can account for.
    if (!node.isPlaying) {
        trace("DRY     ch %d  the node had already stopped when this arrived", id);
        log_once("audio: a queued buffer arrived after the node had run dry; the "
                 "stream has a gap in it because the next chunk came too late");
        [node play];
    }
    return (int32_t)accepted;
}

extern "C" void host_audio_completed(int32_t id, uint64_t generation) {
    // No lock, ever. See the note at the top of the file: this is called from
    // inside [node stop], and a lock here is the deadlock.
    if (id < 0 || id >= MAX_AUDIO_CHANNELS)
        return;
    g_finished[id].store(generation, std::memory_order_release);
}

extern "C" uint64_t host_audio_clipped_samples(void) {
    return g_clipped_samples.load(std::memory_order_relaxed);
}

// Three rates a test compares, and nothing else uses. They are facts about the
// graph rather than a restatement of how it was built, which is the point: a
// seam that recomputed the rule would pass whatever the rule did.
//
// The invariant is that all three agree. Where they do not, a sample-rate
// conversion sits somewhere between the mixer and the output, and if it is
// after the clipper it rings past the corner the clipper just made.
extern "C" double host_audio_clipper_output_rate(void) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    if (!g_limiter)
        return 0.0; // no node at all, not a fallback
    AVAudioFormat *f = [g_limiter outputFormatForBus:0];
    return f ? f.sampleRate : 0.0;
}

extern "C" double host_audio_mixer_output_rate(void) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    if (!g_engine)
        return 0.0;
    AVAudioFormat *f = [g_engine.mainMixerNode outputFormatForBus:0];
    return f ? f.sampleRate : 0.0;
}

extern "C" double host_audio_output_bus_rate(void) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    if (!g_engine)
        return 0.0;
    AVAudioFormat *f = [g_engine.outputNode inputFormatForBus:0];
    return f ? f.sampleRate : 0.0;
}

extern "C" float host_audio_worst_overshoot(void) {
    uint32_t bits = g_worst_over_bits.load(std::memory_order_relaxed);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

extern "C" uint32_t host_audio_lock_violations(void) {
    return g_lock_violations.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The engine, for the other things that make sound.
//
// The music is MIDI and its synth is an audio unit, not a player node, so it
// needs the engine this file owns rather than one of its own: two engines
// means two output devices fighting over the same hardware, and the second one
// usually loses silently. midi.mm attaches its synth to this mixer.
// ---------------------------------------------------------------------------
extern "C" void *host_audio_engine(void) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    {
        DataLock held;
        if (!ensure_engine())
            return nullptr;
    }
    return (__bridge void *)g_engine;
}

extern "C" void host_audio_engine_run(void) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    ensure_running();
}

// How many channels have something playing on them. This is what separates a
// silence that is correct from one that is a fault, so the capture asks for it
// once per block. It takes the data lock only, never the API mutex: the data
// lock is held briefly and never across a node call, which is what makes it
// safe to take from the tap's queue.
extern "C" uint32_t host_audio_playing_channels(void) {
    DataLock held;
    if (!g_channels)
        return 0;
    uint32_t n = 0;
    for (const auto &entry : *g_channels)
        if (entry.second.playing)
            ++n;
    return n;
}

// --- capture: what the mixer actually produced ------------------------------
//
// Two ways in, because the two hosts hear differently. The windowed host runs
// against real hardware, so a tap on the main mixer is the only place the
// finished mix exists. The headless host renders the same graph offline, so
// its capture is the render itself and no tap is needed.
//
// The tap runs on AVFoundation's own queue rather than on the render thread,
// which is why it may take a lock and write a file at all.
const double kCaptureRate = 48000.0;
AVAudioConverter *g_capture_converter = nil;
AVAudioFormat *g_capture_format = nil;
bool g_capture_tapped = false;

// Feed capture with the audio actually rendered by the mixer. Resample device-rate
// buffers when necessary instead of labeling them with a different sample rate.
void capture_block(AVAudioPCMBuffer *buffer) {
    if (!buffer || !host_capture_active())
        return;
    int busy = (int)host_audio_playing_channels();
    if (!g_capture_converter) {
        if (buffer.format.commonFormat != AVAudioPCMFormatFloat32)
            return;
        const float *l = buffer.floatChannelData ? buffer.floatChannelData[0] : nullptr;
        const float *r = buffer.format.channelCount > 1 ? buffer.floatChannelData[1] : nullptr;
        host_capture_write(l, r, buffer.frameLength, busy);
        return;
    }
    // The device does not have to run at the rate the capture is written at,
    // so what the tap hands over is resampled rather than relabelled.
    AVAudioFrameCount capacity =
        (AVAudioFrameCount)(buffer.frameLength * kCaptureRate / buffer.format.sampleRate) + 64;
    AVAudioPCMBuffer *out = [[AVAudioPCMBuffer alloc] initWithPCMFormat:g_capture_format
                                                          frameCapacity:capacity];
    if (!out)
        return;
    __block AVAudioPCMBuffer *input = buffer;
    NSError *error = nil;
    AVAudioConverterOutputStatus status =
        [g_capture_converter convertToBuffer:out
                                       error:&error
                          withInputFromBlock:^AVAudioBuffer *(AVAudioPacketCount need,
                                                              AVAudioConverterInputStatus *status) {
                            (void)need;
                            if (!input) {
                                *status = AVAudioConverterInputStatus_NoDataNow;
                                return nil;
                            }
                            *status = AVAudioConverterInputStatus_HaveData;
                            AVAudioPCMBuffer *give = input;
                            input = nil;
                            return give;
                          }];
    if (status == AVAudioConverterOutputStatus_Error || !out.frameLength)
        return;
    host_capture_write(out.floatChannelData[0],
                       out.format.channelCount > 1 ? out.floatChannelData[1] : nullptr,
                       out.frameLength, busy);
}

// Start a mixed-output capture, attaching a tap for live playback. Offline rendering
// feeds capture directly and must not install a second path that duplicates each block.
extern "C" int host_audio_capture_begin(const char *path) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    {
        DataLock held;
        if (!ensure_engine())
            return 0;
    }
    if (!g_engine)
        return 0;
    if (!host_capture_open(path, (uint32_t)kCaptureRate))
        return 0;

    // Offline: the render is the capture, and host_audio_offline_render feeds
    // it. Nothing to tap, and a tap on a manually rendered graph would double
    // every block.
    if (g_engine.manualRenderingMode == AVAudioEngineManualRenderingModeOffline)
        return 1;

    AVAudioFormat *bus = [g_engine.mainMixerNode outputFormatForBus:0];
    g_capture_format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kCaptureRate
                                                                      channels:2];
    g_capture_converter = nil;
    if (bus.sampleRate != kCaptureRate || bus.channelCount != 2)
        g_capture_converter = [[AVAudioConverter alloc] initFromFormat:bus
                                                              toFormat:g_capture_format];
    [g_engine.mainMixerNode installTapOnBus:0
                                 bufferSize:4096
                                     format:nil
                                      block:^(AVAudioPCMBuffer *buffer, AVAudioTime *when) {
                                        (void)when;
                                        capture_block(buffer);
                                      }];
    g_capture_tapped = true;
    printf("[host] audio capture: %s at %.0f Hz, from a tap on the mixer "
           "running at %.0f Hz\n",
           path ? path : "(measured, not written)", kCaptureRate, bus.sampleRate);
    fflush(stdout);
    return 1;
}

extern "C" void host_audio_capture_end(void) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    if (g_capture_tapped && g_engine) {
        [g_engine.mainMixerNode removeTapOnBus:0];
        g_capture_tapped = false;
    }
    g_capture_converter = nil;
    g_capture_format = nil;
    host_capture_close();
}

// --- offline rendering, which is how a test hears anything -------------------
//
// A test that wants to know whether the synth made a sound cannot open the
// audio hardware to find out: there may be none, and a test that plays out of
// the speakers is a test nobody will run twice. Manual rendering mode runs the
// same graph into a buffer instead of into a device, so what a test measures is
// what the engine would have played.
AVAudioPCMBuffer *g_offline_buffer = nil;

// Switch the graph to manual rendering for deterministic audio tests. Playback cursors
// then advance with rendered samples, so this mode must be established before Play.
extern "C" int host_audio_offline_begin(double sample_rate, uint32_t max_frames) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    {
        DataLock held;
        if (!ensure_engine())
            return 0;
    }
    if (!g_engine)
        return 0;
    if (g_engine.isRunning)
        [g_engine stop];
    if (g_engine.manualRenderingMode == AVAudioEngineManualRenderingModeOffline &&
        g_offline_buffer) {
        g_manual_render.store(true, std::memory_order_release);
        return 1;
    }
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sample_rate
                                                                           channels:2];
    NSError *error = nil;
    if (![g_engine enableManualRenderingMode:AVAudioEngineManualRenderingModeOffline
                                      format:format
                           maximumFrameCount:max_frames
                                       error:&error]) {
        fprintf(stderr, "[host] offline rendering refused: %s\n",
                error.localizedDescription.UTF8String);
        return 0;
    }
    // Manual rendering replaces the output's format, so the clipper has to be
    // put back on the output's side of the conversion.
    connect_clipper_locked();
    g_offline_buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:g_engine.manualRenderingFormat
                                                     frameCapacity:max_frames];
    if (!g_offline_buffer)
        return 0;
    g_render_rate = g_engine.manualRenderingFormat.sampleRate;
    if (g_render_rate <= 0.0)
        g_render_rate = sample_rate > 0 ? sample_rate : 48000.0;
    // From here the play cursors count rendered audio rather than seconds.
    // Set before anything is played, so no channel straddles the two clocks.
    g_rendered_frames.store(0, std::memory_order_release);
    g_manual_render.store(true, std::memory_order_release);
    return 1;
}

extern "C" uint32_t host_audio_offline_render(uint32_t frames, float *peak) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    if (peak)
        *peak = 0.0f;
    if (!g_engine || !g_offline_buffer)
        return 0;
    if (!g_engine.isRunning) {
        NSError *error = nil;
        if (![g_engine startAndReturnError:&error]) {
            fprintf(stderr, "[host] offline engine would not start: %s\n",
                    error.localizedDescription.UTF8String);
            return 0;
        }
    }
    g_render_rate = g_engine.manualRenderingFormat.sampleRate;
    if (g_render_rate <= 0.0)
        g_render_rate = 48000.0;
    uint32_t done = 0;
    float loudest = 0.0f;
    while (done < frames) {
        AVAudioFrameCount want = (AVAudioFrameCount)(frames - done);
        if (want > g_offline_buffer.frameCapacity)
            want = g_offline_buffer.frameCapacity;
        NSError *error = nil;
        AVAudioEngineManualRenderingStatus status = [g_engine renderOffline:want
                                                                   toBuffer:g_offline_buffer
                                                                      error:&error];
        if (status != AVAudioEngineManualRenderingStatusSuccess)
            break;
        AVAudioFrameCount got = g_offline_buffer.frameLength;
        if (!got)
            break;
        if (host_capture_active()) {
            uint32_t busy = 0;
            {
                DataLock held;
                if (g_channels)
                    for (const auto &entry : *g_channels)
                        if (entry.second.playing)
                            ++busy;
            }
            host_capture_write(g_offline_buffer.floatChannelData[0],
                               g_offline_buffer.format.channelCount > 1
                                   ? g_offline_buffer.floatChannelData[1]
                                   : nullptr,
                               got, (int)busy);
        }
        for (AVAudioChannelCount ch = 0; ch < g_offline_buffer.format.channelCount; ++ch) {
            const float *p = g_offline_buffer.floatChannelData[ch];
            for (AVAudioFrameCount i = 0; i < got; ++i) {
                float a = p[i] < 0 ? -p[i] : p[i];
                if (a > loudest)
                    loudest = a;
            }
        }
        done += got;
        g_rendered_frames.fetch_add(got, std::memory_order_release);
    }
    if (peak)
        *peak = loudest;
    return done;
}

// Time that will not be rendered, counted anyway. A host that has fallen a long
// way behind - a slow load, a debugger, a machine doing something else - cannot
// catch up by rendering minutes of audio nobody was there to hear, and if it
// does not count the time at all every play cursor stops with it. So the time
// is skipped: the cursors move, and the audio that was never rendered is audio
// that could never have been heard.
extern "C" void host_audio_offline_skip(uint32_t frames) {
    if (!frames)
        return;
    g_rendered_frames.fetch_add(frames, std::memory_order_release);
}

extern "C" void host_audio_offline_end(void) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    g_manual_render.store(false, std::memory_order_release);
    if (!g_engine)
        return;
    if (g_engine.isRunning)
        [g_engine stop];
    [g_engine disableManualRenderingMode];
    g_offline_buffer = nil;
    // NOT reconnected here, and that is a gap rather than a decision. Leaving
    // manual rendering puts the engine back on the device while the mixer,
    // clipper and output stay pinned to the rate the offline render used, so
    // anything played after an offline session runs through edges chosen for a
    // rate that is no longer in force. Nothing in this process does that today
    // - the hosts that render offline never go back to a device - so it is
    // recorded rather than fixed blind. A run that did would need
    // connect_clipper_locked() here and a test that plays after offline_end.
}

extern "C" void host_audio_play(const HostAudioPlay *p) {
    if (!p || !p->pcm || !p->bytes)
        return;
    std::lock_guard<std::mutex> api(g_api_mutex);

    // The loudest sample in what is being submitted. The game streams its
    // music by playing a looping buffer of silence and refilling it, so the
    // moment that buffer stops being silent is the moment the music should
    // start - and if nothing is heard after it, the fault is downstream of
    // here rather than in the data.
    double peak = 0.0;
    {
        const uint8_t *pcm = (const uint8_t *)p->pcm;
        int bits = p->bits == 8 ? 8 : 16;
        uint32_t step = bits == 8 ? 1u : 2u;
        for (uint32_t i = 0; i + step <= p->bytes; i += step) {
            double v = bits == 8 ? (double)pcm[i] - 128.0
                                 : (double)(int16_t)(uint16_t)(pcm[i] | (pcm[i + 1] << 8));
            double a = (v < 0 ? -v : v) / (bits == 8 ? 128.0 : 32768.0);
            if (a > peak)
                peak = a;
        }
    }

    Job job;
    bool was_silent_loop = false;
    {
        DataLock held;
        Channel *channel = channel_for(p->channel, true);
        if (!channel)
            return;
        channel->channels = p->channels == 2 ? 2 : 1;
        channel->bits = p->bits == 8 ? 8 : 16;
        channel->base_rate = (uint32_t)(p->sample_rate > 0 ? p->sample_rate : 22050);
        channel->rate = host_audio_play_rate(channel->base_rate, channel->rate,
                                             channel->rate_overridden ? 1 : 0);
        channel->loop = p->loop != 0;
        channel->volume_mb = p->volume;
        channel->pan_mb = p->pan;
        // A looping channel being re-submitted has to STOP and reschedule: a
        // buffer scheduled with the loop option keeps playing the bytes it was
        // given, so a refill that only rewrote the guest's memory would never
        // be heard. perform() stops the node before it schedules, which is what
        // makes a refill audible; this records that it happened.
        was_silent_loop = channel->playing && channel->loop && channel->silent;
        channel->silent = peak <= 0.0001;
        if (was_silent_loop && !channel->silent) {
            printf("[host] audio channel %d was looping silence and has been "
                   "refilled with sound (peak %.3f); it is stopped and "
                   "rescheduled so the new content plays\n",
                   p->channel, peak);
            fflush(stdout);
        }
        channel->pcm.assign((const uint8_t *)p->pcm, (const uint8_t *)p->pcm + p->bytes);
        // A Play replaces whatever was on the channel, stream included, and the
        // queue accounting starts again with it.
        channel->streaming = false;
        channel->ring_mode = false;
        channel->ring = nil;
        if (p->channel >= 0 && p->channel < MAX_AUDIO_CHANNELS) {
            g_queued_total[p->channel].store(0, std::memory_order_release);
            g_queued_played[p->channel].store(0, std::memory_order_release);
        }
        job = plan_locked(p->channel, *channel, p->start_offset);
    }
    // Stamped against the capture's own clock, so a run can be asked
    // afterwards which of the sounds it was told to play were ever heard.
    host_capture_note_play(p->channel);
    trace("play    ch %d  %u Hz %d ch %d bit  %u bytes from %u%s  peak %.3f", p->channel,
          (unsigned)(p->sample_rate ? p->sample_rate : 22050), p->channels == 2 ? 2 : 1,
          p->bits == 8 ? 8 : 16, p->bytes, p->start_offset, p->loop ? "  looping" : "", peak);
    perform(job);
}

extern "C" void host_audio_stop(int32_t id) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    AVAudioPlayerNode *node = nil;
    uint64_t sample_time = 0;
    bool have_time = false;
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        if (!channel)
            return;
        node = channel->node;
    }
    // Read where it reached before stopping it, and do both without the lock.
    have_time = node_sample_time(id, node, &sample_time);
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        if (!channel)
            return;
        // The cursor is what a DirectSound buffer keeps across a Stop, and what
        // the next Play resumes from.
        if (have_time)
            channel->cursor = host_audio_position_bytes(
                sample_time, channel->start_offset, (uint32_t)channel->pcm.size(), channel->bits,
                channel->channels, channel->loop ? 1 : 0);
        ++channel->generation; // orphan any completion in flight
        channel->playing = false;
    }
    audio_check_unlocked("stopping a node");
    if (g_ops) {
        if (g_ops->stop)
            g_ops->stop(id, 0);
        return;
    }
    [node stop];
}

extern "C" void host_audio_set_volume(int32_t id, int32_t volume) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    AVAudioPlayerNode *node = nil;
    float gain = 1.0f;
    {
        DataLock held;
        Channel *channel = channel_for(id, true);
        if (!channel)
            return;
        channel->volume_mb = volume;
        node = channel->node;
        gain = host_audio_gain_from_millibels(volume);
    }
    audio_check_unlocked("setting a node's volume");
    node.volume = gain;
}

// Pan and frequency both live in the samples or in the buffer's rate, so a live
// change re-schedules the rest of the sound from where it had reached. The
// sound does not restart. The node time is read, and the new scheduling is
// performed, with the lock released.
static void reschedule_from_current(int32_t id, void (^apply)(Channel &)) {
    AVAudioPlayerNode *node = nil;
    bool playing = false;
    {
        DataLock held;
        Channel *channel = channel_for(id, true);
        if (!channel)
            return;
        apply(*channel);
        reconcile_locked(id, *channel);
        node = channel->node;
        playing = channel->playing && !channel->pcm.empty();
    }
    if (!playing)
        return;
    uint64_t sample_time = 0;
    bool have_time = node_sample_time(id, node, &sample_time);
    Job job;
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        if (!channel)
            return;
        uint32_t at = channel->cursor;
        if (have_time)
            at = host_audio_position_bytes(sample_time, channel->start_offset,
                                           (uint32_t)channel->pcm.size(), channel->bits,
                                           channel->channels, channel->loop ? 1 : 0);
        job = plan_locked(id, *channel, at);
    }
    perform(job);
}

extern "C" void host_audio_set_pan(int32_t id, int32_t pan) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    __block bool changed = false;
    reschedule_from_current(id, ^(Channel &ch) {
      changed = ch.pan_mb != pan;
      ch.pan_mb = pan;
      if (!changed)
          ch.playing = false; // nothing to redo
    });
}

extern "C" void host_audio_set_frequency(int32_t id, uint32_t hz) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    reschedule_from_current(id, ^(Channel &ch) {
      // DSBFREQUENCY_ORIGINAL: go back to the rate the buffer was created
      // with, and stop overriding it, so a later Play of a differently-rated
      // buffer uses its own rate too.
      ch.rate_overridden = hz != 0;
      uint32_t rate = hz ? hz : ch.base_rate;
      if (rate == ch.rate)
          ch.playing = false; // nothing to redo
      ch.rate = rate;
    });
}

// The play cursor, and whether the channel is still going. Callers hold the
// api mutex and no data lock.
//
// The node's own position is used when it is actually reporting one. When it is
// not - stopped, engine not running, or a sample time still at zero - the
// cursor is modelled from the wall clock at the buffer's byte rate.
//
// That fallback is not a nicety. A DirectSound play cursor advances from the
// moment Play was called whether or not anything is audible, and the game's
// video player at 0057a9a0 queues a chunk, polls GetCurrentPosition and sleeps
// until the cursor has moved far enough to queue the next one. A cursor that
// stops because the audio backend is not running stops the video with it, and
// the front end freezes with neither sound nor picture.
static uint32_t audio_advance(int32_t id, bool *playing_out) {
    if (playing_out)
        *playing_out = false;
    AVAudioPlayerNode *node = nil;
    {
        DataLock held;
        Channel *channel = channel_for(id, false);
        if (!channel)
            return 0;
        reconcile_locked(id, *channel);
        if (!channel->playing)
            return channel->cursor;
        node = channel->node;
    }
    uint64_t sample_time = 0;
    bool have_time = node_sample_time(id, node, &sample_time);

    DataLock held;
    Channel *channel = channel_for(id, false);
    if (!channel)
        return 0;
    uint32_t total = (uint32_t)channel->pcm.size();
    uint32_t next;
    if (have_time && sample_time > 0) {
        next = host_audio_position_bytes(sample_time, channel->start_offset, total, channel->bits,
                                         channel->channels, channel->loop ? 1 : 0);
    } else {
        uint32_t rate = channel->rate ? channel->rate : channel->base_rate;
        next = host_audio_wall_clock_bytes(audio_clock() - channel->started, rate, channel->bits,
                                           channel->channels, channel->start_offset, total,
                                           channel->loop ? 1 : 0);
    }
    // A stream is refilled rather than finished, so it never runs off an end.
    if (channel->streaming) {
        channel->cursor = next;
        if (playing_out)
            *playing_out = true;
        return next;
    }
    // A one-shot that has run off the end is over, whichever model said so:
    // waiting for a completion handler that nothing will send would leave the
    // guest asking about a sound that finished long ago.
    if (!channel->loop && next >= total) {
        channel->playing = false;
        channel->cursor = total;
        return total;
    }
    channel->cursor = next;
    if (playing_out)
        *playing_out = true;
    return next;
}

extern "C" uint32_t host_audio_position(int32_t id) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    return audio_advance(id, nullptr);
}

extern "C" int32_t host_audio_is_playing(int32_t id) {
    std::lock_guard<std::mutex> api(g_api_mutex);
    bool playing = false;
    audio_advance(id, &playing);
    return playing ? 1 : 0;
}
