// midi.mm - the game's music, which is MIDI through a SoundFont.
//
// Populous has no streamed music. It looks for a midiOut device whose name
// begins with "SoundFont" (0x575e40 walks the devices comparing nine
// characters against the literal at 0x5eb5b0), opens it, sends a twelve-byte
// sysex, and then plays the score note by note with midiOutShortMsg. The bank
// it means is Sound/POPFIGHT.SF2, which ships with the game; on Windows it
// would have been loaded by Creative's SFMAN32.DLL into a SoundFont-capable
// card or its software synth. Nothing in the game renders a note itself, so
// with no host synth there is simply no music.
//
// The synth here is an audio unit on the same AVAudioEngine that plays the
// sound effects. It is not a second engine: two engines means two output
// devices competing for the same hardware, and the one that loses is silent
// without saying why.
//
// WHY NOT AVAudioUnitSampler
//
// AVAudioUnitSampler loads a SoundFont in one line, which makes it the obvious
// choice, and it is the wrong one here: it is mono-timbral. Every MIDI channel
// plays the instrument it was last given, so a sixteen-channel score comes out
// as sixteen parts played by one instrument and the drums come out as pitched
// notes of it. The units below are multi-timbral - a program change on one
// channel leaves the other fifteen alone, and channel 10 is percussion -
// which is what a score written for a General MIDI device expects.
//
// Three of them are tried in turn, because a bank that one refuses another
// often takes:
//
//   1. AUMIDISynth with POPFIGHT.SF2. The modern multi-timbral synth.
//   2. AUDLSSynth with POPFIGHT.SF2. The older one, which reads .sf2 and .dls
//      through the same property.
//   3. AUDLSSynth with its own built-in General MIDI bank, for a run whose
//      Sound directory is missing or whose bank will not load. The music is
//      then in the wrong voices but it is there, and a wrong instrument is a
//      complaint where silence is a bug report.
//
// What runs is printed, so a run that sounds wrong says which of the three it
// got before anybody has to guess.
//
// WHEN THE BANK IS LOADED, AND WHY THERE IS NO LOCK IN THIS FILE
//
// Every host_midi_* call except the first arrives on a guest thread holding the
// cooperative scheduler baton, which stops every other guest thread until it
// returns. Parsing a SoundFont there would freeze the whole game at the moment
// the music starts, with nothing anywhere pointing at MIDI.
//
// So the synth is built at host startup - host_midi_startup, called on the main
// thread after the executable is loaded and before the guest entry point runs.
// There is no guest thread then, and no baton. midiOutOpen afterwards only
// publishes what already exists.
//
// That is also why there is no mutex here, and the absence is the design rather
// than an oversight:
//
//   * g_synth, g_engine and g_which are written once, by host_midi_startup, on
//     the main thread with no guest running, and published with a release
//     store to g_ready. Nothing writes them again.
//   * open, close, short, sysex and reset all run on guest threads, which the
//     baton serialises against each other.
//   * host_midi_short therefore takes nothing at all, which matters: it is
//     called per note, thousands of times a second while music plays.
//
// The rule that a lock here would have broken, and which the shape above makes
// unbreakable: THE AUDIO LAYER IS NEVER CALLED WITH A SYNTH LOCK HELD. There is
// no lock here to hold, so there is no synth-before-audio order to violate, and
// the matching half is stated in audio.mm: nothing there may call a host_midi_
// function while holding its own lock. Both halves have to stay true; either
// one alone is only half of an ABBA deadlock. Building
// the synth attaches and connects nodes and starts the engine, each of which
// takes audio.mm's own API mutex. A synth mutex held across that would be a
// lock order - synth before audio - that becomes a deadlock the first time the
// audio layer calls a MIDI function under its own lock. audio.mm's
// audio_check_unlocked catches that inversion for its own lock and cannot see
// one taken here.
#include "audio.h"
#include "midi.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <stdio.h>
#include <string.h>

