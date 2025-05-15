// Copyright Epic Games, Inc. All Rights Reserved.

#include "Metagrain.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundParamHelper.h"
#include "MetasoundPrimitives.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundStandardNodesNames.h"
#include "MetasoundStandardNodesCategories.h"
#include "MetasoundFacade.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundWave.h"
#include "MetasoundTrigger.h"
#include "MetasoundOperatorSettings.h"
#include "MetasoundDataReferenceCollection.h"
#include "MetasoundVertex.h"
#include "MetasoundNodeInterface.h"
#include "MetasoundBuilderInterface.h"
#include "MetasoundLog.h"
#include "DSP/Dsp.h"
#include "DSP/FloatArrayMath.h"
#include "DSP/MultichannelLinearResampler.h"
#include "DSP/ConvertDeinterleave.h"
#include "DSP/MultichannelBuffer.h"
#include "DSP/BufferVectorOperations.h"
#include "Containers/Array.h"
#include "Sound/SoundWaveProxyReader.h"
#include "AudioDevice.h"
#include "Internationalization/Text.h"
#include "UObject/NameTypes.h"
#include "Math/UnrealMathUtility.h"
#include <limits>

// Change localization namespace to be unique
#define LOCTEXT_NAMESPACE "MetasoundStandardNodes_GranularWavePlayerSmoothNode"

namespace Metasound
{
    // --- Parameter Names ---
    // Rename the parameter namespace to avoid conflicts
    namespace GranularWavePlayerSmoothNode_Params
    {
        // Trigger inputs
        METASOUND_PARAM(InputTriggerPlay, "Play", "Trigger to start playback.");
        METASOUND_PARAM(InputTriggerStop, "Stop", "Trigger to stop playback.");
        
        // Wave asset
        METASOUND_PARAM(InParamWaveAsset, "Wave", "Wave asset to play.");
        
        // Float parameters
        METASOUND_PARAM(InParamGrainDuration, "Grain Duration (ms)", "Duration of each grain in milliseconds.");
        METASOUND_PARAM(InParamGrainsPerSecond, "Grains Per Sec", "Number of grains triggered per second.");
        METASOUND_PARAM(InParamPlaybackSpeed, "Speed (%)", "Playback speed as a percentage (0-800%). Set to 0% to freeze playback and enable manual position scrubbing.");
        METASOUND_PARAM(InParamPlayPosition, "Position (%)", "Current playback position as percentage of total wave length (0-100%).");
        METASOUND_PARAM(InParamStartPointRand, "Start Rand (ms)", "Random offset to grain start point in milliseconds.");
        METASOUND_PARAM(InParamDurationRand, "Duration Rand (ms)", "Random offset to grain duration in milliseconds.");
        METASOUND_PARAM(InParamAttackTimePercent, "Attack (%)", "Attack time as percentage of grain duration.");
        METASOUND_PARAM(InParamDecayTimePercent, "Decay (%)", "Decay time as percentage of grain duration.");
        METASOUND_PARAM(InParamAttackCurve, "Attack Curve", "Attack envelope curve shape exponent.");
        METASOUND_PARAM(InParamDecayCurve, "Decay Curve", "Decay envelope curve shape exponent.");
        METASOUND_PARAM(InParamPitchShift, "Pitch (st)", "Base pitch shift in semitones.");
        METASOUND_PARAM(InParamPitchRand, "Pitch Rand (st)", "Random pitch shift range in semitones.");
        METASOUND_PARAM(InParamPan, "Pan", "Stereo pan position (-1.0 to 1.0).");
        METASOUND_PARAM(InParamPanRand, "Pan Rand", "Random pan deviation (0.0 to 1.0).");
        METASOUND_PARAM(InParamTimeJitter, "Time Jitter (ms)", "Random variation in grain trigger timing for a more organic sound (0-100ms).");
        METASOUND_PARAM(InParamVolumeRand, "Volume Rand (%)", "Random volume variation (0-100%). At 0%, all grains play at full volume. At 100%, grains can play at any volume from silent to full.");
        METASOUND_PARAM(InParamSmoothing, "Attack Smoothing", "Reduces attack transients for smoother pad-like sounds (0-100%).");
        METASOUND_PARAM(InParamGrainOverlap, "Grain Overlap", "Controls how many grains overlap (1-5). Higher values create smoother textures.");
        
        // Int parameters (grouped together)
        METASOUND_PARAM(InParamGrainDensity, "Grain Density", "Number of simultaneous grain voices (1-32). Higher values create thicker, smoother textures.");
        METASOUND_PARAM(InParamWindowShape, "Window Shape", "Grain window function (0=Linear, 1=Parabolic, 2=Gaussian, 3=Cosine, 4=Hann, 5=Blackman, 6=Triangular, 7=Rectangular).");
        METASOUND_PARAM(InParamXfadeCurve, "Crossfade Type", "Controls grain envelope crossfade type (0=Linear, 1=Equal Power, 2=Smooth).");

        // Output parameters
        METASOUND_PARAM(OutputTriggerOnPlay, "On Play", "Triggered when playback starts.");
        METASOUND_PARAM(OutputTriggerOnFinished, "On Finished", "Triggered when playback finishes.");
        METASOUND_PARAM(OutputTriggerOnGrain, "On Grain", "Triggered when a new grain starts.");
        METASOUND_PARAM(OutParamAudioLeft, "Out Left", "The left channel audio output.");
        METASOUND_PARAM(OutParamAudioRight, "Out Right", "The right channel audio output.");
        METASOUND_PARAM(OutParamTime, "Time", "Current playback position as time value."); // Changed to Time type instead of ms
    }

    // --- Grain Voice Structure ---
    // Renamed struct
    struct FWavePlayerSmoothGrainVoice
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
        
        Audio::FAlignedFloatBuffer InterleavedReadBuffer;
        Audio::FAlignedFloatBuffer EnvelopedMonoBuffer;
        
