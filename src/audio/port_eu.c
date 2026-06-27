#include <ultra64.h>
#include <stdio.h>
#include <stdarg.h>
#include "internal.h"
#include "load.h"
#include "data.h"
#include "seqplayer.h"
#include "synthesis.h"
#include "heap.h"

#ifdef VERSION_EU

// ---------------------------------------------------------------------------
// EU audio debug logging (temporary diagnostic instrumentation)
// Writes to "eu_audio_debug.log" in the current working directory.
// gCurrAudioFrameDmaCount comes from data.h; gSeqLoadStatus/gBankLoadStatus
// from heap.h (both already included above).
// ---------------------------------------------------------------------------
static FILE *sEuAudioLog = NULL;

void eu_audio_log(const char *fmt, ...) {
    if (sEuAudioLog == NULL) {
        sEuAudioLog = fopen("eu_audio_debug.log", "w");
        if (sEuAudioLog == NULL) {
            return;
        }
        fprintf(stderr, "[eu_audio] writing diagnostics to eu_audio_debug.log\n");
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(sEuAudioLog, fmt, ap);
    va_end(ap);
    fflush(sEuAudioLog); // flush every line so a crash still leaves a record
}

// Producer/consumer counters for the OSMesgQueues[1] command pipeline.
u32 gEuDbgFlushCount = 0; // times func_802ad7a0() flushed a command range
u32 gEuDbgDrainCount = 0; // times create_next_audio_buffer() drained one

// alloc_note() request/failure counters (defined in seqplayer.c).
extern u32 gEuDbgAllocCalls;
extern u32 gEuDbgAllocNull;

#ifdef __sgi
#define stubbed_printf
#else
#define stubbed_printf(...)
#endif

#define SAMPLES_TO_OVERPRODUCE 0x10
#define EXTRA_BUFFERED_AI_SAMPLES_TARGET 0x40

#ifdef VERSION_JP
typedef u16 FadeT;
#else
typedef s32 FadeT;
#endif

extern volatile u8 gAudioResetStatus;
extern u8 gAudioResetPresetIdToLoad;
extern OSMesgQueue *OSMesgQueues[];
extern struct EuAudioCmd sAudioCmd[0x100];

void func_8031D690(s32 player, FadeT fadeInTime);
void sequence_player_fade_out_internal(s32 player, FadeT fadeOutTime);
void port_eu_init_queues(void);
void decrease_sample_dma_ttls(void);
s32 audio_shut_down_and_reset_step(void);
void func_802ad7ec(u32);

struct SPTask *create_next_audio_frame_task(void) {
    return NULL;
}
void create_next_audio_buffer(s16 *samples, u32 num_samples) {
    s32 writtenCmds;
    OSMesg msg;
    gAudioFrameCount++;
    // On the N64 this per-audio-frame counter is awaited and zeroed inside
    // create_next_audio_frame_task(), which this port stubs out to return NULL.
    // Without the reset, dma_sample_data() keeps incrementing it and indexes
    // gCurrAudioFrameDmaIoMesgBufs[]/gCurrAudioFrameDmaMesgBufs[] (each of size
    // AUDIO_FRAME_DMA_QUEUE_SIZE == 0x40) out of bounds after ~1-2 s, corrupting
    // adjacent audio memory and silencing output. The port's DMA is a synchronous
    // memcpy, so there is nothing to await here -- just reset the counter.
    gCurrAudioFrameDmaCount = 0;
    decrease_sample_dma_ttls();
    if (osRecvMesg(OSMesgQueues[2], &msg, 0) != -1) {
        gAudioResetPresetIdToLoad = (u8) (s32) msg;
        gAudioResetStatus = 5;
        eu_audio_log("RESET recv preset=%d frame=%u\n",
                     (int) (u8) (s32) msg, (unsigned) gAudioFrameCount);
    }

    if (gAudioResetStatus != 0) {
        eu_audio_log("RESET run status=%d preset=%d frame=%u\n",
                     (int) gAudioResetStatus, (int) gAudioResetPresetIdToLoad,
                     (unsigned) gAudioFrameCount);
        audio_reset_session();
        gAudioResetStatus = 0;
    }
    if (osRecvMesg(OSMesgQueues[1], &msg, OS_MESG_NOBLOCK) != -1) {
        gEuDbgDrainCount++;
        func_802ad7ec((u32) msg);
    }
    synthesis_execute(gAudioCmdBuffers[0], &writtenCmds, samples, num_samples);
    gAudioRandom = ((gAudioRandom + gAudioFrameCount) * gAudioFrameCount);
    gAudioRandom = gAudioRandom + writtenCmds / 8;

    // Measure the actual PCM that synthesis just produced (stereo s16) and how
    // many notes are live. This separates "synthesis went silent" from a
    // downstream/output problem, and pinpoints the exact cutout frame.
    s32 maxAmp = 0;
    {
        u32 si;
        for (si = 0; si < num_samples * 2; si++) {
            s32 v = samples[si];
            if (v < 0) v = -v;
            if (v > maxAmp) maxAmp = v;
        }
    }
    s32 activeNotes = 0;
    {
        s32 ni;
        for (ni = 0; ni < gMaxSimultaneousNotes; ni++) {
            if (gNotes[ni].noteSubEu.enabled) {
                activeNotes++;
            }
        }
    }
    {
        static s32 sPrevSilent = -1;
        s32 isSilent = (maxAmp == 0);
        if (isSilent != sPrevSilent) {
            eu_audio_log("OUTPUT %s frame=%u maxAmp=%d activeNotes=%d\n",
                         isSilent ? "SILENT" : "active", (unsigned) gAudioFrameCount,
                         (int) maxAmp, (int) activeNotes);
            sPrevSilent = isSilent;
        }
    }

    // Heartbeat every 32 audio buffers (~0.5s) snapshotting the seq players.
    if ((gAudioFrameCount & 0x1f) == 0) {
        s32 p;
        eu_audio_log("HB frame=%u dmaCnt=%d reset=%d q1valid=%d flush=%u drain=%u written=%d maxAmp=%d notes=%d allocCalls=%u allocNull=%u\n",
                     (unsigned) gAudioFrameCount, (int) gCurrAudioFrameDmaCount,
                     (int) gAudioResetStatus, (int) OSMesgQueues[1]->validCount,
                     (unsigned) gEuDbgFlushCount, (unsigned) gEuDbgDrainCount,
                     (int) writtenCmds, (int) maxAmp, (int) activeNotes,
                     (unsigned) gEuDbgAllocCalls, (unsigned) gEuDbgAllocNull);
        for (p = 0; p < SEQUENCE_PLAYERS; p++) {
            struct SequencePlayer *sp = &gSequencePlayers[p];
            eu_audio_log("  SP%d en=%d mute=%d st=%d seqId=%d bank=%d seqDma=%d bankDma=%d "
                         "fadeVol=%.4f fadeVel=%.5f tempo=%d tempoAcc=%u seqLd=%d bankLd=%d\n",
                         (int) p, (int) sp->enabled, (int) sp->muted, (int) sp->state,
                         (int) sp->seqId, (int) sp->defaultBank[0],
                         (int) sp->seqDmaInProgress, (int) sp->bankDmaInProgress,
                         (double) sp->fadeVolume, (double) sp->fadeVelocity,
                         (int) sp->tempo, (unsigned) sp->tempoAcc,
                         (int) gSeqLoadStatus[sp->seqId],
                         (int) gBankLoadStatus[sp->defaultBank[0]]);
        }
        // Note free-list occupancy. alloc_note() draws from disabled/decaying/
        // active; if they all drain to 0 the sequencer can't spawn new notes
        // (permanent silence while it keeps running). "floating" = notes not on
        // any list (prev==NULL) -> leaked, the signature of a missing free.
        {
            s32 ni, floating = 0, priDisabled = 0;
            for (ni = 0; ni < gMaxSimultaneousNotes; ni++) {
                if (gNotes[ni].listItem.prev == NULL) {
                    floating++;
                }
                if (gNotes[ni].priority == NOTE_PRIORITY_DISABLED) {
                    priDisabled++;
                }
            }
            eu_audio_log("  NOTES free[dis=%d dec=%d rel=%d act=%d] floating=%d priDisabled=%d max=%d\n",
                         (int) gNoteFreeLists.disabled.u.count,
                         (int) gNoteFreeLists.decaying.u.count,
                         (int) gNoteFreeLists.releasing.u.count,
                         (int) gNoteFreeLists.active.u.count,
                         (int) floating, (int) priDisabled, (int) gMaxSimultaneousNotes);
        }
    }
}

void eu_process_audio_cmd(struct EuAudioCmd *cmd) {
    s32 i;

    switch (cmd->u.s.op) {
    case 0x81:
        preload_sequence(cmd->u.s.arg2, 3);
        break;

    case 0x82:
    case 0x88:
        // load_sequence(arg1, arg2, 0);
        eu_audio_log("CMD load_sequence player=%d seqId=%d async=%d fadeIn=%d frame=%u\n",
                     (int) cmd->u.s.arg1, (int) cmd->u.s.arg2, (int) cmd->u.s.arg3,
                     (int) cmd->u2.as_s32, (unsigned) gAudioFrameCount);
        load_sequence(cmd->u.s.arg1, cmd->u.s.arg2, cmd->u.s.arg3);
        func_8031D690(cmd->u.s.arg1, cmd->u2.as_s32);
        break;

    case 0x83:
        eu_audio_log("CMD stop/fade player=%d fadeOut=%d enabled=%d frame=%u\n",
                     (int) cmd->u.s.arg1, (int) cmd->u2.as_s32,
                     (int) gSequencePlayers[cmd->u.s.arg1].enabled, (unsigned) gAudioFrameCount);
        if (gSequencePlayers[cmd->u.s.arg1].enabled != FALSE) {
            if (cmd->u2.as_s32 == 0) {
                sequence_player_disable(&gSequencePlayers[cmd->u.s.arg1]);
            }
            else {
                sequence_player_fade_out_internal(cmd->u.s.arg1, cmd->u2.as_s32);
            }
        }
        break;

    case 0xf0:
        gSoundMode = cmd->u2.as_s32;
        break;

    case 0xf1:
        for (i = 0; i < 4; i++) {
            gSequencePlayers[i].muted = TRUE;
            gSequencePlayers[i].recalculateVolume = TRUE;
        }
        break;

    case 0xf2:
        for (i = 0; i < 4; i++) {
            gSequencePlayers[i].muted = FALSE;
            gSequencePlayers[i].recalculateVolume = TRUE;
        }
        break;
    }
}

const char undefportcmd[] = "Undefined Port Command %d\n";

extern OSMesgQueue *OSMesgQueues[];
extern u8 D_EU_80302010;
extern u8 D_EU_80302014;
extern OSMesg OSMesg0;
extern OSMesg OSMesg1;
extern OSMesg OSMesg2;
extern OSMesg OSMesg3;

void sequence_player_fade_out_internal(s32 player, FadeT fadeOutTime) {
    if (fadeOutTime == 0) {
        fadeOutTime = 1;
    }
    gSequencePlayers[player].fadeVelocity = -(gSequencePlayers[player].fadeVolume / fadeOutTime);
    gSequencePlayers[player].state = 2;
    gSequencePlayers[player].fadeTimer = fadeOutTime;

}

void func_8031D690(s32 player, FadeT fadeInTime) {
    if (fadeInTime != 0) {
        gSequencePlayers[player].state = 1;
        gSequencePlayers[player].fadeTimerUnkEu = fadeInTime;
        gSequencePlayers[player].fadeTimer = fadeInTime;
        gSequencePlayers[player].fadeVolume = 0.0f;
        gSequencePlayers[player].fadeVelocity = 0.0f;
    }
}

void port_eu_init_queues(void) {
    D_EU_80302010 = 0;
    D_EU_80302014 = 0;
    osCreateMesgQueue(OSMesgQueues[0], &OSMesg0, 1);
    osCreateMesgQueue(OSMesgQueues[1], &OSMesg1, 4);
    osCreateMesgQueue(OSMesgQueues[2], &OSMesg2, 1);
    osCreateMesgQueue(OSMesgQueues[3], &OSMesg3, 1);
}

void func_802ad6f0(s32 arg0, s32 *arg1) {
    struct EuAudioCmd *cmd = &sAudioCmd[D_EU_80302010 & 0xff];
    cmd->u.first = arg0;
    cmd->u2.as_u32 = *arg1;
    D_EU_80302010++;
}

void func_802ad728(u32 arg0, f32 arg1) {
    func_802ad6f0(arg0, (s32*) &arg1);
}

void func_802ad74c(u32 arg0, u32 arg1) {
    func_802ad6f0(arg0, (s32*) &arg1);
}

void func_802ad770(u32 arg0, s8 arg1) {
    s32 sp1C = arg1 << 24;
    func_802ad6f0(arg0, &sp1C);
}

void func_802ad7a0(void) {
    s32 sendResult = osSendMesg(OSMesgQueues[1],
            (OSMesg)(u32)((D_EU_80302014 & 0xff) << 8 | (D_EU_80302010 & 0xff)),
            OS_MESG_NOBLOCK);
    gEuDbgFlushCount++;
    if (sendResult == -1) {
        // Command-queue overflow: a flush was dropped (consumer fell behind).
        eu_audio_log("FLUSH DROPPED (q1 full) start=%u end=%u valid=%d frame=%u\n",
                     (unsigned) (D_EU_80302014 & 0xff), (unsigned) (D_EU_80302010 & 0xff),
                     (int) OSMesgQueues[1]->validCount, (unsigned) gAudioFrameCount);
    }
    D_EU_80302014 = D_EU_80302010;
}

void func_802ad7ec(u32 arg0) {
    struct EuAudioCmd *cmd;
    struct SequencePlayer *seqPlayer;
    struct SequenceChannel *chan;
    u8 end = arg0 & 0xff;
    u8 i = (arg0 >> 8) & 0xff;

    for (;;) {
        if (i == end) break;
        cmd = &sAudioCmd[i++ & 0xff];

        if (cmd->u.s.arg1 < SEQUENCE_PLAYERS) {
            seqPlayer = &gSequencePlayers[cmd->u.s.arg1];
            if ((cmd->u.s.op & 0x80) != 0) {
                eu_process_audio_cmd(cmd);
            }
            else if ((cmd->u.s.op & 0x40) != 0) {
                switch (cmd->u.s.op) {
                case 0x41:
                    seqPlayer->fadeVolumeScale = cmd->u2.as_f32;
                    seqPlayer->recalculateVolume = TRUE;
                    break;

                case 0x47:
                    seqPlayer->tempo = cmd->u2.as_s32 * TATUMS_PER_BEAT;
                    break;

                case 0x48:
                    seqPlayer->transposition = cmd->u2.as_s8;
                    break;

                case 0x46:
                    seqPlayer->seqVariationEu[cmd->u.s.arg3] = cmd->u2.as_s8;
                    break;
                }
            }
            else if (seqPlayer->enabled != FALSE && cmd->u.s.arg2 < 0x10) {
                chan = seqPlayer->channels[cmd->u.s.arg2];
                if (IS_SEQUENCE_CHANNEL_VALID(chan))
                {
                    switch (cmd->u.s.op) {
                    case 1:
                        chan->volumeScale = cmd->u2.as_f32;
                        chan->changes.as_bitfields.volume = TRUE;
                        break;
                    case 2:
                        chan->volume = cmd->u2.as_f32;
                        chan->changes.as_bitfields.volume = TRUE;
                        break;
                    case 3:
                        chan->newPan = cmd->u2.as_s8;
                        chan->changes.as_bitfields.pan = TRUE;
                        break;
                    case 4:
                        chan->freqScale = cmd->u2.as_f32;
                        chan->changes.as_bitfields.freqScale = TRUE;
                        break;
                    case 5:
                        chan->reverb = cmd->u2.as_s8;
                        break;
                    case 6:
                        if (cmd->u.s.arg3 < 8) {
                            chan->soundScriptIO[cmd->u.s.arg3] = cmd->u2.as_s8;
                        }
                        break;
                    case 8:
                        chan->stopSomething2 = cmd->u2.as_s8;
                    }
                }
            }
        }

        cmd->u.s.op = 0;
    }
}

void port_eu_init(void) {
    port_eu_init_queues();
}

#endif
