/**
 * Legio implementation of PRat distortion, featuring:
 *
 * - stereo signal path;
 * - knobs with dedicated CV controls;
 * - hard clip, ruetz and tight mods;
 * - noise gate (with a bypass and adjustable threshold / release).
 *
 */
#include "daisy_legio.h"
#include "daisysp.h"

#include "PRatDist.h"
#include "NoiseGate.h"
#include "GrabValue.h"

using namespace daisy;
using namespace daisysp;

using namespace prat;


// Hardware object for the Legio
DaisyLegio hw;

// Settings
bool initialized = false;

// PRat distortion
PRatDist dist;
// PRat noise gate
NoiseGate ng;

// UX values
float envVal = 0.f;
float satVal = 0.f;


void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    static bool first = true;

    // init module with gain / level in mid position
    static float cur_gain = 0.5f;
    static float cur_level = 0.5f;

    static GrabValue<float> cv_filter = 0.f;
    static GrabValue<float> cv_mix = 0.f;

    static GrabValue<float> ng_threshold = 0.4f;  // -45db
    static GrabValue<float> ng_release = 0.5f;    // 100ms

    hw.ProcessAnalogControls();

    // pass-thru until module is initialized
    if (!initialized) {
        Utils::Copy(IN_L, IN_R, OUT_L, OUT_R, size);
        return;
    }

    const bool shift = hw.encoder.Pressed() && !first;

    const float cv0 = hw.GetKnobValue(DaisyLegio::CONTROL_KNOB_TOP);
    const float cv1 = hw.GetKnobValue(DaisyLegio::CONTROL_KNOB_BOTTOM);

    if (!shift) {
        cv_filter.Update(cv0);
        cv_mix.Update(cv1);

        ng_threshold.Lock();
        ng_release.Lock();
    } else {
        cv_filter.Lock();
        cv_mix.Lock();

        ng_threshold.Update(cv0);
        ng_release.Update(cv1);
    }

    const float encInc = hw.encoder.Increment() / 16;
    if (shift) {
        cur_level = fclamp(cur_level + encInc, 0.f, 1.f);
    } else {
        cur_gain = fclamp(cur_gain + encInc, 0.f, 1.f);
    }

    const float gain_cv = hw.controls[DaisyLegio::CONTROL_PITCH].Value();

    const float gain = fclamp(cur_gain + gain_cv, 0.f, 1.f);
    const float filter = fclamp(cv_filter.Get(), 0.f, 1.f);
    const float level = fclamp(cur_level, 0.f, 1.f);
    const float mix = fclamp(cv_mix.Get(), 0.f, 1.f);

    const int sw_clip = hw.sw[DaisyLegio::SW_LEFT].Read();
    const int sw_mod = hw.sw[DaisyLegio::SW_RIGHT].Read();

    const float hard = (sw_clip == Switch3::POS_CENTER || hw.Gate()) ? 1.f : 0.f;
    const float bypass = sw_clip == Switch3::POS_DOWN ? 1.f : 0.f;
    const float ruetz = sw_mod == Switch3::POS_CENTER ? 1.f : 0.f;
    const float tight = sw_mod == Switch3::POS_DOWN ? 1.f : 0.f;

    dist.SetParam(PRatDist::P_GAIN, gain);
    dist.SetParam(PRatDist::P_FILTER, filter);
    dist.SetParam(PRatDist::P_LEVEL, level);
    // in hard mode, mix Silicon / Led clippers
    if (hard >= 0.5f) {
        dist.SetParam(PRatDist::P_DRYWET, 1.f);
        dist.SetParam(PRatDist::P_SILED, mix);
    } else {
        dist.SetParam(PRatDist::P_DRYWET, mix);
    }
    dist.SetParam(PRatDist::P_HARD, hard);
    dist.SetParam(PRatDist::P_TIGHT, tight);
    dist.SetParam(PRatDist::P_RUETZ, ruetz);
    dist.SetParam(PRatDist::P_BYPASS, bypass);

    dist.Update();

    if (first || shift) {
        ng.SetParam(NoiseGate::P_THRESHOLD, ng_threshold.Get());
        ng.SetParam(NoiseGate::P_RELEASE, ng_release.Get());
        // a threshold < -75db disable the noise gate
        ng.SetParam(NoiseGate::P_BYPASS, ng_threshold.Get() < 0.05f ? 1.f : 0.f);

        ng.Update();
    }

    dist.Process(IN_L, IN_R, OUT_L, OUT_R, size);
    // noise gate uses left input for volume detection
    ng.Process(OUT_L, OUT_R, IN_L, OUT_L, OUT_R, size);

    // output the distortion saturation
    satVal = fclamp(dist.GetSaturation() / 5.f, 0.f, 1.f);
    // noise gate envelope follower signal (boosted to be in 0-1v range)
    envVal = fclamp(ng.GetEnvelope() * 2.5f, 0.f, 1.f);

    first = false;
}


int main(void)
{
    static bool first = true;

    hw.Init();
    //hw.StartLog();
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.SetAudioBlockSize(4);

    dist.Init(hw.AudioSampleRate());
    ng.Init(hw.AudioSampleRate());

    ng.SetParam(NoiseGate::P_DETECTOR_GAIN, 0.5);  // * 1
    ng.SetParam(NoiseGate::P_REDUCTION, 0.4);      // -40db
    ng.SetParam(NoiseGate::P_SLOPE, 0.3, true);    // 3

    uint32_t boottime = System::GetNow();

    hw.StartAudio(AudioCallback);
    hw.StartAdc();

    while (1) {
        if (!initialized) {
            if (System::GetNow() - boottime < 1000) {
                if (first) {
                    hw.SetLed(DaisyLegio::LED_LEFT, 1.f, 0.f, 0.f);
                    hw.SetLed(DaisyLegio::LED_RIGHT, 1.f, 0.f, 1.f);
                    hw.UpdateLeds();

                    first = false;
                }
            } else {
                initialized = true;
            }
        } else {
            hw.SetLed(DaisyLegio::LED_LEFT, 0.f, envVal, 0.f);
            hw.SetLed(DaisyLegio::LED_RIGHT, satVal, 0.f, 0.f);
            hw.UpdateLeds();
        }
    }
}
