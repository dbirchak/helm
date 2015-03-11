/* Copyright 2013-2015 Matt Tytel
 *
 * twytch is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * twytch is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with twytch.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "twytch_voice_handler.h"

#include "bypass_router.h"
#include "delay.h"
#include "distortion.h"
#include "envelope.h"
#include "filter.h"
#include "formant.h"
#include "formant_manager.h"
#include "operators.h"
#include "oscillator.h"
#include "linear_slope.h"
#include "smooth_value.h"
#include "step_generator.h"
#include "twytch_oscillators.h"
#include "trigger_operators.h"
#include "utils.h"
#include "value.h"

#include <fenv.h>
#include <sstream>


#define PITCH_MOD_RANGE 12
#define MIN_GAIN_DB -24.0
#define MAX_GAIN_DB 24.0

#define MAX_FEEDBACK_SAMPLES 20000

namespace mopo {

  namespace {
    struct FormantValues {
      Value* gain;
      Value* resonance;
      Value* frequency;
    };

    static const FormantValues top_left_formants[NUM_FORMANTS] = {
      {new Value(1.0), new Value(6.0), new Value(270.0)},
      {new Value(1.0), new Value(10.0), new Value(2300.0)},
      {new Value(1.0), new Value(8.0), new Value(3000.0)},
      {new Value(0.2), new Value(15.0), new Value(500.0)},
    };

    static const FormantValues top_right_formants[NUM_FORMANTS] = {
      {new Value(1.0), new Value(6.0), new Value(270.0)},
      {new Value(1.0), new Value(12.0), new Value(500.0)},
      {new Value(1.0), new Value(8.0), new Value(2000.0)},
      {new Value(1.0), new Value(9.0), new Value(1500.0)},
    };

    static const FormantValues bottom_left_formants[NUM_FORMANTS] = {
      {new Value(1.0), new Value(6.0), new Value(270.0)},
      {new Value(1.0), new Value(4.0), new Value(2300.0)},
      {new Value(1.0), new Value(8.0), new Value(3000.0)},
      {new Value(0.2), new Value(0.5), new Value(500.0)},
    };

    static const FormantValues bottom_right_formants[NUM_FORMANTS] = {
      {new Value(0.0), new Value(6.0), new Value(270.0)},
      {new Value(0.0), new Value(12.0), new Value(500.0)},
      {new Value(0.0), new Value(8.0), new Value(3000.0)},
      {new Value(0.0), new Value(9.0), new Value(3500.0)},
    };
  } // namespace

  TwytchVoiceHandler::TwytchVoiceHandler() {
    output_ = new Multiply();
    registerOutput(output_->output());

    // Create modulation and pitch wheels.
    mod_wheel_amount_ = new SmoothValue(0);
    pitch_wheel_amount_ = new SmoothValue(0);

    mod_sources_["pitch_wheel"] = pitch_wheel_amount_->output();
    mod_sources_["mod_wheel"] = mod_wheel_amount_->output();

    // Create all synthesizer voice components.
    createArticulation(note(), velocity(), voice_event());
    createOscillators(current_frequency_->output(),
                      amplitude_envelope_->output(Envelope::kFinished));
    createModulators(amplitude_envelope_->output(Envelope::kFinished));
    createFilter(osc_feedback_->output(0), note_from_center_->output(),
                 amplitude_envelope_->output(Envelope::kFinished), voice_event());

    Value* aftertouch_value = new Value();
    aftertouch_value->plug(aftertouch());

    addProcessor(aftertouch_value);
    mod_sources_["aftertouch"] = aftertouch_value->output();

    output_->plug(formant_container_, 0);
    output_->plug(amplitude_, 1);

    addProcessor(output_);
    addGlobalProcessor(pitch_wheel_amount_);
    addGlobalProcessor(mod_wheel_amount_);

    setVoiceKiller(amplitude_envelope_->output(Envelope::kValue));
  }

  void TwytchVoiceHandler::createOscillators(Output* midi, Output* reset) {
    // Pitch bend.
    Value* pitch_bend_range = new Value(2);
    Multiply* pitch_bend = new Multiply();
    pitch_bend->plug(pitch_wheel_amount_, 0);
    pitch_bend->plug(pitch_bend_range, 1);
    Add* bent_midi = new Add();
    bent_midi->plug(midi, 0);
    bent_midi->plug(pitch_bend, 1);

    addGlobalProcessor(pitch_bend);
    addProcessor(bent_midi);

    controls_["pitch_bend_range"] = pitch_bend_range;

    // Oscillator 1.
    oscillators_ = new TwytchOscillators();
    Processor* oscillator1_waveform = createPolyModControl("osc_1_waveform", Wave::kDownSaw, true);
    Processor* oscillator1_transpose = createPolyModControl("osc_1_transpose", 0, false);
    Processor* oscillator1_tune = createPolyModControl("osc_1_tune", 0.0, false);
    Add* oscillator1_transposed = new Add();
    oscillator1_transposed->plug(bent_midi, 0);
    oscillator1_transposed->plug(oscillator1_transpose, 1);
    Add* oscillator1_midi = new Add();
    oscillator1_midi->plug(oscillator1_transposed, 0);
    oscillator1_midi->plug(oscillator1_tune, 1);

    MidiScale* oscillator1_frequency = new MidiScale();
    oscillator1_frequency->plug(oscillator1_midi);
    oscillators_->plug(oscillator1_waveform, TwytchOscillators::kOscillator1Waveform);
    oscillators_->plug(reset, TwytchOscillators::kOscillator1Reset);
    oscillators_->plug(reset, TwytchOscillators::kOscillator2Reset);
    oscillators_->plug(oscillator1_frequency, TwytchOscillators::kOscillator1BaseFrequency);

    Processor* cross_mod = createPolyModControl("cross_modulation", 0.15, false);
    oscillators_->plug(cross_mod, TwytchOscillators::kOscillator1FM);
    oscillators_->plug(cross_mod, TwytchOscillators::kOscillator2FM);

    addProcessor(oscillator1_transposed);
    addProcessor(oscillator1_midi);
    addProcessor(oscillator1_frequency);
    addProcessor(oscillators_);

    // Oscillator 2.
    Processor* oscillator2_waveform = createPolyModControl("osc_2_waveform", Wave::kDownSaw, true);
    Processor* oscillator2_transpose = createPolyModControl("osc_2_transpose", -12, false);
    Processor* oscillator2_tune = createPolyModControl("osc_2_tune", 0.08, false);
    Add* oscillator2_transposed = new Add();
    oscillator2_transposed->plug(bent_midi, 0);
    oscillator2_transposed->plug(oscillator2_transpose, 1);
    Add* oscillator2_midi = new Add();
    oscillator2_midi->plug(oscillator2_transposed, 0);
    oscillator2_midi->plug(oscillator2_tune, 1);

    MidiScale* oscillator2_frequency = new MidiScale();
    oscillator2_frequency->plug(oscillator2_midi);
    oscillators_->plug(oscillator2_waveform, TwytchOscillators::kOscillator2Waveform);
    oscillators_->plug(oscillator2_frequency, TwytchOscillators::kOscillator2BaseFrequency);

    addProcessor(oscillator2_transposed);
    addProcessor(oscillator2_midi);
    addProcessor(oscillator2_frequency);

    // Oscillator mix.
    Processor* oscillator_mix_amount = createPolyModControl("osc_mix", 0.5, false, true);

    Clamp* clamp_mix = new Clamp(0, 1);
    clamp_mix->plug(oscillator_mix_amount);
    oscillator_mix_ = new Interpolate();
    oscillator_mix_->plug(oscillators_->output(0), Interpolate::kFrom);
    oscillator_mix_->plug(oscillators_->output(1), Interpolate::kTo);
    oscillator_mix_->plug(clamp_mix, Interpolate::kFractional);

    addProcessor(oscillator_mix_);
    addProcessor(clamp_mix);

    // Oscillator feedback.
    Processor* osc_feedback_transpose = createPolyModControl("osc_feedback_transpose", -12, false);
    Processor* osc_feedback_amount = createPolyModControl("osc_feedback_amount", 0.0, false);
    Processor* osc_feedback_tune = createPolyModControl("osc_feedback_tune", 0.0, false);
    Add* osc_feedback_transposed = new Add();
    osc_feedback_transposed->plug(bent_midi, 0);
    osc_feedback_transposed->plug(osc_feedback_transpose, 1);
    Add* osc_feedback_midi = new Add();
    osc_feedback_midi->plug(osc_feedback_transposed, 0);
    osc_feedback_midi->plug(osc_feedback_tune, 1);

    MidiScale* osc_feedback_frequency = new MidiScale();
    osc_feedback_frequency->plug(osc_feedback_midi);
    Inverse* osc_feedback_period = new Inverse();
    osc_feedback_period->plug(osc_feedback_frequency);

    addProcessor(osc_feedback_transposed);
    addProcessor(osc_feedback_midi);
    addProcessor(osc_feedback_frequency);
    addProcessor(osc_feedback_period);

    osc_feedback_ = new Delay(MAX_FEEDBACK_SAMPLES);
    osc_feedback_->plug(oscillator_mix_, Delay::kAudio);
    osc_feedback_->plug(osc_feedback_period, Delay::kDelayTime);
    osc_feedback_->plug(osc_feedback_amount, Delay::kFeedback);
    osc_feedback_->plug(&utils::value_half, Delay::kWet);
    addProcessor(osc_feedback_);

    mod_sources_["osc_1"] = oscillators_->getOscillator1Output();
    mod_sources_["osc_2"] = oscillators_->getOscillator2Output();
  }

  void TwytchVoiceHandler::createModulators(Output* reset) {
    // LFO 1.
    Value* lfo1_waveform = new Value(Wave::kSin);
    Processor* lfo1_frequency = createMonoModControl("lfo_1_frequency", 2.0, false);
    lfo1_ = new Oscillator();
    lfo1_->plug(lfo1_waveform, Oscillator::kWaveform);
    lfo1_->plug(lfo1_frequency, Oscillator::kFrequency);

    addGlobalProcessor(lfo1_);
    controls_["lfo_1_waveform"] = lfo1_waveform;

    // LFO 2.
    Value* lfo2_waveform = new Value(Wave::kSin);
    Processor* lfo2_frequency = createPolyModControl("lfo_2_frequency", 2.0, false);
    lfo2_ = new Oscillator();
    lfo2_->plug(reset, Oscillator::kReset);
    lfo2_->plug(lfo2_waveform, Oscillator::kWaveform);
    lfo2_->plug(lfo2_frequency, Oscillator::kFrequency);

    addProcessor(lfo2_);
    controls_["lfo_2_waveform"] = lfo2_waveform;

    // Step Sequencer.
    Value* num_steps = new Value(16);
    Processor* step_frequency = createPolyModControl("step_frequency", 5.0, false);
    step_sequencer_ = new StepGenerator(MAX_STEPS);
    step_sequencer_->plug(num_steps, StepGenerator::kNumSteps);
    step_sequencer_->plug(step_frequency, StepGenerator::kFrequency);

    addProcessor(step_sequencer_);
    controls_["num_steps"] = num_steps;

    for (int i = 0; i < MAX_STEPS; ++i) {
      std::string num = std::to_string(i);
      if (num.length() == 1)
        num = "0" + num;
      Value* step = new Value(0.0);
      controls_[std::string("step_seq_") + num] = step;
      step_sequencer_->plug(step, StepGenerator::kSteps + i);
    }

    // Modulation sources/destinations.
    mod_sources_["lfo_1"] = lfo1_->output();
    mod_sources_["lfo_2"] = lfo2_->output();
    mod_sources_["step_sequencer"] = step_sequencer_->output();
  }

  void TwytchVoiceHandler::createFilter(
      Output* audio, Output* keytrack, Output* reset, Output* note_event) {
    // Filter envelope.
    Processor* filter_attack = createPolyModControl("fil_attack", 0.01, false);
    Processor* filter_decay = createPolyModControl("fil_decay", 0.3, true);
    Processor* filter_sustain = createPolyModControl("fil_sustain", 0.3, false);
    Processor* filter_release = createPolyModControl("fil_release", 0.3, true);

    TriggerFilter* note_off = new TriggerFilter(VoiceEvent::kVoiceOff);
    note_off->plug(note_event);
    TriggerCombiner* filter_env_trigger = new TriggerCombiner();
    filter_env_trigger->plug(note_off, 0);
    filter_env_trigger->plug(reset, 1);

    filter_envelope_ = new Envelope();
    filter_envelope_->plug(filter_attack, Envelope::kAttack);
    filter_envelope_->plug(filter_decay, Envelope::kDecay);
    filter_envelope_->plug(filter_sustain, Envelope::kSustain);
    filter_envelope_->plug(filter_release, Envelope::kRelease);
    filter_envelope_->plug(filter_env_trigger, Envelope::kTrigger);

    Processor* filter_envelope_depth = createPolyModControl("fil_env_depth", 48, false);
    Multiply* scaled_envelope = new Multiply();
    scaled_envelope->plug(filter_envelope_, 0);
    scaled_envelope->plug(filter_envelope_depth, 1);

    addProcessor(filter_envelope_);
    addProcessor(note_off);
    addProcessor(filter_env_trigger);
    addProcessor(scaled_envelope);

    // Filter.
    Value* filter_type = new Value(Filter::kLowPass);
    Processor* keytrack_amount = createPolyModControl("keytrack", 0.0, false);
    Multiply* current_keytrack = new Multiply();
    current_keytrack->plug(keytrack, 0);
    current_keytrack->plug(keytrack_amount, 1);

    Processor* base_cutoff = createPolyModControl("cutoff", 80, true, true);
    Add* keytracked_cutoff = new Add();
    keytracked_cutoff->setControlRate();
    keytracked_cutoff->plug(base_cutoff, 0);
    keytracked_cutoff->plug(current_keytrack, 1);

    Add* midi_cutoff = new Add();
    midi_cutoff->setControlRate();
    midi_cutoff->plug(keytracked_cutoff, 0);
    midi_cutoff->plug(scaled_envelope, 1);

    MidiScale* frequency_cutoff = new MidiScale();
    frequency_cutoff->setControlRate();
    frequency_cutoff->plug(midi_cutoff);

    Processor* resonance = createPolyModControl("resonance", 0.5, true);
    ResonanceScale* final_resonance = new ResonanceScale();
    final_resonance->setControlRate();
    final_resonance->plug(resonance);

    Value* min_db = new Value(MIN_GAIN_DB);
    Value* max_db = new Value(MAX_GAIN_DB);
    Interpolate* decibals = new Interpolate();
    decibals->setControlRate();
    decibals->plug(min_db, Interpolate::kFrom);
    decibals->plug(max_db, Interpolate::kTo);
    decibals->plug(resonance, Interpolate::kFractional);
    MagnitudeScale* final_gain = new MagnitudeScale();
    final_gain->setControlRate();
    final_gain->plug(decibals);

    Processor* filter_saturation = createPolyModControl("filter_saturation", 0.0, false);
    MagnitudeScale* saturation_magnitude = new MagnitudeScale();
    saturation_magnitude->plug(filter_saturation);

    Multiply* saturated_audio = new Multiply();
    saturated_audio->plug(audio, 0);
    saturated_audio->plug(saturation_magnitude, 1);

    filter_ = new Filter();
    filter_->plug(saturated_audio, Filter::kAudio);
    filter_->plug(filter_type, Filter::kType);
    filter_->plug(reset, Filter::kReset);
    filter_->plug(frequency_cutoff, Filter::kCutoff);
    filter_->plug(final_resonance, Filter::kResonance);
    filter_->plug(final_gain, Filter::kGain);

    distorted_filter_ = new Distortion();
    Value* distortion_type = new Value(Distortion::kTanh);
    Value* distortion_threshold = new Value(0.5);
    distorted_filter_->plug(filter_, Distortion::kAudio);
    distorted_filter_->plug(distortion_type, Distortion::kType);
    distorted_filter_->plug(distortion_threshold, Distortion::kThreshold);

    addProcessor(current_keytrack);
    addProcessor(saturated_audio);
    addProcessor(keytracked_cutoff);
    addProcessor(midi_cutoff);
    addProcessor(final_resonance);
    addProcessor(decibals);
    addProcessor(final_gain);
    addProcessor(frequency_cutoff);
    addProcessor(filter_);

    addProcessor(saturation_magnitude);
    addProcessor(distorted_filter_);

    controls_["filter_type"] = filter_type;

    mod_sources_["filter_env"] = filter_envelope_->output();

    // Formant Filter.
    formant_container_ = new BypassRouter();
    Value* formant_bypass = new Value(1);
    formant_container_->plug(formant_bypass, BypassRouter::kBypass);
    formant_container_->plug(distorted_filter_, BypassRouter::kAudio);

    formant_filter_ = new FormantManager(NUM_FORMANTS);
    Value* formant_passthrough = new Value(0.0);
    formant_filter_->plug(distorted_filter_, FormantManager::kAudio);
    formant_filter_->plug(formant_passthrough, FormantManager::kPassthroughGain);
    formant_filter_->plug(reset, FormantManager::kReset);

    controls_["formant_bypass"] = formant_bypass;
    controls_["formant_passthrough"] = formant_passthrough;

    Processor* formant_x = createPolyModControl("formant_x", 0.0, false, true);
    Processor* formant_y = createPolyModControl("formant_y", 0.0, false, true);

    for (int i = 0; i < NUM_FORMANTS; ++i) {
      BilinearInterpolate* formant_gain = new BilinearInterpolate();
      BilinearInterpolate* formant_q = new BilinearInterpolate();
      formant_q->setControlRate();
      BilinearInterpolate* formant_frequency = new BilinearInterpolate();
      formant_frequency->setControlRate();

      formant_gain->plug(top_left_formants[i].gain, BilinearInterpolate::kTopLeft);
      formant_gain->plug(top_right_formants[i].gain, BilinearInterpolate::kTopRight);
      formant_gain->plug(bottom_left_formants[i].gain, BilinearInterpolate::kBottomLeft);
      formant_gain->plug(bottom_right_formants[i].gain, BilinearInterpolate::kBottomRight);

      formant_q->plug(top_left_formants[i].resonance, BilinearInterpolate::kTopLeft);
      formant_q->plug(top_right_formants[i].resonance, BilinearInterpolate::kTopRight);
      formant_q->plug(bottom_left_formants[i].resonance, BilinearInterpolate::kBottomLeft);
      formant_q->plug(bottom_right_formants[i].resonance, BilinearInterpolate::kBottomRight);

      formant_frequency->plug(top_left_formants[i].frequency, BilinearInterpolate::kTopLeft);
      formant_frequency->plug(top_right_formants[i].frequency, BilinearInterpolate::kTopRight);
      formant_frequency->plug(bottom_left_formants[i].frequency, BilinearInterpolate::kBottomLeft);
      formant_frequency->plug(bottom_right_formants[i].frequency, BilinearInterpolate::kBottomRight);

      formant_gain->plug(formant_x, BilinearInterpolate::kXPosition);
      formant_q->plug(formant_x, BilinearInterpolate::kXPosition);
      formant_frequency->plug(formant_x, BilinearInterpolate::kXPosition);

      formant_gain->plug(formant_y, BilinearInterpolate::kYPosition);
      formant_q->plug(formant_y, BilinearInterpolate::kYPosition);
      formant_frequency->plug(formant_y, BilinearInterpolate::kYPosition);

      formant_filter_->getFormant(i)->plug(formant_gain, Formant::kGain);
      formant_filter_->getFormant(i)->plug(formant_q, Formant::kResonance);
      formant_filter_->getFormant(i)->plug(formant_frequency, Formant::kFrequency);

      addProcessor(formant_gain);
      addProcessor(formant_q);
      addProcessor(formant_frequency);
    }

    formant_container_->addProcessor(formant_filter_);
    formant_container_->registerOutput(formant_filter_->output());

    addProcessor(formant_container_);
  }

  void TwytchVoiceHandler::createArticulation(
      Output* note, Output* velocity, Output* trigger) {
    // Legato.
    Value* legato = new Value(0);
    LegatoFilter* legato_filter = new LegatoFilter();
    legato_filter->plug(legato, LegatoFilter::kLegato);
    legato_filter->plug(trigger, LegatoFilter::kTrigger);

    controls_["legato"] = legato;
    addProcessor(legato_filter);

    // Amplitude envelope.
    Processor* amplitude_attack = createPolyModControl("amp_attack", 0.01, false);
    Processor* amplitude_decay = createPolyModControl("amp_decay", 0.7, true);
    Processor* amplitude_sustain = createPolyModControl("amp_sustain", 0.5, false);
    Processor* amplitude_release = createPolyModControl("amp_release", 0.3, true);

    amplitude_envelope_ = new Envelope();
    amplitude_envelope_->plug(legato_filter->output(LegatoFilter::kRetrigger),
                              Envelope::kTrigger);
    amplitude_envelope_->plug(amplitude_attack, Envelope::kAttack);
    amplitude_envelope_->plug(amplitude_decay, Envelope::kDecay);
    amplitude_envelope_->plug(amplitude_sustain, Envelope::kSustain);
    amplitude_envelope_->plug(amplitude_release, Envelope::kRelease);
    addProcessor(amplitude_envelope_);

    // Voice and frequency resetting logic.
    note_change_trigger_ = new TriggerCombiner();
    note_change_trigger_->plug(legato_filter->output(LegatoFilter::kRemain), 0);
    note_change_trigger_->plug(amplitude_envelope_->output(Envelope::kFinished), 1);

    TriggerWait* note_wait = new TriggerWait();
    Value* current_note = new Value();
    note_wait->plug(note, TriggerWait::kWait);
    note_wait->plug(note_change_trigger_, TriggerWait::kTrigger);
    current_note->plug(note_wait);

    Value* max_midi_invert = new Value(1.0 / (MIDI_SIZE - 1));
    Multiply* note_percentage = new Multiply();
    note_percentage->plug(max_midi_invert, 0);
    note_percentage->plug(current_note, 1);

    addProcessor(note_change_trigger_);
    addProcessor(note_wait);
    addProcessor(current_note);

    // Key tracking.
    Value* center_adjust = new Value(-MIDI_SIZE / 2);
    note_from_center_ = new Add();
    note_from_center_->plug(center_adjust, 0);
    note_from_center_->plug(current_note, 1);

    addProcessor(note_from_center_);
    addProcessor(note_percentage);
    addGlobalProcessor(center_adjust);

    // Velocity tracking.
    TriggerWait* velocity_wait = new TriggerWait();
    Value* current_velocity = new Value();
    velocity_wait->plug(velocity, TriggerWait::kWait);
    velocity_wait->plug(note_change_trigger_, TriggerWait::kTrigger);
    current_velocity->plug(velocity_wait);

    addProcessor(velocity_wait);
    addProcessor(current_velocity);

    Processor* velocity_track_amount = createPolyModControl("velocity_track", 0.3, false);
    Interpolate* velocity_track_mult = new Interpolate();
    velocity_track_mult->plug(&utils::value_one, Interpolate::kFrom);
    velocity_track_mult->plug(current_velocity, Interpolate::kTo);
    velocity_track_mult->plug(velocity_track_amount, Interpolate::kFractional);
    addProcessor(velocity_track_mult);

    // Current amplitude using envelope and velocity.
    amplitude_ = new Multiply();
    amplitude_->plug(amplitude_envelope_->output(Envelope::kValue), 0);
    amplitude_->plug(velocity_track_mult, 1);

    addProcessor(amplitude_);

    // Portamento.
    Value* portamento = new Value(0.01);
    Value* portamento_type = new Value(0);
    PortamentoFilter* portamento_filter = new PortamentoFilter();
    portamento_filter->plug(portamento_type, PortamentoFilter::kPortamento);
    portamento_filter->plug(note_change_trigger_, PortamentoFilter::kFrequencyTrigger);
    portamento_filter->plug(trigger, PortamentoFilter::kVoiceTrigger);
    addProcessor(portamento_filter);

    current_frequency_ = new LinearSlope();
    current_frequency_->plug(current_note, LinearSlope::kTarget);
    current_frequency_->plug(portamento, LinearSlope::kRunSeconds);
    current_frequency_->plug(portamento_filter, LinearSlope::kTriggerJump);

    addProcessor(current_frequency_);
    controls_["portamento"] = portamento;
    controls_["portamento_type"] = portamento_type;

    mod_sources_["amplitude_env"] = amplitude_envelope_->output();
    mod_sources_["note"] = note_percentage->output();
    mod_sources_["velocity"] = current_velocity->output();
  }

  void TwytchVoiceHandler::setModWheel(mopo_float value) {
    mod_wheel_amount_->set(value);
  }

  void TwytchVoiceHandler::setPitchWheel(mopo_float value) {
    pitch_wheel_amount_->set(value);
  }
} // namespace mopo