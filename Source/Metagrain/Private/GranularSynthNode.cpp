// Copyright Epic Games, Inc. All Rights Reserved. // Or your copyright notice

#include "Metagrain.h" 
#include "MetasoundExecutableOperator.h"
#include "MetasoundPrimitives.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundStandardNodesNames.h" // For StandardNodes::Namespace
#include "MetasoundFacade.h"           // Include for FNodeFacade
#include "MetasoundParamHelper.h"
#include "MetasoundAudioBuffer.h"      // For FAudioBuffer
#include "MetasoundWave.h"             // For FWaveAsset
#include "MetasoundTime.h"             // Include for FTime
#include "MetasoundTrigger.h"          // Include for FTrigger
#include "MetasoundOperatorSettings.h" // Required for FOperatorSettings
#include "MetasoundDataReferenceCollection.h" // Required for FDataReferenceCollection
#include "MetasoundVertex.h"           // Required for FInputVertexInterface, FOutputVertexInterface etc.
#include "MetasoundNodeInterface.h"    // Required for FNodeInterface, FVertexInterface, PluginNodeMissingPrompt
#include "MetasoundBuilderInterface.h" // Required for FBuildOperatorParams, FBuildResults
#include "MetasoundLog.h"              // For UE_LOG specific to Metasounds
#include "DSP/Dsp.h"                   // For SampleRate, BlockRate, etc.
#include "DSP/FloatArrayMath.h"        // Correct header for Audio::ArrayMixIn, etc.
#include "DSP/MultichannelLinearResampler.h" // Needed for pitch shifting
#include "DSP/ConvertDeinterleave.h"   // Needed to prepare audio for resampler
#include "DSP/MultichannelBuffer.h"    // Needed for deinterleaved buffers
#include "Containers/Array.h"          // Required for TArray
#include "Sound/SoundWaveProxyReader.h" // Include the wave reader
#include "AudioDevice.h"              // Required for FAudioDevice::GetMainAudioDevice() potentially needed for reader init
#include "Algo/Reverse.h"              // For Algo::Reverse

#include "Internationalization/Text.h" // Required for LOCTEXT, FText
#include "UObject/NameTypes.h"         // Required for FName
#include "Math/UnrealMathUtility.h"    // Required for FMath, FMath::Cos, FMath::Sin, PI
#include <limits>                     // Required for TNumericLimits

// Define a unique namespace for localization text
#define LOCTEXT_NAMESPACE "GranularSynthNode" 

namespace Metasound
{
    // --- Parameter Names ---
    namespace GranularSynthNode_VertexNames 
    {
        // Inputs
        METASOUND_PARAM(InputTriggerPlay, "Play", "Start generating grains.");
        METASOUND_PARAM(InputTriggerStop, "Stop", "Stop generating grains.");
        METASOUND_PARAM(InParamWaveAsset, "Wave Asset", "The audio wave to granulate.");
        METASOUND_PARAM(InParamGrainDuration, "Grain Duration (ms)", "The base duration of each grain in milliseconds.");
        METASOUND_PARAM(InParamDurationRand, "Duration Rand (ms)", "Maximum POSITIVE random variation applied to the grain duration in milliseconds.");
        METASOUND_PARAM(InParamActiveVoices, "Active Voices", "Target number of grains overlapping on average. Determines grain density based on duration (e.g., 1 = one grain starts as previous ends; 2 = two grains overlap on average).");
        METASOUND_PARAM(InParamTimeJitter, "Time Jitter (%)", "Amount of randomization to apply to the grain spawn interval (0% = no jitter, 100% = interval can vary from 0 to 2x base interval).");
        METASOUND_PARAM(InParamStartPoint, "Start Point (s)", "The base time in seconds to start reading grains from. This value, after randomization, will wrap around the audio file's duration if it exceeds it.");
        METASOUND_PARAM(InParamStartPointRand, "Start Point Rand (ms)", "Maximum POSITIVE random offset applied to the Start Point in milliseconds. The randomization occurs relative to the Start Point, and the result is then wrapped.");
        METASOUND_PARAM(InParamReverseChance, "Reverse Chance (%)", "Percentage chance (0-100) that a grain will play in reverse.");
        METASOUND_PARAM(InParamAttackTimePercent, "Attack", "Attack time as a percentage of grain duration (0.0 - 1.0).");
        METASOUND_PARAM(InParamDecayTimePercent, "Decay", "Decay time as a percentage of grain duration (0.0 - 1.0).");
        METASOUND_PARAM(InParamAttackCurve, "Attack Curve", "Attack envelope curve shape exponent.");
        METASOUND_PARAM(InParamDecayCurve, "Decay Curve", "Decay envelope curve shape exponent.");
        METASOUND_PARAM(InParamPitchShift, "Pitch Shift (Semi)", "Base pitch shift in semitones.");
        METASOUND_PARAM(InParamPitchRand, "Pitch Rand (Semi)", "Maximum random pitch variation (+/-) in semitones.");
        METASOUND_PARAM(InParamPan, "Pan", "Stereo pan position (-1.0 Left to 1.0 Right).");
        METASOUND_PARAM(InParamPanRand, "Pan Rand", "Maximum random pan variation (+/-) (0.0 to 1.0).");
        METASOUND_PARAM(InParamVolumeRand, "Volume Rand (%)", "Maximum random volume reduction (0% = full volume, 100% = can be silent).");
        METASOUND_PARAM(InputWarmStart, "Warm Start", "If true, attempts to trigger multiple grains immediately on play, based on Active Voices count."); // New Input

        // Outputs
        METASOUND_PARAM(OutputTriggerOnPlay, "On Play", "Triggers when Play is triggered.");
        METASOUND_PARAM(OutputTriggerOnFinished, "On Finished", "Triggers when Stop is triggered or generation otherwise finishes.");
        METASOUND_PARAM(OutputTriggerOnGrain, "On Grain", "Triggers when a new grain is successfully started.");
        METASOUND_PARAM(OutParamAudioLeft, "Out Left", "The left channel audio output.");
        METASOUND_PARAM(OutParamAudioRight, "Out Right", "The right channel audio output.");
        METASOUND_PARAM(OutputGrainStartTime, "Grain Start Time", "The final calculated start time of the triggered grain within the source audio file (in seconds).");
        METASOUND_PARAM(OutputGrainDurationSec, "Grain Duration", "The final calculated duration of the triggered grain (in seconds).");
        METASOUND_PARAM(OutputGrainIsReversed, "Grain Reversed", "True if the triggered grain is playing in reverse.");
        METASOUND_PARAM(OutputGrainVolume, "Grain Volume", "The final calculated volume scale (0.0-1.0) of the triggered grain.");
        METASOUND_PARAM(OutputGrainPitch, "Grain Pitch", "The final calculated pitch shift (in semitones) of the triggered grain.");
        METASOUND_PARAM(OutputGrainPan, "Grain Pan", "The final calculated stereo pan position (-1.0 to 1.0) of the triggered grain.");
    }

    // --- Grain Voice Structure ---
    struct FGrainVoice
    {
        TUniquePtr<FSoundWaveProxyReader> Reader;
        TUniquePtr<Audio::FMultichannelLinearResampler> Resampler;
        Audio::FMultichannelCircularBuffer SourceCircularBuffer;
        bool bIsActive = false;
        int32 NumChannels = 0;
        int32 SamplesRemaining = 0;
        int32 SamplesPlayed = 0;
        int32 TotalGrainSamples = 0;
        float PanPosition = 0.0f;
        float VolumeScale = 1.0f;
        bool bIsReversed = false;
        Audio::FMultichannelBuffer FullGrainSegmentBuffer;
        int32 FullGrainSegmentReadOffset = 0;
        Audio::FAlignedFloatBuffer InterleavedReadBuffer;
        Audio::FAlignedFloatBuffer EnvelopedMonoBuffer;
    };

    // --- Operator ---
    class FGranularSynthOperator : public TExecutableOperator<FGranularSynthOperator>
    {
        static constexpr int32 MaxGrainVoices = 32;
        static constexpr float MinGrainDurationSeconds = 0.005f;
        static constexpr float MaxAbsPitchShiftSemitones = 60.0f;
        static constexpr int32 DeinterleaveBlockSizeFrames = 256;
        static constexpr float MinActiveVoicesParam = 0.01f; // Minimum value for ActiveVoices to calculate interval
        static constexpr float MinSamplesPerGrainInterval = 1.0f;
        static constexpr float Epsilon = 1e-6f;

