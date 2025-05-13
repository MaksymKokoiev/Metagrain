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

#include "Internationalization/Text.h" // Required for LOCTEXT, FText
#include "UObject/NameTypes.h"         // Required for FName
#include "Math/UnrealMathUtility.h"    // Required for FMath, FMath::Cos, FMath::Sin, PI
#include <limits>                     // Required for TNumericLimits

// Define a unique namespace for localization text
#define LOCTEXT_NAMESPACE "MetasoundStandardNodes_GranularSynthNode_Step8" // Updated namespace

namespace Metasound
{
    // --- Parameter Names ---
    namespace GranularSynthNode_Step8_VertexNames
    {
        METASOUND_PARAM(InputTriggerPlay, "Play", "Start generating grains.");
        METASOUND_PARAM(InputTriggerStop, "Stop", "Stop generating grains.");
        METASOUND_PARAM(InParamWaveAsset, "Wave Asset", "The audio wave to granulate.");
        METASOUND_PARAM(InParamGrainDuration, "Grain Duration (ms)", "The base duration of each grain in milliseconds.");
        METASOUND_PARAM(InParamGrainsPerSecond, "Grains Per Second", "How many grains to trigger per second.");
        METASOUND_PARAM(InParamStartPoint, "Start Point", "The base time to start reading grains from.");
        METASOUND_PARAM(InParamEndPoint, "End Point", "The time after which grains should not start. <= 0 means end of file.");
        METASOUND_PARAM(InParamStartPointRand, "Start Point Rand (ms)", "Maximum POSITIVE random offset applied to the start point in milliseconds.");
        METASOUND_PARAM(InParamDurationRand, "Duration Rand (ms)", "Maximum POSITIVE random variation applied to the grain duration in milliseconds.");
        METASOUND_PARAM(InParamAttackTimePercent, "Attack (%)", "Attack time as a percentage of grain duration (0.0 - 1.0).");
        METASOUND_PARAM(InParamDecayTimePercent, "Decay (%)", "Decay time as a percentage of grain duration (0.0 - 1.0).");
        METASOUND_PARAM(InParamAttackCurve, "Attack Curve", "Attack curve factor (1.0=linear, <1.0 logarithmic, >1.0 exponential).");
        METASOUND_PARAM(InParamDecayCurve, "Decay Curve", "Decay curve factor (1.0=linear, >1.0 logarithmic, <1.0 exponential).");
        METASOUND_PARAM(InParamPitchShift, "Pitch Shift (Semi)", "Base pitch shift in semitones.");
        METASOUND_PARAM(InParamPitchRand, "Pitch Rand (Semi)", "Maximum random pitch variation (+/-) in semitones.");
        METASOUND_PARAM(InParamPan, "Pan", "Stereo pan position (-1.0 Left to 1.0 Right).");
        METASOUND_PARAM(InParamPanRand, "Pan Rand", "Maximum random pan variation (+/-) (0.0 to 1.0).");
        METASOUND_PARAM(OutputTriggerOnPlay, "On Play", "Triggers when Play is triggered.");
        METASOUND_PARAM(OutputTriggerOnFinished, "On Finished", "Triggers when Stop is triggered or generation otherwise finishes.");
        METASOUND_PARAM(OutputTriggerOnGrain, "On Grain", "Triggers when a new grain is successfully started.");
        METASOUND_PARAM(OutParamAudioLeft, "Out Left", "The left channel audio output.");
        METASOUND_PARAM(OutParamAudioRight, "Out Right", "The right channel audio output.");
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
        Audio::FAlignedFloatBuffer InterleavedReadBuffer;
        Audio::FAlignedFloatBuffer EnvelopedMonoBuffer;
    };

    // --- Operator ---
    class FGranularSynthOperator_Step8 : public TExecutableOperator<FGranularSynthOperator_Step8>
    {
        static constexpr int32 MaxGrainVoices = 32;
        static constexpr float MinGrainDurationSeconds = 0.005f;
        static constexpr float MaxAbsPitchShiftSemitones = 60.0f;
        static constexpr int32 DeinterleaveBlockSizeFrames = 256;

