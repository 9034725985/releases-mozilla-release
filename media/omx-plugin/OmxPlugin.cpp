/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <stagefright/DataSource.h>
#include <stagefright/MediaExtractor.h>
#include <stagefright/MetaData.h>
#include <stagefright/OMXCodec.h>
#ifdef MOZ_WIDGET_GONK
#include <OMX.h>
#else
#include <stagefright/OMXClient.h>
#endif
#include <algorithm>

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"
#include "MPAPI.h"

#include "android/log.h"

#undef LOG
#define LOG(args...)  __android_log_print(ANDROID_LOG_INFO, "OmxPlugin" , ## args)

using namespace MPAPI;

namespace android {

// MediaStreamSource is a DataSource that reads from a MPAPI media stream.

class MediaStreamSource : public DataSource {
  PluginHost *mPluginHost;
public:
  MediaStreamSource(PluginHost *aPluginHost, Decoder *aDecoder);

  virtual status_t initCheck() const;
  virtual ssize_t readAt(off64_t offset, void *data, size_t size);
  virtual ssize_t readAt(off_t offset, void *data, size_t size) {
    return readAt(static_cast<off64_t>(offset), data, size);
  }
  virtual status_t getSize(off_t *size) {
    off64_t size64;
    status_t status = getSize(&size64);
    *size = size64;
    return status;
  }
  virtual status_t getSize(off64_t *size);
  virtual uint32_t flags() {
    return kWantsPrefetching;
  }

  virtual ~MediaStreamSource();

private:
  Decoder *mDecoder;

  MediaStreamSource(const MediaStreamSource &);
  MediaStreamSource &operator=(const MediaStreamSource &);
};

MediaStreamSource::MediaStreamSource(PluginHost *aPluginHost, Decoder *aDecoder) :
  mPluginHost(aPluginHost)
{
  mDecoder = aDecoder;
}

MediaStreamSource::~MediaStreamSource()
{
}

status_t MediaStreamSource::initCheck() const
{
  return OK;
}

ssize_t MediaStreamSource::readAt(off64_t offset, void *data, size_t size)
{
  char *ptr = reinterpret_cast<char *>(data);
  size_t todo = size;
  while (todo > 0) {
    uint32_t bytesRead;
    if (!mPluginHost->Read(mDecoder, ptr, offset, todo, &bytesRead)) {
      return ERROR_IO;
    }
    offset += bytesRead;
    todo -= bytesRead;
    ptr += bytesRead;
  }
  return size;
}

status_t MediaStreamSource::getSize(off64_t *size)
{
  uint64_t length = mPluginHost->GetLength(mDecoder);
  if (length == static_cast<uint64_t>(-1))
    return ERROR_UNSUPPORTED;

  *size = length;

  return OK;
}

}  // namespace android

using namespace android;

namespace OmxPlugin {

const int OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka = 0x7FA30C01;
const int OMX_QCOM_COLOR_FormatYVU420SemiPlanar = 0x7FA30C00;

class OmxDecoder {
  PluginHost *mPluginHost;
  Decoder *mDecoder;
#ifndef MOZ_WIDGET_GONK
  OMXClient mClient;
#endif
  sp<MediaSource> mVideoTrack;
  sp<MediaSource> mVideoSource;
  sp<MediaSource> mAudioTrack;
  sp<MediaSource> mAudioSource;
  int32_t mVideoWidth;
  int32_t mVideoHeight;
  int32_t mVideoColorFormat;
  int32_t mVideoStride;
  int32_t mVideoSliceHeight;
  int32_t mVideoCropLeft;
  int32_t mVideoCropTop;
  int32_t mVideoRotation;
  int32_t mAudioChannels;
  int32_t mAudioSampleRate;
  int64_t mDurationUs;
  MediaBuffer *mVideoBuffer;
  VideoFrame mVideoFrame;
  MediaBuffer *mAudioBuffer;
  AudioFrame mAudioFrame;

  // 'true' if a read from the audio stream was done while reading the metadata
  bool mAudioMetadataRead;

  void ReleaseVideoBuffer();
  void ReleaseAudioBuffer();