namespace {

// Written once by host_midi_startup, before any guest thread exists.
AVAudioUnitMIDIInstrument *g_synth = nil;
AVAudioEngine *g_engine = nil;
const char *g_which = "";

// g_ready publishes the three above. g_live is the game's own open device: a
// synth that is built but not opened accepts nothing, and one that has been
// closed goes back to accepting nothing without being torn down.
std::atomic<bool> g_ready{false};
std::atomic<bool> g_live{false};

std::atomic<uint32_t> g_shorts{0}, g_sysexes{0}, g_notes_on{0};

// A short message is one, two or three bytes and the status byte says which.
// 0x80..0xbf and 0xe0..0xef carry two data bytes; 0xc0..0xdf (program change
// and channel pressure) carry one. Sending a data byte the unit is not
// expecting is not harmless: it is the next note in the wrong place.
int message_length(uint8_t status) {
    if (status < 0x80)
        return 0;
    uint8_t high = (uint8_t)(status & 0xf0);
    if (high == 0xc0 || high == 0xd0)
        return 2;
    if (high == 0xf0)
        return 0; // system messages do not come this way
    return 3;
}

AVAudioUnitMIDIInstrument *make_unit(OSType subtype) {
    AudioComponentDescription desc;
    memset(&desc, 0, sizeof desc);
    desc.componentType = kAudioUnitType_MusicDevice;
    desc.componentSubType = subtype;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    return [[AVAudioUnitMIDIInstrument alloc] initWithAudioComponentDescription:desc];
}

// The bank has to be set before the unit is initialized, which is before the
// engine is started with it attached. Setting it later is accepted and then
// ignored, which sounds exactly like a bank that did not load.
bool load_bank(AVAudioUnitMIDIInstrument *unit, const char *path) {
    if (!unit || !path || !*path)
        return false;
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    if (!url)
        return false;
    CFURLRef ref = (__bridge CFURLRef)url;
    OSStatus status = AudioUnitSetProperty(unit.audioUnit, kMusicDeviceProperty_SoundBankURL,
                                           kAudioUnitScope_Global, 0, &ref, sizeof ref);
    return status == noErr;
}

// Attaches the unit and starts the engine with it in the graph. Returns false
// if either refuses, having taken the unit back out so the next candidate
// starts from the graph it expected.
bool attach(AVAudioUnitMIDIInstrument *unit) {
    if (!unit || !g_engine)
        return false;
    [g_engine attachNode:unit];
    [g_engine connect:unit to:g_engine.mainMixerNode format:nil];
    host_audio_engine_run();
    // In manual rendering mode a test starts the engine itself, so a stopped
    // engine here is only a failure when there is a device to start.
    if (!g_engine.isRunning &&
        g_engine.manualRenderingMode != AVAudioEngineManualRenderingModeOffline) {
        [g_engine detachNode:unit];
        return false;
    }
    return true;
}

// Everything off, on all sixteen channels. A synth that is closed or reset with
// notes still held holds them for ever: there is nothing left to send the
// note-off to.
void all_off() {
    if (!g_synth)
        return;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        [g_synth sendMIDIEvent:(UInt8)(0xb0 | ch) data1:0x78 data2:0]; // all sound off
        [g_synth sendMIDIEvent:(UInt8)(0xb0 | ch) data1:0x7b data2:0]; // all notes off
        [g_synth sendMIDIEvent:(UInt8)(0xb0 | ch) data1:0x79 data2:0]; // controllers
        [g_synth sendMIDIEvent:(UInt8)(0xe0 | ch) data1:0 data2:64];   // bend centred
    }
}

} // namespace

extern "C" int host_midi_startup(const char *sf2_path) {
    if (g_ready.load(std::memory_order_acquire))
        return 1;

    g_engine = (__bridge AVAudioEngine *)host_audio_engine();
    if (!g_engine) {
        printf("[host] midi: there is no audio engine, so there is no music\n");
        fflush(stdout);
        return 0;
    }

    struct Candidate {
        OSType subtype;
        bool bank;
        const char *name;
    };
    const Candidate candidates[] = {
        {kAudioUnitSubType_MIDISynth, true, "AUMIDISynth with the game's SoundFont"},
        {kAudioUnitSubType_DLSSynth, true, "AUDLSSynth with the game's SoundFont"},
        {kAudioUnitSubType_DLSSynth, false, "AUDLSSynth with Apple's General MIDI bank"},
    };
    for (const Candidate &c : candidates) {
        if (c.bank && (!sf2_path || !*sf2_path))
            continue;
        AVAudioUnitMIDIInstrument *unit = make_unit(c.subtype);
        if (!unit)
            continue;
        if (c.bank && !load_bank(unit, sf2_path))
            continue;
        if (!attach(unit))
            continue;
        g_synth = unit;
        g_which = c.name;
        // Release: everything the guest threads will read is written above it.
        g_ready.store(true, std::memory_order_release);
        printf("[host] midi: %s%s%s\n", c.name, c.bank ? ", from " : "", c.bank ? sf2_path : "");
        fflush(stdout);
        return 1;
    }

    printf("[host] midi: no synth would start%s, so the music is accepted and "
           "not heard\n",
           sf2_path && *sf2_path ? "" : " and no SoundFont was found");
    fflush(stdout);
    return 0;
}

