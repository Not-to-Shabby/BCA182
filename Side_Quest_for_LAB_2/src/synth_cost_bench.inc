/*
 * On-board cost benchmark of the voice types. Included at the end of yamaha_fm_synth.c, and only
 * when the firmware is built with -DSYNTH_COST_BENCH. It runs once at boot before the audio thread
 * exists, so nothing competes for the CPU, and leaves the results in g_cost_rows for a debugger.
 *
 * Every case is timed at two voice counts; the difference divided by the voice count is the cost
 * of one more voice for one 256-frame block, free of the fixed cost of the reverb and master stage.
 */

#include <stm32f4xx.h>

#define CB_BLOCK_WORDS  512U            /* 256 stereo frames, the block the audio thread renders */
#define CB_ROWS         24
#define CB_PCM_SAMPLES  3000U

typedef struct {
    char name[36];
    uint32_t n1;
    uint32_t n2;
    uint32_t cycles1;                   /* average cycles per block with n1 voices */
    uint32_t cycles2;
    uint32_t per_voice;                 /* cycles one voice adds to a block */
    uint8_t ops6;                       /* mix of the n2-voice run */
    uint8_t ops4;
    uint8_t ops2;
    uint8_t drums;
    uint8_t asked;                      /* notes started */
    uint8_t first_block;                /* voices sounding after the first block */
    uint8_t steals;                     /* voices taken over while starting them */
} cost_row_t;

cost_row_t g_cost_rows[CB_ROWS];
uint32_t g_cost_count;
uint32_t g_cost_budget;
uint32_t g_cost_clock;
uint32_t g_cost_idle;
volatile uint32_t g_cost_done;

typedef struct {
    const char *name;
    uint8_t pre_prog;                   /* held strings that push the busy count up to pick the tier */
    uint32_t pre;
    uint8_t prog;
    uint8_t ch;
    uint32_t m1;
    uint32_t m2;
    bool third;
    bool vcf;
    bool opl4;
    uint32_t warm;
    uint32_t meas;
} cb_case_t;

static int16_t s_cb_buf[CB_BLOCK_WORDS];
static const uint8_t CB_DRUM_NOTES[8] = {49, 57, 55, 52, 51, 59, 53, 49};

static uint32_t cb_measure(uint32_t warm, uint32_t meas)
{
    uint64_t sum = 0;

    for (uint32_t i = 0; i < warm; i++) {
        yamaha_fm_synth_render(s_cb_buf, CB_BLOCK_WORDS);
    }
    for (uint32_t i = 0; i < meas; i++) {
        uint32_t t0 = DWT->CYCCNT;
        yamaha_fm_synth_render(s_cb_buf, CB_BLOCK_WORDS);
        sum += DWT->CYCCNT - t0;
    }
    return (uint32_t)(sum / meas);
}

static uint32_t cb_one(const cb_case_t *k, uint32_t m, cost_row_t *mix)
{
    yamaha_fm_synth_init(44100);
    yamaha_fm_set_third_operator(k->third);
    yamaha_fm_set_vcf(k->vcf);
    yamaha_fm_set_opl4_drums(k->opl4);
    yamaha_fm_control_change(0, 7, 60);
    yamaha_fm_control_change(1, 7, 60);
    yamaha_fm_control_change(9, 7, 60);

    /* Every note is distinct: a repeated note on one channel releases the earlier one, which would
     * leave fewer voices sounding than were asked for. The filler voices are plain 2-operator
     * flutes; they only raise the busy count that picks the operator tier. */
    yamaha_fm_program_change(1, k->pre_prog);
    for (uint32_t i = 0; i < k->pre; i++) {
        yamaha_fm_note_on(1, (uint8_t)(30U + i), 70);
    }
    yamaha_fm_program_change(k->ch, k->prog);
    for (uint32_t i = 0; i < m; i++) {
        uint8_t note = (k->ch == FM_DRUM_CHANNEL) ? CB_DRUM_NOTES[i % 8U] : (uint8_t)(40U + i);
        yamaha_fm_note_on(k->ch, note, 70);
    }

    uint32_t before_steals = yamaha_fm_get_steal_count();
    uint32_t c;

    if (mix != NULL) {
        yamaha_fm_synth_render(s_cb_buf, CB_BLOCK_WORDS);
        uint32_t n = 0;
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            n += s_voices[i].active ? 1U : 0U;
        }
        mix->first_block = (uint8_t)n;
        mix->asked = (uint8_t)(k->pre + m);
        mix->steals = (uint8_t)(yamaha_fm_get_steal_count() - before_steals);
    }
    c = cb_measure(k->warm, k->meas);

    if (mix != NULL) {
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            if (!s_voices[i].active) {
                continue;
            }
            if (s_voices[i].is_drum) {
                mix->drums++;
            } else if (s_voices[i].ops == 6) {
                mix->ops6++;
            } else if (s_voices[i].ops == 4) {
                mix->ops4++;
            } else {
                mix->ops2++;
            }
        }
    }
    return c;
}

