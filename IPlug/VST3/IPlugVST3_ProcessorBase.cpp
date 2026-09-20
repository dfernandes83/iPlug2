/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
 */

#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "public.sdk/source/vst/vsteventshelper.h"
#include "IPlugVST3_ProcessorBase.h"

using namespace iplug;
using namespace Steinberg;
using namespace Vst;

#ifndef CUSTOM_BUSTYPE_FUNC
uint64_t iplug::GetAPIBusTypeForChannelIOConfig(int configIdx, ERoute dir, int busIdx, const IOConfig* pConfig, WDL_TypedBuf<uint64_t>* APIBusTypes)
{
  assert(pConfig != nullptr);
  assert(busIdx >= 0 && busIdx < pConfig->NBuses(dir));
  
  int numChans = pConfig->GetBusInfo(dir, busIdx)->NChans();
  
  switch (numChans)
  {
    case 0: return SpeakerArr::kEmpty;
    case 1: return SpeakerArr::kMono;
    case 2: return SpeakerArr::kStereo;
    case 3: return SpeakerArr::k30Cine; // CHECK - not the same as protools
    case 4: return SpeakerArr::kAmbi1stOrderACN;
    case 5: return SpeakerArr::k50;
    case 6: return SpeakerArr::k51;
    case 7: return SpeakerArr::k70Cine;
    case 8: return SpeakerArr::k71CineSideFill; // CHECK - not the same as protools
    case 9: return SpeakerArr::kAmbi2cdOrderACN;
    case 10:return SpeakerArr::k71_2; // aka k91Atmos
    case 16:return SpeakerArr::kAmbi3rdOrderACN;
    default:
      DBGMSG("do not yet know what to do here\n");
      assert(0);
      return SpeakerArr::kEmpty;
  }
}
#endif

IPlugVST3ProcessorBase::IPlugVST3ProcessorBase(Config c, IPlugAPIBase& plug)
: IPlugProcessor(c, kAPIVST3)
, mPlug(plug)
{
  SetChannelConnections(ERoute::kInput, 0, MaxNChannels(ERoute::kInput), true);
  SetChannelConnections(ERoute::kOutput, 0, MaxNChannels(ERoute::kOutput), true);
  
  mMaxNChansForMainInputBus = MaxNChannelsForBus(ERoute::kInput, 0);

  InitLatencyDelay();

  IPlugProcessor::SetBlockSize(DEFAULT_BLOCK_SIZE);
  
  // Make sure the process context is predictably initialised in case it is used before process is called
  memset(&mProcessContext, 0, sizeof(ProcessContext));

#if IPLUG_VST3_SAMPLE_ACCURATE_PARAMS
  mParamPoints.reserve(IPLUG_VST3_MAX_PARAM_POINTS); // once, here: the audio thread must never grow it
#endif
}

void IPlugVST3ProcessorBase::ProcessMidiIn(IEventList* pEventList, IPlugQueue<IMidiMsg>& editorQueue, IPlugQueue<IMidiMsg>& processorQueue)
{
  IMidiMsg msg;
    
  if (pEventList)
  {
    int32 numEvent = pEventList->getEventCount();
    for (int32 i=0; i<numEvent; i++)
    {
      Event event;
      if (pEventList->getEvent(i, event) == kResultOk)
      {
        switch (event.type)
        {
          case Event::kNoteOnEvent:
          {
            msg.MakeNoteOnMsg(event.noteOn.pitch, event.noteOn.velocity * 127, event.sampleOffset, event.noteOn.channel);
            ProcessMidiMsg(msg);
            processorQueue.Push(msg);
            break;
          }
            
          case Event::kNoteOffEvent:
          {
            msg.MakeNoteOffMsg(event.noteOff.pitch, event.sampleOffset, event.noteOff.channel);
            ProcessMidiMsg(msg);
            processorQueue.Push(msg);
            break;
          }
          case Event::kPolyPressureEvent:
          {
            msg.MakePolyATMsg(event.polyPressure.pitch, event.polyPressure.pressure * 127., event.sampleOffset, event.polyPressure.channel);
            ProcessMidiMsg(msg);
            processorQueue.Push(msg);
            break;
          }
          case Event::kDataEvent:
          {
            ISysEx syx = ISysEx(event.sampleOffset, event.data.bytes, event.data.size);
            ProcessSysEx(syx);
            break;
          }
        }
      }
    }
  }
  
  while (editorQueue.Pop(msg))
  {
    ProcessMidiMsg(msg);
  }
}

