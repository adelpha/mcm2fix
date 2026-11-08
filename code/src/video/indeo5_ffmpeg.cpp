// Indeo 5 decoding with FFmpeg's libavcodec (LGPL 2.1 or later), built with
// only the indeo5 decoder enabled; see tools/build-ffmpeg.sh.
#include <cstring>
#include <vector>

#include "indeo5.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace video {
namespace {

class FfmpegIndeo5 final : public Indeo5Decoder {
 public:
  ~FfmpegIndeo5() override {
    av_frame_free(&frame_);
    av_frame_free(&next_);
    av_packet_free(&packet_);
    avcodec_free_context(&ctx_);
  }

  bool Init(int width, int height) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_INDEO5);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    next_ = av_frame_alloc();
    if (!ctx_ || !packet_ || !frame_ || !next_) return false;
    ctx_->width = width;
    ctx_->height = height;
    ctx_->codec_tag = MKTAG('I', 'V', '5', '0');
    ctx_->thread_count = 1;
    return avcodec_open2(ctx_, codec, nullptr) == 0;
  }

  bool Decode(const uint8_t* data, size_t size, YuvPicture* pic) override {
    buf_.assign(data, data + size);
    buf_.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    packet_->data = buf_.data();
    packet_->size = static_cast<int>(size);
    if (avcodec_send_packet(ctx_, packet_) < 0) return false;
    // Receive into a second frame: a failed receive clears its frame, and
    // the last picture must stay valid for empty (repeat) frames.
    bool got = false;
    while (avcodec_receive_frame(ctx_, next_) == 0) {
      av_frame_unref(frame_);
      av_frame_move_ref(frame_, next_);
      got = true;
    }
    if (!got || frame_->format != AV_PIX_FMT_YUV410P) return false;
    for (int i = 0; i < 3; ++i) {
      pic->plane[i] = frame_->data[i];
      pic->stride[i] = frame_->linesize[i];
    }
    pic->width = frame_->width;
    pic->height = frame_->height;
    pic->chroma_w = (frame_->width + 3) / 4;
    pic->chroma_h = (frame_->height + 3) / 4;
    return true;
  }

  void Reset() override { avcodec_flush_buffers(ctx_); }

 private:
  AVCodecContext* ctx_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;  // the picture handed out last
  AVFrame* next_ = nullptr;
  std::vector<uint8_t> buf_;
};

}  // namespace

std::unique_ptr<Indeo5Decoder> CreateIndeo5Decoder(int width, int height) {
  auto d = std::make_unique<FfmpegIndeo5>();
  if (!d->Init(width, height)) return nullptr;
  return d;
}

}  // namespace video
