#include "daisy_seed.h"
#include "daisysp.h"
#include "bt_link.h"
#include "Simple-DSP/Source/sknightdsp.h"

using namespace daisy;
using namespace daisysp;
using namespace sknight;

DaisySeed hw;

struct Settings
{
    float high;
    float low;

    bool operator!=(const Settings &a) const
    { return !(a.high == high && a.low == low); }
};

PersistentStorage<Settings> SavedSettings(hw.qspi);

constexpr uint32_t SR = 48000;

effects::MoorerReverb     reverb;
effects::BitCrusher       bitcrusher;
effects::PitchShifter<>   pitchshifter;
effects::DelayLine<SR>    delay_line;
effects::SimpleDistortion distortion_reverb;

// if fb is below this it turns off the delay completely
constexpr float FB_GATE = 0.05f;

// if it calibrated lower than this it rejects it
constexpr float LDR_MIN_SPAN = 300.0f;

constexpr int   BITCRUSH_MIN_DEPTH = 6;
constexpr float BITCRUSH_MIN_SR    = 2000.0f;

constexpr uint32_t BLINK_SLOW_MS = 500;
constexpr uint32_t BLINK_FAST_MS = 200;

bool can_change_pitch = true;

GPIO   led;
Switch bypass_fs;

bool is_bypassed = false;

constexpr float coeff           = 0.001f;
float           knob_1_smoothed = 0.0f; // delay time
float           knob_2_smoothed = 0.0f; // delay fb
float           ldr_smoothed    = 0.0f; // reverb size
float           joy_x_smoothed  = 0.0f; // distortions
float           joy_y_smoothed  = 0.0f; // bitcrusher
float           mix_smoothed    = 0.0f; // master dry/wet

bool  is_latched   = false;
bool  has_pressed  = false;
float joy_x_mapped = 0.0f;
float joy_y_mapped = 0.0f;

float envelope
    = 0.0f; // envelope for gating non-reverb signal when reverb is playing with ldr

// ldr bounds (manually calibrated but these are the default)
float ldr_high = 3000.0f; // when open
float ldr_low  = 1450.0f; // when covering

float ldr_high_sum   = 0.0f;
float ldr_low_sum    = 0.0f;
int   ldr_high_count = 0;
int   ldr_low_count  = 0;

float start_time     = 0.0f;
bool  has_set_time   = false;
bool  has_calculated = false;

bool trigger_save = false;

enum LedStates
{
    Normal,
    Bypassed,
    Calibrating,
    Blinking
};

LedStates led_state = Normal;

void  InitiailizeEffects();
void  ProcessADC();
void  LdrCalibration();
void  UpdateLed();
float LdrWet();

void Load()
{
    Settings &LocalSettings = SavedSettings.GetSettings();

    ldr_high = LocalSettings.high;
    ldr_low  = LocalSettings.low;
}

void Save()
{
    Settings &LocalSettings = SavedSettings.GetSettings();

    LocalSettings.high = ldr_high;
    LocalSettings.low  = ldr_low;

    trigger_save = true;
}

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    ProcessADC();

    float fb = fminf(knob_2_smoothed, 0.999f);

    // makes it so when fb knob is 0, delay shuts off
    float delay_mix = BTLink::Knob2f() > FB_GATE ? 0.5f : 0.0f;

    for(size_t i = 0; i < size; i++)
    {
        float dry = in[0][i];
        float sig = dry;

        float delayed = delay_line.Read();
        delay_line.Write(sig + delayed * fb);

        sig = delayed * delay_mix + sig * (1.0f - delay_mix);

        float pitchshifted = pitchshifter.Process(sig);

        sig = pitchshifted * joy_x_mapped + sig * (1.0f - joy_x_mapped);

        float crushed = bitcrusher.Process(sig);

        sig = crushed * joy_y_mapped + sig * (1.0f - joy_y_mapped);

        float verb_sig = reverb.Process(sig * ldr_smoothed);
        fonepole(envelope, fabsf(verb_sig), 0.001f);

        float scaled_env = fminf(envelope * 2.0f, 1.0f);

        verb_sig = distortion_reverb.Process(verb_sig) * 0.5f;

        float wet   = verb_sig + sig * (1.0f - scaled_env);
        float mixed = wet * mix_smoothed + dry * (1.0f - mix_smoothed);

        out[0][i] = is_bypassed ? dry : mixed;
    }
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(4);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.SetLed(true);

    BTLink::Init();

    led.Init(seed::D15, GPIO::Mode::OUTPUT);
    bypass_fs.Init(seed::D16);

    AdcChannelConfig adc_cfg;
    adc_cfg.InitSingle(seed::A10);
    hw.adc.Init(&adc_cfg, 1);
    hw.adc.Start();

    InitiailizeEffects();

    Settings DefaultSettings = {3000.0f, 1450.0f};
    SavedSettings.Init(DefaultSettings);
    Load();


    hw.StartAudio(AudioCallback);

    while(1)
    {
        LdrCalibration();
        UpdateLed();

        if(trigger_save)
        {
            SavedSettings.Save();
            trigger_save = false;
        }

        System::Delay(100);
    }
}