void IPlugVST3ProcessorBase::ProcessMidiOut(IPlugQueue<SysExData>& sysExQueue, SysExData& sysExBuf, IEventList* pOutputEvents, int32 numSamples)
{
  if (!mMidiOutputQueue.Empty() && pOutputEvents)
  {
    Event toAdd = {0};
    IMidiMsg msg;
    
    while (!mMidiOutputQueue.Empty())
    {
      IMidiMsg& msg = mMidiOutputQueue.Peek();

      if (msg.StatusMsg() == IMidiMsg::kNoteOn)
      {
        Helpers::init(toAdd, Event::kNoteOnEvent, 0 /*bus id*/, msg.mOffset);

        toAdd.noteOn.channel = msg.Channel();
        toAdd.noteOn.pitch = msg.NoteNumber();
        toAdd.noteOn.tuning = 0.;
        toAdd.noteOn.velocity = (float) msg.Velocity() * (1.f / 127.f);
        pOutputEvents->addEvent(toAdd);
      }
      else if (msg.StatusMsg() == IMidiMsg::kNoteOff)
      {
        Helpers::init(toAdd, Event::kNoteOffEvent, 0 /*bus id*/, msg.mOffset);

        toAdd.noteOff.channel = msg.Channel();
        toAdd.noteOff.pitch = msg.NoteNumber();
        toAdd.noteOff.velocity = (float) msg.Velocity() * (1.f / 127.f);
        pOutputEvents->addEvent(toAdd);
      }
      else if (msg.StatusMsg() == IMidiMsg::kPolyAftertouch)
      { 
        Helpers::initLegacyMIDICCOutEvent(toAdd, ControllerNumbers::kCtrlPolyPressure, msg.Channel(), msg.mData1, msg.mData2);
        toAdd.sampleOffset = msg.mOffset;
        pOutputEvents->addEvent(toAdd);
      }
      else if (msg.StatusMsg() == IMidiMsg::kChannelAftertouch)
      {
        Helpers::initLegacyMIDICCOutEvent(toAdd, ControllerNumbers::kAfterTouch, msg.Channel(), msg.mData1, msg.mData2);
        toAdd.sampleOffset = msg.mOffset;
        pOutputEvents->addEvent(toAdd);
      }
      else if (msg.StatusMsg() == IMidiMsg::kProgramChange)
      {
        Helpers::initLegacyMIDICCOutEvent(toAdd, ControllerNumbers::kCtrlProgramChange, msg.Channel(), msg.Program(), 0);
        toAdd.sampleOffset = msg.mOffset;
        pOutputEvents->addEvent(toAdd);
      }
      else if (msg.StatusMsg() == IMidiMsg::kControlChange)
      {
        Helpers::initLegacyMIDICCOutEvent(toAdd, msg.mData1, msg.Channel(), msg.mData2, 0 /* value2?*/);
        toAdd.sampleOffset = msg.mOffset;
        pOutputEvents->addEvent(toAdd);
      }
      else if (msg.StatusMsg() == IMidiMsg::kPitchWheel)
      {
        toAdd.type = Event::kLegacyMIDICCOutEvent;
        toAdd.midiCCOut.channel = msg.Channel();
        toAdd.sampleOffset = msg.mOffset;
        toAdd.midiCCOut.controlNumber = ControllerNumbers::kPitchBend;
        int16 tmp = static_cast<int16> (msg.PitchWheel() * 0x3FFF);
        toAdd.midiCCOut.value = (tmp & 0x7F);
        toAdd.midiCCOut.value2 = ((tmp >> 7) & 0x7F);
        pOutputEvents->addEvent(toAdd);
      }

      mMidiOutputQueue.Remove();
    }
  }
  
  mMidiOutputQueue.Flush(numSamples);
  
  // Output SYSEX from the editor, which has bypassed the processors' ProcessSysEx()
  if (sysExQueue.ElementsAvailable())
  {
    Event toAdd = {0};
    
    while (sysExQueue.Pop(sysExBuf))
    {
      toAdd.type = Event::kDataEvent;
      toAdd.sampleOffset = sysExBuf.mOffset;
      toAdd.data.type = DataEvent::kMidiSysEx;
      toAdd.data.size = sysExBuf.mSize;
      toAdd.data.bytes = (uint8*) sysExBuf.mData; // TODO!  this is a problem if more than one message in this block!
      pOutputEvents->addEvent(toAdd);
    }
  }
}