    public:
        // Constructor
        FGranularSynthOperator_Step8(const FOperatorSettings& InSettings,
            const FTriggerReadRef& InPlayTrigger,
            const FTriggerReadRef& InStopTrigger,
            const FWaveAssetReadRef& InWaveAsset,
            const FFloatReadRef& InGrainDurationMs,
            const FFloatReadRef& InGrainsPerSecond,
            const FTimeReadRef& InStartPointTime,
            const FTimeReadRef& InEndPointTime,
            const FFloatReadRef& InStartPointRandMs,
            const FFloatReadRef& InDurationRandMs,
            const FFloatReadRef& InAttackTimePercent,
            const FFloatReadRef& InDecayTimePercent,
            const FFloatReadRef& InAttackCurve,
            const FFloatReadRef& InDecayCurve,
            const FFloatReadRef& InPitchShift,
            const FFloatReadRef& InPitchRand,
            const FFloatReadRef& InPan,
            const FFloatReadRef& InPanRand)
            : PlayTrigger(InPlayTrigger)
            , StopTrigger(InStopTrigger)
            , WaveAssetInput(InWaveAsset)
            , GrainDurationMsInput(InGrainDurationMs)
            , GrainsPerSecondInput(InGrainsPerSecond)
            , StartPointTimeInput(InStartPointTime)
            , EndPointTimeInput(InEndPointTime)
            , StartPointRandMsInput(InStartPointRandMs)
            , DurationRandMsInput(InDurationRandMs)
            , AttackTimePercentInput(InAttackTimePercent)
            , DecayTimePercentInput(InDecayTimePercent)
            , AttackCurveInput(InAttackCurve)
            , DecayCurveInput(InDecayCurve)
            , PitchShiftInput(InPitchShift)
            , PitchRandInput(InPitchRand)
            , PanInput(InPan)
            , PanRandInput(InPanRand)
            , OnPlayTrigger(FTriggerWriteRef::CreateNew(InSettings))
            , OnFinishedTrigger(FTriggerWriteRef::CreateNew(InSettings))
            , OnGrainTriggered(FTriggerWriteRef::CreateNew(InSettings))
            , AudioOutputLeft(FAudioBufferWriteRef::CreateNew(InSettings))
            , AudioOutputRight(FAudioBufferWriteRef::CreateNew(InSettings))
            , SampleRate(InSettings.GetSampleRate())
            , BlockSize(InSettings.GetNumFramesPerBlock())
            , bIsPlaying(false)
        {
            GrainVoices.SetNum(MaxGrainVoices);
            for (FGrainVoice& Voice : GrainVoices)
            {
                Voice.InterleavedReadBuffer.SetNumUninitialized(DeinterleaveBlockSizeFrames * 2);
                Voice.EnvelopedMonoBuffer.SetNumUninitialized(BlockSize);
                Audio::SetMultichannelCircularBufferCapacity(2, DeinterleaveBlockSizeFrames + BlockSize * 4, Voice.SourceCircularBuffer);
            }
            SamplesUntilNextGrain = 0.0f;
            CachedSoundWaveDuration = 0.0f;
            DeinterleavedSourceBuffer.SetNum(2);
            Audio::SetMultichannelBufferSize(2, DeinterleaveBlockSizeFrames, DeinterleavedSourceBuffer);
        }