void InitiailizeEffects()
{
    reverb.Init(SR);
    bitcrusher.Init(SR);
    delay_line.Init();
    pitchshifter.Init();

    bitcrusher.SetBitDepth(16);
    bitcrusher.SetSamplingRate(SR);

    distortion_reverb.SetGain(6.0f);
    distortion_reverb.SetType(
        effects::SimpleDistortion::ClippingType::SoftClip);
    pitchshifter.SetTransposition(0);
    reverb.SetDecayTime(5.0f);
}

void ProcessADC()
{
    fonepole(knob_1_smoothed, BTLink::Knob1f(), coeff);
    fonepole(knob_2_smoothed, BTLink::Knob2f(), coeff);
    fonepole(ldr_smoothed, LdrWet(), coeff);
    fonepole(joy_x_smoothed, BTLink::JoyXf(), coeff);
    fonepole(joy_y_smoothed, BTLink::JoyYf(), coeff);
    fonepole(
        mix_smoothed,
        1.0f - hw.adc.GetFloat(0),
        coeff); // knob is backwards in first version where i f'd up. Will fix eventually

    bypass_fs.Debounce();
    if(bypass_fs.RisingEdge())
    {
        is_bypassed = !is_bypassed;

        if(led_state == Normal || led_state == Bypassed)
            led_state = is_bypassed ? Bypassed : Normal;
    }

    delay_line.SetDelay(fmap(knob_1_smoothed, 0.2f, 0.9f * SR));

    // make sure it doesn't spam when held
    if(BTLink::Switch() && !has_pressed)
    {
        is_latched  = !is_latched;
        has_pressed = true;
    }
    if(!BTLink::Switch() && has_pressed)
        has_pressed = false;

    // since joy x mapped freezes when latched
    float joy_x_live = fabsf((joy_x_smoothed - 0.5f) * 2.0f);
    float joy_y_live = fabsf((joy_y_smoothed - 0.5f) * 2.0f);

    if(!is_latched)
    {
        joy_x_mapped = joy_x_live;
        joy_y_mapped = joy_y_live;

        // one side changes sample rate other does bit depth
        if(joy_y_smoothed > 0.5f)
        {
            bitcrusher.SetBitDepth(static_cast<int>(
                fmap(1.0f - joy_y_mapped, BITCRUSH_MIN_DEPTH, 16.0f)));
            bitcrusher.SetSamplingRate(SR);
        }
        else
        {
            bitcrusher.SetBitDepth(16);
            bitcrusher.SetSamplingRate(fmap(1.0f - joy_y_mapped,
                                            BITCRUSH_MIN_SR,
                                            1.0f * SR,
                                            daisysp::Mapping::LOG));
        }
    }

    if(can_change_pitch && joy_x_live > 0.05f && !is_latched)
    {
        pitchshifter.SetTransposition(BTLink::JoyXf() > 0.5f ? -12.0f : 12.0f);
        can_change_pitch = false;
    }

    if(!can_change_pitch && joy_x_live < 0.05f)
        can_change_pitch = true;
}

void LdrCalibration()
{
    bool isHeld = BTLink::Switch();

    if(!isHeld)
    {
        ldr_high_sum   = 0.0f;
        ldr_low_sum    = 0.0f;
        ldr_high_count = 0;
        ldr_low_count  = 0;
        has_set_time   = false;
        has_calculated = false;
        led_state      = is_bypassed ? Bypassed : Normal;
    }

    if(isHeld && !has_calculated)
    {
        float time = System::GetNow();
        if(!has_set_time)
        {
            start_time   = time;
            has_set_time = true;
        }

        float elapsed_time = time - start_time;
        if(elapsed_time > 7000)
        {
            // average values
            if(ldr_high_count > 0 && ldr_low_count > 0)
            {
                float high = ldr_high_sum / ldr_high_count;
                float low  = ldr_low_sum / ldr_low_count;

                if(high - low >= LDR_MIN_SPAN)
                {
                    ldr_high = high;
                    ldr_low  = low;
                    Save();
                }
            }

            has_calculated = true;
            led_state      = is_bypassed ? Bypassed : Normal;
        }
        else if(elapsed_time > 5000)
        {
            // setting value when covered (ldr_low)
            ldr_low_sum += BTLink::Ldr();
            ldr_low_count++;
            led_state = Calibrating;
        }
        else if(elapsed_time > 3000)
        {
            // led blinks to tell you to cover it
            led_state = Blinking;
        }
        else if(elapsed_time > 1000)
        {
            // setting value when open (ldr_high)
            ldr_high_sum += BTLink::Ldr();
            ldr_high_count++;
            led_state  = Calibrating;
            is_latched = false;
        }
    }
}

void UpdateLed()
{
    uint32_t time = System::GetNow();

    switch(led_state)
    {
        case Calibrating: led.Write((time / BLINK_SLOW_MS) % 2 == 0); break;
        case Blinking: led.Write((time / BLINK_FAST_MS) % 2 == 0); break;
        case Bypassed: led.Write(false); break;
        default: led.Write(true); break;
    }
}

// wet amount 0-1. Covering lowers values so it's inverted at the end
float LdrWet()
{
    float span = ldr_high - ldr_low;

    if(!(span >= LDR_MIN_SPAN))
        return 0.0f;

    return fclamp((ldr_high - BTLink::Ldr()) / span, 0.0f, 1.0f);
}