void IPlugVST3ProcessorBase::AttachBuffers(ERoute direction, int idx, int n, AudioBusBuffers& pBus, int nFrames, int32 sampleSize, int offset)
{
  if (offset == 0)
  {
    if (sampleSize == kSample32)
      IPlugProcessor::AttachBuffers(direction, idx, n, pBus.channelBuffers32, nFrames);
    else if (sampleSize == kSample64)
      IPlugProcessor::AttachBuffers(direction, idx, n, pBus.channelBuffers64, nFrames);

    return;
  }

  // A sub-block: hand over per-channel pointers shifted by offset. The array has a fixed size so nothing is
  // allocated on the audio thread; ProcessSampleAccurate() never splits a block whose buses are wider than this.
  constexpr int kMaxShiftedChannels = 64;
  assert(n <= kMaxShiftedChannels);
  const int nChans = std::min(n, kMaxShiftedChannels);

  if (sampleSize == kSample32)
  {
    Sample32* shifted[kMaxShiftedChannels];

    for (int c = 0; c < nChans; ++c)
      shifted[c] = pBus.channelBuffers32[c] + offset;

    IPlugProcessor::AttachBuffers(direction, idx, nChans, shifted, nFrames);
  }
  else if (sampleSize == kSample64)
  {
    Sample64* shifted[kMaxShiftedChannels];

    for (int c = 0; c < nChans; ++c)
      shifted[c] = pBus.channelBuffers64[c] + offset;

    IPlugProcessor::AttachBuffers(direction, idx, nChans, shifted, nFrames);
  }
}

bool IPlugVST3ProcessorBase::SetupProcessing(const ProcessSetup& setup, ProcessSetup& storedSetup)
{
  if ((setup.symbolicSampleSize != kSample32) && (setup.symbolicSampleSize != kSample64))
    return false;
  
  storedSetup = setup;
  
  SetSampleRate(setup.sampleRate);
  IPlugProcessor::SetBlockSize(setup.maxSamplesPerBlock);
  mMidiOutputQueue.Resize(setup.maxSamplesPerBlock);
  OnReset();
    
  return true;
}

bool IPlugVST3ProcessorBase::SetProcessing(bool state)
{
  if (!state)
    OnReset();
  
  return true;
}

bool IPlugVST3ProcessorBase::CanProcessSampleSize(int32 symbolicSampleSize)
{
  switch (symbolicSampleSize)
  {
    case kSample32:   // fall through
    case kSample64:   return true;
    default:          return false;
  }
}

void IPlugVST3ProcessorBase::PrepareProcessContext(ProcessData& data, ProcessSetup& setup)
{
  ITimeInfo timeInfo;
  
  if (data.processContext)
    memcpy(&mProcessContext, data.processContext, sizeof(ProcessContext));
  
  if (mProcessContext.state & ProcessContext::kProjectTimeMusicValid)
    timeInfo.mSamplePos = (double) mProcessContext.projectTimeSamples;
  timeInfo.mPPQPos = mProcessContext.projectTimeMusic;
  timeInfo.mTempo = mProcessContext.tempo;
  timeInfo.mLastBar = mProcessContext.barPositionMusic;
  timeInfo.mCycleStart = mProcessContext.cycleStartMusic;
  timeInfo.mCycleEnd = mProcessContext.cycleEndMusic;
  timeInfo.mNumerator = mProcessContext.timeSigNumerator;
  timeInfo.mDenominator = mProcessContext.timeSigDenominator;
  timeInfo.mTransportIsRunning = mProcessContext.state & ProcessContext::kPlaying;
  timeInfo.mTransportLoopEnabled = mProcessContext.state & ProcessContext::kCycleActive;
  const bool offline = setup.processMode == Steinberg::Vst::kOffline;
  SetTimeInfo(timeInfo);
  SetRenderingOffline(offline);
}