    public:
        FGranularSynthOperator(const FOperatorSettings& InSettings, 
            const FTriggerReadRef& InPlayTrigger,
            const FTriggerReadRef& InStopTrigger,
            const FWaveAssetReadRef& InWaveAsset,
            const FFloatReadRef& InGrainDurationMs,
            const FFloatReadRef& InDurationRandMs,
            const FFloatReadRef& InActiveVoices,
            const FFloatReadRef& InTimeJitter,
            const FTimeReadRef& InStartPointTime,
            const FFloatReadRef& InStartPointRandMs,
            const FFloatReadRef& InReverseChance,
            const FFloatReadRef& InAttackTimePercent,
            const FFloatReadRef& InDecayTimePercent,
            const FFloatReadRef& InAttackCurve,
            const FFloatReadRef& InDecayCurve,
            const FFloatReadRef& InPitchShift,
            const FFloatReadRef& InPitchRand,
            const FFloatReadRef& InPan,
            const FFloatReadRef& InPanRand,
            const FFloatReadRef& InVolumeRand,
            const FBoolReadRef& InWarmStart 
        )
            : PlayTrigger(InPlayTrigger)
            , StopTrigger(InStopTrigger)
            , WaveAssetInput(InWaveAsset)
            , GrainDurationMsInput(InGrainDurationMs)
            , DurationRandMsInput(InDurationRandMs)
            , ActiveVoicesInput(InActiveVoices)
            , TimeJitterInput(InTimeJitter)
            , StartPointTimeInput(InStartPointTime)
            , StartPointRandMsInput(InStartPointRandMs)
            , ReverseChanceInput(InReverseChance)
            , AttackTimePercentInput(InAttackTimePercent)
            , DecayTimePercentInput(InDecayTimePercent)
            , AttackCurveInput(InAttackCurve)
            , DecayCurveInput(InDecayCurve)
            , PitchShiftInput(InPitchShift)
            , PitchRandInput(InPitchRand)
            , PanInput(InPan)
            , PanRandInput(InPanRand)
            , VolumeRandInput(InVolumeRand)
            , WarmStartInput(InWarmStart)
            , OnPlayTrigger(FTriggerWriteRef::CreateNew(InSettings))
            , OnFinishedTrigger(FTriggerWriteRef::CreateNew(InSettings))
            , OnGrainTriggered(FTriggerWriteRef::CreateNew(InSettings))
            , AudioOutputLeft(FAudioBufferWriteRef::CreateNew(InSettings))
            , AudioOutputRight(FAudioBufferWriteRef::CreateNew(InSettings))
            , OutputGrainStartTimeRef(FTimeWriteRef::CreateNew(FTime(0.0)))
            , OutputGrainDurationSecRef(FFloatWriteRef::CreateNew(0.0f))
            , OutputGrainIsReversedRef(FBoolWriteRef::CreateNew(false))
            , OutputGrainVolumeRef(FFloatWriteRef::CreateNew(0.0f))
            , OutputGrainPitchRef(FFloatWriteRef::CreateNew(0.0f))
            , OutputGrainPanRef(FFloatWriteRef::CreateNew(0.0f))
            , SampleRate(InSettings.GetSampleRate())
            , BlockSize(InSettings.GetNumFramesPerBlock() > 0 ? InSettings.GetNumFramesPerBlock() : 256)
            , bIsPlaying(false)
        {
            if (InSettings.GetNumFramesPerBlock() <= 0)
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS Constructor: OperatorSettings provided an invalid BlockSize: %d. Defaulting to 256."), InSettings.GetNumFramesPerBlock());
            }
            GrainVoices.SetNum(MaxGrainVoices);
            for (FGrainVoice& Voice : GrainVoices)
            {
                Voice.EnvelopedMonoBuffer.SetNumUninitialized(BlockSize);
            }
            SamplesUntilNextGrain = 0.0f;
            CachedSoundWaveDuration = 0.0f;
        }