        // --- Metasound Node Interface ---
        static const FVertexInterface& DeclareVertexInterface()
        {
            using namespace GranularSynthNode_Step8_VertexNames;
            static const FVertexInterface Interface(
                FInputVertexInterface(
                    TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTriggerPlay)),
                    TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTriggerStop)),
                    TInputDataVertex<FWaveAsset>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamWaveAsset)), // FWaveAsset input
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamGrainDuration), 100.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamGrainsPerSecond), 10.0f),
                    TInputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamStartPoint)),
                    TInputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamEndPoint)),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamStartPointRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDurationRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamAttackTimePercent), 0.1f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDecayTimePercent), 0.1f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamAttackCurve), 1.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDecayCurve), 1.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPitchShift), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPitchRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPan), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPanRand), 0.0f)
                ),
                FOutputVertexInterface(
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnPlay)),
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnFinished)),
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnGrain)),
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
                    Metadata.ClassName = { FName("GranularSynth"), FName("Step8"), FName("") };
                    Metadata.MajorVersion = 1; Metadata.MinorVersion = 0;
                    Metadata.DisplayName = LOCTEXT("GranularSynth_Step8_DisplayName", "Granular Synth (Triggers)");
                    Metadata.Description = LOCTEXT("GranularSynth_Step8_Description", "Granular synthesizer with Play/Stop triggers.");
                    Metadata.Author = TEXT("Charles Matthews");
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
            using namespace GranularSynthNode_Step8_VertexNames;
            const FInputVertexInterfaceData& InputData = InParams.InputData;
            FTriggerReadRef PlayTriggerIn = InputData.GetOrConstructDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(InputTriggerPlay), InParams.OperatorSettings);
            FTriggerReadRef StopTriggerIn = InputData.GetOrConstructDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(InputTriggerStop), InParams.OperatorSettings);
            // Use GetOrCreateDefaultDataReadReference for FWaveAsset
            FWaveAssetReadRef WaveAssetIn = InputData.GetOrCreateDefaultDataReadReference<FWaveAsset>(METASOUND_GET_PARAM_NAME(InParamWaveAsset), InParams.OperatorSettings);
            FFloatReadRef GrainDurationIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamGrainDuration), InParams.OperatorSettings);
            FFloatReadRef GrainsPerSecondIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamGrainsPerSecond), InParams.OperatorSettings);
            FTimeReadRef StartPointIn = InputData.GetOrCreateDefaultDataReadReference<FTime>(METASOUND_GET_PARAM_NAME(InParamStartPoint), InParams.OperatorSettings);
            FTimeReadRef EndPointIn = InputData.GetOrCreateDefaultDataReadReference<FTime>(METASOUND_GET_PARAM_NAME(InParamEndPoint), InParams.OperatorSettings);
            FFloatReadRef StartPointRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamStartPointRand), InParams.OperatorSettings);
            FFloatReadRef DurationRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamDurationRand), InParams.OperatorSettings);
            FFloatReadRef AttackTimePercentIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamAttackTimePercent), InParams.OperatorSettings);
            FFloatReadRef DecayTimePercentIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamDecayTimePercent), InParams.OperatorSettings);
            FFloatReadRef AttackCurveIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamAttackCurve), InParams.OperatorSettings);
            FFloatReadRef DecayCurveIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamDecayCurve), InParams.OperatorSettings);
            FFloatReadRef PitchShiftIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPitchShift), InParams.OperatorSettings);
            FFloatReadRef PitchRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPitchRand), InParams.OperatorSettings);
            FFloatReadRef PanIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPan), InParams.OperatorSettings);
            FFloatReadRef PanRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPanRand), InParams.OperatorSettings);
            return MakeUnique<FGranularSynthOperator_Step8>(InParams.OperatorSettings, PlayTriggerIn, StopTriggerIn, WaveAssetIn, GrainDurationIn, GrainsPerSecondIn, StartPointIn, EndPointIn, StartPointRandIn, DurationRandIn, AttackTimePercentIn, DecayTimePercentIn, AttackCurveIn, DecayCurveIn, PitchShiftIn, PitchRandIn, PanIn, PanRandIn);
        }

        // --- IOperator Interface ---
        virtual void BindInputs(FInputVertexInterfaceData& InOutVertexData) override
        {
            using namespace GranularSynthNode_Step8_VertexNames;
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputTriggerPlay), PlayTrigger);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputTriggerStop), StopTrigger);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamWaveAsset), WaveAssetInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamGrainDuration), GrainDurationMsInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamGrainsPerSecond), GrainsPerSecondInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamStartPoint), StartPointTimeInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamEndPoint), EndPointTimeInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamStartPointRand), StartPointRandMsInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamDurationRand), DurationRandMsInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamAttackTimePercent), AttackTimePercentInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamDecayTimePercent), DecayTimePercentInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamAttackCurve), AttackCurveInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamDecayCurve), DecayCurveInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPitchShift), PitchShiftInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPitchRand), PitchRandInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPan), PanInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPanRand), PanRandInput);
        }
        virtual void BindOutputs(FOutputVertexInterfaceData& InOutVertexData) override
        {
            using namespace GranularSynthNode_Step8_VertexNames;
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnPlay), OnPlayTrigger);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnFinished), OnFinishedTrigger);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnGrain), OnGrainTriggered);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutParamAudioLeft), AudioOutputLeft);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutParamAudioRight), AudioOutputRight);
        }
        virtual FDataReferenceCollection GetInputs() const override
        {
            using namespace GranularSynthNode_Step8_VertexNames;
            FDataReferenceCollection InputDataReferences;
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputTriggerPlay), PlayTrigger);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InputTriggerStop), StopTrigger);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamWaveAsset), WaveAssetInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamGrainDuration), GrainDurationMsInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamGrainsPerSecond), GrainsPerSecondInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamStartPoint), StartPointTimeInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamEndPoint), EndPointTimeInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamStartPointRand), StartPointRandMsInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamDurationRand), DurationRandMsInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamAttackTimePercent), AttackTimePercentInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamDecayTimePercent), DecayTimePercentInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamAttackCurve), AttackCurveInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamDecayCurve), DecayCurveInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPitchShift), PitchShiftInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPitchRand), PitchRandInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPan), PanInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPanRand), PanRandInput);
            return InputDataReferences;
        }
        virtual FDataReferenceCollection GetOutputs() const override
        {
            using namespace GranularSynthNode_Step8_VertexNames;
            FDataReferenceCollection OutputDataReferences;
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnPlay), OnPlayTrigger);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnFinished), OnFinishedTrigger);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnGrain), OnGrainTriggered);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutParamAudioLeft), AudioOutputLeft);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutParamAudioRight), AudioOutputRight);
            return OutputDataReferences;
        }


        // --- Execution ---
        void Execute()
        {
            // Advance output triggers
            OnPlayTrigger->AdvanceBlock();
            OnFinishedTrigger->AdvanceBlock();
            OnGrainTriggered->AdvanceBlock();

            bool bTriggeredStopThisBlock = false;
            int32 StopFrame = -1;

            // Check Stop trigger first
            for (int32 Frame : StopTrigger->GetTriggeredFrames())
            {
                if (bIsPlaying)
                {
                    bTriggeredStopThisBlock = true;
                    StopFrame = Frame;
                    UE_LOG(LogMetaSound, VeryVerbose, TEXT("GS: Stop Trigger received at frame %d."), Frame);
                    break;
                }
            }

            // Check Play trigger
            for (int32 Frame : PlayTrigger->GetTriggeredFrames())
            {
                bool bStartSuccess = TryStartPlayback(Frame);
                if (bStartSuccess)
                {
                    bTriggeredStopThisBlock = false; // Play overrides stop
                }
                else
                {
                    // TryStartPlayback failed (e.g., no valid wave).
                    // Ensure we are stopped and trigger OnFinished.
                    bIsPlaying = false;
                    // Trigger finished *only if it wasn't already stopped* by a previous Stop trigger or failure.
                    // And ensure we don't double-trigger if multiple Play triggers fail in one block
                    if (!OnFinishedTrigger->IsTriggeredInBlock()) // Avoid double-triggering if Stop also happened
                    {
                        OnFinishedTrigger->TriggerFrame(Frame);
                    }
                }
            }

            // Handle stop triggered earlier if not overridden by a successful play
            if (bTriggeredStopThisBlock)
            {
                if (bIsPlaying) // Check if it was playing before stop was processed
                {
                    bIsPlaying = false;
                    ResetVoices();
                    OnFinishedTrigger->TriggerFrame(StopFrame);
                }
            }

            // If not playing after trigger checks, output silence and return
            if (!bIsPlaying)
            {
                AudioOutputLeft->Zero();
                AudioOutputRight->Zero();
                // Ensure state reflects stopped status if wave became invalid etc.
                if (CurrentWaveProxy.IsValid() || CurrentNumChannels > 0 || ConvertDeinterleave.IsValid())
                {
                    ResetVoices(); // Reset voices just in case
                    CurrentWaveProxy.Reset();
                    CachedSoundWaveDuration = 0.0f;
                    CurrentNumChannels = 0;
                    ConvertDeinterleave.Reset();
                }
                return;
            }

            // --- Playing State Logic ---

            // --- Check Current Wave Asset Validity ---
            if (!CurrentWaveProxy.IsValid())
            {
                UE_LOG(LogMetaSound, Error, TEXT("GS: Invalid CurrentWaveProxy despite bIsPlaying=true. Stopping."));
                ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0);
                AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
            }

            // --- Handle Wave Asset Change ---
            bool bWaveJustChanged = false;
            const FSoundWaveProxyPtr InputProxy = WaveAssetInput->GetSoundWaveProxy();
            if (InputProxy.IsValid() && CurrentWaveProxy != InputProxy)
            {
                UE_LOG(LogMetaSound, Log, TEXT("GS: Wave Asset Changed during playback block. Re-initializing."));
                bWaveJustChanged = true;
                if (!InitializeWaveData(InputProxy))
                {
                    ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0);
                    AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
                }
            }
            else if (!InputProxy.IsValid() && CurrentWaveProxy.IsValid())
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS: Wave Asset Input became invalid during playback. Stopping."));
                ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0);
                AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
            }

            // --- Final Sanity Checks ---
            if (CurrentNumChannels <= 0 || !ConvertDeinterleave.IsValid() || CachedSoundWaveDuration <= 0.0f)
            {
                UE_LOG(LogMetaSound, Error, TEXT("GS: Invalid state after wave check/re-init. Stopping."));
                ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0);
                AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
            }


            // --- Get Input Values ---
            const float BaseGrainDurationSeconds = FMath::Max(MinGrainDurationSeconds, *GrainDurationMsInput / 1000.0f);
            const float MaxDurationRandSeconds = FMath::Max(0.0f, *DurationRandMsInput / 1000.0f);
            const float GrainsPerSec = FMath::Max(0.1f, *GrainsPerSecondInput);
            const float SamplesPerGrainInterval = (GrainsPerSec > 0.0f) ? (SampleRate / GrainsPerSec) : TNumericLimits<float>::Max();
            const float BaseStartPointSeconds = FMath::Max(0.0f, StartPointTimeInput->GetSeconds());
            const float BaseEndPointSeconds = EndPointTimeInput->GetSeconds();
            const float MaxStartPointRandSeconds = FMath::Max(0.0f, *StartPointRandMsInput / 1000.0f);
            const float AttackPercent = FMath::Clamp(*AttackTimePercentInput, 0.0f, 1.0f);
            const float DecayPercent = FMath::Clamp(*DecayTimePercentInput, 0.0f, 1.0f);
            const float ClampedDecayPercent = FMath::Min(DecayPercent, 1.0f - AttackPercent);
            const float AttackCurveFactor = FMath::Max(UE_SMALL_NUMBER, *AttackCurveInput);
            const float DecayCurveFactor = FMath::Max(UE_SMALL_NUMBER, *DecayCurveInput);
            const float BasePitchShiftSemitones = FMath::Clamp(*PitchShiftInput, -MaxAbsPitchShiftSemitones, MaxAbsPitchShiftSemitones);
            const float PitchRandSemitones = FMath::Max(0.0f, *PitchRandInput);
            const float BasePan = FMath::Clamp(*PanInput, -1.0f, 1.0f);
            const float PanRandAmount = FMath::Clamp(*PanRandInput, 0.0f, 1.0f);


            // Get Stereo Output Buffers & Zero
            float* OutputAudioLeftPtr = AudioOutputLeft->GetData();
            float* OutputAudioRightPtr = AudioOutputRight->GetData();
            if (!bWaveJustChanged)
            {
                FMemory::Memset(OutputAudioLeftPtr, 0, BlockSize * sizeof(float));
                FMemory::Memset(OutputAudioRightPtr, 0, BlockSize * sizeof(float));
            }


            // --- Calculate Effective Playback Region ---
            const float EffectiveEndPointSeconds = (BaseEndPointSeconds <= 0.0f || BaseEndPointSeconds > CachedSoundWaveDuration) ? CachedSoundWaveDuration : BaseEndPointSeconds;
            const float ClampedBaseStartPointSeconds = FMath::Min(BaseStartPointSeconds, EffectiveEndPointSeconds - MinGrainDurationSeconds);

            // --- Calculate Valid Start Point Randomization Range ---
            float PotentialMaxStartTime = ClampedBaseStartPointSeconds + MaxStartPointRandSeconds;
            float ValidRegionEndTime = FMath::Max(0.0f, EffectiveEndPointSeconds - MinGrainDurationSeconds);
            float ClampedMaxStartTime = FMath::Clamp(PotentialMaxStartTime, ClampedBaseStartPointSeconds, ValidRegionEndTime);
            if (ClampedBaseStartPointSeconds > ClampedMaxStartTime) { ClampedMaxStartTime = ClampedBaseStartPointSeconds; }


            // --- Trigger New Grains ---
            int32 GrainsToTriggerThisBlock = 0;
            float ElapsedSamples = BlockSize;
            if (SamplesPerGrainInterval > 0.0f && SamplesPerGrainInterval < TNumericLimits<float>::Max())
            {
                while (SamplesUntilNextGrain <= ElapsedSamples) { GrainsToTriggerThisBlock++; SamplesUntilNextGrain += SamplesPerGrainInterval; }
                SamplesUntilNextGrain -= ElapsedSamples;
            }

            for (int i = 0; i < GrainsToTriggerThisBlock; ++i)
            {
                float GrainStartTimeSeconds = FMath::FRandRange(ClampedBaseStartPointSeconds, ClampedMaxStartTime);
                float DurationOffset = FMath::FRandRange(0.0f, MaxDurationRandSeconds);
                float GrainDurationSeconds = FMath::Max(MinGrainDurationSeconds, BaseGrainDurationSeconds + DurationOffset);
                int32 GrainDurationSamples = FMath::CeilToInt(GrainDurationSeconds * SampleRate);
                float PitchOffset = FMath::FRandRange(-PitchRandSemitones, PitchRandSemitones);
                float TargetPitchShift = FMath::Clamp(BasePitchShiftSemitones + PitchOffset, -MaxAbsPitchShiftSemitones, MaxAbsPitchShiftSemitones);
                float FrameRatio = FMath::Pow(2.0f, TargetPitchShift / 12.0f);
                float PanOffset = FMath::FRandRange(-PanRandAmount, PanRandAmount);
                float GrainPanPosition = FMath::Clamp(BasePan + PanOffset, -1.0f, 1.0f);

                if (TriggerGrain(CurrentWaveProxy, GrainDurationSamples, GrainStartTimeSeconds, FrameRatio, GrainPanPosition))
                {
                    int32 TriggerFrameInBlock = FMath::Clamp(BlockSize - static_cast<int32>(SamplesUntilNextGrain), 0, BlockSize - 1);
                    OnGrainTriggered->TriggerFrame(TriggerFrameInBlock);
                }
            }


            // --- Process Active Voices ---
            for (FGrainVoice& Voice : GrainVoices)
            {
                if (Voice.bIsActive)
                {
                    if (!Voice.Reader.IsValid() || !Voice.Resampler.IsValid()) { Voice.bIsActive = false; continue; }
                    const int32 OutputFramesToProcess = FMath::Min(BlockSize, Voice.SamplesRemaining);
                    if (OutputFramesToProcess <= 0) { Voice.bIsActive = false; Voice.Reader.Reset(); Voice.Resampler.Reset(); continue; }
                    Voice.EnvelopedMonoBuffer.SetNumUninitialized(OutputFramesToProcess);
                    float* MonoBufferPtr = Voice.EnvelopedMonoBuffer.GetData();
                    int32 ActualFramesResampled = 0;
                    int32 InputFramesAvailable = Audio::GetMultichannelBufferNumFrames(Voice.SourceCircularBuffer);
                    int32 InputFramesNeeded = Voice.Resampler->GetNumInputFramesNeededToProduceOutputFrames(OutputFramesToProcess);
                    while (InputFramesAvailable < InputFramesNeeded)
                    {
                        Voice.InterleavedReadBuffer.SetNumUninitialized(DeinterleaveBlockSizeFrames * Voice.NumChannels);
                        const int32 SamplesRead = Voice.Reader->PopAudio(Voice.InterleavedReadBuffer);
                        if (SamplesRead <= 0 || Voice.Reader->HasFailed())
                        {
                            InputFramesNeeded = InputFramesAvailable;
                            if (InputFramesNeeded <= 0) { Voice.bIsActive = false; Voice.Reader.Reset(); Voice.Resampler.Reset(); }
                            break;
                        }
                        const int32 FramesRead = SamplesRead / Voice.NumChannels;
                        Audio::SetMultichannelBufferSize(Voice.NumChannels, FramesRead, DeinterleavedSourceBuffer);
                        ConvertDeinterleave->ProcessAudio(TArrayView<const float>(Voice.InterleavedReadBuffer.GetData(), SamplesRead), DeinterleavedSourceBuffer);
                        Audio::FMultichannelBufferView DeinterleavedView = Audio::MakeMultichannelBufferView(DeinterleavedSourceBuffer);
                        for (int32 Chan = 0; Chan < Voice.NumChannels; ++Chan) { Voice.SourceCircularBuffer[Chan].Push(DeinterleavedView[Chan]); }
                        InputFramesAvailable = Audio::GetMultichannelBufferNumFrames(Voice.SourceCircularBuffer);
                    }
                    if (!Voice.bIsActive) continue;
                    Audio::FMultichannelBuffer ResampledOutputBuffer;
                    Audio::SetMultichannelBufferSize(Voice.NumChannels, OutputFramesToProcess, ResampledOutputBuffer);
                    ActualFramesResampled = Voice.Resampler->ProcessAndConsumeAudio(Voice.SourceCircularBuffer, ResampledOutputBuffer);
                    if (ActualFramesResampled > 0)
                    {
                        const float PanAngle = (Voice.PanPosition + 1.0f) * 0.5f * UE_PI * 0.5f;
                        const float LeftGain = FMath::Cos(PanAngle);
                        const float RightGain = FMath::Sin(PanAngle);
                        for (int32 i = 0; i < ActualFramesResampled; ++i)
                        {
                            float MonoSample = 0.0f;
                            if (Voice.NumChannels == 1) { MonoSample = ResampledOutputBuffer[0][i]; }
                            else if (Voice.NumChannels >= 2) { MonoSample = (ResampledOutputBuffer[0][i] + ResampledOutputBuffer[1][i]) * 0.5f; }
                            float EnvelopeScale = 1.0f;
                            const int32 CurrentFrameInGrain = Voice.SamplesPlayed + i;
                            const int32 AttackSamples = FMath::CeilToInt(Voice.TotalGrainSamples * AttackPercent);
                            const int32 DecaySamples = FMath::CeilToInt(Voice.TotalGrainSamples * ClampedDecayPercent);
                            if (CurrentFrameInGrain < AttackSamples) { EnvelopeScale = FMath::Pow((AttackSamples > 0) ? (float)CurrentFrameInGrain / AttackSamples : 1.0f, AttackCurveFactor); }
                            else if (CurrentFrameInGrain >= (Voice.TotalGrainSamples - DecaySamples)) { EnvelopeScale = FMath::Pow((DecaySamples > 0) ? (float)(Voice.TotalGrainSamples - CurrentFrameInGrain) / DecaySamples : 0.0f, DecayCurveFactor); }
                            EnvelopeScale = FMath::Clamp(EnvelopeScale, 0.0f, 1.0f);
                            MonoBufferPtr[i] = MonoSample * EnvelopeScale;
                        }
                        Audio::ArrayMixIn(TArrayView<const float>(MonoBufferPtr, ActualFramesResampled), TArrayView<float>(OutputAudioLeftPtr, ActualFramesResampled), LeftGain);
                        Audio::ArrayMixIn(TArrayView<const float>(MonoBufferPtr, ActualFramesResampled), TArrayView<float>(OutputAudioRightPtr, ActualFramesResampled), RightGain);
                        Voice.SamplesPlayed += ActualFramesResampled;
                        Voice.SamplesRemaining -= ActualFramesResampled;
                    }
                    if (Voice.SamplesRemaining <= 0) { Voice.bIsActive = false; Voice.Reader.Reset(); Voice.Resampler.Reset(); }
                }
            }
        }

        // --- Reset ---
        void Reset(const IOperator::FResetParams& InParams)
        {
            ResetVoices();
            AudioOutputLeft->Zero();
            AudioOutputRight->Zero();
            SamplesUntilNextGrain = 0.0f;
            CurrentWaveProxy.Reset(); CachedSoundWaveDuration = 0.0f; CurrentNumChannels = 0;
            ConvertDeinterleave.Reset();
            OnPlayTrigger->Reset();
            OnFinishedTrigger->Reset();
            OnGrainTriggered->Reset();
            bIsPlaying = false;
            UE_LOG(LogMetaSound, Log, TEXT("Granular Synth: Operator Reset.")); // Use Metasound Log Category
        }

    private:
        // --- Helper Functions ---

        // Attempts to initialize wave-dependent state and potentially start playback
        // Returns true on success, false on failure.
        bool TryStartPlayback(int32 InFrame)
        {
            bool bWasPlayingBeforeAttempt = bIsPlaying;
            bIsPlaying = false; // Assume failure

            // Check WaveAsset Input Validity FIRST
            if (!WaveAssetInput->IsSoundWaveValid())
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS: Play Trigger at frame %d failed: Wave Asset input is not valid."), InFrame);
                // Trigger Finished ONLY if it wasn't already playing (otherwise Stop trigger handles it)
                if (bWasPlayingBeforeAttempt)
                {
                    OnFinishedTrigger->TriggerFrame(InFrame);
                }
                ResetVoices(); CurrentWaveProxy.Reset(); CachedSoundWaveDuration = 0.0f; CurrentNumChannels = 0; ConvertDeinterleave.Reset();
                return false;
            }

            const FSoundWaveProxyPtr SoundWaveProxy = WaveAssetInput->GetSoundWaveProxy();
            if (!SoundWaveProxy.IsValid())
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GS: Play Trigger at frame %d failed: Could not get valid SoundWaveProxy from Wave Asset."), InFrame);
                if (bWasPlayingBeforeAttempt)
                {
                    OnFinishedTrigger->TriggerFrame(InFrame);
                }
                ResetVoices(); CurrentWaveProxy.Reset(); CachedSoundWaveDuration = 0.0f; CurrentNumChannels = 0; ConvertDeinterleave.Reset();
                return false;
            }


            // Initialize or re-initialize wave data
            if (!InitializeWaveData(SoundWaveProxy))
            {
                UE_LOG(LogMetaSound, Error, TEXT("GS: Play Trigger at frame %d failed: Could not initialize wave data."), InFrame);
                if (bWasPlayingBeforeAttempt)
                {
                    OnFinishedTrigger->TriggerFrame(InFrame);
                }
                ResetVoices();
                return false;
            }

            // Success
            bIsPlaying = true;
            ResetVoices(); // Clear old grains on start/restart
            SamplesUntilNextGrain = 0.0f;
            OnPlayTrigger->TriggerFrame(InFrame);
            UE_LOG(LogMetaSound, Log, TEXT("GS: Playback %s at frame %d."), bWasPlayingBeforeAttempt ? TEXT("Restarted") : TEXT("Started"), InFrame);
            return true;
        }

        // Initializes cached wave data and required components (like deinterleaver)
        // Returns true on success, false on failure.
        bool InitializeWaveData(const FSoundWaveProxyPtr& InSoundWaveProxy)
        {
            // Input proxy is assumed valid by caller (TryStartPlayback)
            CurrentWaveProxy = InSoundWaveProxy; // Update tracked proxy

            FSoundWaveProxyReader::FSettings TempSettings;
            auto TempReader = FSoundWaveProxyReader::Create(CurrentWaveProxy.ToSharedRef(), TempSettings);
            if (TempReader.IsValid())
            {
                CachedSoundWaveDuration = (float)TempReader->GetNumFramesInWave() / FMath::Max(1.0f, TempReader->GetSampleRate());
                CurrentNumChannels = TempReader->GetNumChannels();
                if (CurrentNumChannels <= 0 || CachedSoundWaveDuration <= 0.0f)
                {
                    UE_LOG(LogMetaSound, Error, TEXT("GS: Wave Asset reports invalid duration (%.2f) or channels (%d)."), CachedSoundWaveDuration, CurrentNumChannels);
                    CurrentNumChannels = 0; CachedSoundWaveDuration = 0.0f; ConvertDeinterleave.Reset(); return false;
                }
                Audio::FConvertDeinterleaveParams ConvertParams;
                ConvertParams.NumInputChannels = CurrentNumChannels; ConvertParams.NumOutputChannels = CurrentNumChannels;
                ConvertDeinterleave = Audio::IConvertDeinterleave::Create(ConvertParams);
                if (!ConvertDeinterleave.IsValid())
                {
                    UE_LOG(LogMetaSound, Error, TEXT("GS: Failed to create deinterleaver for %d channels."), CurrentNumChannels);
                    CurrentNumChannels = 0; CachedSoundWaveDuration = 0.0f; return false;
                }
                Audio::SetMultichannelBufferSize(CurrentNumChannels, DeinterleaveBlockSizeFrames, DeinterleavedSourceBuffer);
                return true; // Success
            }
            else { /* ... Error Handling ... */ return false; }
        }


        // TriggerGrain signature remains the same
        bool TriggerGrain(const FSoundWaveProxyPtr& InSoundWaveProxy, int32 InGrainDurationSamples, float InStartTimeSeconds, float InFrameRatio, float InPanPosition)
        {
            // ... (Same implementation as before, relies on caller passing valid proxy) ...
            InStartTimeSeconds = FMath::Max(0.0f, InStartTimeSeconds);
            if (!InSoundWaveProxy.IsValid() || InGrainDurationSamples <= 0 || CurrentNumChannels <= 0) return false;
            int32 VoiceIndex = -1;
            for (int32 i = 0; i < GrainVoices.Num(); ++i) { if (!GrainVoices[i].bIsActive) { VoiceIndex = i; break; } }
            if (VoiceIndex == -1) return false;
            FGrainVoice& NewVoice = GrainVoices[VoiceIndex];
            FSoundWaveProxyReader::FSettings ReaderSettings;
            ReaderSettings.StartTimeInSeconds = InStartTimeSeconds;
            ReaderSettings.bIsLooping = false;
            const uint32 DecodeSizeQuantization = 128; const uint32 MinDecodeSize = 128;
            uint32 DesiredDecodeSize = BlockSize * 2;
            uint32 ConformedDecodeSize = FMath::Max(DesiredDecodeSize, MinDecodeSize);
            ConformedDecodeSize = ((ConformedDecodeSize + DecodeSizeQuantization - 1) / DecodeSizeQuantization) * DecodeSizeQuantization;
            ReaderSettings.MaxDecodeSizeInFrames = ConformedDecodeSize;
            NewVoice.Reader = FSoundWaveProxyReader::Create(InSoundWaveProxy.ToSharedRef(), ReaderSettings);
            if (!NewVoice.Reader.IsValid()) { UE_LOG(LogMetaSound, Error, TEXT("GS: Failed reader create voice %d time %.2f."), VoiceIndex, InStartTimeSeconds); return false; }
            NewVoice.NumChannels = CurrentNumChannels;
            NewVoice.Resampler = MakeUnique<Audio::FMultichannelLinearResampler>(NewVoice.NumChannels);
            NewVoice.Resampler->SetFrameRatio(InFrameRatio, 0);
            NewVoice.SourceCircularBuffer.Empty();
            NewVoice.InterleavedReadBuffer.SetNumUninitialized(DeinterleaveBlockSizeFrames * NewVoice.NumChannels, EAllowShrinking::No);
            NewVoice.EnvelopedMonoBuffer.SetNumUninitialized(BlockSize, EAllowShrinking::No);
            Audio::SetMultichannelCircularBufferCapacity(NewVoice.NumChannels, DeinterleaveBlockSizeFrames + BlockSize * 4, NewVoice.SourceCircularBuffer);
            NewVoice.bIsActive = true;
            NewVoice.SamplesRemaining = InGrainDurationSamples;
            NewVoice.SamplesPlayed = 0;
            NewVoice.TotalGrainSamples = InGrainDurationSamples;
            NewVoice.PanPosition = InPanPosition;
            return true;
        }

        void ResetVoices()
        {
            // ... (Same implementation as before) ...
            for (FGrainVoice& Voice : GrainVoices)
            {
                Voice.bIsActive = false; Voice.NumChannels = 0; Voice.SamplesRemaining = 0;
                Voice.SamplesPlayed = 0; Voice.TotalGrainSamples = 0; Voice.PanPosition = 0.0f;
                Voice.Reader.Reset(); Voice.Resampler.Reset(); Voice.SourceCircularBuffer.Empty();
            }
        }

        // --- Input Parameter References ---
        FTriggerReadRef PlayTrigger;
        FTriggerReadRef StopTrigger;
        FWaveAssetReadRef WaveAssetInput; // Keep using FWaveAssetReadRef here
        FFloatReadRef GrainDurationMsInput;
        FFloatReadRef GrainsPerSecondInput;
        FTimeReadRef StartPointTimeInput;
        FTimeReadRef EndPointTimeInput;
        FFloatReadRef StartPointRandMsInput;
        FFloatReadRef DurationRandMsInput;
        FFloatReadRef AttackTimePercentInput;
        FFloatReadRef DecayTimePercentInput;
        FFloatReadRef AttackCurveInput;
        FFloatReadRef DecayCurveInput;
        FFloatReadRef PitchShiftInput;
        FFloatReadRef PitchRandInput;
        FFloatReadRef PanInput;
        FFloatReadRef PanRandInput;


        // --- Output Parameter References ---
        FTriggerWriteRef OnPlayTrigger;
        FTriggerWriteRef OnFinishedTrigger;
        FTriggerWriteRef OnGrainTriggered;
        FAudioBufferWriteRef AudioOutputLeft;
        FAudioBufferWriteRef AudioOutputRight;


        // --- Operator Settings ---
        float SampleRate;
        int32 BlockSize;

        // --- Internal State ---
        bool bIsPlaying;
        float SamplesUntilNextGrain = 0.0f;
        TArray<FGrainVoice> GrainVoices;
        FSoundWaveProxyPtr CurrentWaveProxy; // Store the validated proxy ptr
        float CachedSoundWaveDuration = 0.0f;
        int32 CurrentNumChannels = 0;
        TUniquePtr<Audio::IConvertDeinterleave> ConvertDeinterleave;
        Audio::FMultichannelBuffer DeinterleavedSourceBuffer;
    };

    // --- Node ---
    class FGranularSynthNode_Step8 : public FNodeFacade
    {
    public:
        FGranularSynthNode_Step8(const FNodeInitData& InitData)
            : FNodeFacade(InitData.InstanceName, InitData.InstanceID, TFacadeOperatorClass<FGranularSynthOperator_Step8>())
        {
        }
    };

    // --- Registration ---
    METASOUND_REGISTER_NODE(FGranularSynthNode_Step8)

} // namespace Metasound

// Undefine the LOCTEXT namespace
#undef LOCTEXT_NAMESPACE
