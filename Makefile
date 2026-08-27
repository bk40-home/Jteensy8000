# =============================================================================
# Makefile — JT-8000 v2 host-side unit tests
# =============================================================================
# The core/ tree has zero Arduino dependencies, so it compiles and runs on
# the PC.  This is the "refactor with confidence" loop from the design brief
# (§11):  make test  after every change, before anything touches hardware.
#
# The warning flags are the PROJECT flags, not test-only flags — code that
# does not build cleanly here does not ship to the Teensy.
#   -Wdouble-promotion  catches accidental double math (M7 FPU is fp32;
#                       doubles are software-emulated and ~10x slower).
#
# Targets:
#   make test    build and run all unit tests (default)
#   make clean   remove build artefacts
# =============================================================================

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -g \
            -Wall -Wextra -Wdouble-promotion -Werror \
            -Isrc -Itest \
            -DJT_TESTING

BUILD    := build
TARGET   := $(BUILD)/jt8000_tests

# Core sources under test + the test suites themselves.
CORE := src/core/dsp/Curves.cpp \
        src/core/dsp/Crc32.cpp \
        src/core/dsp/EnvGen.cpp \
        src/core/dsp/Lfo.cpp \
        src/core/dsp/SlewedValue.cpp \
        src/core/dsp/TempoClock.cpp \
        src/core/dsp/FeedbackComb.cpp \
        src/core/dsp/OBXaCore.cpp \
        src/core/dsp/OscCore.cpp \
        src/core/dsp/SupersawOsc.cpp \
        src/core/dsp/PlateReverb.cpp \
        src/core/dsp/FxChain.cpp \
        src/core/dsp/StepSequencer.cpp \
        src/core/dsp/Arpeggiator.cpp \
        src/core/ParameterStore.cpp \
        src/core/PerfRouter.cpp \
        src/core/Patch.cpp \
        src/core/Performance.cpp \
        src/core/MidiParamTransport.cpp \
        src/core/ParamBroadcast.cpp \
        src/core/FilterSection.cpp \
        src/core/OscSection.cpp \
        src/core/WavetableLib.cpp \
        src/core/Voice.cpp \
        src/core/VoiceAllocator.cpp \
        src/core/SynthCore.cpp

SRCS := $(CORE) \
        src/platform/ExternalClock.cpp \
        test/test_main.cpp \
        test/test_broadcast.cpp \
        test/test_curves.cpp \
        test/test_parameter_store.cpp \
        test/test_perf_router.cpp \
        test/test_patch.cpp \
        test/test_performance_file.cpp \
        test/test_midi_transport.cpp \
        test/test_engine.cpp \
        test/test_osc.cpp \
        test/test_supersaw.cpp \
        test/test_osc_section.cpp \
        test/test_feedback.cpp \
        test/test_filter_section.cpp \
        test/test_korg35.cpp \
        test/test_moog_ladder.cpp \
        test/test_moogdv.cpp \
        test/test_param_slew.cpp \
        test/test_filter_env.cpp \
        test/test_pitch_env.cpp \
        test/test_velocity.cpp \
        test/test_wavetables.cpp \
        test/test_lfo.cpp \
        test/test_bpmclock.cpp \
        test/test_performance.cpp \
        test/test_reverb.cpp \
        test/test_fxchain.cpp \
        test/test_sequencer.cpp \
        test/test_arpeggiator.cpp

OBJS := $(SRCS:%.cpp=$(BUILD)/%.o)

RENDER_OBJS := $(CORE:%.cpp=$(BUILD)/%.o) $(BUILD)/tools/render_wav.o
RENDER_BIN  := $(BUILD)/render_wav

# Section-attribute gate: compile every core TU with the Teensy attribute
# macros ACTIVE (host codegen).  Catches .progmem section-type conflicts
# (COMDAT vs plain linkage) that the normal host build cannot see — a real
# firmware link failure taught us this.  See OBXaCore.h's linkage rule.
attrcheck:
	@set -e; for f in $$(find src/core -name "*.cpp"); do \
	  g++ -std=c++17 -O2 -Wall -Wextra -Wdouble-promotion \
	    -D__IMXRT1062__ -Isrc -S -o /dev/null $$f; \
	done
	@# PLACEMENT gate: prove the flash attributes actually LAND.  Compiles
	@# the wavetable TU with the Teensy PROGMEM branch active (host objdump
	@# reads the sections), then asserts (1) the .progmem section carries
	@# the catalogue (>= 400 KB) and (2) no wave symbol leaked into .rodata
	@# -> DTCM.  Exists because a sed once self-mangled AkwfCompat.h and
	@# 510 KB moved to RAM with zero warnings.
	@mkdir -p $(BUILD)
	@g++ -std=gnu++17 -O2 -DARDUINO=10819 -D__IMXRT1062__ -Isrc \
	  -Itest/stubs/teensy_progmem -c src/core/WavetableLib.cpp \
	  -o $(BUILD)/attr_wt.o
	@hex=$$(objdump -h $(BUILD)/attr_wt.o | awk '/\.progmem/ {print $$3; exit}'); \
	  sz=$$(printf '%d' "0x$$hex" 2>/dev/null || echo 0); \
	  test "$$sz" -ge 400000 || \
	  { echo "attrcheck FAIL: .progmem only $$sz bytes — PROGMEM lost?"; exit 1; }
	@objdump -t $(BUILD)/attr_wt.o | grep " \.rodata" | grep -q "akwf_akwf" && \
	  { echo "attrcheck FAIL: wave tables leaked into .rodata (DTCM)"; exit 1; } || true
	@g++ -std=gnu++17 -O2 -D__IMXRT1062__ -Isrc -c src/core/dsp/OBXaCore.cpp \
	  -o $(BUILD)/attr_ob.o
	@objdump -t $(BUILD)/attr_ob.o | grep kObxaPoleMix | grep -q "\.progmem" || \
	  { echo "attrcheck FAIL: kObxaPoleMix not in .progmem"; exit 1; }
	@echo "attrcheck: OK (sections verified, .progmem carries the tables)"

.PHONY: test render clean attrcheck
.DEFAULT_GOAL := test

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Depend on every header conservatively: the project is small enough that
# a full rebuild on any header change is cheaper than maintaining .d files.
$(BUILD)/%.o: %.cpp $(wildcard src/core/*.h src/core/dsp/*.h src/gen/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Offline audition/regression renders (design brief §11) — WAVs in ./renders/
render: $(RENDER_BIN)
	@mkdir -p renders
	./$(RENDER_BIN)

$(RENDER_BIN): $(RENDER_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD)