void IPlugVST3ProcessorBase::ApplyParameterChange(int idx, double value, int offsetSamples, IPlugQueue<IMidiMsg>& fromProcessor)
{
  switch (idx)
  {
    case kBypassParam:
    {
      const bool bypassed = (value > 0.5);

      if (bypassed != GetBypassed())
        SetBypassed(bypassed);

      break;
    }
    default:
    {
      if (idx >= 0 && idx < mPlug.NParams())
      {
#ifdef PARAMS_MUTEX
        mPlug.mParams_mutex.Enter();
#endif
        mPlug.GetParam(idx)->SetNormalized(value);

        // In VST3 non distributed the same parameter value is also set via IPlugVST3Controller::setParamNormalized(ParamID tag, ParamValue value)
        mPlug.OnParamChange(idx, kHost, offsetSamples);
#ifdef PARAMS_MUTEX
        mPlug.mParams_mutex.Leave();
#endif
      }
      else if (idx >= kMIDICCParamStartIdx)
      {
        int index = idx - kMIDICCParamStartIdx;
        int channel = index / kCountCtrlNumber;
        int ctrlr = index % kCountCtrlNumber;

        IMidiMsg msg;

        if (ctrlr == kAfterTouch)
          msg.MakeChannelATMsg((int) (value * 127.), offsetSamples, channel);
        else if (ctrlr == kPitchBend)
          msg.MakePitchWheelMsg((value * 2.)-1., channel, offsetSamples);
        else
          msg.MakeControlChangeMsg((IMidiMsg::EControlChangeMsg) ctrlr, value, channel, offsetSamples);

        fromProcessor.Push(msg);
        ProcessMidiMsg(msg);
      }
    }
      break;
  }
}

void IPlugVST3ProcessorBase::ProcessParameterChanges(ProcessData& data, IPlugQueue<IMidiMsg>& fromProcessor)
{
  IParameterChanges* paramChanges = data.inputParameterChanges;

  if (paramChanges)
  {
    int32 numParamsChanged = paramChanges->getParameterCount();

    for (int32 i = 0; i < numParamsChanged; i++)
    {
      IParamValueQueue* paramQueue = paramChanges->getParameterData(i);
      if (paramQueue)
      {
        int32 numPoints = paramQueue->getPointCount();
        int32 offsetSamples;
        double value;

        // Default behaviour: only the last point of each queue is applied, at the start of the block.
        // See IPLUG_VST3_SAMPLE_ACCURATE_PARAMS / ProcessSampleAccurate() for the sample-accurate alternative.
        if (paramQueue->getPoint(numPoints - 1,  offsetSamples, value) == kResultTrue)
          ApplyParameterChange(static_cast<int>(paramQueue->getParameterId()), value, offsetSamples, fromProcessor);
      }
    }
  }
}

void IPlugVST3ProcessorBase::ProcessAudio(ProcessData& data, ProcessSetup& setup, const BusList& ins, const BusList& outs, int offset, int nFrames)
{
  int32 sampleSize = setup.symbolicSampleSize;
  const int frames = nFrames >= 0 ? nFrames : data.numSamples;
    
  if (sampleSize == kSample32 || sampleSize == kSample64)
  {
    if (data.numInputs)
    {
      SetChannelConnections(ERoute::kInput, 0, MaxNChannels(ERoute::kInput), false);

      if (ins.size() > 1)
      {
        if (ins[1].get()->isActive()) // Sidechain is active
        {
          mSidechainActive = true;
          SetChannelConnections(ERoute::kInput, 0, data.inputs[0].numChannels, true);
          SetChannelConnections(ERoute::kInput, mMaxNChansForMainInputBus, data.inputs[1].numChannels, true);
        }
        else
        {
          if (mSidechainActive)
          {
            ZeroScratchBuffers();
            mSidechainActive = false;
          }
          
          SetChannelConnections(ERoute::kInput, 0, data.inputs[0].numChannels, true);
        }
        
        AttachBuffers(ERoute::kInput, 0, data.inputs[0].numChannels, data.inputs[0], frames, sampleSize, offset);
        
        if(mSidechainActive)
          AttachBuffers(ERoute::kInput, mMaxNChansForMainInputBus, data.inputs[1].numChannels, data.inputs[1], frames, sampleSize, offset);
      }
      else
      {
        SetChannelConnections(ERoute::kInput, 0, MaxNChannels(ERoute::kInput), false);
        SetChannelConnections(ERoute::kInput, 0, data.inputs[0].numChannels, true);
        AttachBuffers(ERoute::kInput, 0, data.inputs[0].numChannels, data.inputs[0], frames, sampleSize, offset);
      }
    }
    
    for (int outBus = 0, chanOffset = 0; outBus < data.numOutputs; outBus++)
    {
      int busChannels = data.outputs[outBus].numChannels;
      SetChannelConnections(ERoute::kOutput, chanOffset, busChannels, outs[outBus].get()->isActive());
      SetChannelConnections(ERoute::kOutput, chanOffset + busChannels, MaxNChannels(ERoute::kOutput) - (chanOffset + busChannels), false);
      AttachBuffers(ERoute::kOutput, chanOffset, busChannels, data.outputs[outBus], frames, sampleSize, offset);
      chanOffset += busChannels;
    }
    
    if (GetBypassed())
    {
      if (sampleSize == kSample32)
        PassThroughBuffers(0.f, frames); // single precision
      else
        PassThroughBuffers(0.0, frames); // double precision
    }
    else
    {
#ifdef PARAMS_MUTEX
      mPlug.mParams_mutex.Enter();
#endif
      if (sampleSize == kSample32)
        ProcessBuffers(0.f, frames); // single precision
      else
        ProcessBuffers(0.0, frames); // double precision
#ifdef PARAMS_MUTEX
      mPlug.mParams_mutex.Leave();
#endif
    }
  }
}