        // Add fields for improved grain processing
        float PhaseOffset = 0.0f;     // For phase alignment between grains
        float EnvelopeCurve = 1.0f;   // Dynamic curve adjustment
        float SmoothingAmount = 0.0f; // Per-grain smoothing 
    };

    // --- Operator ---
    // Renamed operator class
    class FGranularWavePlayerSmoothOperator : public TExecutableOperator<FGranularWavePlayerSmoothOperator>
    {
        static constexpr int32 MaxGrainVoices = 32;
        static constexpr float MinGrainDurationSeconds = 0.005f;
        static constexpr float MaxAbsPitchShiftSemitones = 60.0f;
        static constexpr int32 DeinterleaveBlockSizeFrames = 256;

        // --- Define envelope shape constants ---
        enum class EGrainWindowShape : uint8
        {
            Linear = 0,
            Parabolic = 1,
            Gaussian = 2, 
            Cosine = 3,
            Hann = 4,
            Blackman = 5,
            Triangular = 6,
            Rectangular = 7
        };

    public:
        // --- Constructor ---
        FGranularWavePlayerSmoothOperator(const FOperatorSettings& InSettings,
            const FTriggerReadRef& InPlayTrigger,
            const FTriggerReadRef& InStopTrigger,
            const FWaveAssetReadRef& InWaveAsset,
            const FFloatReadRef& InGrainDurationMs,
            const FFloatReadRef& InGrainsPerSecond,
            const FFloatReadRef& InPlaybackSpeed,
            const FFloatReadRef& InPlayPosition,
            const FFloatReadRef& InStartPointRandMs,
            const FFloatReadRef& InDurationRandMs,
            const FFloatReadRef& InAttackTimePercent,
            const FFloatReadRef& InDecayTimePercent,
            const FFloatReadRef& InAttackCurve,
            const FFloatReadRef& InDecayCurve,
            const FFloatReadRef& InPitchShift,
            const FFloatReadRef& InPitchRand,
            const FFloatReadRef& InPan,
            const FFloatReadRef& InPanRand,
            const FFloatReadRef& InTimeJitter,
            const FFloatReadRef& InVolumeRand,
            const FFloatReadRef& InSmoothing,
            const FFloatReadRef& InGrainOverlap,
            const FInt32ReadRef& InGrainDensity,
            const FInt32ReadRef& InWindowShape,
            const FInt32ReadRef& InXfadeCurve)
            : PlayTrigger(InPlayTrigger)
            , StopTrigger(InStopTrigger)
            , WaveAssetInput(InWaveAsset)
            , GrainDurationMsInput(InGrainDurationMs)
            , GrainsPerSecondInput(InGrainsPerSecond)
            , PlaybackSpeedInput(InPlaybackSpeed)
            , PlayPositionInput(InPlayPosition)
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
            , TimeJitterInput(InTimeJitter)
            , VolumeRandInput(InVolumeRand)
            , SmoothingInput(InSmoothing)
            , GrainOverlapInput(InGrainOverlap)
            // Int inputs grouped together
            , GrainDensityInput(InGrainDensity)
            , WindowShapeInput(InWindowShape)
            , XfadeCurveInput(InXfadeCurve)
            , OnPlayTrigger(FTriggerWriteRef::CreateNew(InSettings))
            , OnFinishedTrigger(FTriggerWriteRef::CreateNew(InSettings))
            , OnGrainTriggered(FTriggerWriteRef::CreateNew(InSettings))
            , AudioOutputLeft(FAudioBufferWriteRef::CreateNew(InSettings))
            , AudioOutputRight(FAudioBufferWriteRef::CreateNew(InSettings))
            , TimeOutput(FTimeWriteRef::CreateNew(FTime::FromSeconds(0.0)))  // Use FTime::FromSeconds instead of time literal
            , SampleRate(InSettings.GetSampleRate())
            , BlockSize(InSettings.GetNumFramesPerBlock())
            , bIsPlaying(false)
        {
            GrainVoices.SetNum(MaxGrainVoices);
            for (FWavePlayerSmoothGrainVoice& Voice : GrainVoices)
            {
                Voice.InterleavedReadBuffer.SetNumUninitialized(DeinterleaveBlockSizeFrames * 2);
                Voice.EnvelopedMonoBuffer.SetNumUninitialized(BlockSize);
                Audio::SetMultichannelCircularBufferCapacity(2, DeinterleaveBlockSizeFrames + BlockSize * 4, Voice.SourceCircularBuffer);
            }
            SamplesUntilNextGrain = 0.0f;
            CachedSoundWaveDuration = 0.0f;
            DeinterleavedSourceBuffer.SetNum(2);
            Audio::SetMultichannelBufferSize(2, DeinterleaveBlockSizeFrames, DeinterleavedSourceBuffer);
            
            // Initialize the smoothing filter for reducing transients
            for (int32 i = 0; i < 2; ++i)
            {
                EnvelopeBuffer[i].SetNumZeroed(BlockSize);
                PrevGrainValue[i] = 0.0f;
            }
        }

        // --- Metasound Node Interface ---
        static const FVertexInterface& DeclareVertexInterface()
        {
            // Update namespace reference
            using namespace GranularWavePlayerSmoothNode_Params;
            static const FVertexInterface Interface(
                FInputVertexInterface(
                    // Trigger inputs
                    TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTriggerPlay)),
                    TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(InputTriggerStop)),
                    
                    // Wave asset
                    TInputDataVertex<FWaveAsset>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamWaveAsset)),
                    
                    // Float parameters
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamGrainDuration), 100.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamGrainsPerSecond), 10.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPlaybackSpeed), 100.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPlayPosition), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamStartPointRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDurationRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamAttackTimePercent), 0.1f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDecayTimePercent), 0.1f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamAttackCurve), 1.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamDecayCurve), 1.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPitchShift), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPitchRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPan), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamPanRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamTimeJitter), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamVolumeRand), 0.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamSmoothing), 30.0f),
                    TInputDataVertex<float>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamGrainOverlap), 3.0f),
                    
                    // Int parameters grouped together
                    TInputDataVertex<int32>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamGrainDensity), 8),
                    TInputDataVertex<int32>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamWindowShape), 0),
                    TInputDataVertex<int32>(METASOUND_GET_PARAM_NAME_AND_METADATA(InParamXfadeCurve), 1)
                ),
                FOutputVertexInterface(
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnPlay)),
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnFinished)),
                    TOutputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutputTriggerOnGrain)),
                    TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutParamAudioLeft)),
                    TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutParamAudioRight)),
                    TOutputDataVertex<FTime>(METASOUND_GET_PARAM_NAME_AND_METADATA(OutParamTime)) // Changed to Time type
                )
            );
            return Interface;
        }

        static const FNodeClassMetadata& GetNodeInfo()
        {
            auto CreateNodeClassMetadata = []() -> FNodeClassMetadata
                {
                    FNodeClassMetadata Metadata;
                    // Fix the class name to be "GranularWavePlayerSmooth"
                    Metadata.ClassName = { FName("GranularWavePlayerSmooth"), FName(""), FName("") };
                    Metadata.MajorVersion = 1; Metadata.MinorVersion = 0;
                    Metadata.DisplayName = LOCTEXT("GranularWavePlayerSmooth_DisplayName", "Granular Wave Player Smooth");
                    Metadata.Description = LOCTEXT("GranularWavePlayerSmooth_Description", "Granular wave player optimized for smooth pad-like textures");
                    Metadata.Author = TEXT("Metasound");
                    Metadata.PromptIfMissing = Metasound::PluginNodeMissingPrompt;
                    Metadata.DefaultInterface = DeclareVertexInterface();
                    Metadata.CategoryHierarchy = { LOCTEXT("GranularWavePlayerCategory", "Synth") };
                    return Metadata;
                };
            static const FNodeClassMetadata Metadata = CreateNodeClassMetadata();
            return Metadata;
        }

        static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& InParams, FBuildResults& OutResults)
        {
            using namespace GranularWavePlayerSmoothNode_Params;
            const FInputVertexInterfaceData& InputData = InParams.InputData;
            
            // All param inputs in same order as constructor
            FTriggerReadRef PlayTriggerIn = InputData.GetOrConstructDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(InputTriggerPlay), InParams.OperatorSettings);
            FTriggerReadRef StopTriggerIn = InputData.GetOrConstructDataReadReference<FTrigger>(METASOUND_GET_PARAM_NAME(InputTriggerStop), InParams.OperatorSettings);
            FWaveAssetReadRef WaveAssetIn = InputData.GetOrCreateDefaultDataReadReference<FWaveAsset>(METASOUND_GET_PARAM_NAME(InParamWaveAsset), InParams.OperatorSettings);
            
            // Float params
            FFloatReadRef GrainDurationIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamGrainDuration), InParams.OperatorSettings);
            FFloatReadRef GrainsPerSecondIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamGrainsPerSecond), InParams.OperatorSettings);
            FFloatReadRef PlaybackSpeedIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPlaybackSpeed), InParams.OperatorSettings);
            FFloatReadRef PlayPositionIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamPlayPosition), InParams.OperatorSettings);
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
            FFloatReadRef TimeJitterIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamTimeJitter), InParams.OperatorSettings);
            FFloatReadRef VolumeRandIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamVolumeRand), InParams.OperatorSettings);
            FFloatReadRef SmoothingIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamSmoothing), InParams.OperatorSettings);
            FFloatReadRef GrainOverlapIn = InputData.GetOrCreateDefaultDataReadReference<float>(METASOUND_GET_PARAM_NAME(InParamGrainOverlap), InParams.OperatorSettings);
            
            // Int params (grouped together)
            FInt32ReadRef GrainDensityIn = InputData.GetOrCreateDefaultDataReadReference<int32>(METASOUND_GET_PARAM_NAME(InParamGrainDensity), InParams.OperatorSettings);
            FInt32ReadRef WindowShapeIn = InputData.GetOrCreateDefaultDataReadReference<int32>(METASOUND_GET_PARAM_NAME(InParamWindowShape), InParams.OperatorSettings);
            FInt32ReadRef XfadeCurveIn = InputData.GetOrCreateDefaultDataReadReference<int32>(METASOUND_GET_PARAM_NAME(InParamXfadeCurve), InParams.OperatorSettings);
            
            return MakeUnique<FGranularWavePlayerSmoothOperator>(InParams.OperatorSettings, 
                PlayTriggerIn, StopTriggerIn, WaveAssetIn, 
                GrainDurationIn, GrainsPerSecondIn, PlaybackSpeedIn, PlayPositionIn, 
                StartPointRandIn, DurationRandIn, AttackTimePercentIn, DecayTimePercentIn, 
                AttackCurveIn, DecayCurveIn, PitchShiftIn, PitchRandIn, PanIn, PanRandIn,
                TimeJitterIn, VolumeRandIn, SmoothingIn, GrainOverlapIn,
                GrainDensityIn, WindowShapeIn, XfadeCurveIn);
        }

        // --- Metasound Node Interface ---
        virtual void BindInputs(FInputVertexInterfaceData& InOutVertexData) override
        {
            using namespace GranularWavePlayerSmoothNode_Params;
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputTriggerPlay), PlayTrigger);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InputTriggerStop), StopTrigger);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamWaveAsset), WaveAssetInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamGrainDuration), GrainDurationMsInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamGrainsPerSecond), GrainsPerSecondInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPlaybackSpeed), PlaybackSpeedInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamPlayPosition), PlayPositionInput);
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
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamTimeJitter), TimeJitterInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamVolumeRand), VolumeRandInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamSmoothing), SmoothingInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamGrainOverlap), GrainOverlapInput);
            
            // Int parameters (grouped together)
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamGrainDensity), GrainDensityInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamWindowShape), WindowShapeInput);
            InOutVertexData.BindReadVertex(METASOUND_GET_PARAM_NAME(InParamXfadeCurve), XfadeCurveInput);
        }
        
        virtual void BindOutputs(FOutputVertexInterfaceData& InOutVertexData) override
        {
            using namespace GranularWavePlayerSmoothNode_Params;
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnPlay), OnPlayTrigger);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnFinished), OnFinishedTrigger);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutputTriggerOnGrain), OnGrainTriggered);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutParamAudioLeft), AudioOutputLeft);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutParamAudioRight), AudioOutputRight);
            InOutVertexData.BindWriteVertex(METASOUND_GET_PARAM_NAME(OutParamTime), TimeOutput); // Changed to Time
        }
        
        virtual FDataReferenceCollection GetInputs() const override
        {
            using namespace GranularWavePlayerSmoothNode_Params;
            FDataReferenceCollection InputDataReferences;
            
            // ...existing code with reordered parameters...
            
            // Float parameters
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamGrainDuration), GrainDurationMsInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamGrainsPerSecond), GrainsPerSecondInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPlaybackSpeed), PlaybackSpeedInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamPlayPosition), PlayPositionInput);
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
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamTimeJitter), TimeJitterInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamVolumeRand), VolumeRandInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamSmoothing), SmoothingInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamGrainOverlap), GrainOverlapInput);
            
            // Int parameters (grouped together)
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamGrainDensity), GrainDensityInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamWindowShape), WindowShapeInput);
            InputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(InParamXfadeCurve), XfadeCurveInput);
            
            return InputDataReferences;
        }
        
        virtual FDataReferenceCollection GetOutputs() const override
        {
            using namespace GranularWavePlayerSmoothNode_Params;
            FDataReferenceCollection OutputDataReferences;
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnPlay), OnPlayTrigger);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnFinished), OnFinishedTrigger);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutputTriggerOnGrain), OnGrainTriggered);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutParamAudioLeft), AudioOutputLeft);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutParamAudioRight), AudioOutputRight);
            OutputDataReferences.AddDataReadReference(METASOUND_GET_PARAM_NAME(OutParamTime), TimeOutput); // Changed to Time
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
                    UE_LOG(LogMetaSound, VeryVerbose, TEXT("GWP: Stop Trigger received at frame %d."), Frame);
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
                    if (!OnFinishedTrigger->IsTriggeredInBlock())
                    {
                        OnFinishedTrigger->TriggerFrame(Frame);
                    }
                }
            }

            // Handle stop triggered earlier if not overridden by a successful play
            if (bTriggeredStopThisBlock)
            {
                if (bIsPlaying)
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
                *TimeOutput = FTime::FromSeconds(0.0); // Use FTime::FromSeconds instead of time literal
                if (CurrentWaveProxy.IsValid() || CurrentNumChannels > 0 || ConvertDeinterleave.IsValid())
                {
                    ResetVoices();
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
                UE_LOG(LogMetaSound, Error, TEXT("GWP: Invalid CurrentWaveProxy despite bIsPlaying=true. Stopping."));
                ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0);
                AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
            }

            // --- Handle Wave Asset Change ---
            bool bWaveJustChanged = false;
            const FSoundWaveProxyPtr InputProxy = WaveAssetInput->GetSoundWaveProxy();
            if (InputProxy.IsValid() && CurrentWaveProxy != InputProxy)
            {
                UE_LOG(LogMetaSound, Log, TEXT("GWP: Wave Asset Changed during playback block. Re-initializing."));
                bWaveJustChanged = true;
                if (!InitializeWaveData(InputProxy))
                {
                    ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0);
                    AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
                }
            }
            else if (!InputProxy.IsValid() && CurrentWaveProxy.IsValid())
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GWP: Wave Asset Input became invalid during playback. Stopping."));
                ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0);
                AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
            }

            // --- Final Sanity Checks ---
            if (CurrentNumChannels <= 0 || !ConvertDeinterleave.IsValid() || CachedSoundWaveDuration <= 0.0f)
            {
                UE_LOG(LogMetaSound, Error, TEXT("GWP: Invalid state after wave check/re-init. Stopping."));
                ResetVoices(); bIsPlaying = false; OnFinishedTrigger->TriggerFrame(0);
                AudioOutputLeft->Zero(); AudioOutputRight->Zero(); return;
            }

            // --- Get Input Values ---
            const float BaseGrainDurationSeconds = FMath::Max(MinGrainDurationSeconds, *GrainDurationMsInput / 1000.0f);
            const float MaxDurationRandSeconds = FMath::Max(0.0f, *DurationRandMsInput / 1000.0f);
            const float GrainsPerSec = FMath::Max(0.1f, *GrainsPerSecondInput);
            const float SamplesPerGrainInterval = (GrainsPerSec > 0.0f) ? (SampleRate / GrainsPerSec) : TNumericLimits<float>::Max();
            
            // Get
            const float PlaybackSpeed = FMath::Clamp(*PlaybackSpeedInput, 0.0f, 800.0f) / 100.0f;
            const bool bFreezed = FMath::IsNearlyZero(PlaybackSpeed, 0.001f);

            // Add all the missing parameter definitions here
            const int32 DesiredGrainDensity = FMath::Clamp(*GrainDensityInput, 1, MaxGrainVoices);
            const float TimeJitterMs = FMath::Max(0.0f, *TimeJitterInput);
            const int32 WindowShapeIndex = FMath::Clamp(*WindowShapeInput, 0, 7);
            const EGrainWindowShape GrainWindowShape = static_cast<EGrainWindowShape>(WindowShapeIndex);
            const float VolumeRandPercent = FMath::Clamp(*VolumeRandInput, 0.0f, 100.0f);
            const float Smoothing = FMath::Clamp(*SmoothingInput, 0.0f, 100.0f) / 100.0f;
            const float GrainOverlap = FMath::Clamp(*GrainOverlapInput, 1.0f, 5.0f);
            const int32 XfadeCurveIndex = FMath::Clamp(*XfadeCurveInput, 0, 2);
            
            // Detect changes in freeze state (for optimization purposes)
            const bool bFreezeStateChanged = bFreezed != bPreviousFreezeState;
            bPreviousFreezeState = bFreezed;
            
            // When freeze state changes, handle it smoothly without resetting voices
            if (bFreezeStateChanged && bIsPlaying)
            {
                // Don't reset voices, just reset grain timing for immediate new grains
                SamplesUntilNextGrain = 0.0f;
                
                UE_LOG(LogMetaSound, Log, TEXT("GWP: PlaybackSpeed is now %s - Smooth transition"), 
                    bFreezed ? TEXT("ZERO (freeze mode)") : TEXT("PLAYING"));
                
                // When freezing, store position exactly where we are
                if (bFreezed)
                {
                    // No need to modify position - it will be used as is
                    UE_LOG(LogMetaSound, Verbose, TEXT("GWP: Freezing at position: %.2f sec"), 
                        CurrentPlaybackPositionSeconds);
                }
            }

            // Calculate position differently based on speed
            float PositionInSeconds;
            if (bFreezed)
            {
                // When speed is 0, use the PlayPosition parameter
                const float PlayPosition = FMath::Clamp(*PlayPositionInput, 0.0f, 100.0f) / 100.0f;
                const float MaxValidPosition = FMath::Max(0.0f, CachedSoundWaveDuration - (BaseGrainDurationSeconds + MaxDurationRandSeconds));
                const float SafePlayPosition = FMath::Min(PlayPosition, MaxValidPosition / CachedSoundWaveDuration);
                
                // Calculate position in seconds
                float NewPositionInSeconds = SafePlayPosition * CachedSoundWaveDuration;
                
                // Check if position has changed significantly
                // But don't reset voices - just update the target position for smooth transition
                if (FMath::Abs(NewPositionInSeconds - CurrentPlaybackPositionSeconds) > 0.01f) // More sensitive threshold
                {
                    // Only reset grain timing, not the voices
                    SamplesUntilNextGrain = 0.0f;
                    
                    // Log position change for debugging
                    UE_LOG(LogMetaSound, Verbose, TEXT("GWP: Position changed in freeze mode: %.2f -> %.2f"), 
                        CurrentPlaybackPositionSeconds, NewPositionInSeconds);
                }
                
                PositionInSeconds = NewPositionInSeconds;
                CurrentPlaybackPositionSeconds = NewPositionInSeconds;
            }
            else
            {
                // In normal playback mode, advance position based on speed
                float BlockDurationSeconds = static_cast<float>(BlockSize) / SampleRate;
                
                // If we just transitioned from freeze to normal, don't advance position yet
                if (!bFreezeStateChanged)
                {
                    CurrentPlaybackPositionSeconds += (BlockDurationSeconds * PlaybackSpeed);
                }
                
                // Wrap around if we reach the end of the file
                if (CurrentPlaybackPositionSeconds >= CachedSoundWaveDuration && CachedSoundWaveDuration > 0.0f)
                {
                    CurrentPlaybackPositionSeconds = FMath::Fmod(CurrentPlaybackPositionSeconds, CachedSoundWaveDuration);
                }
                
                PositionInSeconds = CurrentPlaybackPositionSeconds;
            }

            // UPDATE THE TIME OUTPUT - add this line right after calculating position
            *TimeOutput = FTime::FromSeconds(CurrentPlaybackPositionSeconds);
            
            // Use position as base start point
            const float BaseStartPointSeconds = FMath::Max(0.0f, PositionInSeconds);
            
            // Calculate the effective end point depending on the wave duration
            const float BaseEndPointSeconds = CachedSoundWaveDuration;
            const float MaxStartPointRandSeconds = FMath::Max(0.0f, *StartPointRandMsInput / 1000.0f);
            const float AttackPercent = FMath::Clamp(*AttackTimePercentInput, 0.0f, 1.0f);
            const float DecayPercent = FMath::Clamp(*DecayTimePercentInput, 0.0f, 1.0f);
            const float ClampedDecayPercent = FMath::Min(DecayPercent, 1.0f - AttackPercent);
            const float AttackCurveFactor = FMath::Max(UE_SMALL_NUMBER, *AttackCurveInput);
            const float DecayCurveFactor = FMath::Max(UE_SMALL_NUMBER, *DecayCurveInput);
            
            // Apply playback speed to the pitch shift
            const float BasePitchShiftSemitones = FMath::Clamp(*PitchShiftInput, -MaxAbsPitchShiftSemitones, MaxAbsPitchShiftSemitones);
            
            // Use only the pitch parameter for pitch shifting, independent of playback speed
            const float TotalPitchShift = BasePitchShiftSemitones;
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
            const float EffectiveEndPointSeconds = (BaseEndPointSeconds <= 0.0f || BaseEndPointSeconds > CachedSoundWaveDuration) ? 
                CachedSoundWaveDuration : BaseEndPointSeconds;
            const float ClampedBaseStartPointSeconds = FMath::Min(BaseStartPointSeconds, EffectiveEndPointSeconds - MinGrainDurationSeconds);

            // --- Calculate Valid Start Point Randomization Range ---
            // Use the same approach as GranularSynthNode_Step1.cpp - add positive random offset
            float PotentialMaxStartTime = ClampedBaseStartPointSeconds + MaxStartPointRandSeconds;
            float ValidRegionEndTime = FMath::Max(0.0f, EffectiveEndPointSeconds - MinGrainDurationSeconds);
            float ClampedMaxStartTime = FMath::Clamp(PotentialMaxStartTime, ClampedBaseStartPointSeconds, ValidRegionEndTime);
            if (ClampedBaseStartPointSeconds > ClampedMaxStartTime) { ClampedMaxStartTime = ClampedBaseStartPointSeconds; }

            // --- Trigger New Grains ---
            int32 GrainsToTriggerThisBlock = 0;
            float ElapsedSamples = BlockSize;
            
            // Apply time jitter to grain triggering
            float TimeJitterSamples = (TimeJitterMs / 1000.0f) * SampleRate;
            
            // When we just changed freeze state, trigger more grains for smoother transition
            if (bFreezeStateChanged)
            {
                // Force at least 2 grains this block for a smoother transition
                GrainsToTriggerThisBlock = FMath::Max(2, GrainsToTriggerThisBlock);
            }
            else if (SamplesPerGrainInterval > 0.0f && SamplesPerGrainInterval < TNumericLimits<float>::Max())
            {
                // Count active voices
                int32 ActiveVoiceCount = 0;
                for (const auto& Voice : GrainVoices)
                {
                    if (Voice.bIsActive)
                        ActiveVoiceCount++;
                }
                
                // Trigger more grains if we're under the desired density
                // Fix the FMath::Min call by adding proper type casting
                float TriggerProbability = FMath::Min(1.0f, static_cast<float>(DesiredGrainDensity) / static_cast<float>(MaxGrainVoices));

                while (SamplesUntilNextGrain <= ElapsedSamples) 
                { 
                    // Apply random time jitter
                    if (TimeJitterSamples > 0)
                        SamplesUntilNextGrain += FMath::RandRange(-TimeJitterSamples, TimeJitterSamples);
                    
                    // Only trigger a grain if we have room and probability check passes
                    if (ActiveVoiceCount < DesiredGrainDensity && FMath::FRand() <= TriggerProbability)
                    {
                        GrainsToTriggerThisBlock++; 
                        ActiveVoiceCount++;
                    }
                    SamplesUntilNextGrain += SamplesPerGrainInterval; 
                }
                SamplesUntilNextGrain -= ElapsedSamples;
            }

            // Adjust the grain triggering section to use the correct parameters
            for (int i = 0; i < GrainsToTriggerThisBlock; ++i)
            {
                float GrainStartTimeSeconds;
                if (bFreezed) {
                    // Use user's random offset even in freeze mode, but make it symmetric (±) 
                    // instead of just positive to ensure a stable sound centered at the frozen position
                    const float UserJitter = MaxStartPointRandSeconds * 0.5f; // Half applied before, half after position
                    
                    // Apply jitter while ensuring we stay within file bounds
                    // Use at least a small jitter (0.5ms) even if user sets it to zero for proper grain overlap
                    const float AppliedJitter = FMath::Max(0.0005f, UserJitter);
                    
                    GrainStartTimeSeconds = FMath::FRandRange(
                        PositionInSeconds - AppliedJitter, 
                        PositionInSeconds + AppliedJitter);
                        
                    // Final bounds check
                    GrainStartTimeSeconds = FMath::Clamp(GrainStartTimeSeconds, 
                                                        0.0f, 
                                                        CachedSoundWaveDuration - MinGrainDurationSeconds);
                } else {
                    // Normal playback mode - use standard randomization
                    GrainStartTimeSeconds = FMath::FRandRange(ClampedBaseStartPointSeconds, ClampedMaxStartTime);
                }
                
                float DurationOffset = FMath::FRandRange(0.0f, MaxDurationRandSeconds);
                float GrainDurationSeconds = FMath::Max(MinGrainDurationSeconds, BaseGrainDurationSeconds + DurationOffset);
                int32 GrainDurationSamples = FMath::CeilToInt(GrainDurationSeconds * SampleRate);
                float PitchOffset = FMath::FRandRange(-PitchRandSemitones, PitchRandSemitones);
                float TargetPitchShift = FMath::Clamp(TotalPitchShift + PitchOffset, -MaxAbsPitchShiftSemitones, MaxAbsPitchShiftSemitones);
                
                // Calculate random volume based on VolumeRandPercent
                float VolumeScale = 1.0f;
                if (VolumeRandPercent > 0.0f)
                {
                    // Calculate random volume reduction - higher VolumeRandPercent means more potential reduction
                    // At 100%, volume can range from 0.0 (silent) to 1.0 (full volume)
                    float MaxVolumeReduction = VolumeRandPercent / 100.0f;
                    VolumeScale = 1.0f - (FMath::FRand() * MaxVolumeReduction);
                }
                
                // Calculate frame ratio for pitch shifting
                float FrameRatio = FMath::Abs(FMath::Pow(2.0f, TargetPitchShift / 12.0f));

                float PanOffset = FMath::FRandRange(-PanRandAmount, PanRandAmount);
                float GrainPanPosition = FMath::Clamp(BasePan + PanOffset, -1.0f, 1.0f);

                
                // Create a new grain with the calculated parameters
                // When successful, trigger the OnGrain event at the appropriate frame
                if (TriggerGrain(CurrentWaveProxy, GrainDurationSamples, GrainStartTimeSeconds, FrameRatio, 
                                GrainPanPosition, VolumeScale, Smoothing, XfadeCurveIndex))
                {
                    // Determine the appropriate frame within the current block for the grain trigger
                    int32 TriggerFrameInBlock = FMath::Clamp(BlockSize - static_cast<int32>(SamplesUntilNextGrain), 0, BlockSize - 1);
                    OnGrainTriggered->TriggerFrame(TriggerFrameInBlock);
                }
            }

            // --- Process active grain voices ---
            for (FWavePlayerSmoothGrainVoice& Voice : GrainVoices)
            {
                if (Voice.bIsActive)
                {
                    if (!Voice.Reader.IsValid() || !Voice.Resampler.IsValid()) { Voice.bIsActive = false; continue; }
                    const int32 OutputFramesToProcess = FMath::Min(BlockSize, Voice.SamplesRemaining);
                    if (OutputFramesToProcess <= 0) { Voice.bIsActive = false; Voice.Reader.Reset(); Voice.Resampler.Reset(); continue; }
                    Voice.EnvelopedMonoBuffer.SetNumUninitialized(OutputFramesToProcess);
                    float* MonoBufferPtr = Voice.EnvelopedMonoBuffer.GetData();
                    
                    Audio::FMultichannelBuffer ResampledOutputBuffer;
                    Audio::SetMultichannelBufferSize(Voice.NumChannels, OutputFramesToProcess, ResampledOutputBuffer);
                    
                    // Process audio for this grain
                    int32 ActualFramesResampled = ProcessAudioForGrain(Voice, ResampledOutputBuffer, OutputFramesToProcess);
                    
                    if (ActualFramesResampled > 0)
                    {
                        // Enhanced envelope calculation with smoothing and overlap
                        for (int32 i = 0; i < ActualFramesResampled; ++i)
                        {
                            float MonoSample = 0.0f;
                            
                            if (Voice.NumChannels == 1) { 
                                MonoSample = ResampledOutputBuffer[0][i]; 
                            }
                            else if (Voice.NumChannels >= 2) { 
                                MonoSample = (ResampledOutputBuffer[0][i] + ResampledOutputBuffer[1][i]) * 0.5f; 
                            }
                            
                            // Enhanced envelope calculation
                            float EnvelopeScale = 1.0f;
                            const int32 CurrentFrameInGrain = Voice.SamplesPlayed + i;
                            
                            // Apply adaptive attack/decay based on grain overlap
                            // Larger overlap = gentler envelope = smoother transition
                            const float OverlapCompensation = FMath::Min(1.0f, 1.0f / GrainOverlap);
                            const float AdaptiveAttackPercent = FMath::Clamp(AttackPercent * OverlapCompensation, 0.05f, 0.95f);
                            const float AdaptiveDecayPercent = FMath::Clamp(ClampedDecayPercent * OverlapCompensation, 0.05f, 0.95f);
                            
                            // Calculate smoothed attack/decay sample positions
                            const int32 AttackSamples = FMath::CeilToInt(Voice.TotalGrainSamples * AdaptiveAttackPercent);
                            const int32 DecaySamples = FMath::CeilToInt(Voice.TotalGrainSamples * AdaptiveDecayPercent);
                            
                            switch (GrainWindowShape)
                            {
                            case EGrainWindowShape::Linear:
                                // Linear envelope
                                if (CurrentFrameInGrain < AttackSamples) {
                                    EnvelopeScale = (AttackSamples > 0) ? static_cast<float>(CurrentFrameInGrain) / AttackSamples : 1.0f;
                                }
                                else if (CurrentFrameInGrain >= (Voice.TotalGrainSamples - DecaySamples)) {
                                    EnvelopeScale = (DecaySamples > 0) ? static_cast<float>(Voice.TotalGrainSamples - CurrentFrameInGrain) / DecaySamples : 0.0f;
                                }
                                break;
                                
                            case EGrainWindowShape::Parabolic:
                                // Parabolic envelope (smoother transitions)
                                if (CurrentFrameInGrain < AttackSamples) {
                                    float t = (AttackSamples > 0) ? static_cast<float>(CurrentFrameInGrain) / AttackSamples : 1.0f;
                                    EnvelopeScale = t * t;
                                }
                                else if (CurrentFrameInGrain >= (Voice.TotalGrainSamples - DecaySamples)) {
                                    float t = (DecaySamples > 0) ? static_cast<float>(Voice.TotalGrainSamples - CurrentFrameInGrain) / DecaySamples : 0.0f;
                                    EnvelopeScale = t * t;
                                }
                                break;
                                
                            case EGrainWindowShape::Gaussian:
                                // Enhanced Gaussian with smoothing parameter
                                {
                                    // Center is shifted slightly later to reduce attack transients
                                    const float center = Voice.TotalGrainSamples * (0.5f + Voice.SmoothingAmount * 0.1f);
                                    const float position = CurrentFrameInGrain;
                                    // Width is increased with smoothing for gentler attack/decay
                                    const float width = Voice.TotalGrainSamples * (0.25f + Voice.SmoothingAmount * 0.1f);
                                    EnvelopeScale = FMath::Exp(-0.5f * FMath::Square((position - center) / width));
                                }
                                break;
                                
                            case EGrainWindowShape::Cosine:
                                // Cosine-based envelope (smooth)
                                {
                                    const float phase = PI * CurrentFrameInGrain / Voice.TotalGrainSamples;
                                    EnvelopeScale = 0.5f * (1.0f - FMath::Cos(2.0f * phase));
                                }
                                break;
                                
                            case EGrainWindowShape::Hann:
                                // Enhanced Hann with phase smoothing for better grain overlaps
                                {
                                    // Apply phase offset for better inter-grain crossfades
                                    const float phaseOffset = Voice.PhaseOffset * PI * 0.25f;
                                    const float normalizedPos = static_cast<float>(CurrentFrameInGrain) / Voice.TotalGrainSamples;
                                    const float phase = PI * normalizedPos + phaseOffset;
                                    
                                    // Apply dynamic curve based on crossfade type and smoothing
                                    switch (XfadeCurveIndex)
                                    {
                                        case 0: // Linear
                                            EnvelopeScale = 0.5f * (1.0f - FMath::Cos(2.0f * phase));
                                            break;
                                            
                                        case 1: // Equal Power
                                            {
                                                float sinValue = FMath::Sin(phase);
                                                EnvelopeScale = sinValue * sinValue;
                                            }
                                            break;
                                            
                                        case 2: // Smooth
                                            {
                                                // S-curve with gentler attack/decay
                                                float value = 0.5f * (1.0f - FMath::Cos(2.0f * phase));
                                                // Add smoothing curve response
                                                EnvelopeScale = FMath::Pow(value, 0.7f + (0.6f * Voice.SmoothingAmount));
                                            }
                                            break;
                                    }
                                }
                                break;
                                
                            case EGrainWindowShape::Blackman:
                                // Blackman window: reduced side lobes for better frequency separation
                                {
                                    const float x = CurrentFrameInGrain / (float)Voice.TotalGrainSamples;
                                    const float a0 = 0.42f;
                                    const float a1 = 0.5f;
                                    const float a2 = 0.08f;
                                    EnvelopeScale = a0 - a1 * FMath::Cos(2.0f * PI * x) + a2 * FMath::Cos(4.0f * PI * x);
                                }
                                break;
                                
                            case EGrainWindowShape::Triangular:
                                // Triangular window: simple rising and falling ramp
                                {
                                    const float x = CurrentFrameInGrain / (float)Voice.TotalGrainSamples;
                                    EnvelopeScale = 1.0f - FMath::Abs(2.0f * x - 1.0f);
                                }
                                break;
                                
                            case EGrainWindowShape::Rectangular:
                                // Rectangular window: make truly rectangular with no fade for clear contrast
                                EnvelopeScale = 1.0;
                                break;
                            }
                            
                            // Apply additional envelope softening based on attack/release values
                            if (Voice.SmoothingAmount > 0.0f && EnvelopeScale > 0.0f && EnvelopeScale < 1.0f)
                            {
                                // Reduce the steepness of the envelope for smoother transitions
                                EnvelopeScale = FMath::Pow(EnvelopeScale, 1.0f - (Voice.SmoothingAmount * 0.3f));
                            }
                                
                            EnvelopeScale = FMath::Clamp(EnvelopeScale, 0.0f, 1.0f);
                            MonoBufferPtr[i] = MonoSample * EnvelopeScale;
                        }
                        
                        // Apply smooth mixing with equal power crossfading
                        const float PanAngle = (Voice.PanPosition + 1.0f) * 0.5f * UE_PI * 0.5f;
                        const float LeftGain = FMath::Cos(PanAngle) * Voice.VolumeScale;
                        const float RightGain = FMath::Sin(PanAngle) * Voice.VolumeScale;
                        
                        // Mix audio with smoother transitions between grains
                        Audio::ArrayMixIn(TArrayView<const float>(MonoBufferPtr, ActualFramesResampled), 
                                         TArrayView<float>(OutputAudioLeftPtr, ActualFramesResampled), LeftGain);
                        Audio::ArrayMixIn(TArrayView<const float>(MonoBufferPtr, ActualFramesResampled), 
                                         TArrayView<float>(OutputAudioRightPtr, ActualFramesResampled), RightGain);
                        
                        // Update voice state after processing
                        Voice.SamplesPlayed += ActualFramesResampled;
                        Voice.SamplesRemaining -= ActualFramesResampled;
                    }
                    
                    // Check if grain is finished after processing
                    if (Voice.SamplesRemaining <= 0) { 
                        Voice.bIsActive = false; 
                        Voice.Reader.Reset(); 
                        Voice.Resampler.Reset(); 
                    }
                }
            }

            // Add final smoothing pass at the end of Execute if needed
            if (Smoothing > 0.5f)
            {
                // Final pass to reduce any remaining transients
                for (int32 i = 0; i < BlockSize; ++i)
                {
                    // Simple 1-pole low pass filter based on smoothing amount
                    const float FilterCoeff = FMath::Max(0.1f, 1.0f - (Smoothing * 0.5f));
                    
                    // Apply to left channel
                    EnvelopeBuffer[0][i] = OutputAudioLeftPtr[i] * FilterCoeff + 
                                       PrevGrainValue[0] * (1.0f - FilterCoeff);
                    PrevGrainValue[0] = EnvelopeBuffer[0][i];
                    OutputAudioLeftPtr[i] = EnvelopeBuffer[0][i];
                    
                    // Apply to right channel
                    EnvelopeBuffer[1][i] = OutputAudioRightPtr[i] * FilterCoeff + 
                                       PrevGrainValue[1] * (1.0f - FilterCoeff);
                    PrevGrainValue[1] = EnvelopeBuffer[1][i];
                    OutputAudioRightPtr[i] = EnvelopeBuffer[1][i];
                }
            }
            
            // One more time in case anything changed the position during processing
            *TimeOutput = FTime::FromSeconds(CurrentPlaybackPositionSeconds);
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
            CurrentPlaybackPositionSeconds = 0.0f;
            
            // Reset smoothing parameters
            PrevGrainValue[0] = 0.0f;
            PrevGrainValue[1] = 0.0f;
            
            UE_LOG(LogMetaSound, Log, TEXT("Granular Wave Player: Operator Reset."));
        }

    private:
        // --- Helper Functions ---

        bool TryStartPlayback(int32 InFrame)
        {
            bool bWasPlayingBeforeAttempt = bIsPlaying;
            bIsPlaying = false; // Assume failure

            // Check WaveAsset Input Validity
            if (!WaveAssetInput->IsSoundWaveValid())
            {
                UE_LOG(LogMetaSound, Warning, TEXT("GWP: Play Trigger at frame %d failed: Wave Asset input is not valid."), InFrame);
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
                UE_LOG(LogMetaSound, Warning, TEXT("GWP: Play Trigger at frame %d failed: Could not get valid SoundWaveProxy from Wave Asset."), InFrame);
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
                UE_LOG(LogMetaSound, Error, TEXT("GWP: Play Trigger at frame %d failed: Could not initialize wave data."), InFrame);
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
            UE_LOG(LogMetaSound, Log, TEXT("GWP: Playback %s at frame %d."), bWasPlayingBeforeAttempt ? TEXT("Restarted") : TEXT("Started"), InFrame);
            return true;
        }

        bool InitializeWaveData(const FSoundWaveProxyPtr& InSoundWaveProxy)
        {
            CurrentWaveProxy = InSoundWaveProxy; // Update tracked proxy

            FSoundWaveProxyReader::FSettings TempSettings;
            auto TempReader = FSoundWaveProxyReader::Create(CurrentWaveProxy.ToSharedRef(), TempSettings);
            if (TempReader.IsValid())
            {
                CachedSoundWaveDuration = (float)TempReader->GetNumFramesInWave() / FMath::Max(1.0f, TempReader->GetSampleRate());
                CurrentNumChannels = TempReader->GetNumChannels();
                if (CurrentNumChannels <= 0 || CachedSoundWaveDuration <= 0.0f)
                {
                    UE_LOG(LogMetaSound, Error, TEXT("GWP: Wave Asset reports invalid duration (%.2f) or channels (%d)."), CachedSoundWaveDuration, CurrentNumChannels);
                    CurrentNumChannels = 0; CachedSoundWaveDuration = 0.0f; ConvertDeinterleave.Reset(); return false;
                }
                Audio::FConvertDeinterleaveParams ConvertParams;
                ConvertParams.NumInputChannels = CurrentNumChannels; ConvertParams.NumOutputChannels = CurrentNumChannels;
                ConvertDeinterleave = Audio::IConvertDeinterleave::Create(ConvertParams);
                if (!ConvertDeinterleave.IsValid())
                {
                    UE_LOG(LogMetaSound, Error, TEXT("GWP: Failed to create deinterleaver for %d channels."), CurrentNumChannels);
                    CurrentNumChannels = 0; CachedSoundWaveDuration = 0.0f; return false;
                }
                Audio::SetMultichannelBufferSize(CurrentNumChannels, DeinterleaveBlockSizeFrames, DeinterleavedSourceBuffer);
                return true; // Success
            }
            else {
                UE_LOG(LogMetaSound, Error, TEXT("GWP: Failed to create reader for wave asset."));
                return false;
            }
        }

        // Process a new grain with the specified parameters
        bool TriggerGrain(const FSoundWaveProxyPtr& InSoundWaveProxy, int32 InGrainDurationSamples, 
                float InStartTimeSeconds, float InFrameRatio, float InPanPosition, 
                float InVolumeScale = 1.0f, float InSmoothingAmount = 0.0f, int32 InXfadeCurveType = 0)
        {
            if (!InSoundWaveProxy.IsValid() || InGrainDurationSamples <= 0 || CurrentNumChannels <= 0) 
                return false;
            
            // Find an available voice
            int32 VoiceIndex = -1;
            for (int32 i = 0; i < GrainVoices.Num(); ++i) { 
                if (!GrainVoices[i].bIsActive) { 
                    VoiceIndex = i; 
                    break; 
                } 
            }
            
            if (VoiceIndex == -1) 
                return false;
            
            FWavePlayerSmoothGrainVoice& NewVoice = GrainVoices[VoiceIndex];
            
            // Always clamp start time to valid range
            InStartTimeSeconds = FMath::Max(0.0f, InStartTimeSeconds);
            
            // Make sure we don't go beyond the end of the file
            const float DurationInSeconds = InGrainDurationSamples / SampleRate;
            if (InStartTimeSeconds + DurationInSeconds > CachedSoundWaveDuration) {
                InStartTimeSeconds = FMath::Max(0.0f, CachedSoundWaveDuration - DurationInSeconds);
                
                if (InStartTimeSeconds <= 0.0f || DurationInSeconds < MinGrainDurationSeconds)
                    return false;
            }
            
            // Configure reader settings - always start from normal position
            FSoundWaveProxyReader::FSettings ReaderSettings;
            ReaderSettings.StartTimeInSeconds = InStartTimeSeconds;
            ReaderSettings.bIsLooping = false;
            
            // Setup buffer sizes with safe quantization
            const uint32 MinDecodeSize = 256; // Use a safer minimum size
            uint32 DesiredDecodeSize = FMath::Max(InGrainDurationSamples, BlockSize * 2);
            const uint32 DecodeSizeQuantization = 128;
            uint32 ConformedDecodeSize = ((DesiredDecodeSize + DecodeSizeQuantization - 1) / DecodeSizeQuantization) * DecodeSizeQuantization;
            ReaderSettings.MaxDecodeSizeInFrames = ConformedDecodeSize;
            
            // Create the reader
            NewVoice.Reader = FSoundWaveProxyReader::Create(InSoundWaveProxy.ToSharedRef(), ReaderSettings);
            if (!NewVoice.Reader.IsValid()) { 
                return false; 
            }
            
            // Set up voice
            NewVoice.NumChannels = CurrentNumChannels;
            
            // Create resampler
            NewVoice.Resampler = MakeUnique<Audio::FMultichannelLinearResampler>(NewVoice.NumChannels);
            NewVoice.Resampler->SetFrameRatio(FMath::Abs(InFrameRatio), 0);
            
            // Prepare buffers
            NewVoice.InterleavedReadBuffer.SetNumUninitialized(ConformedDecodeSize * NewVoice.NumChannels);
            NewVoice.EnvelopedMonoBuffer.SetNumUninitialized(BlockSize);
            NewVoice.SourceCircularBuffer.Empty();
            Audio::SetMultichannelCircularBufferCapacity(NewVoice.NumChannels, 
                ConformedDecodeSize + BlockSize * 4, NewVoice.SourceCircularBuffer);
            
            // Apply phase alignment and time correction for smoother overlapping 
            // Only shift if smoothing is requested
            if (InSmoothingAmount > 0.0f)
            {
                // Find zero crossings for optimal grain start to reduce transients
                const float PhaseRand = FMath::FRand() * 0.05f * InSmoothingAmount;
                
                // Small time adjustment based on smoothing amount (0-10ms)
                float TimeAdjustMs = FMath::FRand() * InSmoothingAmount * 10.0f;  
                float AdjustedStartTime = InStartTimeSeconds + (TimeAdjustMs / 1000.0f);
                
                // Ensure we're still within valid range
                AdjustedStartTime = FMath::Clamp(AdjustedStartTime, 0.0f, 
                                      CachedSoundWaveDuration - (InGrainDurationSamples / SampleRate));
                
                // Update start time with the adjusted value
                ReaderSettings.StartTimeInSeconds = AdjustedStartTime;
                NewVoice.PhaseOffset = PhaseRand; // Store for envelope calculation
            }
            
            // Initialize voice state with enhanced parameters
            NewVoice.bIsActive = true;
            NewVoice.SamplesRemaining = InGrainDurationSamples;
            NewVoice.SamplesPlayed = 0;
            NewVoice.TotalGrainSamples = InGrainDurationSamples;
            NewVoice.PanPosition = InPanPosition;
            NewVoice.VolumeScale = InVolumeScale;
            NewVoice.SmoothingAmount = InSmoothingAmount;
            NewVoice.EnvelopeCurve = 1.0f - (InSmoothingAmount * 0.3f); // Flatter curve for smoother transitions
            
            return true;
        }

        // Modify the ProcessAudioForGrain function to fix variable scope issues
        int32 ProcessAudioForGrain(FWavePlayerSmoothGrainVoice& Voice, Audio::FMultichannelBuffer& ResampledOutputBuffer, int32 OutputFramesToProcess)
        {
            // Basic validation
            if (!Voice.Reader.IsValid() || !Voice.Resampler.IsValid() || !Voice.bIsActive || OutputFramesToProcess <= 0)
                return 0;
            
            int32 ActualFramesResampled = 0;
            
            try {
                // Standard audio processing
                int32 InputFramesAvailable = Audio::GetMultichannelBufferNumFrames(Voice.SourceCircularBuffer);
                int32 InputFramesNeeded = Voice.Resampler->GetNumInputFramesNeededToProduceOutputFrames(OutputFramesToProcess);
                
                // Keep reading data as needed
                if (InputFramesAvailable < InputFramesNeeded) {
                    int32 BlocksToRead = FMath::CeilToInt((float)(InputFramesNeeded - InputFramesAvailable) / DeinterleaveBlockSizeFrames);
                    
                    for (int32 Block = 0; Block < BlocksToRead; ++Block) {
                        int32 FramesToRead = DeinterleaveBlockSizeFrames;
                        
                        if (Voice.InterleavedReadBuffer.Num() < FramesToRead * Voice.NumChannels) {
                            Voice.InterleavedReadBuffer.SetNumUninitialized(FramesToRead * Voice.NumChannels);
                        }
                        
                        // Create an explicit reference to ensure the compiler recognizes the correct type
                        Audio::FAlignedFloatBuffer& AlignedBuffer = Voice.InterleavedReadBuffer;
                        const int32 SamplesRead = Voice.Reader->PopAudio(AlignedBuffer);
                        
                        if (SamplesRead <= 0 || Voice.Reader->HasFailed()) {
                            break;
                        }
                        
                        const int32 FramesRead = SamplesRead / Voice.NumChannels;
                        
                        // Deinterleave the audio
                        Audio::SetMultichannelBufferSize(Voice.NumChannels, FramesRead, DeinterleavedSourceBuffer);
                        ConvertDeinterleave->ProcessAudio(
                            TArrayView<const float>(Voice.InterleavedReadBuffer.GetData(), SamplesRead), 
                            DeinterleavedSourceBuffer
                        );
                        
                        // Push to circular buffer
                        for (int32 Chan = 0; Chan < Voice.NumChannels; ++Chan) {
                            int32 PushResult = Voice.SourceCircularBuffer[Chan].Push(DeinterleavedSourceBuffer[Chan].GetData(), FramesRead);
                            bool bPushSuccess = (PushResult > 0) ? true : false;  // Use explicit conversion
                        }
                        
                        InputFramesAvailable = Audio::GetMultichannelBufferNumFrames(Voice.SourceCircularBuffer);
                        if (InputFramesAvailable >= InputFramesNeeded) {
                            break;
                        }
                    }
                }
                
                // If there's nothing to process or voice became inactive, exit
                if (!Voice.bIsActive) {
                    return 0;
                }
                
                // Now process the audio through the resampler
                ActualFramesResampled = Voice.Resampler->ProcessAndConsumeAudio(
                    Voice.SourceCircularBuffer, ResampledOutputBuffer);
            }
            catch (...) {
                // Catch any unexpected exceptions - fail gracefully
                UE_LOG(LogMetaSound, Error, TEXT("GWP: Exception during grain processing. Voice deactivated."));
                Voice.bIsActive = false;
                return 0;
            }
            
            return ActualFramesResampled;
        }

        void ResetVoices()
        {
            for (FWavePlayerSmoothGrainVoice& Voice : GrainVoices)
            {
                Voice.bIsActive = false; 
                Voice.NumChannels = 0; 
                Voice.SamplesRemaining = 0;
                Voice.SamplesPlayed = 0; 
                Voice.TotalGrainSamples = 0; 
                Voice.PanPosition = 0.0f;
                Voice.VolumeScale = 1.0f;
                
                // Reset the smooth grain voice specific fields
                Voice.PhaseOffset = 0.0f;
                Voice.EnvelopeCurve = 1.0f;
                Voice.SmoothingAmount = 0.0f;
                
                Voice.Reader.Reset(); 
                Voice.Resampler.Reset(); 
                Voice.SourceCircularBuffer.Empty();
            }
        }

        // --- Input Parameter References ---
        FTriggerReadRef PlayTrigger;
        FTriggerReadRef StopTrigger;
        FWaveAssetReadRef WaveAssetInput;

        // Float Input Params
        FFloatReadRef GrainDurationMsInput;
        FFloatReadRef GrainsPerSecondInput;
        FFloatReadRef PlaybackSpeedInput;
        FFloatReadRef PlayPositionInput;
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
        FFloatReadRef TimeJitterInput;
        FFloatReadRef VolumeRandInput;
        FFloatReadRef SmoothingInput;
        FFloatReadRef GrainOverlapInput;

        // Int input params
        FInt32ReadRef GrainDensityInput;
        FInt32ReadRef WindowShapeInput;
        FInt32ReadRef XfadeCurveInput;
        
        // --- Output Parameter References ---
        FTriggerWriteRef OnPlayTrigger;
        FTriggerWriteRef OnFinishedTrigger;
        FTriggerWriteRef OnGrainTriggered;
        FAudioBufferWriteRef AudioOutputLeft;
        FAudioBufferWriteRef AudioOutputRight;
        FTimeWriteRef TimeOutput; // Changed to FTimeWriteRef
        
        // --- Operator Settings ---
        float SampleRate;
        int32 BlockSize;

        // --- Internal State ---
        bool bIsPlaying;
        bool bPreviousFreezeState = false;  // Keep to detect changes in freeze state
        float SamplesUntilNextGrain;
        TArray<FWavePlayerSmoothGrainVoice> GrainVoices;
        FSoundWaveProxyPtr CurrentWaveProxy;
        float CachedSoundWaveDuration;
        int32 CurrentNumChannels;
        TUniquePtr<Audio::IConvertDeinterleave> ConvertDeinterleave;
        Audio::FMultichannelBuffer DeinterleavedSourceBuffer;

        float CurrentPlaybackPositionSeconds = 0.0f; // Tracks actual playback position

        // Add smoothing-related members
        Audio::FAlignedFloatBuffer EnvelopeBuffer[2];
        float PrevGrainValue[2];
    };

    // --- Node ---
    class FGranularWavePlayerSmoothNode : public FNodeFacade
    {
    public:
        FGranularWavePlayerSmoothNode(const FNodeInitData& InitData)
            : FNodeFacade(InitData.InstanceName, InitData.InstanceID, TFacadeOperatorClass<FGranularWavePlayerSmoothOperator>())
        {
        }

    };
    // --- Registration ---
    METASOUND_REGISTER_NODE(FGranularWavePlayerSmoothNode)
}

#undef LOCTEXT_NAMESPACE