  void ToVideoFrame_YUV420Planar(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame);
  void ToVideoFrame_CbYCrY(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame);
  void ToVideoFrame_YUV420SemiPlanar(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame);
  void ToVideoFrame_YVU420SemiPlanar(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame);
  void ToVideoFrame_YUV420PackedSemiPlanar(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame);
  void ToVideoFrame_YVU420PackedSemiPlanar32m4ka(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame);
  bool ToVideoFrame(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame);
  bool ToAudioFrame(AudioFrame *aFrame, int64_t aTimeUs, void *aData, size_t aDataOffset, size_t aSize,
                    int32_t aAudioChannels, int32_t aAudioSampleRate);
public:
  OmxDecoder(PluginHost *aPluginHost, Decoder *aDecoder);
  ~OmxDecoder();

  bool Init();
  bool SetVideoFormat();
  bool SetAudioFormat();

  void GetDuration(int64_t *durationUs) {
    *durationUs = mDurationUs;
  }

  void GetVideoParameters(int32_t *width, int32_t *height) {
    *width = mVideoWidth;
    *height = mVideoHeight;
  }

  void GetAudioParameters(int32_t *numChannels, int32_t *sampleRate) {
    *numChannels = mAudioChannels;
    *sampleRate = mAudioSampleRate;
  }

  bool HasVideo() {
    return mVideoSource != NULL;
  }

  bool HasAudio() {
    return mAudioSource != NULL;
  }

  bool ReadVideo(VideoFrame *aFrame, int64_t aSeekTimeUs);
  bool ReadAudio(AudioFrame *aFrame, int64_t aSeekTimeUs);
};

OmxDecoder::OmxDecoder(PluginHost *aPluginHost, Decoder *aDecoder) :
  mPluginHost(aPluginHost),
  mDecoder(aDecoder),
  mVideoWidth(0),
  mVideoHeight(0),
  mVideoColorFormat(0),
  mVideoStride(0),
  mVideoSliceHeight(0),
  mVideoCropLeft(0),
  mVideoCropTop(0),
  mVideoRotation(0),
  mAudioChannels(-1),
  mAudioSampleRate(-1),
  mDurationUs(-1),
  mVideoBuffer(NULL),
  mAudioBuffer(NULL),
  mAudioMetadataRead(false)
{
}

OmxDecoder::~OmxDecoder()
{
  ReleaseVideoBuffer();
  ReleaseAudioBuffer();

  if (mVideoSource.get()) {
    mVideoSource->stop();
  }

  if (mAudioSource.get()) {
    mAudioSource->stop();
  }
#ifndef MOZ_WIDGET_GONK
  mClient.disconnect();
#endif
}

class AutoStopMediaSource {
  sp<MediaSource> mMediaSource;
public:
  AutoStopMediaSource(sp<MediaSource> aMediaSource) : mMediaSource(aMediaSource) {
  }