static void cb_add(const char *name, uint32_t n1, uint32_t n2, uint32_t c1, uint32_t c2, const cost_row_t *mix)
{
    if (g_cost_count >= CB_ROWS) {
        return;
    }
    cost_row_t *r = &g_cost_rows[g_cost_count++];

    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, sizeof(r->name) - 1U);
    r->n1 = n1;
    r->n2 = n2;
    r->cycles1 = c1;
    r->cycles2 = c2;
    r->per_voice = (c2 > c1 && n2 > n1) ? (c2 - c1) / (n2 - n1) : 0U;
    if (mix != NULL) {
        r->ops6 = mix->ops6;
        r->ops4 = mix->ops4;
        r->ops2 = mix->ops2;
        r->drums = mix->drums;
        r->asked = mix->asked;
        r->first_block = mix->first_block;
        r->steals = mix->steals;
    }
}

static void cb_case(const cb_case_t *k)
{
    cost_row_t mix;

    memset(&mix, 0, sizeof(mix));
    uint32_t c1 = cb_one(k, k->m1, NULL);
    uint32_t c2 = cb_one(k, k->m2, &mix);

    cb_add(k->name, k->m1, k->m2, c1, c2, &mix);
}

/* ---- Stand-in for a sampled-instrument voice: the same loop the OPL4 drums use (16.16 position,
 * linear interpolation, gain, two buses), run over a table in Flash or in core-coupled RAM. ---- */

static DTCM_BSS int16_t s_cb_dtcm_pcm[CB_PCM_SAMPLES];
static DTCM_BSS int32_t s_cb_l[256];            /* like the real per-channel buses */
static DTCM_BSS int32_t s_cb_r[256];

static uint32_t cb_pcm_block(const int16_t *pcm, uint32_t voices, uint32_t *pos, const uint32_t *step)
{
    uint32_t t0 = DWT->CYCCNT;

    memset(s_cb_l, 0, sizeof(s_cb_l));
    memset(s_cb_r, 0, sizeof(s_cb_r));
    for (uint32_t v = 0; v < voices; v++) {
        uint32_t p = pos[v];
        const uint32_t st = step[v];

        for (uint32_t i = 0; i < 256U; i++) {
            uint32_t idx = p >> 16;

            if (idx + 1U >= CB_PCM_SAMPLES) {
                p = 0;
                idx = 0;
            }
            int32_t frac = (int32_t)((p >> 8) & 0xFFU);
            int32_t s0 = pcm[idx];
            int32_t s1 = pcm[idx + 1U];
            int32_t o = (((s0 * (256 - frac) + s1 * frac) >> 8) * 9000) >> 15;

            s_cb_l[i] += (o * 23000) >> 15;
            s_cb_r[i] += (o * 17000) >> 15;
            p += st;
        }
        pos[v] = p;
    }
    return DWT->CYCCNT - t0;
}

static uint32_t cb_pcm_run(const int16_t *pcm, uint32_t voices)
{
    uint32_t pos[24];
    uint32_t step[24];
    uint64_t sum = 0;

    for (uint32_t v = 0; v < 24U; v++) {
        pos[v] = (v * 160U) << 16;
        step[v] = 0x8000U + v * 0x1400U;           /* 0.5 .. 1.9 samples per output sample */
    }
    for (uint32_t i = 0; i < 6U; i++) {
        (void)cb_pcm_block(pcm, voices, pos, step);
    }
    for (uint32_t i = 0; i < 60U; i++) {
        sum += cb_pcm_block(pcm, voices, pos, step);
    }
    return (uint32_t)(sum / 60U);
}