        static const FVertexInterface& DeclareVertexInterface()
        {
            using namespace GranularSynthNode_VertexNames;
            static const FVertexInterface Interface(
                FInputVertexInterface(
                    TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTriggerPlay)),
                    TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTriggerStop)),
                    TInputDataVertex<FWaveAsset>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamWaveAsset)),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamGrainDuration), 100.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDurationRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamActiveVoices), 1.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamReverseChance), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamTimeJitter), 0.0f),
                    TInputDataVertex<bool>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputWarmStart), false), 
                    TInputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamStartPoint)),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamStartPointRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamAttackTimePercent), 0.1f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDecayTimePercent), 0.1f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamAttackCurve), 1.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDecayCurve), 1.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPitchShift), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPitchRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPan), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPanRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamVolumeRand), 0.0f)
                ),
                FOutputVertexInterface(
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnPlay)),
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnFinished)),
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnGrain)),
                    TOutputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputGrainStartTime)),
                    TOutputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputGrainDurationSec)),
                    TOutputDataVertex<bool>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputGrainIsReversed)),
                    TOutputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputGrainVolume)),
                    TOutputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputGrainPitch)),
                    TOutputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputGrainPan)),
                    TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutParamAudioLeft)),
                    TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutParamAudioRight))
                )
            );
            return Interface;
        }

        static const FNodeClassMetadata& GetNodeInfo()
        {
            auto CreateNodeClassMetadata = []() -> FNodeClassMetadata
                {
                    FNodeClassMetadata Metadata;
                    Metadata.ClassName = { FName("GranularSynth"), FName(""), FName("Metagrain") };
                    Metadata.MajorVersion = 0; Metadata.MinorVersion = 6; 
                    Metadata.DisplayName = LOCTEXT("GranularSynth_DisplayName", "Granular Synth"); 
                    Metadata.Description = LOCTEXT("GranularSynth_Description", "Granular synthesizer with active voice controls");
                    Metadata.Author = TEXT("Maksym Kokoiev & Wouter Meija");
                    Metadata.PromptIfMissing = Metasound::PluginNodeMissingPrompt;
                    Metadata.DefaultInterface = DeclareVertexInterface();
                    Metadata.CategoryHierarchy = { LOCTEXT("GranularSynthCategory", "Synth") };
                    Metadata.Keywords = TArray<FText>();
                    return Metadata;
                };
            static const FNodeClassMetadata Metadata = CreateNodeClassMetadata();
            return Metadata;
        }

        static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& InParams, FBuildResults& OutResults)
        {
            using namespace GranularSynthNode_VertexNames;
            const FInputVertexInterfaceData& InputData = InParams.InputData;
            FTriggerReadRef PlayTriggerIn = InputData.GetOrConstructDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(InputTriggerPlay), InParams.OperatorSettings);
            FTriggerReadRef StopTriggerIn = InputData.GetOrConstructDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(InputTriggerStop), InParams.OperatorSettings);
            FWaveAssetReadRef WaveAssetIn = InputData.GetOrCreateDefaultDataReadReference<FWaveAsset>(METASOUND_GET_PARAM_NAME(InParamWaveAsset), InParams.OperatorSettings);
            FFloatReadRef GrainDurationIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamGrainDuration), InParams.OperatorSettings);
            FFloatReadRef DurationRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamDurationRand), InParams.OperatorSettings);
            FFloatReadRef ActiveVoicesIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamActiveVoices), InParams.OperatorSettings);
            FFloatReadRef TimeJitterIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamTimeJitter), InParams.OperatorSettings);
            FTimeReadRef StartPointIn = InputData.GetOrCreateDefaultDataReadReference<FTime>(METASOUND_GET_PARAM_NAME(InParamStartPoint), InParams.OperatorSettings);
            FFloatReadRef StartPointRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamStartPointRand), InParams.OperatorSettings);
            FFloatReadRef ReverseChanceIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamReverseChance), InParams.OperatorSettings);
            FFloatReadRef AttackTimePercentIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamAttackTimePercent), InParams.OperatorSettings);
            FFloatReadRef DecayTimePercentIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamDecayTimePercent), InParams.OperatorSettings);
            FFloatReadRef AttackCurveIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamAttackCurve), InParams.OperatorSettings);
            FFloatReadRef DecayCurveIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamDecayCurve), InParams.OperatorSettings);
            FFloatReadRef PitchShiftIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPitchShift), InParams.OperatorSettings);
            FFloatReadRef PitchRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPitchRand), InParams.OperatorSettings);
            FFloatReadRef PanIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPan), InParams.OperatorSettings);
            FFloatReadRef PanRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPanRand), InParams.OperatorSettings);
            FFloatReadRef VolumeRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamVolumeRand), InParams.OperatorSettings);
            FBoolReadRef WarmStartIn = InputData.GetOrCreateDefaultDataReadReference<bool>(METASOUND_GET_PARAM_NAME(InputWarmStart), InParams.OperatorSettings); // Get new input

            return MakeUnique<FGranularSynthOperator>(InParams.OperatorSettings,
                PlayTriggerIn, StopTriggerIn, WaveAssetIn, GrainDurationIn, DurationRandIn,
                ActiveVoicesIn, TimeJitterIn,
                StartPointIn, StartPointRandIn, ReverseChanceIn,
                AttackTimePercentIn, DecayTimePercentIn, AttackCurveIn, DecayCurveIn,
                PitchShiftIn, PitchRandIn, PanIn, PanRandIn, VolumeRandIn,
                WarmStartIn);
        }

        virtual void BindInputs(FInputVertexInterfaceData& InOutVertexData) override
        {
            using namespace GranularSynthNode_VertexNames;
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputTriggerPlay), PlayTrigger);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputTriggerStop), StopTrigger);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamWaveAsset), WaveAssetInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamGrainDuration), GrainDurationMsInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamDurationRand), DurationRandMsInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamActiveVoices), ActiveVoicesInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamTimeJitter), TimeJitterInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamStartPoint), StartPointTimeInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamStartPointRand), StartPointRandMsInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamReverseChance), ReverseChanceInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamAttackTimePercent), AttackTimePercentInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamDecayTimePercent), DecayTimePercentInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamAttackCurve), AttackCurveInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamDecayCurve), DecayCurveInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPitchShift), PitchShiftInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPitchRand), PitchRandInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPan), PanInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPanRand), PanRandInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamVolumeRand), VolumeRandInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputWarmStart), WarmStartInput);
        }
        virtual void BindOutputs(FOutputVertexInterfaceData& InOutVertexData) override
        {
            using namespace GranularSynthNode_VertexNames;
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnPlay), OnPlayTrigger);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnFinished), OnFinishedTrigger);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnGrain), OnGrainTriggered);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutParamAudioLeft), AudioOutputLeft);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutParamAudioRight), AudioOutputRight);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputGrainStartTime), OutputGrainStartTimeRef);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputGrainDurationSec), OutputGrainDurationSecRef);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputGrainIsReversed), OutputGrainIsReversedRef);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputGrainVolume), OutputGrainVolumeRef);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputGrainPitch), OutputGrainPitchRef);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputGrainPan), OutputGrainPanRef);
        }
        virtual FDataReferenceCollection GetInputs() const override
        {
            using namespace GranularSynthNode_VertexNames;
            FDataReferenceCollection InputDataReferences;
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputTriggerPlay), PlayTrigger);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputTriggerStop), StopTrigger);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamWaveAsset), WaveAssetInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamGrainDuration), GrainDurationMsInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamDurationRand), DurationRandMsInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamActiveVoices), ActiveVoicesInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamTimeJitter), TimeJitterInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamStartPoint), StartPointTimeInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamStartPointRand), StartPointRandMsInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamReverseChance), ReverseChanceInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamAttackTimePercent), AttackTimePercentInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamDecayTimePercent), DecayTimePercentInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamAttackCurve), AttackCurveInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamDecayCurve), DecayCurveInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPitchShift), PitchShiftInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPitchRand), PitchRandInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPan), PanInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPanRand), PanRandInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamVolumeRand), VolumeRandInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputWarmStart), WarmStartInput); 
            return InputDataReferences;
        }
        virtual FDataReferenceCollection GetOutputs() const override
        {
            using namespace GranularSynthNode_VertexNames;
            FDataReferenceCollection OutputDataReferences;
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnPlay), OnPlayTrigger);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnFinished), OnFinishedTrigger);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnGrain), OnGrainTriggered);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutParamAudioLeft), AudioOutputLeft);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutParamAudioRight), AudioOutputRight);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputGrainStartTime), OutputGrainStartTimeRef);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputGrainDurationSec), OutputGrainDurationSecRef);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputGrainIsReversed), OutputGrainIsReversedRef);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputGrainVolume), OutputGrainVolumeRef);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputGrainPitch), OutputGrainPitchRef);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputGrainPan), OutputGrainPanRef);
            return OutputDataReferences;
        }

        void Execute()
        {
            if (BlockSize <= 0)
            {
                UE_LOG(LogMetaSound, Error, TEXT("GS: Execute called with invalid BlockSize: %d. Aborting execution."), BlockSize);
                AudioOutputLeft->Zero(); AudioOutputRight->Zero();
                if (bIsPlaying) { OnFinishedTrigger->TriggerFrame(0); bIsPlaying = false; }
                return;
            }

            OnPlayTrigger->AdvanceBlock();
            OnFinishedTrigger->AdvanceBlock();
            OnGrainTriggered->AdvanceBlock();

            bool bTriggeredStopThisBlock = false; int32 StopFrame = -1;
            for (int32 Frame : StopTrigger->GetTriggeredFrames())
            {
                if (bIsPlaying) { bTriggeredStopThisBlock = true; StopFrame = Frame; break; }
            }

            for (int32 Frame : PlayTrigger->GetTriggeredFrames())
            {
                if (!TryStartPlayback(Frame))
                {
                    OnFinishedTrigger->TriggerFrame(Frame);
                    bIsPlaying = false;
                    bTriggeredStopThisBlock = false;
                }
                else
                {
                    bTriggeredStopThisBlock = false;
                }
            }

            if (bTriggeredStopThisBlock && bIsPlaying)
            {
                bIsPlaying = false;
                ResetVoices();
                OnFinishedTrigger->TriggerFrame(StopFrame);
            }

            if (!bIsPlaying)
            {
                AudioOutputLeft->Zero(); AudioOutputRight->Zero();
                if (CurrentWaveProxy.IsValid() || CurrentNumChannels > 0 || ConvertDeinterleave.IsValid())
                {
                    ResetVoices(); CurrentWaveProxy.Reset(); CachedSoundWaveDuration = 0.0f; CurrentNumChannels = 0; ConvertDeinterleave.Reset();
                }
                return;
            }

            bool bWaveJustChanged = false;
            const FSoundWaveProxyPtr InputProxy = WaveAssetInput->GetSoundWaveProxy();
            if (InputProxy.IsValid() && CurrentWaveProxy != InputProxy)
            {
                bWaveJustChanged = true;
                if (!InitializeWaveData(InputProxy))
                {
                    ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0); AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
                }
            }
            else if (!InputProxy.IsValid() && CurrentWaveProxy.IsValid())
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS: Wave Asset Input became invalid. Stopping."));
                ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0); AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
            }

            if (CurrentNumChannels <= 0 || !ConvertDeinterleave.IsValid() || CachedSoundWaveDuration < MinGrainDurationSeconds)
            {
                UE_LOG(LogMetaSound, Error, TEXT("GS: Invalid state (channels/deinterleaver/duration). Stopping. Duration: %.3f"), CachedSoundWaveDuration);
                ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0); AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
            }

            const float BaseGrainDurationSeconds = FMath::Max(MinGrainDurationSeconds, *GrainDurationMsInput / 1000.0f);
            const float MaxDurationRandSeconds = FMath::Max(0.0f, *DurationRandMsInput / 1000.0f);
            const float EffectiveActiveVoices = FMath::Max(MinActiveVoicesParam, *ActiveVoicesInput); // Used for interval calculation
            const float TimeJitterPercent = FMath::Clamp(*TimeJitterInput, 0.0f, 100.0f);
            const float BaseSamplesPerGrainInterval = (EffectiveActiveVoices > 0.0f && BaseGrainDurationSeconds > 0.0f && SampleRate > 0.0f)
                ? (BaseGrainDurationSeconds / EffectiveActiveVoices) * SampleRate : TNumericLimits<float>::Max();

            const float UserBaseStartPointSeconds = StartPointTimeInput->GetSeconds();
            const float MaxStartPointRandMsValue = FMath::Max(0.0f, *StartPointRandMsInput);
            const float ReverseChanceVal = FMath::Clamp(*ReverseChanceInput, 0.0f, 100.0f);
            const float AttackPercent = FMath::Clamp(*AttackTimePercentInput, 0.0f, 1.0f);
            const float DecayPercent = FMath::Clamp(*DecayTimePercentInput, 0.0f, 1.0f);
            const float ClampedDecayPercent = FMath::Min(DecayPercent, 1.0f - AttackPercent);
            const float AttackCurveFactor = FMath::Max(UE_SMALL_NUMBER, *AttackCurveInput);
            const float DecayCurveFactor = FMath::Max(UE_SMALL_NUMBER, *DecayCurveInput);
            const float BasePitchShiftSemitones = FMath::Clamp(*PitchShiftInput, -MaxAbsPitchShiftSemitones, MaxAbsPitchShiftSemitones);
            const float PitchRandSemitones = FMath::Max(0.0f, *PitchRandInput);
            const float BasePan = FMath::Clamp(*PanInput, -1.0f, 1.0f);
            const float PanRandAmount = FMath::Clamp(*PanRandInput, 0.0f, 1.0f);
            const float VolumeRandPercent = FMath::Clamp(*VolumeRandInput, 0.0f, 100.0f);

            float* OutputAudioLeftPtr = AudioOutputLeft->GetData();
            float* OutputAudioRightPtr = AudioOutputRight->GetData();
            if (!bWaveJustChanged) { FMemory::Memset(OutputAudioLeftPtr, 0, BlockSize * sizeof(float)); FMemory::Memset(OutputAudioRightPtr, 0, BlockSize * sizeof(float)); }

            int32 GrainsToTriggerThisBlock = 0; float ElapsedSamples = static_cast<float>(BlockSize);
            if (BaseSamplesPerGrainInterval > 0.0f && BaseSamplesPerGrainInterval < TNumericLimits<float>::Max())
            {
                while (SamplesUntilNextGrain <= ElapsedSamples)
                {
                    GrainsToTriggerThisBlock++;
                    float JitteredInterval = FMath::Max(MinSamplesPerGrainInterval, BaseSamplesPerGrainInterval + FMath::FRandRange(-1.0f, 1.0f) * BaseSamplesPerGrainInterval * (TimeJitterPercent / 100.0f));
                    SamplesUntilNextGrain += JitteredInterval;
                }
                SamplesUntilNextGrain -= ElapsedSamples;
            }

            for (int i = 0; i < GrainsToTriggerThisBlock; ++i)
            {
                float RandomStartOffsetSeconds = FMath::FRandRange(0.0f, MaxStartPointRandMsValue / 1000.0f);
                float ConceptualStartPointSecs = UserBaseStartPointSeconds + RandomStartOffsetSeconds;

                float DurationOffset = FMath::FRandRange(0.0f, MaxDurationRandSeconds);
                float FinalOutputGrainDurationSeconds = FMath::Max(MinGrainDurationSeconds, BaseGrainDurationSeconds + DurationOffset);
                int32 OutputGrainDurationSamples = FMath::Max(1, FMath::CeilToInt(FinalOutputGrainDurationSeconds * SampleRate));

                float PitchOffset = FMath::FRandRange(-PitchRandSemitones, PitchRandSemitones);
                float FinalTargetPitchShift = FMath::Clamp(BasePitchShiftSemitones + PitchOffset, -MaxAbsPitchShiftSemitones, MaxAbsPitchShiftSemitones);
                float FrameRatio = FMath::Pow(2.0f, FinalTargetPitchShift / 12.0f);
                FrameRatio = FMath::Max(UE_SMALL_NUMBER, FrameRatio);

                bool bFinalPlayThisGrainReverse = FMath::FRandRange(0.0f, 100.0f) < ReverseChanceVal;

                float FinalReaderStartTimeForSegment = 0.0f;
                int32 NumSourceFramesToReadForSegment = 0;
                bool bIsValidSegment = true;

                if (bFinalPlayThisGrainReverse)
                {
                    float SourceMaterialNeededSeconds = FinalOutputGrainDurationSeconds * FrameRatio;
                    if (SourceMaterialNeededSeconds < Epsilon) { bIsValidSegment = false; }
                    else
                    {
                        float ConceptualSegmentEndInSource = FMath::Fmod(ConceptualStartPointSecs, CachedSoundWaveDuration);
                        if (ConceptualSegmentEndInSource < 0.0f) ConceptualSegmentEndInSource += CachedSoundWaveDuration;
                        float ConceptualSegmentStartInSource = ConceptualSegmentEndInSource - SourceMaterialNeededSeconds;
                        float ActualSegmentStart = FMath::Max(0.0f, ConceptualSegmentStartInSource);
                        float ActualSegmentEnd = FMath::Min(CachedSoundWaveDuration, ConceptualSegmentEndInSource);
                        if (ActualSegmentStart == 0.0f && SourceMaterialNeededSeconds > 0.0f) { ActualSegmentEnd = FMath::Min(CachedSoundWaveDuration, SourceMaterialNeededSeconds); }
                        if (ActualSegmentStart >= ActualSegmentEnd - Epsilon) { NumSourceFramesToReadForSegment = 0; bIsValidSegment = false; }
                        else { NumSourceFramesToReadForSegment = FMath::CeilToInt((ActualSegmentEnd - ActualSegmentStart) * SampleRate); if (NumSourceFramesToReadForSegment <= 0) { bIsValidSegment = false; } }
                        FinalReaderStartTimeForSegment = ActualSegmentStart;
                    }
                }
                else
                {
                    FinalReaderStartTimeForSegment = FMath::Fmod(ConceptualStartPointSecs, CachedSoundWaveDuration);
                    if (FinalReaderStartTimeForSegment < 0.0f) FinalReaderStartTimeForSegment += CachedSoundWaveDuration;
                    FinalReaderStartTimeForSegment = FMath::Min(FinalReaderStartTimeForSegment, CachedSoundWaveDuration - MinGrainDurationSeconds);
                    FinalReaderStartTimeForSegment = FMath::Max(0.0f, FinalReaderStartTimeForSegment);
                }

                if (!bIsValidSegment)
                {
                    UE_LOG(LogMetaSound, Verbose, TEXT("GS: Skipping grain due to invalid/zero-length source segment. Reversed: %d, NumSourceFrames: %d"),
                        bFinalPlayThisGrainReverse, NumSourceFramesToReadForSegment);
                    continue;
                }

                float PanOffset = FMath::FRandRange(-PanRandAmount, PanRandAmount);
                float FinalGrainPanPosition = FMath::Clamp(BasePan + PanOffset, -1.0f, 1.0f);
                float MinVolumeScale = 1.0f - (VolumeRandPercent / 100.0f);
                float FinalGrainVolumeScale = FMath::FRandRange(MinVolumeScale, 1.0f);

                if (TriggerGrain(CurrentWaveProxy, OutputGrainDurationSamples, FinalReaderStartTimeForSegment, FrameRatio, FinalGrainPanPosition, FinalGrainVolumeScale, bFinalPlayThisGrainReverse, NumSourceFramesToReadForSegment))
                {
                    float ApproxTimeOfThisGrainSpawn = ElapsedSamples - (SamplesUntilNextGrain + (GrainsToTriggerThisBlock - 1 - i) * (BaseSamplesPerGrainInterval > Epsilon ? BaseSamplesPerGrainInterval : OutputGrainDurationSamples));
                    int32 TriggerFrameInBlock = FMath::Clamp(static_cast<int32>(ApproxTimeOfThisGrainSpawn), 0, BlockSize - 1);

                    *OutputGrainStartTimeRef = FTime(FinalReaderStartTimeForSegment);
                    *OutputGrainDurationSecRef = FinalOutputGrainDurationSeconds;
                    *OutputGrainIsReversedRef = bFinalPlayThisGrainReverse;
                    *OutputGrainVolumeRef = FinalGrainVolumeScale;
                    *OutputGrainPitchRef = FinalTargetPitchShift;
                    *OutputGrainPanRef = FinalGrainPanPosition;

                    OnGrainTriggered->TriggerFrame(TriggerFrameInBlock);
                }
            }

            for (FGrainVoice& Voice : GrainVoices)
            {
                if (Voice.bIsActive)
                {
                    const int32 OutputFramesToProcessThisBlock = FMath::Max(0, FMath::Min(BlockSize, Voice.SamplesRemaining));
                    if (OutputFramesToProcessThisBlock <= 0)
                    {
                        Voice.bIsActive = false;
                        Voice.Reader.Reset();
                        Voice.Resampler.Reset();
                        continue;
                    }

                    Voice.EnvelopedMonoBuffer.SetNumUninitialized(OutputFramesToProcessThisBlock, EAllowShrinking::No);
                    float* MonoBufferPtr = Voice.EnvelopedMonoBuffer.GetData();
                    int32 FramesSuccessfullyResampled = 0;

                    if ((Voice.bIsReversed || Voice.Reader.IsValid()) && Voice.Resampler.IsValid() && Voice.TotalGrainSamples > 0)
                    {
                        int32 InputFramesAvailableInCircularBuffer = Audio::GetMultichannelBufferNumFrames(Voice.SourceCircularBuffer);
                        int32 InputFramesNeededForOutput = Voice.Resampler->GetNumInputFramesNeededToProduceOutputFrames(OutputFramesToProcessThisBlock);

                        bool bSourceAudioEffectivelyExhausted = false;
                        while (InputFramesAvailableInCircularBuffer < InputFramesNeededForOutput && !bSourceAudioEffectivelyExhausted && InputFramesNeededForOutput > 0)
                        {
                            int32 FramesAvailableBeforeGenerate = InputFramesAvailableInCircularBuffer;
                            GenerateSourceAudio(Voice);
                            InputFramesAvailableInCircularBuffer = Audio::GetMultichannelBufferNumFrames(Voice.SourceCircularBuffer);

                            if (InputFramesAvailableInCircularBuffer == FramesAvailableBeforeGenerate)
                            {
                                bSourceAudioEffectivelyExhausted = true;
                            }
                        }

                        Audio::FMultichannelBuffer ResampledOutputBuffer;
                        Audio::SetMultichannelBufferSize(Voice.NumChannels, OutputFramesToProcessThisBlock, ResampledOutputBuffer);
                        FramesSuccessfullyResampled = Voice.Resampler->ProcessAndConsumeAudio(Voice.SourceCircularBuffer, ResampledOutputBuffer);

                        if (FramesSuccessfullyResampled > 0)
                        {
                            for (int32 FrameIndex = 0; FrameIndex < FramesSuccessfullyResampled; ++FrameIndex)
                            {
                                float MonoSample = 0.0f;
                                if (Voice.NumChannels == 1) { MonoSample = ResampledOutputBuffer[0][FrameIndex]; }
                                else if (Voice.NumChannels >= 2) { MonoSample = (ResampledOutputBuffer[0][FrameIndex] + ResampledOutputBuffer[1][FrameIndex]) * 0.5f; }

                                MonoSample *= Voice.VolumeScale;

                                float EnvelopeScale = 1.0f;
                                const int32 CurrentFrameInGrain = Voice.SamplesPlayed + FrameIndex;
                                const int32 AttackSamples = FMath::CeilToInt(Voice.TotalGrainSamples * AttackPercent);
                                const int32 DecaySamples = FMath::CeilToInt(Voice.TotalGrainSamples * ClampedDecayPercent);

                                if (CurrentFrameInGrain < AttackSamples)
                                {
                                    EnvelopeScale = FMath::Pow((AttackSamples > 0) ? (float)CurrentFrameInGrain / AttackSamples : 1.0f, AttackCurveFactor);
                                }
                                else if (CurrentFrameInGrain >= (Voice.TotalGrainSamples - DecaySamples))
                                {
                                    EnvelopeScale = FMath::Pow((DecaySamples > 0) ? (float)(Voice.TotalGrainSamples - CurrentFrameInGrain) / DecaySamples : 0.0f, DecayCurveFactor);
                                }
                                MonoBufferPtr[FrameIndex] = MonoSample * FMath::Clamp(EnvelopeScale, 0.0f, 1.0f);
                            }
                        }
                    }

                    if (FramesSuccessfullyResampled < OutputFramesToProcessThisBlock)
                    {
                        for (int32 FrameIndex = FramesSuccessfullyResampled; FrameIndex < OutputFramesToProcessThisBlock; ++FrameIndex)
                        {
                            MonoBufferPtr[FrameIndex] = 0.0f;
                        }
                    }

                    if (OutputFramesToProcessThisBlock > 0)
                    {
                        const float PanAngle = (Voice.PanPosition + 1.0f) * 0.5f * UE_PI * 0.5f;
                        Audio::ArrayMixIn(TArrayView<const float>(MonoBufferPtr, OutputFramesToProcessThisBlock), TArrayView<float>(OutputAudioLeftPtr, OutputFramesToProcessThisBlock), FMath::Cos(PanAngle));
                        Audio::ArrayMixIn(TArrayView<const float>(MonoBufferPtr, OutputFramesToProcessThisBlock), TArrayView<float>(OutputAudioRightPtr, OutputFramesToProcessThisBlock), FMath::Sin(PanAngle));
                    }

                    Voice.SamplesPlayed += OutputFramesToProcessThisBlock;
                    Voice.SamplesRemaining -= OutputFramesToProcessThisBlock;
                    if (Voice.SamplesRemaining <= 0) { Voice.bIsActive = false; Voice.Reader.Reset(); Voice.Resampler.Reset(); }
                }
            }
        }

        void Reset(const IOperator::FResetParams& InParams)
        {
            ResetVoices();
            AudioOutputLeft->Zero();
            AudioOutputRight->Zero();

            SamplesUntilNextGrain = 0.0f;
            CurrentWaveProxy.Reset();
            CachedSoundWaveDuration = 0.0f;
            CurrentNumChannels = 0;
            ConvertDeinterleave.Reset();

            OnPlayTrigger->Reset();
            OnFinishedTrigger->Reset();
            OnGrainTriggered->Reset();

            *OutputGrainStartTimeRef = FTime(0.0);
            *OutputGrainDurationSecRef = 0.0f;
            *OutputGrainIsReversedRef = false;
            *OutputGrainVolumeRef = 0.0f;
            *OutputGrainPitchRef = 0.0f;
            *OutputGrainPanRef = 0.0f;

            bIsPlaying = false;
            UE_LOG(LogMetaSound, Log, TEXT("Granular Synth (V23_WarmStart): Operator Reset."));
        }

    private:
        bool TryStartPlayback(int32 InFrame)
        {
            bool bPreviouslyPlaying = bIsPlaying;
            bIsPlaying = false;

            if (!WaveAssetInput->IsSoundWaveValid())
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS: Play Trigger: Wave Asset input is not valid."));
                ResetVoices(); CurrentWaveProxy.Reset(); CachedSoundWaveDuration = 0.0f; CurrentNumChannels = 0; ConvertDeinterleave.Reset();
                return false;
            }

            const FSoundWaveProxyPtr SoundWaveProxy = WaveAssetInput->GetSoundWaveProxy();
            if (!SoundWaveProxy.IsValid())
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS: Play Trigger: Could not get valid SoundWaveProxy."));
                ResetVoices(); CurrentWaveProxy.Reset(); CachedSoundWaveDuration = 0.0f; CurrentNumChannels = 0; ConvertDeinterleave.Reset();
                return false;
            }

            if (!InitializeWaveData(SoundWaveProxy))
            {
                UE_LOG(LogMetaSound, Error, TEXT("GS: Play Trigger: Failed to initialize wave data."));
                ResetVoices(); CurrentWaveProxy.Reset(); CachedSoundWaveDuration = 0.0f; CurrentNumChannels = 0; ConvertDeinterleave.Reset();
                return false;
            }

            bIsPlaying = true;
            ResetVoices();
            // SamplesUntilNextGrain will be set based on WarmStartInput below
            OnPlayTrigger->TriggerFrame(InFrame);
            UE_LOG(LogMetaSound, Log, TEXT("GS: Playback %s at frame %d."), bPreviouslyPlaying ? TEXT("Restarted") : TEXT("Started"), InFrame);

            if (*WarmStartInput && CurrentWaveProxy.IsValid() && CachedSoundWaveDuration >= MinGrainDurationSeconds && SampleRate > 0)
            {
                // Read parameters needed for grain calculation (similar to Execute)
                const float BaseGrainDurationSeconds = FMath::Max(MinGrainDurationSeconds, *GrainDurationMsInput / 1000.0f);
                const float MaxDurationRandSeconds = FMath::Max(0.0f, *DurationRandMsInput / 1000.0f);
                const float UserBaseStartPointSeconds = StartPointTimeInput->GetSeconds();
                const float MaxStartPointRandMsValue = FMath::Max(0.0f, *StartPointRandMsInput);
                const float ReverseChanceVal = FMath::Clamp(*ReverseChanceInput, 0.0f, 100.0f);
                const float BasePitchShiftSemitones = FMath::Clamp(*PitchShiftInput, -MaxAbsPitchShiftSemitones, MaxAbsPitchShiftSemitones);
                const float PitchRandSemitones = FMath::Max(0.0f, *PitchRandInput);
                const float BasePan = FMath::Clamp(*PanInput, -1.0f, 1.0f);
                const float PanRandAmount = FMath::Clamp(*PanRandInput, 0.0f, 1.0f);
                const float VolumeRandPercent = FMath::Clamp(*VolumeRandInput, 0.0f, 100.0f);

                int32 NumVoicesToWarmStart = FMath::FloorToInt(*ActiveVoicesInput);
                if (*ActiveVoicesInput < 1.0f && *ActiveVoicesInput > 0.0f) // If fractional but > 0, warm start at least 1
                {
                    NumVoicesToWarmStart = 1;
                }
                NumVoicesToWarmStart = FMath::Clamp(NumVoicesToWarmStart, 0, MaxGrainVoices);


                for (int32 WarmUpIndex = 0; WarmUpIndex < NumVoicesToWarmStart; ++WarmUpIndex)
                {
                    float RandomStartOffsetSeconds = FMath::FRandRange(0.0f, MaxStartPointRandMsValue / 1000.0f);
                    float ConceptualStartPointSecs = UserBaseStartPointSeconds + RandomStartOffsetSeconds;

                    float DurationOffset = FMath::FRandRange(0.0f, MaxDurationRandSeconds);
                    float FinalOutputGrainDurationSeconds = FMath::Max(MinGrainDurationSeconds, BaseGrainDurationSeconds + DurationOffset);
                    int32 OutputGrainDurationSamples = FMath::Max(1, FMath::CeilToInt(FinalOutputGrainDurationSeconds * SampleRate));

                    float PitchOffset = FMath::FRandRange(-PitchRandSemitones, PitchRandSemitones);
                    float FinalTargetPitchShift = FMath::Clamp(BasePitchShiftSemitones + PitchOffset, -MaxAbsPitchShiftSemitones, MaxAbsPitchShiftSemitones);
                    float FrameRatio = FMath::Pow(2.0f, FinalTargetPitchShift / 12.0f);
                    FrameRatio = FMath::Max(UE_SMALL_NUMBER, FrameRatio);

                    bool bFinalPlayThisGrainReverse = FMath::FRandRange(0.0f, 100.0f) < ReverseChanceVal;

                    float FinalReaderStartTimeForSegment = 0.0f;
                    int32 NumSourceFramesToReadForSegment = 0;
                    bool bIsValidSegment = true;

                    if (bFinalPlayThisGrainReverse)
                    {
                        float SourceMaterialNeededSeconds = FinalOutputGrainDurationSeconds * FrameRatio;
                        if (SourceMaterialNeededSeconds < Epsilon) { bIsValidSegment = false; }
                        else
                        {
                            float ConceptualSegmentEndInSource = FMath::Fmod(ConceptualStartPointSecs, CachedSoundWaveDuration);
                            if (ConceptualSegmentEndInSource < 0.0f) ConceptualSegmentEndInSource += CachedSoundWaveDuration;
                            float ConceptualSegmentStartInSource = ConceptualSegmentEndInSource - SourceMaterialNeededSeconds;
                            float ActualSegmentStart = FMath::Max(0.0f, ConceptualSegmentStartInSource);
                            float ActualSegmentEnd = FMath::Min(CachedSoundWaveDuration, ConceptualSegmentEndInSource);
                            if (ActualSegmentStart == 0.0f && SourceMaterialNeededSeconds > 0.0f) { ActualSegmentEnd = FMath::Min(CachedSoundWaveDuration, SourceMaterialNeededSeconds); }
                            if (ActualSegmentStart >= ActualSegmentEnd - Epsilon) { NumSourceFramesToReadForSegment = 0; bIsValidSegment = false; }
                            else { NumSourceFramesToReadForSegment = FMath::CeilToInt((ActualSegmentEnd - ActualSegmentStart) * SampleRate); if (NumSourceFramesToReadForSegment <= 0) { bIsValidSegment = false; } }
                            FinalReaderStartTimeForSegment = ActualSegmentStart;
                        }
                    }
                    else
                    {
                        FinalReaderStartTimeForSegment = FMath::Fmod(ConceptualStartPointSecs, CachedSoundWaveDuration);
                        if (FinalReaderStartTimeForSegment < 0.0f) FinalReaderStartTimeForSegment += CachedSoundWaveDuration;
                        FinalReaderStartTimeForSegment = FMath::Min(FinalReaderStartTimeForSegment, CachedSoundWaveDuration - MinGrainDurationSeconds);
                        FinalReaderStartTimeForSegment = FMath::Max(0.0f, FinalReaderStartTimeForSegment);
                    }

                    if (!bIsValidSegment) continue;

                    float PanOffset = FMath::FRandRange(-PanRandAmount, PanRandAmount);
                    float FinalGrainPanPosition = FMath::Clamp(BasePan + PanOffset, -1.0f, 1.0f);
                    float MinVolumeScale = 1.0f - (VolumeRandPercent / 100.0f);
                    float FinalGrainVolumeScale = FMath::FRandRange(MinVolumeScale, 1.0f);

                    if (TriggerGrain(CurrentWaveProxy, OutputGrainDurationSamples, FinalReaderStartTimeForSegment, FrameRatio, FinalGrainPanPosition, FinalGrainVolumeScale, bFinalPlayThisGrainReverse, NumSourceFramesToReadForSegment))
                    {
                        *OutputGrainStartTimeRef = FTime(FinalReaderStartTimeForSegment);
                        *OutputGrainDurationSecRef = FinalOutputGrainDurationSeconds;
                        *OutputGrainIsReversedRef = bFinalPlayThisGrainReverse;
                        *OutputGrainVolumeRef = FinalGrainVolumeScale;
                        *OutputGrainPitchRef = FinalTargetPitchShift;
                        *OutputGrainPanRef = FinalGrainPanPosition;
                        OnGrainTriggered->TriggerFrame(InFrame);
                    }
                }

                // After warm start, schedule the next grain based on the interval.
                const float EffectiveActiveVoicesForInterval = FMath::Max(MinActiveVoicesParam, *ActiveVoicesInput);
                const float CalculatedBaseSamplesPerGrainInterval = (EffectiveActiveVoicesForInterval > 0.0f && BaseGrainDurationSeconds > 0.0f && SampleRate > 0.0f)
                    ? (BaseGrainDurationSeconds / EffectiveActiveVoicesForInterval) * SampleRate : TNumericLimits<float>::Max();

                if (CalculatedBaseSamplesPerGrainInterval < TNumericLimits<float>::Max())
                {
                    SamplesUntilNextGrain = CalculatedBaseSamplesPerGrainInterval;
                }
                else
                {
                    SamplesUntilNextGrain = TNumericLimits<float>::Max();
                }
            }
            else
            {
                SamplesUntilNextGrain = 0.0f; // Standard behavior: trigger first grain ASAP via Execute()
            }
            return true;
        }

        bool InitializeWaveData(const FSoundWaveProxyPtr& InSoundWaveProxy)
        {
            CurrentWaveProxy = InSoundWaveProxy;
            FSoundWaveProxyReader::FSettings TempReaderSettings;
            auto TempReader = FSoundWaveProxyReader::Create(CurrentWaveProxy.ToSharedRef(), TempReaderSettings);

            if (TempReader.IsValid())
            {
                CachedSoundWaveDuration = (float)TempReader->GetNumFramesInWave() / FMath::Max(1.0f, (float)TempReader->GetSampleRate());
                CurrentNumChannels = TempReader->GetNumChannels();

                if (CurrentNumChannels <= 0 || CachedSoundWaveDuration <= 0.0f)
                {
                    UE_LOG(LogMetaSound, Error, TEXT("GS: Wave Asset '%s' reports invalid duration (%.2fs) or channels (%d)."), *CurrentWaveProxy->GetFName().ToString(), CachedSoundWaveDuration, CurrentNumChannels); CurrentNumChannels = 0; CachedSoundWaveDuration = 0.0f; ConvertDeinterleave.Reset(); CurrentWaveProxy.Reset(); return false;
                }

                Audio::FConvertDeinterleaveParams ConvertParams;
                ConvertParams.NumInputChannels = CurrentNumChannels;
                ConvertParams.NumOutputChannels = CurrentNumChannels;
                ConvertDeinterleave = Audio::IConvertDeinterleave::Create(ConvertParams);

                if (!ConvertDeinterleave.IsValid())
                {
                    UE_LOG(LogMetaSound, Error, TEXT("GS: Failed to create deinterleaver for %d channels."), CurrentNumChannels); CurrentNumChannels = 0; CachedSoundWaveDuration = 0.0f; ConvertDeinterleave.Reset(); CurrentWaveProxy.Reset(); return false;
                }

                Audio::SetMultichannelBufferSize(CurrentNumChannels, DeinterleaveBlockSizeFrames, DeinterleavedSourceBuffer);

                UE_LOG(LogMetaSound, Verbose, TEXT("GS: Initialized wave data: %s, Duration: %.2fs, Channels: %d"), *CurrentWaveProxy->GetFName().ToString(), CachedSoundWaveDuration, CurrentNumChannels);
                return true;
            }
            else
            {
                UE_LOG(LogMetaSound, Error, TEXT("GS: Failed to create temporary reader for wave asset '%s'."), *CurrentWaveProxy->GetFName().ToString()); CurrentNumChannels = 0; CachedSoundWaveDuration = 0.0f; ConvertDeinterleave.Reset(); CurrentWaveProxy.Reset(); return false;
            }
        }

        bool TriggerGrain(const FSoundWaveProxyPtr& InSoundWaveProxy,
            int32 InOutputGrainDurationSamples,
            float InReaderStartTimeForSegment,
            float InFrameRatio,
            float InPanPosition,
            float InVolumeScale,
            bool bInIsReversed,
            int32 InNumSourceFramesToReadForReverseSegment)
        {
            if (!InSoundWaveProxy.IsValid() || CurrentNumChannels <= 0 || CachedSoundWaveDuration < MinGrainDurationSeconds)
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS: TriggerGrain failed pre-check (Proxy, Channels, or CachedDuration).")); return false;
            }

            if (InOutputGrainDurationSamples <= 0)
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS: TriggerGrain failed due to zero or negative InOutputGrainDurationSamples: %d"), InOutputGrainDurationSamples); return false;
            }

            if (bInIsReversed && InNumSourceFramesToReadForReverseSegment <= 0)
            {
                UE_LOG(LogMetaSound, Verbose, TEXT("GS: TriggerGrain skipped reversed grain with zero/negative source frames to read (%d)."), InNumSourceFramesToReadForReverseSegment); return false;
            }

            int32 VoiceIndex = -1;
            for (int32 i = 0; i < GrainVoices.Num(); ++i) { if (!GrainVoices[i].bIsActive) { VoiceIndex = i; break; } }
            if (VoiceIndex == -1) { UE_LOG(LogMetaSound, Verbose, TEXT("GS: No available grain voices.")); return false; }

            FGrainVoice& NewVoice = GrainVoices[VoiceIndex];
            NewVoice.NumChannels = CurrentNumChannels;
            NewVoice.bIsReversed = bInIsReversed;
            NewVoice.FullGrainSegmentReadOffset = 0;
            NewVoice.FullGrainSegmentBuffer.Empty();

            FSoundWaveProxyReader::FSettings ReaderSettings;
            ReaderSettings.StartTimeInSeconds = FMath::Max(0.0f, InReaderStartTimeForSegment);
            ReaderSettings.bIsLooping = !bInIsReversed;

            const uint32 DecodeSizeQuantization = FSoundWaveProxyReader::DecodeSizeQuantizationInFrames;
            const uint32 MinDecodeSize = FSoundWaveProxyReader::DefaultMinDecodeSizeInFrames;
            uint32 DesiredDecodeSize = FMath::Max(static_cast<uint32>(DeinterleaveBlockSizeFrames), MinDecodeSize);
            ReaderSettings.MaxDecodeSizeInFrames = ((DesiredDecodeSize + DecodeSizeQuantization - 1) / DecodeSizeQuantization) * DecodeSizeQuantization;

            NewVoice.Reader = FSoundWaveProxyReader::Create(InSoundWaveProxy.ToSharedRef(), ReaderSettings);
            if (!NewVoice.Reader.IsValid()) { UE_LOG(LogMetaSound, Error, TEXT("GS: Failed to create Reader for voice %d."), VoiceIndex); return false; }

            NewVoice.Resampler = MakeUnique<Audio::FMultichannelLinearResampler>(NewVoice.NumChannels);
            NewVoice.Resampler->SetFrameRatio(InFrameRatio, 0);

            NewVoice.SourceCircularBuffer.Empty(NewVoice.NumChannels);
            for (int32 i = 0; i < NewVoice.NumChannels; ++i)
            {
                int32 MaxInputFramesNeededByResampler = BlockSize * FMath::CeilToInt(Audio::FMultichannelLinearResampler::MaxFrameRatio); // Max possible ratio
                NewVoice.SourceCircularBuffer.Emplace(DeinterleaveBlockSizeFrames + MaxInputFramesNeededByResampler);
            }

            int32 ActualOutputGrainSamplesForVoice = InOutputGrainDurationSamples;

            if (bInIsReversed)
            {
                Audio::FAlignedFloatBuffer InterleavedSegmentForReverse;
                InterleavedSegmentForReverse.SetNumUninitialized(InNumSourceFramesToReadForReverseSegment * NewVoice.NumChannels);
                int32 SamplesActuallyRead = NewVoice.Reader->PopAudio(InterleavedSegmentForReverse);
                int32 FramesActuallyRead = SamplesActuallyRead / NewVoice.NumChannels;

                if (FramesActuallyRead > 0)
                {
                    Audio::SetMultichannelBufferSize(NewVoice.NumChannels, FramesActuallyRead, NewVoice.FullGrainSegmentBuffer);
                    ConvertDeinterleave->ProcessAudio(TArrayView<const float>(InterleavedSegmentForReverse.GetData(), SamplesActuallyRead), NewVoice.FullGrainSegmentBuffer);
                    for (int32 Chan = 0; Chan < NewVoice.NumChannels; ++Chan)
                    {
                        Algo::Reverse(NewVoice.FullGrainSegmentBuffer[Chan]);
                    }
                    // If InFrameRatio is pitch (e.g., 2.0 = octave up = plays twice as fast),
                    // then FramesActuallyRead (source) will produce FramesActuallyRead / InFrameRatio output samples.
                    int32 MaxPossibleOutputSamplesFromReadSegment = FMath::Max(1, FMath::CeilToInt(static_cast<float>(FramesActuallyRead) / InFrameRatio));
                    ActualOutputGrainSamplesForVoice = FMath::Min(InOutputGrainDurationSamples, MaxPossibleOutputSamplesFromReadSegment);
                    ActualOutputGrainSamplesForVoice = FMath::Max(1, ActualOutputGrainSamplesForVoice);
                }
                else
                {
                    UE_LOG(LogMetaSound, Verbose, TEXT("GS: Reversed grain %d read 0 frames for segment despite requesting %d. Will not activate."), VoiceIndex, InNumSourceFramesToReadForReverseSegment);
                    NewVoice.Reader.Reset();
                    return false;
                }
                NewVoice.Reader.Reset();
            }
            else
            {
                NewVoice.InterleavedReadBuffer.SetNumUninitialized(DeinterleaveBlockSizeFrames * NewVoice.NumChannels, EAllowShrinking::No);
            }

            NewVoice.EnvelopedMonoBuffer.SetNumUninitialized(BlockSize, EAllowShrinking::No);
            NewVoice.bIsActive = true;
            NewVoice.SamplesRemaining = ActualOutputGrainSamplesForVoice;
            NewVoice.SamplesPlayed = 0;
            NewVoice.TotalGrainSamples = ActualOutputGrainSamplesForVoice;
            NewVoice.PanPosition = InPanPosition;
            NewVoice.VolumeScale = InVolumeScale;

            UE_LOG(LogMetaSound, Verbose, TEXT("GS: Triggered Grain %d: StartReadTime=%.3fs, OutputSamples=%d (Actual: %d), PitchRatio=%.2f, Reversed=%d, SourceFramesToRead=%d, VoiceChans=%d"),
                VoiceIndex, ReaderSettings.StartTimeInSeconds, InOutputGrainDurationSamples, ActualOutputGrainSamplesForVoice, InFrameRatio, bInIsReversed, InNumSourceFramesToReadForReverseSegment, NewVoice.NumChannels);
            return true;
        }

        void GenerateSourceAudio(FGrainVoice& ForVoice)
        {
            if (!bIsPlaying || !ConvertDeinterleave.IsValid()) return;

            if (ForVoice.bIsReversed)
            {
                if (Audio::GetMultichannelBufferNumFrames(ForVoice.FullGrainSegmentBuffer) > 0 &&
                    ForVoice.FullGrainSegmentReadOffset < Audio::GetMultichannelBufferNumFrames(ForVoice.FullGrainSegmentBuffer))
                {
                    const int32 FramesRemainingInSegment = Audio::GetMultichannelBufferNumFrames(ForVoice.FullGrainSegmentBuffer) - ForVoice.FullGrainSegmentReadOffset;
                    const int32 FramesToCopyToCircular = FMath::Min(DeinterleaveBlockSizeFrames, FramesRemainingInSegment);

                    if (FramesToCopyToCircular > 0)
                    {
                        for (int32 ChannelIndex = 0; ChannelIndex < ForVoice.NumChannels; ++ChannelIndex)
                        {
                            if (ChannelIndex < ForVoice.FullGrainSegmentBuffer.Num() && ForVoice.FullGrainSegmentBuffer[ChannelIndex].Num() >= ForVoice.FullGrainSegmentReadOffset + FramesToCopyToCircular)
                            {
                                TArrayView<const float> SegmentChannelView = ForVoice.FullGrainSegmentBuffer[ChannelIndex];
                                TArrayView<const float> ChunkToPush = SegmentChannelView.Slice(ForVoice.FullGrainSegmentReadOffset, FramesToCopyToCircular);
                                ForVoice.SourceCircularBuffer[ChannelIndex].Push(ChunkToPush.GetData(), FramesToCopyToCircular);
                            }
                        }
                        ForVoice.FullGrainSegmentReadOffset += FramesToCopyToCircular;
                    }
                }
            }
            else
            {
                if (ForVoice.Reader.IsValid() && !ForVoice.Reader->HasFailed())
                {
                    ForVoice.InterleavedReadBuffer.SetNumUninitialized(DeinterleaveBlockSizeFrames * ForVoice.NumChannels, EAllowShrinking::No);
                    int32 SamplesPopped = ForVoice.Reader->PopAudio(ForVoice.InterleavedReadBuffer);

                    if (SamplesPopped > 0)
                    {
                        if (DeinterleavedSourceBuffer.Num() != ForVoice.NumChannels || Audio::GetMultichannelBufferNumFrames(DeinterleavedSourceBuffer) != DeinterleaveBlockSizeFrames)
                        {
                            Audio::SetMultichannelBufferSize(ForVoice.NumChannels, DeinterleaveBlockSizeFrames, DeinterleavedSourceBuffer);
                        }
                        ConvertDeinterleave->ProcessAudio(TArrayView<const float>(ForVoice.InterleavedReadBuffer.GetData(), SamplesPopped), DeinterleavedSourceBuffer);

                        int32 FramesPopped = SamplesPopped / ForVoice.NumChannels;
                        for (int32 ChannelIndex = 0; ChannelIndex < ForVoice.NumChannels; ++ChannelIndex)
                        {
                            ForVoice.SourceCircularBuffer[ChannelIndex].Push(DeinterleavedSourceBuffer[ChannelIndex].GetData(), FramesPopped);
                        }
                    }
                }
            }
        }

        void ResetVoices()
        {
            for (FGrainVoice& Voice : GrainVoices)
            {
                Voice.bIsActive = false; Voice.NumChannels = 0; Voice.SamplesRemaining = 0; Voice.SamplesPlayed = 0;
                Voice.TotalGrainSamples = 0; Voice.PanPosition = 0.0f; Voice.VolumeScale = 1.0f;
                Voice.bIsReversed = false;
                Voice.FullGrainSegmentBuffer.Empty(); Voice.FullGrainSegmentReadOffset = 0;
                Voice.Reader.Reset(); Voice.Resampler.Reset(); Voice.SourceCircularBuffer.Empty();
            }
        }

        // Input ReadRefs
        FTriggerReadRef PlayTrigger; FTriggerReadRef StopTrigger; FWaveAssetReadRef WaveAssetInput;
        FFloatReadRef GrainDurationMsInput; FFloatReadRef DurationRandMsInput; FFloatReadRef ActiveVoicesInput; FFloatReadRef TimeJitterInput;
        FTimeReadRef StartPointTimeInput; FFloatReadRef StartPointRandMsInput; FFloatReadRef ReverseChanceInput;
        FFloatReadRef AttackTimePercentInput; FFloatReadRef DecayTimePercentInput; FFloatReadRef AttackCurveInput; FFloatReadRef DecayCurveInput;
        FFloatReadRef PitchShiftInput; FFloatReadRef PitchRandInput; FFloatReadRef PanInput; FFloatReadRef PanRandInput; FFloatReadRef VolumeRandInput;
        FBoolReadRef WarmStartInput; 

        // Output WriteRefs
        FTriggerWriteRef OnPlayTrigger; FTriggerWriteRef OnFinishedTrigger; FTriggerWriteRef OnGrainTriggered;
        FAudioBufferWriteRef AudioOutputLeft; FAudioBufferWriteRef AudioOutputRight;
        FTimeWriteRef OutputGrainStartTimeRef;
        FFloatWriteRef OutputGrainDurationSecRef;
        FBoolWriteRef OutputGrainIsReversedRef;
        FFloatWriteRef OutputGrainVolumeRef;
        FFloatWriteRef OutputGrainPitchRef;
        FFloatWriteRef OutputGrainPanRef;

        // Operator State
        float SampleRate; int32 BlockSize;
        bool bIsPlaying; float SamplesUntilNextGrain;
        TArray<FGrainVoice> GrainVoices;
        FSoundWaveProxyPtr CurrentWaveProxy;
        float CachedSoundWaveDuration;
        int32 CurrentNumChannels;
        TUniquePtr<Audio::IConvertDeinterleave> ConvertDeinterleave;
        Audio::FMultichannelBuffer DeinterleavedSourceBuffer;
    };

    // --- Node Facade ---
    class FGranularSynthNode : public FNodeFacade 
    {
    public:
        FGranularSynthNode(const FNodeInitData& InitData) 
            : FNodeFacade(InitData.InstanceName, InitData.InstanceID, TFacadeOperatorClass<FGranularSynthOperator>()) 
        {
        }
    };

    METASOUND_REGISTER_NODE(FGranularSynthNode) 
}
#undef LOCTEXT_NAMESPACE