// midiOutOpen. The synth is already built and attached; this only hands it to
// the game. Nothing here parses anything, which is the whole point: it runs on
// a guest thread holding the baton.
extern "C" int host_midi_open(const char *sf2_path) {
    if (!g_ready.load(std::memory_order_acquire)) {
        // The shim still resolves the bank and passes it, which is right: it
        // keeps the contract honest for a host that has no startup hook. This
        // host has one, and will not parse a SoundFont on a guest thread
        // holding the scheduler baton just because the path arrived here. So
        // the path is named in the complaint rather than acted on.
        static bool told = false;
        if (!told) {
            told = true;
            printf("[host] midi: no synth was built at startup%s%s, so the music "
                   "is accepted and not heard; a host that wants it calls "
                   "host_midi_startup before the guest runs\n",
                   sf2_path && *sf2_path ? " and the bank offered here is " : "",
                   sf2_path && *sf2_path ? sf2_path : "");
            fflush(stdout);
        }
        return 0;
    }
    g_shorts.store(0, std::memory_order_relaxed);
    g_sysexes.store(0, std::memory_order_relaxed);
    g_notes_on.store(0, std::memory_order_relaxed);
    // Release, and it is load-bearing for a variable it does not mention.
    // host_midi_short synchronises on g_live and not on g_ready, so its view of
    // g_synth arrives by a chain: startup writes g_synth and releases g_ready,
    // this acquires g_ready and releases g_live, the note acquires g_live.
    // Transitive happens-before carries the pointer all the way. Relaxing this
    // store - which looks harmless, g_live being a bool - breaks the
    // publication of g_synth and sends a note through a null pointer on some
    // machines and not others. impl-t6's reading, and it is right.
    g_live.store(true, std::memory_order_release);
    return 1;
}

extern "C" void host_midi_short(uint32_t msg) {
    // No lock, no allocation, no engine call. Per note, thousands a second.
    if (!g_live.load(std::memory_order_acquire))
        return;
    uint8_t status = (uint8_t)(msg & 0xff);
    uint8_t data1 = (uint8_t)((msg >> 8) & 0x7f);
    uint8_t data2 = (uint8_t)((msg >> 16) & 0x7f);
    int length = message_length(status);
    if (!length)
        return;
    g_shorts.fetch_add(1, std::memory_order_relaxed);
    // A note-on with velocity zero is a note-off, which is how most scores end
    // a note; counting it as one on would say the music is playing for ever.
    if ((status & 0xf0) == 0x90 && data2)
        g_notes_on.fetch_add(1, std::memory_order_relaxed);
    if (length == 2)
        [g_synth sendMIDIEvent:status data1:data1];
    else
        [g_synth sendMIDIEvent:status data1:data1 data2:data2];
}

extern "C" void host_midi_sysex(const void *data, uint32_t bytes) {
    if (!g_live.load(std::memory_order_acquire) || !data || !bytes)
        return;
    g_sysexes.fetch_add(1, std::memory_order_relaxed);
    const uint8_t *p = (const uint8_t *)data;
    // The General MIDI reset, which every synth understands and which means
    // exactly what all_off_locked does.
    static const uint8_t gm_reset[] = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
    if (bytes >= sizeof gm_reset && !memcmp(p, gm_reset, sizeof gm_reset)) {
        all_off();
        return;
    }
    // Everything else is a manufacturer's message for a card this is not. The
    // twelve bytes this game sends are for Creative's SoundFont manager, and
    // passing them to an Apple synth asks it to interpret a private message it
    // has never seen. Accepted and dropped, once said out loud.
    static bool told = false;
    if (!told) {
        told = true;
        printf("[host] midi: a %u-byte sysex for another maker's synth was "
               "accepted and dropped (%02x %02x %02x %02x)\n",
               bytes, p[0], bytes > 1 ? p[1] : 0, bytes > 2 ? p[2] : 0, bytes > 3 ? p[3] : 0);
        fflush(stdout);
    }
}

extern "C" void host_midi_reset(void) {
    if (!g_live.load(std::memory_order_acquire))
        return;
    all_off();
}

// midiOutClose. The synth stays built and attached: it is the host's, not the
// game's, and a game that closes and reopens its device should get the same one
// back rather than pay for another SoundFont parse on a guest thread.
extern "C" void host_midi_close(void) {
    if (!g_live.exchange(false, std::memory_order_acq_rel))
        return;
    all_off();
    printf("[host] midi: closed after %u messages and %u sysexes, %u notes "
           "started\n",
           g_shorts.load(std::memory_order_relaxed), g_sysexes.load(std::memory_order_relaxed),
           g_notes_on.load(std::memory_order_relaxed));
    fflush(stdout);
}

extern "C" int host_midi_is_open(void) {
    return g_live.load(std::memory_order_acquire) ? 1 : 0;
}

// A string literal that outlives everything, so the caller may hold it.
extern "C" const char *host_midi_synth_name(void) {
    return g_ready.load(std::memory_order_acquire) ? g_which : "";
}

extern "C" uint32_t host_midi_notes_started(void) {
    return g_notes_on.load(std::memory_order_relaxed);
}