void yamaha_fm_cost_bench(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    g_cost_clock = SystemCoreClock;
    g_cost_budget = (uint32_t)(((uint64_t)SystemCoreClock * 256U) / 44100U);
    g_cost_count = 0;
    g_cost_done = 0;

    yamaha_fm_synth_init(44100);
    g_cost_idle = cb_measure(8, 40);

    /* name, filler prog, filler n, prog, channel, n1, n2, third op, vcf, opl4 drums, warm, measured blocks.
     * Operator tier is set by how many voices are busy when a note starts: up to 12 gives 6-op,
     * 13 to 22 gives 4-op. 13 fillers put the measured notes in the 4-op range. */
    static const cb_case_t CASES[] = {
        {"2-op piano, VCF off",      73, 0,  0, 0,  8, 24, false, false, true, 24, 40},
        {"2-op piano, VCF on",       73, 0,  0, 0,  8, 24, false, true,  true, 24, 40},
        {"2-op strings, VCF on",     73, 0, 48, 0,  8, 24, false, true,  true, 24, 40},
        {"2-op flute, VCF on",       73, 0, 73, 0,  8, 24, false, true,  true, 24, 40},
        {"4-op strings (ensemble)",  73, 13, 48, 0,  2,  8, true,  true,  true, 24, 40},
        {"4-op piano (tine)",        73, 13,  0, 0,  2,  8, true,  true,  true, 24, 40},
        {"6-op strings (ensemble)",  73, 0, 48, 0,  4, 12, true,  true,  true, 24, 40},
        {"6-op piano (tine)",        73, 0,  0, 0,  4, 12, true,  true,  true, 24, 40},
        {"drum hits, OPL4 PCM on",   73, 0,  0, 9,  2,  8, true,  true,  true,  2, 24},
        {"drum hits, synth only",    73, 0,  0, 9,  2,  8, true,  true,  false, 2, 24},
    };

    for (uint32_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        cb_case(&CASES[i]);
    }

    /* Fixed cost of the stages after the voices, measured with 8 held notes so that no
     * silence shortcut applies. n1 = n2 = 8; the value is in cycles1. */
    static const struct {
        const char *name;
        uint8_t effects;
        bool comp;
        bool eq;
    } STAGES[] = {
        {"8 notes, everything on",     100, true,  true},
        {"8 notes, reverb+chorus off",   0, true,  true},
        {"8 notes, compressor off",    100, false, true},
        {"8 notes, master EQ off",     100, true,  false},
        {"8 notes, all three off",       0, false, false},
    };

    for (uint32_t s = 0; s < sizeof(STAGES) / sizeof(STAGES[0]); s++) {
        yamaha_fm_synth_init(44100);
        yamaha_fm_set_third_operator(false);
        yamaha_fm_set_effects_level(STAGES[s].effects);
        yamaha_fm_set_compressor(STAGES[s].comp);
        dsp_set_eq(STAGES[s].eq);
        yamaha_fm_program_change(0, 73);
        for (uint32_t i = 0; i < 8U; i++) {
            yamaha_fm_note_on(0, (uint8_t)(50U + i), 70);
        }
        uint32_t c = cb_measure(24, 40);

        cb_add(STAGES[s].name, 8, 8, c, c, NULL);
    }

    for (uint32_t i = 0; i < CB_PCM_SAMPLES; i++) {
        s_cb_dtcm_pcm[i] = OPL4_PCM_CRASH[i];
    }
    cb_add("sample loop, table in Flash", 4, 16, cb_pcm_run(OPL4_PCM_CRASH, 4), cb_pcm_run(OPL4_PCM_CRASH, 16), NULL);
    cb_add("sample loop, table in fast RAM", 4, 16, cb_pcm_run(s_cb_dtcm_pcm, 4), cb_pcm_run(s_cb_dtcm_pcm, 16), NULL);

    yamaha_fm_synth_init(44100);
    g_cost_done = 1;
}