#if IPLUG_VST3_SAMPLE_ACCURATE_PARAMS
bool IPlugVST3ProcessorBase::ProcessSampleAccurate(ProcessData& data, ProcessSetup& setup, const BusList& ins, const BusList& outs, IPlugQueue<IMidiMsg>& fromProcessor)
{
  IParameterChanges* paramChanges = data.inputParameterChanges;

  // MIDI I/O keeps the default path: its events carry offsets into the whole block
  if (!paramChanges || DoesMIDIIn() || DoesMIDIOut() || data.numSamples <= IPLUG_VST3_PARAM_MIN_SEGMENT)
    return false;

  // AttachBuffers() shifts channel pointers through a fixed size array
  for (int32 i = 0; i < data.numInputs; i++)
  {
    if (data.inputs[i].numChannels > 64)
      return false;
  }

  for (int32 i = 0; i < data.numOutputs; i++)
  {
    if (data.outputs[i].numChannels > 64)
      return false;
  }

  mParamPoints.clear(); // keeps the reserved capacity
  bool needsSplit = false;
  uint32_t seq = 0;
  const int32 numParamsChanged = paramChanges->getParameterCount();

  for (int32 i = 0; i < numParamsChanged; i++)
  {
    IParamValueQueue* paramQueue = paramChanges->getParameterData(i);

    if (!paramQueue)
      continue;

    const int32 numPoints = paramQueue->getPointCount();

    for (int32 p = 0; p < numPoints; p++)
    {
      int32 offsetSamples;
      double value;

      if (paramQueue->getPoint(p, offsetSamples, value) != kResultTrue)
        continue;

      // More points than reserved: leave the block to the default path rather than allocating here
      if (mParamPoints.size() == mParamPoints.capacity())
        return false;

      mParamPoints.push_back({offsetSamples, seq++, static_cast<int32_t>(paramQueue->getParameterId()), value});

      if (offsetSamples >= IPLUG_VST3_PARAM_MIN_SEGMENT)
        needsSplit = true;
    }
  }

  // Nothing changes after the first few samples: the default path (all values applied at the start) is equivalent
  if (!needsSplit)
    return false;

  SortVST3ParamPoints(mParamPoints.data(), mParamPoints.size());

  ProcessWithVST3ParamSegments(
    mParamPoints.data(), mParamPoints.size(), data.numSamples, IPLUG_VST3_PARAM_MIN_SEGMENT,
    [&](const VST3ParamPoint& point) {
      // the point is applied right before its sub-block, so it takes effect at sample 0 of that sub-block
      ApplyParameterChange(point.paramID, point.value, 0, fromProcessor);
    },
    [&](int32_t startSample, int32_t numFrames) {
      ProcessAudio(data, setup, ins, outs, startSample, numFrames);
    });

  return true;
}
#endif

void IPlugVST3ProcessorBase::Process(ProcessData& data, ProcessSetup& setup, const BusList& ins, const BusList& outs, IPlugQueue<IMidiMsg>& fromEditor, IPlugQueue<IMidiMsg>& fromProcessor, IPlugQueue<SysExData>& sysExFromEditor, SysExData& sysExBuf)
{
  PrepareProcessContext(data, setup);

#if IPLUG_VST3_SAMPLE_ACCURATE_PARAMS
  if (ProcessSampleAccurate(data, setup, ins, outs, fromProcessor))
    return;
#endif

  ProcessParameterChanges(data, fromProcessor);
  
  if (DoesMIDIIn())
  {
    ProcessMidiIn(data.inputEvents, fromEditor, fromProcessor);
  }
  
  ProcessAudio(data, setup, ins, outs);
  
  if (DoesMIDIOut())
  {
    ProcessMidiOut(sysExFromEditor, sysExBuf, data.outputEvents, data.numSamples);
  }
}

bool IPlugVST3ProcessorBase::SendMidiMsg(const IMidiMsg& msg)
{
  mMidiOutputQueue.Add(msg);
  return true;
}
