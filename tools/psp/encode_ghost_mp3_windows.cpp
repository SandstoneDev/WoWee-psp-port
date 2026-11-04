#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <cstdio>
#include <mmreg.h>
#include <wmcodecdsp.h>
#include <mftransform.h>
using Microsoft::WRL::ComPtr;
#define OK(x) do { HRESULT h=(x);if(FAILED(h)){std::printf("failed line %d hr=%08lx\n",__LINE__,h);return 1;}}while(0)
int wmain(int argc,wchar_t** argv){if(argc!=3)return 2;OK(CoInitializeEx(nullptr,COINIT_MULTITHREADED));OK(MFStartup(MF_VERSION));
ComPtr<IMFAttributes> attr;OK(MFCreateAttributes(&attr,1));
ComPtr<IMFSourceReader> reader;OK(MFCreateSourceReaderFromURL(argv[1],attr.Get(),&reader));
ComPtr<IMFMediaType> pcm;OK(MFCreateMediaType(&pcm));OK(pcm->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio));OK(pcm->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM));OK(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2));OK(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,44100));OK(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));OK(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,4));OK(pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,176400));
OK(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,nullptr,pcm.Get()));
ComPtr<IMFSinkWriter> writer;OK(MFCreateSinkWriterFromURL(argv[2],nullptr,nullptr,&writer));ComPtr<IMFMediaType> mp3;OK(MFCreateMediaType(&mp3));ComPtr<IMFTransform> encoder;OK(CoCreateInstance(CLSID_MP3ACMCodecWrapper,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&encoder)));
for(DWORD i=0;;++i){ComPtr<IMFMediaType> candidate;HRESULT hr=encoder->GetOutputAvailableType(0,i,&candidate);if(FAILED(hr))break;UINT32 rate=0,ch=0,bytes=0;candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,&rate);candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,&ch);candidate->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,&bytes);if(rate==44100 && ch==2 && bytes==16000){mp3=candidate;break;}}
OK(encoder->SetOutputType(0,mp3.Get(),0));OK(encoder->SetInputType(0,pcm.Get(),0));
DWORD stream=0;OK(writer->AddStream(mp3.Get(),&stream));
OK(writer->SetInputMediaType(stream,pcm.Get(),nullptr));
OK(writer->BeginWriting());unsigned samples=0;
for(;;){DWORD flags=0;LONGLONG ts=0;ComPtr<IMFSample> sample;OK(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,nullptr,&flags,&ts,&sample));if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;if(sample){OK(writer->WriteSample(stream,sample.Get()));++samples;}}
OK(writer->Finalize());std::printf("encoded %u samples\n",samples);return 0;}