  ~AutoStopMediaSource() {
    mMediaSource->stop();
  }
};

#ifdef MOZ_WIDGET_GONK
static sp<IOMX> sOMX = NULL;
static sp<IOMX> GetOMX() {
  if(sOMX.get() == NULL) {
    sOMX = reinterpret_cast<IOMX*>(new OMX);
  }
  return sOMX;
}
#endif

static uint32_t GetVideoCreationFlags(PluginHost* aPluginHost)
{
#ifdef MOZ_WIDGET_GONK
  // Flag value of zero means return a hardware or software decoder
  // depending on what the device supports.
  return 0;
#else
  // Check whether the user has set a pref to override our default OMXCodec
  // CreationFlags flags. This is useful for A/B testing hardware and software
  // decoders for performance and bugs. The interesting flag values are:
  //  0 = Let Stagefright choose hardware or software decoding (default)
  //  8 = Force software decoding
  // 16 = Force hardware decoding
  int32_t flags = 0;
  aPluginHost->GetIntPref("media.stagefright.omxcodec.flags", &flags);
  if (flags != 0) {
    LOG("media.stagefright.omxcodec.flags=%d", flags);
    if ((flags & OMXCodec::kHardwareCodecsOnly) != 0) {
      LOG("FORCE HARDWARE DECODING");
    } else if ((flags & OMXCodec::kSoftwareCodecsOnly) != 0) {
      LOG("FORCE SOFTWARE DECODING");
    }
  }
  return static_cast<uint32_t>(flags);
#endif
}

static sp<MediaSource> CreateVideoSource(PluginHost* aPluginHost,
                                         const sp<IOMX>& aOmx,
                                         const sp<MediaSource>& aVideoTrack)
{
  uint32_t flags = GetVideoCreationFlags(aPluginHost);
  if (flags == 0) {
    // Let Stagefright choose hardware or software decoder.
    sp<MediaSource> videoSource = OMXCodec::Create(aOmx, aVideoTrack->getFormat(),
                                                   false, aVideoTrack, NULL, 0);
    if (videoSource == NULL)
      return NULL;

    // Now that OMXCodec has parsed the video's AVCDecoderConfigurationRecord,
    // check whether we know how to decode this video.
    int32_t videoColorFormat;
    if (videoSource->getFormat()->findInt32(kKeyColorFormat, &videoColorFormat)) {
      switch (videoColorFormat) {
        // We know how to convert these color formats.
        case OMX_COLOR_FormatCbYCrY:
        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420SemiPlanar:
        case OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka:
        case OMX_QCOM_COLOR_FormatYVU420SemiPlanar:
        case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
          // Use the decoder Stagefright chose for us!
          return videoSource;

        // Use software decoder for color formats we don't know how to convert.
        default:
          // We need to implement a ToVideoFrame_*() color conversion
          // function for this video color format.
          LOG("Unknown video color format: %#x", videoColorFormat);
          break;
      }
    } else {
      LOG("Video color format not found");
    }

    // Throw away the videoSource and try again with new flags.
    LOG("Falling back to software decoder");
    videoSource.clear();
    flags = OMXCodec::kSoftwareCodecsOnly;
  }

  MOZ_ASSERT(flags != 0);
  return OMXCodec::Create(aOmx, aVideoTrack->getFormat(), false, aVideoTrack,
                          NULL, flags);
}

bool OmxDecoder::Init() {
  //register sniffers, if they are not registered in this process.
  DataSource::RegisterDefaultSniffers();

  sp<DataSource> dataSource = new MediaStreamSource(mPluginHost, mDecoder);
  if (dataSource->initCheck()) {
    return false;
  }

  mPluginHost->SetMetaDataReadMode(mDecoder);

  sp<MediaExtractor> extractor = MediaExtractor::Create(dataSource);
  if (extractor == NULL) {
    return false;
  }

  ssize_t audioTrackIndex = -1;
  ssize_t videoTrackIndex = -1;
  const char *audioMime = NULL;
  const char *videoMime = NULL;

  for (size_t i = 0; i < extractor->countTracks(); ++i) {
    sp<MetaData> meta = extractor->getTrackMetaData(i);

    const char *mime;
    if (!meta->findCString(kKeyMIMEType, &mime)) {
      continue;
    }

    if (videoTrackIndex == -1 && !strncasecmp(mime, "video/", 6)) {
      videoTrackIndex = i;
      videoMime = mime;
    } else if (audioTrackIndex == -1 && !strncasecmp(mime, "audio/", 6)) {
      audioTrackIndex = i;
      audioMime = mime;
    }
  }

  if (videoTrackIndex == -1 && audioTrackIndex == -1) {
    return false;
  }

  mPluginHost->SetPlaybackReadMode(mDecoder);

  int64_t totalDurationUs = 0;

#ifdef MOZ_WIDGET_GONK
  sp<IOMX> omx = GetOMX();
#else
  // OMXClient::connect() always returns OK and abort's fatally if
  // it can't connect. We may need to implement the connect functionality
  // ourselves if this proves to be an issue.
  if (mClient.connect() != OK) {
    LOG("OMXClient failed to connect");
  }
  sp<IOMX> omx = mClient.interface();
#endif

  sp<MediaSource> videoTrack;
  sp<MediaSource> videoSource;
  if (videoTrackIndex != -1 && (videoTrack = extractor->getTrack(videoTrackIndex)) != NULL) {
    videoSource = CreateVideoSource(mPluginHost, omx, videoTrack);
    if (videoSource == NULL) {
      LOG("OMXCodec failed to initialize video decoder for \"%s\"", videoMime);
      return false;
    }

    status_t status = videoSource->start();
    if (status != OK) {
      LOG("videoSource->start() failed with status %#x", status);
      return false;
    }

    int64_t durationUs;
    if (videoTrack->getFormat()->findInt64(kKeyDuration, &durationUs)) {
      if (durationUs < 0)
        LOG("video duration %lld should be nonnegative", durationUs);
      if (durationUs > totalDurationUs)
        totalDurationUs = durationUs;
    }
  }

  sp<MediaSource> audioTrack;
  sp<MediaSource> audioSource;
  if (audioTrackIndex != -1 && (audioTrack = extractor->getTrack(audioTrackIndex)) != NULL)
  {
    if (!strcasecmp(audioMime, "audio/raw")) {
      audioSource = audioTrack;
    } else {
      audioSource = OMXCodec::Create(omx,
                                     audioTrack->getFormat(),
                                     false, // decoder
                                     audioTrack);
    }

    if (audioSource == NULL) {
      LOG("OMXCodec failed to initialize audio decoder for \"%s\"", audioMime);
      return false;
    }

    status_t status = audioSource->start();
    if (status != OK) {
      LOG("audioSource->start() failed with status %#x", status);
      return false;
    }

    int64_t durationUs;
    if (audioTrack->getFormat()->findInt64(kKeyDuration, &durationUs)) {
      if (durationUs < 0)
        LOG("audio duration %lld should be nonnegative", durationUs);
      if (durationUs > totalDurationUs)
        totalDurationUs = durationUs;
    }
  }

  // set decoder state
  mVideoTrack = videoTrack;
  mVideoSource = videoSource;
  mAudioTrack = audioTrack;
  mAudioSource = audioSource;
  mDurationUs = totalDurationUs;

  if (mVideoSource.get() && !SetVideoFormat())
    return false;

  // To reliably get the channel and sample rate data we need to read from the
  // audio source until we get a INFO_FORMAT_CHANGE status
  if (mAudioSource.get()) {
    if (mAudioSource->read(&mAudioBuffer) != INFO_FORMAT_CHANGED) {
      sp<MetaData> meta = mAudioSource->getFormat();
      if (!meta->findInt32(kKeyChannelCount, &mAudioChannels) ||
          !meta->findInt32(kKeySampleRate, &mAudioSampleRate)) {
        return false;
      }
      mAudioMetadataRead = true;

      if (mAudioChannels < 0) {
        LOG("audio channel count %d must be nonnegative", mAudioChannels);
        return false;
      }

      if (mAudioSampleRate < 0) {
        LOG("audio sample rate %d must be nonnegative", mAudioSampleRate);
        return false;
      }
    }
    else if (!SetAudioFormat()) {
        return false;
    }
  }
  return true;
}

bool OmxDecoder::SetVideoFormat() {
  sp<MetaData> format = mVideoSource->getFormat();

  // Stagefright's kKeyWidth and kKeyHeight are what MPAPI calls stride and
  // slice height. Stagefright only seems to use its kKeyStride and
  // kKeySliceHeight to initialize camera video formats.

#ifdef DEBUG
  int32_t unexpected;
  if (format->findInt32(kKeyStride, &unexpected))
    LOG("Expected kKeyWidth, but found kKeyStride %d", unexpected);
  if (format->findInt32(kKeySliceHeight, &unexpected))
    LOG("Expected kKeyHeight, but found kKeySliceHeight %d", unexpected);
#endif // DEBUG

  const char *componentName;

  if (!format->findInt32(kKeyWidth, &mVideoStride) ||
      !format->findInt32(kKeyHeight, &mVideoSliceHeight) ||
      !format->findCString(kKeyDecoderComponent, &componentName) ||
      !format->findInt32(kKeyColorFormat, &mVideoColorFormat) ) {
    return false;
  }

  if (mVideoStride <= 0) {
    LOG("stride %d must be positive", mVideoStride);
    return false;
  }

  if (mVideoSliceHeight <= 0) {
    LOG("slice height %d must be positive", mVideoSliceHeight);
    return false;
  }

  int32_t cropRight, cropBottom;
  if (!format->findRect(kKeyCropRect, &mVideoCropLeft, &mVideoCropTop,
                                      &cropRight, &cropBottom)) {
    mVideoCropLeft = 0;
    mVideoCropTop = 0;
    cropRight = mVideoStride - 1;
    cropBottom = mVideoSliceHeight - 1;
    LOG("crop rect not available, assuming no cropping");
  }

  if (mVideoCropLeft < 0 || mVideoCropLeft >= cropRight || cropRight >= mVideoStride ||
      mVideoCropTop < 0 || mVideoCropTop >= cropBottom || cropBottom >= mVideoSliceHeight) {
    LOG("invalid crop rect %d,%d-%d,%d", mVideoCropLeft, mVideoCropTop, cropRight, cropBottom);
    return false;
  }

  mVideoWidth = cropRight - mVideoCropLeft + 1;
  mVideoHeight = cropBottom - mVideoCropTop + 1;
  MOZ_ASSERT(mVideoWidth > 0 && mVideoWidth <= mVideoStride);
  MOZ_ASSERT(mVideoHeight > 0 && mVideoHeight <= mVideoSliceHeight);

  if (!format->findInt32(kKeyRotation, &mVideoRotation)) {
    mVideoRotation = 0;
    LOG("rotation not available, assuming 0");
  }

  if (mVideoRotation != 0 && mVideoRotation != 90 &&
      mVideoRotation != 180 && mVideoRotation != 270) {
    LOG("invalid rotation %d, assuming 0", mVideoRotation);
  }

  LOG("width: %d height: %d component: %s format: %#x stride: %d sliceHeight: %d rotation: %d crop: %d,%d-%d,%d",
      mVideoWidth, mVideoHeight, componentName, mVideoColorFormat,
      mVideoStride, mVideoSliceHeight, mVideoRotation,
      mVideoCropLeft, mVideoCropTop, cropRight, cropBottom);

  return true;
}

bool OmxDecoder::SetAudioFormat() {
  // If the format changed, update our cached info.
  if (!mAudioSource->getFormat()->findInt32(kKeyChannelCount, &mAudioChannels) ||
      !mAudioSource->getFormat()->findInt32(kKeySampleRate, &mAudioSampleRate)) {
    return false;
  }

  LOG("channelCount: %d sampleRate: %d", mAudioChannels, mAudioSampleRate);

  if (mAudioChannels < 0) {
    LOG("audio channel count %d must be nonnegative", mAudioChannels);
    return false;
  }

  if (mAudioSampleRate < 0) {
    LOG("audio sample rate %d must be nonnegative", mAudioSampleRate);
    return false;
  }

  return true;
}

void OmxDecoder::ReleaseVideoBuffer() {
  if (mVideoBuffer) {
    mVideoBuffer->release();
    mVideoBuffer = NULL;
  }
}

void OmxDecoder::ReleaseAudioBuffer() {
  if (mAudioBuffer) {
    mAudioBuffer->release();
    mAudioBuffer = NULL;
  }
}

void OmxDecoder::ToVideoFrame_YUV420Planar(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame) {
  void *y = aData;
  void *u = static_cast<uint8_t *>(y) + mVideoStride * mVideoSliceHeight;
  void *v = static_cast<uint8_t *>(u) + mVideoStride/2 * mVideoSliceHeight/2;
  aFrame->Set(aTimeUs, aKeyFrame,
              aData, aSize, mVideoStride, mVideoSliceHeight, mVideoRotation,
              y, mVideoStride, mVideoWidth, mVideoHeight, 0, 0,
              u, mVideoStride/2, mVideoWidth/2, mVideoHeight/2, 0, 0,
              v, mVideoStride/2, mVideoWidth/2, mVideoHeight/2, 0, 0);
}

void OmxDecoder::ToVideoFrame_CbYCrY(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame) {
  aFrame->Set(aTimeUs, aKeyFrame,
              aData, aSize, mVideoStride, mVideoSliceHeight, mVideoRotation,
              aData, mVideoStride, mVideoWidth, mVideoHeight, 1, 1,
              aData, mVideoStride, mVideoWidth/2, mVideoHeight/2, 0, 3,
              aData, mVideoStride, mVideoWidth/2, mVideoHeight/2, 2, 3);
}

void OmxDecoder::ToVideoFrame_YUV420SemiPlanar(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame) {
  int32_t videoStride = mVideoStride;
  int32_t videoSliceHeight = mVideoSliceHeight;

  // OMX.SEC.avcdec rounds mVideoStride and mVideoSliceHeight up to the nearest
  // multiple of 16 but the data itself is too small to fit. What we do is check
  // to see if the video size patches the raw width and height. If so we can
  // use those figures instead.

  if (aSize == mVideoWidth * mVideoHeight * 3 / 2) {
    videoStride = mVideoWidth;
    videoSliceHeight = mVideoHeight;
  }

  void *y = aData;
  void *uv = static_cast<uint8_t *>(y) + (videoStride * videoSliceHeight);
  aFrame->Set(aTimeUs, aKeyFrame,
              aData, aSize, videoStride, videoSliceHeight, mVideoRotation,
              y, videoStride, mVideoWidth, mVideoHeight, 0, 0,
              uv, videoStride, mVideoWidth/2, mVideoHeight/2, 0, 1,
              uv, videoStride, mVideoWidth/2, mVideoHeight/2, 1, 1);
}

void OmxDecoder::ToVideoFrame_YVU420SemiPlanar(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame) {
  ToVideoFrame_YUV420SemiPlanar(aFrame, aTimeUs, aData, aSize, aKeyFrame);
  aFrame->Cb.mOffset = 1;
  aFrame->Cr.mOffset = 0;
}

void OmxDecoder::ToVideoFrame_YUV420PackedSemiPlanar(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame) {
  void *y = aData;
  void *uv = static_cast<uint8_t *>(y) + mVideoStride * (mVideoSliceHeight - mVideoCropTop/2);
  aFrame->Set(aTimeUs, aKeyFrame,
              aData, aSize, mVideoStride, mVideoSliceHeight, mVideoRotation,
              y, mVideoStride, mVideoWidth, mVideoHeight, 0, 0,
              uv, mVideoStride, mVideoWidth/2, mVideoHeight/2, 0, 1,
              uv, mVideoStride, mVideoWidth/2, mVideoHeight/2, 1, 1);
}

void OmxDecoder::ToVideoFrame_YVU420PackedSemiPlanar32m4ka(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame) {
  size_t roundedSliceHeight = (mVideoSliceHeight + 31) & ~31;
  size_t roundedStride = (mVideoStride + 31) & ~31;
  void *y = aData;
  void *uv = static_cast<uint8_t *>(y) + (roundedStride * roundedSliceHeight);
  aFrame->Set(aTimeUs, aKeyFrame,
              aData, aSize, mVideoStride, mVideoSliceHeight, mVideoRotation,
              y, mVideoStride, mVideoWidth, mVideoHeight, 0, 0,
              uv, mVideoStride, mVideoWidth/2, mVideoHeight/2, 1, 1,
              uv, mVideoStride, mVideoWidth/2, mVideoHeight/2, 0, 1);
}

bool OmxDecoder::ToVideoFrame(VideoFrame *aFrame, int64_t aTimeUs, void *aData, size_t aSize, bool aKeyFrame) {
  switch (mVideoColorFormat) {
  case OMX_COLOR_FormatYUV420Planar: // e.g. Asus Transformer, Stagefright's software decoder
    ToVideoFrame_YUV420Planar(aFrame, aTimeUs, aData, aSize, aKeyFrame);
    break;
  case OMX_COLOR_FormatCbYCrY: // e.g. Droid 1
    ToVideoFrame_CbYCrY(aFrame, aTimeUs, aData, aSize, aKeyFrame);
    break;
  case OMX_COLOR_FormatYUV420SemiPlanar: // e.g. Galaxy S III
    ToVideoFrame_YUV420SemiPlanar(aFrame, aTimeUs, aData, aSize, aKeyFrame);
    break;
  case OMX_QCOM_COLOR_FormatYVU420SemiPlanar: // e.g. Nexus One
    ToVideoFrame_YVU420SemiPlanar(aFrame, aTimeUs, aData, aSize, aKeyFrame);
    break;
  case OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka: // e.g. Otoro
    ToVideoFrame_YVU420PackedSemiPlanar32m4ka(aFrame, aTimeUs, aData, aSize, aKeyFrame);
    break;
  case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar: // e.g. Galaxy Nexus
    ToVideoFrame_YUV420PackedSemiPlanar(aFrame, aTimeUs, aData, aSize, aKeyFrame);
    break;
  default:
    LOG("Unknown video color format: %#x", mVideoColorFormat);
    return false;
  }
  return true;
}

bool OmxDecoder::ToAudioFrame(AudioFrame *aFrame, int64_t aTimeUs, void *aData, size_t aDataOffset, size_t aSize, int32_t aAudioChannels, int32_t aAudioSampleRate)
{
  aFrame->Set(aTimeUs, reinterpret_cast<char *>(aData) + aDataOffset, aSize, aAudioChannels, aAudioSampleRate);
  return true;
}

bool OmxDecoder::ReadVideo(VideoFrame *aFrame, int64_t aSeekTimeUs)
{
  MOZ_ASSERT(aSeekTimeUs >= -1);

  if (!mVideoSource.get())
    return false;

  ReleaseVideoBuffer();

  status_t err;

  if (aSeekTimeUs != -1) {
    MediaSource::ReadOptions options;
    options.setSeekTo(aSeekTimeUs);
    err = mVideoSource->read(&mVideoBuffer, &options);
  } else {
    err = mVideoSource->read(&mVideoBuffer);
  }

  if (err == OK && mVideoBuffer->range_length() > 0) {
    int64_t timeUs;
    int32_t keyFrame;

    if (!mVideoBuffer->meta_data()->findInt64(kKeyTime, &timeUs) ) {
      LOG("no frame time");
      return false;
    }

    if (timeUs < 0) {
      LOG("frame time %lld must be nonnegative", timeUs);
      return false;
    }

    if (!mVideoBuffer->meta_data()->findInt32(kKeyIsSyncFrame, &keyFrame)) {
       keyFrame = 0;
    }

    char *data = reinterpret_cast<char *>(mVideoBuffer->data()) + mVideoBuffer->range_offset();
    size_t length = mVideoBuffer->range_length();

    if (!ToVideoFrame(aFrame, timeUs, data, length, keyFrame)) {
      return false;
    }
  }
  else if (err == INFO_FORMAT_CHANGED) {
    // If the format changed, update our cached info.
    LOG("mVideoSource INFO_FORMAT_CHANGED");
    if (!SetVideoFormat())
      return false;
    else
      return ReadVideo(aFrame, aSeekTimeUs);
  }
  else if (err == ERROR_END_OF_STREAM) {
    LOG("mVideoSource END_OF_STREAM");
  }
  else if (err != OK) {
    LOG("mVideoSource ERROR %#x", err);
  }

  return err == OK;
}

bool OmxDecoder::ReadAudio(AudioFrame *aFrame, int64_t aSeekTimeUs)
{
  MOZ_ASSERT(aSeekTimeUs >= -1);

  status_t err;
  if (mAudioMetadataRead && aSeekTimeUs == -1) {
    // Use the data read into the buffer during metadata time
    err = OK;
  }
  else {
    ReleaseAudioBuffer();
    if (aSeekTimeUs != -1) {
      MediaSource::ReadOptions options;
      options.setSeekTo(aSeekTimeUs);
      err = mAudioSource->read(&mAudioBuffer, &options);
    } else {
      err = mAudioSource->read(&mAudioBuffer);
    }
  }
  mAudioMetadataRead = false;

  aSeekTimeUs = -1;

  if (err == OK && mAudioBuffer->range_length() != 0) {
    int64_t timeUs;
    if (!mAudioBuffer->meta_data()->findInt64(kKeyTime, &timeUs)) {
      LOG("no frame time");
      return false;
    }

    if (timeUs < 0) {
      LOG("frame time %lld must be nonnegative", timeUs);
      return false;
    }

    return ToAudioFrame(aFrame, timeUs,
                        mAudioBuffer->data(),
                        mAudioBuffer->range_offset(),
                        mAudioBuffer->range_length(),
                        mAudioChannels, mAudioSampleRate);
  }
  else if (err == INFO_FORMAT_CHANGED) {
    // If the format changed, update our cached info.
    LOG("mAudioSource INFO_FORMAT_CHANGED");
    if (!SetAudioFormat())
      return false;
    else
      return ReadAudio(aFrame, aSeekTimeUs);
  }
  else if (err == ERROR_END_OF_STREAM) {
    LOG("mAudioSource END_OF_STREAM");
  }
  else if (err != OK) {
    LOG("mAudioSource ERROR %#x", err);
  }

  return err == OK;
}

static OmxDecoder *cast(Decoder *decoder) {
  return reinterpret_cast<OmxDecoder *>(decoder->mPrivate);
}

static void GetDuration(Decoder *aDecoder, int64_t *durationUs) {
  cast(aDecoder)->GetDuration(durationUs);
}

static void GetVideoParameters(Decoder *aDecoder, int32_t *width, int32_t *height) {
  cast(aDecoder)->GetVideoParameters(width, height);
}

static void GetAudioParameters(Decoder *aDecoder, int32_t *numChannels, int32_t *sampleRate) {
  cast(aDecoder)->GetAudioParameters(numChannels, sampleRate);
}

static bool HasVideo(Decoder *aDecoder) {
  return cast(aDecoder)->HasVideo();
}

static bool HasAudio(Decoder *aDecoder) {
  return cast(aDecoder)->HasAudio();
}

static bool ReadVideo(Decoder *aDecoder, VideoFrame *aFrame, int64_t aSeekTimeUs)
{
  return cast(aDecoder)->ReadVideo(aFrame, aSeekTimeUs);
}

static bool ReadAudio(Decoder *aDecoder, AudioFrame *aFrame, int64_t aSeekTimeUs)
{
  return cast(aDecoder)->ReadAudio(aFrame, aSeekTimeUs);
}

static void DestroyDecoder(Decoder *aDecoder)
{
  if (aDecoder->mPrivate)
    delete reinterpret_cast<OmxDecoder *>(aDecoder->mPrivate);
}

static bool Match(const char *aMimeChars, size_t aMimeLen, const char *aNeedle)
{
  return !strncmp(aMimeChars, aNeedle, aMimeLen);
}

static const char* const gCodecs[] = {
  "avc1.42E01E",  // H.264 Constrained Baseline Profile Level 3.0
  "avc1.42001E",  // H.264 Baseline Profile Level 3.0
  "avc1.42001F",  // H.264 Baseline Profile Level 3.1
  "avc1.4D401E",  // H.264 Main Profile Level 3.0
  "avc1.4D401F",  // H.264 Main Profile Level 3.1
  "mp4a.40.2",    // AAC-LC
  NULL
};

static bool CanDecode(const char *aMimeChars, size_t aMimeLen, const char* const**aCodecs)
{
  if (!Match(aMimeChars, aMimeLen, "video/mp4") &&
      !Match(aMimeChars, aMimeLen, "audio/mp4") &&
      !Match(aMimeChars, aMimeLen, "audio/mpeg") &&
      !Match(aMimeChars, aMimeLen, "application/octet-stream")) { // file urls
    return false;
  }
  *aCodecs = gCodecs;

  return true;
}

static bool CreateDecoder(PluginHost *aPluginHost, Decoder *aDecoder, const char *aMimeChars, size_t aMimeLen)
{
  OmxDecoder *omx = new OmxDecoder(aPluginHost, aDecoder);
  if (!omx || !omx->Init())
    return false;

  aDecoder->mPrivate = omx;
  aDecoder->GetDuration = GetDuration;
  aDecoder->GetVideoParameters = GetVideoParameters;
  aDecoder->GetAudioParameters = GetAudioParameters;
  aDecoder->HasVideo = HasVideo;
  aDecoder->HasAudio = HasAudio;
  aDecoder->ReadVideo = ReadVideo;
  aDecoder->ReadAudio = ReadAudio;
  aDecoder->DestroyDecoder = DestroyDecoder;

  return true;
}

} // namespace OmxPlugin

// Export the manifest so MPAPI can find our entry points.
Manifest MOZ_EXPORT MPAPI_MANIFEST {
  OmxPlugin::CanDecode,
  OmxPlugin::CreateDecoder
};
