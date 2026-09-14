// PROTOTYPE — streammate-pivot #11 "local preview feed from the headless host to the browser".
// Throwaway. Never ship. Adds four verbs (preview.start / preview.stop / preview.stats /
// preview.snapshot) that push program video to the control WebSocket as binary frames, in
// two shapes:
//   codec "jpeg": obs_add_raw_video_callback2 (libobs CPU-scales + converts) → turbojpeg on a
//                 worker thread → one binary WS frame per picture.
//   codec "h264": a scaled libobs video encoder (VideoToolbox, x264 fallback) feeding a
//                 host-registered encoded output whose packets go straight to the WS
//                 (Annex B, SPS/PPS prepended on keyframes) for WebCodecs VideoDecoder.
// preview.snapshot renders program / a scene / a source into a texrender on the graphics
// context and returns a JPEG (base64) — the per-source-thumbnail shape.
//
// Binary frame header (32 bytes, big-endian):
//   0  u8  magic 'S'          1  u8  kind (1 jpeg, 2 h264)   2 u8 flags (bit0 keyframe)
//   3  u8  pixfmt (1 bgra, 2 i420, 0 n/a)                    4 u32 seq
//   8  u64 wallMs (CLOCK_REALTIME at capture/packet receipt) 16 u64 frameTsNs (libobs ts / pts)
//   24 u16 width  26 u16 height  28 u32 encodeUs             32.. payload
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

#if STREAMMATE_HAS_LIBOBS
#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <turbojpeg.h>

// Included from studio_host.cpp after send_all / json_escape / extract_json_* / g_ws_send_mutex
// are defined, so the same-TU definitions resolve whatever their linkage.

namespace proto_preview {

inline std::string err(int code, const std::string &message) {
  return "__error__:" + std::to_string(code) + ":" + message;
}

inline uint64_t wall_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

inline uint64_t mono_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void put_u16(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}
inline void put_u32(std::vector<uint8_t> &out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>((v >> s) & 0xff));
}
inline void put_u64(std::vector<uint8_t> &out, uint64_t v) {
  for (int s = 56; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>((v >> s) & 0xff));
}

// One WebSocket binary frame (opcode 0x2) = header + payload, single send under the mutex.
inline bool send_binary_ws(int fd, const std::vector<uint8_t> &header, const uint8_t *payload, size_t payload_size) {
  const size_t size = header.size() + payload_size;
  std::vector<uint8_t> frame;
  frame.reserve(size + 10);
  frame.push_back(0x82);
  if (size < 126) {
    frame.push_back(static_cast<uint8_t>(size));
  } else if (size <= 0xffff) {
    frame.push_back(126);
    frame.push_back(static_cast<uint8_t>(size >> 8));
    frame.push_back(static_cast<uint8_t>(size & 0xff));
  } else {
    frame.push_back(127);
    put_u64(frame, size);
  }
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), payload, payload + payload_size);
  std::lock_guard<std::mutex> lock(g_ws_send_mutex);
  return send_all(fd, frame.data(), frame.size());
}

// Bytes queued in the kernel send buffer and not yet acked — the backpressure signal.
inline long unsent_bytes(int fd) {
  int n = 0;
  socklen_t len = sizeof(n);
  if (getsockopt(fd, SOL_SOCKET, SO_NWRITE, &n, &len) != 0) return -1;
  return n;
}

inline std::string base64(const uint8_t *data, size_t size) {
  static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < size) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < size) v |= data[i + 2];
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(i + 1 < size ? tbl[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < size ? tbl[v & 63] : '=');
  }
  return out;
}

struct Frame {
  std::vector<uint8_t> planes[3];
  uint32_t linesize[3] = {0, 0, 0};
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t timestamp_ns = 0;
  uint64_t wall = 0;
  uint64_t captured_us = 0;
  bool i420 = false;
};

class Feed {
public:
  static Feed &instance() {
    static Feed feed;
    return feed;
  }

  // preview.start {codec:"jpeg"|"h264", width, height, fpsDivisor, quality, pixfmt:"bgra"|"i420", bitrateKbps}
  std::string start(int fd, const std::string &request) {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (running_) return err(-32602, "preview already running; call preview.stop first");
    obs_video_info ovi{};
    if (!obs_get_video_info(&ovi)) return err(-32603, "video pipeline not initialized");

    codec_ = extract_json_string(request, "codec");
    if (codec_.empty()) codec_ = "jpeg";
    if (codec_ != "jpeg" && codec_ != "h264") return err(-32602, "codec must be jpeg or h264");
    width_ = static_cast<uint32_t>(extract_json_int(request, "width").value_or(static_cast<int>(ovi.output_width)));
    height_ = static_cast<uint32_t>(extract_json_int(request, "height").value_or(static_cast<int>(ovi.output_height)));
    divisor_ = static_cast<uint32_t>(std::max(1, extract_json_int(request, "fpsDivisor").value_or(1)));
    quality_ = std::clamp(extract_json_int(request, "quality").value_or(75), 5, 100);
    bitrate_kbps_ = std::max(200, extract_json_int(request, "bitrateKbps").value_or(2500));
    pixfmt_ = extract_json_string(request, "pixfmt");
    if (pixfmt_.empty()) pixfmt_ = "i420";
    if (pixfmt_ != "bgra" && pixfmt_ != "i420") return err(-32602, "pixfmt must be bgra or i420");
    capture_ = extract_json_string(request, "capture");
    if (capture_.empty()) capture_ = "raw";
    if (capture_ != "raw" && capture_ != "gpu") return err(-32602, "capture must be raw or gpu");
    gpu_pipeline_ = extract_json_int(request, "gpuPipeline").value_or(0) != 0;
    if (width_ < 16 || height_ < 16 || width_ > 3840 || height_ > 2160 || (width_ & 1) || (height_ & 1)) {
      return err(-32602, "width/height must be even and within 16..3840x2160");
    }
    reset_stats();
    fd_ = fd;
    started_us_ = mono_us();
    getrusage(RUSAGE_SELF, &rusage_start_);

    if (codec_ == "jpeg" && capture_ == "gpu") {
      // Third shape: read the program texture straight off the graphics context at preview
      // rate (what OBS's own preview display does, minus the window), JPEG it, push it.
      tj_ = tj3Init(TJINIT_COMPRESS);
      if (!tj_) return err(-32603, "turbojpeg init failed");
      tj3Set(tj_, TJPARAM_QUALITY, quality_);
      tj3Set(tj_, TJPARAM_SUBSAMP, TJSAMP_420);
      tj3Set(tj_, TJPARAM_FASTDCT, 1);
      pixfmt_ = "bgra";
      gpu_interval_us_ = static_cast<uint64_t>(1000000.0 * ovi.fps_den * divisor_ / std::max(1u, ovi.fps_num));
      stop_worker_ = false;
      worker_ = std::thread([this] { gpu_loop(); });
      running_ = true;
      return status_json();
    }
    if (codec_ == "jpeg") {
      tj_ = tj3Init(TJINIT_COMPRESS);
      if (!tj_) return err(-32603, "turbojpeg init failed");
      tj3Set(tj_, TJPARAM_QUALITY, quality_);
      tj3Set(tj_, TJPARAM_SUBSAMP, TJSAMP_420);
      tj3Set(tj_, TJPARAM_FASTDCT, 1);
      stop_worker_ = false;
      worker_ = std::thread([this] { worker_loop(); });
      video_scale_info conversion{};
      conversion.format = pixfmt_ == "bgra" ? VIDEO_FORMAT_BGRA : VIDEO_FORMAT_I420;
      conversion.width = width_;
      conversion.height = height_;
      conversion.colorspace = VIDEO_CS_DEFAULT;
      conversion.range = VIDEO_RANGE_DEFAULT;
      obs_add_raw_video_callback2(&conversion, divisor_, &Feed::raw_video_cb, this);
      running_ = true;
      return status_json();
    }

    // h264
    register_output_type();
    std::string encoder_id;
    encoder_ = create_encoder(encoder_id);
    if (!encoder_) return err(-32603, "no h264 encoder could be created");
    encoder_id_ = encoder_id;
    if (width_ != ovi.output_width || height_ != ovi.output_height) {
      obs_encoder_set_scaled_size(encoder_, width_, height_);
      // Without this libobs downscales for the encoder on the CPU (swscale); with it the scale is a GPU pass.
      if (extract_json_int(request, "gpuScale").value_or(1) != 0) obs_encoder_set_gpu_scale_type(encoder_, OBS_SCALE_BICUBIC);
    }
    if (divisor_ > 1) obs_encoder_set_frame_rate_divisor(encoder_, divisor_);
    obs_encoder_set_video(encoder_, obs_get_video());
    output_ = obs_output_create("streammate_proto_preview_output", "proto-preview-output", nullptr, nullptr);
    if (!output_) {
      obs_encoder_release(encoder_);
      encoder_ = nullptr;
      return err(-32603, "preview output create failed");
    }
    obs_output_set_video_encoder(output_, encoder_);
    if (!obs_output_start(output_)) {
      const char *why = obs_output_get_last_error(output_);
      std::string message = why ? why : "unknown";
      obs_output_release(output_);
      obs_encoder_release(encoder_);
      output_ = nullptr;
      encoder_ = nullptr;
      return err(-32603, "preview output start failed: " + message);
    }
    running_ = true;
    return status_json();
  }

  std::string stop() {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (!running_) return "{\"ok\":true,\"running\":false}";
    std::string final_stats = stats_json();
    if (codec_ == "jpeg" && capture_ == "gpu") {
      {
        std::lock_guard<std::mutex> slot_lock(slot_mutex_);
        stop_worker_ = true;
      }
      slot_cv_.notify_all();
      if (worker_.joinable()) worker_.join();
      if (tj_) {
        tj3Destroy(tj_);
        tj_ = nullptr;
      }
    } else if (codec_ == "jpeg") {
      obs_remove_raw_video_callback(&Feed::raw_video_cb, this);
      {
        std::lock_guard<std::mutex> slot_lock(slot_mutex_);
        stop_worker_ = true;
      }
      slot_cv_.notify_all();
      if (worker_.joinable()) worker_.join();
      if (tj_) {
        tj3Destroy(tj_);
        tj_ = nullptr;
      }
    } else {
      if (output_) {
        obs_output_force_stop(output_);
        obs_output_release(output_);
        output_ = nullptr;
      }
      if (encoder_) {
        obs_encoder_release(encoder_);
        encoder_ = nullptr;
      }
    }
    running_ = false;
    return final_stats;
  }

  std::string stats() {
    std::lock_guard<std::mutex> lock(api_mutex_);
    return stats_json();
  }

  // Render program (target null) or a source into a w×h BGRA buffer via texrender + stage surface.
  // With two stage surfaces, this tick stages and the PREVIOUS tick's surface is mapped, so the
  // map never waits on the GPU (one tick of added latency); with one, map follows stage directly.
  struct GpuRing {
    gs_texrender_t *tr = nullptr;
    gs_stagesurf_t *stage[2] = {nullptr, nullptr};
    int write = 0;
    bool primed = false;
    uint32_t w = 0, h = 0;
    void ensure(uint32_t width, uint32_t height, bool pipeline) {
      if (tr && w == width && h == height) return;
      destroy();
      w = width; h = height;
      tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
      stage[0] = gs_stagesurface_create(w, h, GS_BGRA);
      stage[1] = pipeline ? gs_stagesurface_create(w, h, GS_BGRA) : nullptr;
      write = 0; primed = false;
    }
    void destroy() {
      if (tr) gs_texrender_destroy(tr);
      for (auto *&s : stage) { if (s) gs_stagesurface_destroy(s); s = nullptr; }
      tr = nullptr;
    }
  };

  // Caller holds the graphics context. Returns false if nothing was mapped this tick.
  static bool render_bgra(GpuRing &ring, obs_source_t *target, uint32_t src_w, uint32_t src_h, std::vector<uint8_t> &out) {
    gs_texrender_reset(ring.tr);
    if (!gs_texrender_begin(ring.tr, ring.w, ring.h)) return false;
    vec4 clear{};
    gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0.0f, static_cast<float>(src_w), 0.0f, static_cast<float>(src_h), -100.0f, 100.0f);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    if (target) obs_source_video_render(target);
    else obs_render_main_texture();
    gs_blend_state_pop();
    gs_texrender_end(ring.tr);
    gs_stagesurf_t *to_stage = ring.stage[ring.write];
    gs_stage_texture(to_stage, gs_texrender_get_texture(ring.tr));
    gs_stagesurf_t *to_map = to_stage;
    if (ring.stage[1]) {
      // pipelined: map what was staged last tick
      to_map = ring.primed ? ring.stage[ring.write ^ 1] : nullptr;
      ring.write ^= 1;
      ring.primed = true;
      if (!to_map) return false;
    }
    uint8_t *data = nullptr;
    uint32_t linesize = 0;
    if (!gs_stagesurface_map(to_map, &data, &linesize)) return false;
    out.resize(static_cast<size_t>(ring.w) * ring.h * 4);
    for (uint32_t y = 0; y < ring.h; ++y) {
      std::memcpy(out.data() + static_cast<size_t>(y) * ring.w * 4, data + static_cast<size_t>(y) * linesize, static_cast<size_t>(ring.w) * 4);
    }
    gs_stagesurface_unmap(to_map);
    return true;
  }

  void gpu_loop() {
    GpuRing ring;
    std::vector<uint8_t> bgra;
    unsigned char *jpeg = nullptr;
    size_t jpeg_capacity = 0;
    uint64_t next_tick = mono_us();
    while (true) {
      {
        std::unique_lock<std::mutex> lock(slot_mutex_);
        const uint64_t now = mono_us();
        if (next_tick > now) slot_cv_.wait_for(lock, std::chrono::microseconds(next_tick - now), [this] { return stop_worker_; });
        if (stop_worker_) break;
      }
      next_tick += gpu_interval_us_;
      if (next_tick + gpu_interval_us_ < mono_us()) next_tick = mono_us();  // fell behind: resync
      obs_video_info ovi{};
      if (!obs_get_video_info(&ovi)) continue;
      const uint64_t wall = wall_ms();
      const uint64_t t0 = mono_us();
      obs_enter_graphics();
      ring.ensure(width_, height_, gpu_pipeline_);
      const bool got = render_bgra(ring, nullptr, ovi.base_width, ovi.base_height, bgra);
      obs_leave_graphics();
      const uint64_t t1 = mono_us();
      frames_in_++;
      render_us_total_ += (t1 - t0);
      if (!got) continue;
      size_t jpeg_size = jpeg_capacity;
      int rc = tj3Compress8(tj_, bgra.data(), static_cast<int>(width_), static_cast<int>(width_) * 4, static_cast<int>(height_), TJPF_BGRA, &jpeg, &jpeg_size);
      if (jpeg_size > jpeg_capacity) jpeg_capacity = jpeg_size;
      const uint64_t t2 = mono_us();
      if (rc != 0) { encode_failures_++; continue; }
      frames_encoded_++;
      encode_us_total_ += (t2 - t1);
      if (t2 - t1 > encode_us_max_) encode_us_max_ = t2 - t1;
      if (unsent_bytes(fd_) > 8 * 1024 * 1024) { dropped_backpressure_++; continue; }
      std::vector<uint8_t> header;
      header.reserve(32);
      header.push_back('S'); header.push_back(1); header.push_back(1); header.push_back(1);
      put_u32(header, static_cast<uint32_t>(seq_++));
      put_u64(header, wall);
      put_u64(header, os_gettime_ns());
      put_u16(header, static_cast<uint16_t>(width_));
      put_u16(header, static_cast<uint16_t>(height_));
      put_u32(header, static_cast<uint32_t>(t2 - t1));
      const uint64_t s0 = mono_us();
      if (!send_binary_ws(fd_, header, jpeg, jpeg_size)) { send_failures_++; continue; }
      const uint64_t s1 = mono_us();
      send_us_total_ += (s1 - s0);
      pipeline_us_total_ += (s1 - t0);
      frames_sent_++;
      bytes_sent_ += jpeg_size;
    }
    obs_enter_graphics();
    ring.destroy();
    obs_leave_graphics();
    if (jpeg) tj3Free(jpeg);
  }

  // preview.snapshot {width, height, quality} + one of {sceneId} / {sourceId} (neither = program).
  std::string snapshot(const std::string &request, obs_source_t *target, const char *target_kind) {
    obs_video_info ovi{};
    if (!obs_get_video_info(&ovi)) return err(-32603, "video pipeline not initialized");
    uint32_t w = static_cast<uint32_t>(extract_json_int(request, "width").value_or(320));
    uint32_t h = static_cast<uint32_t>(extract_json_int(request, "height").value_or(180));
    int quality = std::clamp(extract_json_int(request, "quality").value_or(70), 5, 100);
    if (w < 8 || h < 8 || w > 1920 || h > 1080) return err(-32602, "snapshot size out of range");

    uint32_t src_w = ovi.base_width;
    uint32_t src_h = ovi.base_height;
    if (target) {
      src_w = obs_source_get_width(target);
      src_h = obs_source_get_height(target);
      if (src_w == 0 || src_h == 0) return err(-32603, "source has no size yet");
    }

    const uint64_t t0 = mono_us();
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    bool rendered = false;
    obs_enter_graphics();
    gs_texrender_t *tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    if (tr && gs_texrender_begin(tr, w, h)) {
      vec4 clear{};
      gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
      gs_ortho(0.0f, static_cast<float>(src_w), 0.0f, static_cast<float>(src_h), -100.0f, 100.0f);
      gs_blend_state_push();
      gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
      if (target) obs_source_video_render(target);
      else obs_render_main_texture();
      gs_blend_state_pop();
      gs_texrender_end(tr);
      gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_BGRA);
      if (stage) {
        gs_stage_texture(stage, gs_texrender_get_texture(tr));
        uint8_t *data = nullptr;
        uint32_t linesize = 0;
        if (gs_stagesurface_map(stage, &data, &linesize)) {
          for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(bgra.data() + static_cast<size_t>(y) * w * 4, data + static_cast<size_t>(y) * linesize, static_cast<size_t>(w) * 4);
          }
          gs_stagesurface_unmap(stage);
          rendered = true;
        }
        gs_stagesurface_destroy(stage);
      }
    }
    if (tr) gs_texrender_destroy(tr);
    obs_leave_graphics();
    const uint64_t t1 = mono_us();
    if (!rendered) return err(-32603, "snapshot render failed");

    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    if (!tj) return err(-32603, "turbojpeg init failed");
    tj3Set(tj, TJPARAM_QUALITY, quality);
    tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_420);
    tj3Set(tj, TJPARAM_FASTDCT, 1);
    unsigned char *jpeg = nullptr;
    size_t jpeg_size = 0;
    int rc = tj3Compress8(tj, bgra.data(), static_cast<int>(w), static_cast<int>(w) * 4, static_cast<int>(h), TJPF_BGRA, &jpeg, &jpeg_size);
    std::string tj_error = rc == 0 ? "" : tj3GetErrorStr(tj);
    tj3Destroy(tj);
    if (rc != 0) return err(-32603, "jpeg encode failed: " + tj_error);
    const uint64_t t2 = mono_us();
    std::string encoded = base64(jpeg, jpeg_size);
    tj3Free(jpeg);
    snapshot_count_++;
    snapshot_render_us_total_ += (t1 - t0);
    snapshot_encode_us_total_ += (t2 - t1);
    return "{\"ok\":true,\"target\":\"" + std::string(target_kind) + "\",\"width\":" + std::to_string(w) +
           ",\"height\":" + std::to_string(h) + ",\"sourceWidth\":" + std::to_string(src_w) +
           ",\"sourceHeight\":" + std::to_string(src_h) + ",\"renderUs\":" + std::to_string(t1 - t0) +
           ",\"encodeUs\":" + std::to_string(t2 - t1) + ",\"bytes\":" + std::to_string(jpeg_size) +
           ",\"jpegBase64\":\"" + encoded + "\"}";
  }

private:
  Feed() = default;

  // ---- jpeg path -------------------------------------------------------------------------
  static void raw_video_cb(void *param, struct video_data *frame) {
    static_cast<Feed *>(param)->on_raw_frame(frame);
  }

  void on_raw_frame(struct video_data *frame) {
    if (!frame || !frame->data[0]) return;
    const uint64_t now_us = mono_us();
    frames_in_++;
    {
      const uint64_t now_ns = os_gettime_ns();
      if (now_ns > frame->timestamp) obs_delay_us_total_ += (now_ns - frame->timestamp) / 1000;
    }
    std::unique_lock<std::mutex> lock(slot_mutex_);
    if (slot_full_) {
      // Worker still busy with the previous picture: keep the newest, count the loss.
      dropped_busy_++;
    }
    Frame &f = slot_;
    f.width = width_;
    f.height = height_;
    f.timestamp_ns = frame->timestamp;
    f.wall = wall_ms();
    f.captured_us = now_us;
    f.i420 = pixfmt_ == "i420";
    const int planes = f.i420 ? 3 : 1;
    for (int p = 0; p < planes; ++p) {
      const uint32_t plane_w = (p == 0 ? width_ : width_ / 2) * (f.i420 ? 1 : 4);
      const uint32_t plane_h = p == 0 ? height_ : height_ / 2;
      f.linesize[p] = plane_w;
      f.planes[p].resize(static_cast<size_t>(plane_w) * plane_h);
      for (uint32_t y = 0; y < plane_h; ++y) {
        std::memcpy(f.planes[p].data() + static_cast<size_t>(y) * plane_w,
                    frame->data[p] + static_cast<size_t>(y) * frame->linesize[p], plane_w);
      }
    }
    copy_us_total_ += (mono_us() - now_us);
    slot_full_ = true;
    lock.unlock();
    slot_cv_.notify_one();
  }

  void worker_loop() {
    Frame local;
    unsigned char *jpeg = nullptr;
    size_t jpeg_capacity = 0;
    while (true) {
      {
        std::unique_lock<std::mutex> lock(slot_mutex_);
        slot_cv_.wait(lock, [this] { return slot_full_ || stop_worker_; });
        if (stop_worker_) break;
        std::swap(local, slot_);
        slot_full_ = false;
      }
      const uint64_t t0 = mono_us();
      size_t jpeg_size = jpeg_capacity;
      int rc;
      if (local.i420) {
        const unsigned char *planes[3] = {local.planes[0].data(), local.planes[1].data(), local.planes[2].data()};
        const int strides[3] = {static_cast<int>(local.linesize[0]), static_cast<int>(local.linesize[1]), static_cast<int>(local.linesize[2])};
        tj3Set(tj_, TJPARAM_NOREALLOC, 0);
        rc = tj3CompressFromYUVPlanes8(tj_, planes, static_cast<int>(local.width), strides, static_cast<int>(local.height), &jpeg, &jpeg_size);
      } else {
        rc = tj3Compress8(tj_, local.planes[0].data(), static_cast<int>(local.width), static_cast<int>(local.linesize[0]),
                          static_cast<int>(local.height), TJPF_BGRA, &jpeg, &jpeg_size);
      }
      if (jpeg_size > jpeg_capacity) jpeg_capacity = jpeg_size;
      const uint64_t t1 = mono_us();
      if (rc != 0) {
        encode_failures_++;
        continue;
      }
      frames_encoded_++;
      const uint64_t encode_us = t1 - t0;
      encode_us_total_ += encode_us;
      if (encode_us > encode_us_max_) encode_us_max_ = encode_us;

      const long queued = unsent_bytes(fd_);
      if (queued > 8 * 1024 * 1024) {
        dropped_backpressure_++;
        continue;
      }
      std::vector<uint8_t> header;
      header.reserve(32);
      header.push_back('S');
      header.push_back(1);
      header.push_back(1);  // every jpeg is a keyframe
      header.push_back(local.i420 ? 2 : 1);
      put_u32(header, static_cast<uint32_t>(seq_++));
      put_u64(header, local.wall);
      put_u64(header, local.timestamp_ns);
      put_u16(header, static_cast<uint16_t>(local.width));
      put_u16(header, static_cast<uint16_t>(local.height));
      put_u32(header, static_cast<uint32_t>(encode_us));
      const uint64_t s0 = mono_us();
      if (!send_binary_ws(fd_, header, jpeg, jpeg_size)) {
        send_failures_++;
        continue;
      }
      const uint64_t s1 = mono_us();
      send_us_total_ += (s1 - s0);
      pipeline_us_total_ += (s1 - local.captured_us);
      frames_sent_++;
      bytes_sent_ += jpeg_size;
    }
    if (jpeg) tj3Free(jpeg);
  }

  // ---- h264 path -------------------------------------------------------------------------
  struct OutputCtx {
    obs_output_t *output;
  };

  static void register_output_type() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    static obs_output_info info{};
    info.id = "streammate_proto_preview_output";
    info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
    info.get_name = [](void *) -> const char * { return "proto preview output"; };
    info.create = [](obs_data_t *, obs_output_t *output) -> void * { return new OutputCtx{output}; };
    info.destroy = [](void *data) { delete static_cast<OutputCtx *>(data); };
    info.start = [](void *data) -> bool {
      auto *ctx = static_cast<OutputCtx *>(data);
      if (!obs_output_can_begin_data_capture(ctx->output, 0)) return false;
      if (!obs_output_initialize_encoders(ctx->output, 0)) return false;
      return obs_output_begin_data_capture(ctx->output, 0);
    };
    info.stop = [](void *data, uint64_t) { obs_output_end_data_capture(static_cast<OutputCtx *>(data)->output); };
    info.encoded_packet = [](void *, struct encoder_packet *packet) { Feed::instance().on_packet(packet); };
    info.encoded_video_codecs = "h264";
    obs_register_output(&info);
  }

  obs_encoder_t *create_encoder(std::string &actual_id) {
    std::vector<std::string> candidates;
    const char *id = nullptr;
    for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
      if (!id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) continue;
      const char *codec = obs_get_encoder_codec(id);
      if (!codec || std::string(codec) != "h264") continue;
      std::string encoder_id(id);
      if (encoder_id.find("videotoolbox") != std::string::npos || encoder_id.find("com.apple") != std::string::npos) {
        candidates.push_back(encoder_id);
      }
    }
    candidates.push_back("obs_x264");
    for (const std::string &candidate : candidates) {
      obs_data_t *settings = obs_data_create();
      obs_data_set_int(settings, "bitrate", bitrate_kbps_);
      obs_data_set_int(settings, "keyint_sec", 1);
      obs_data_set_string(settings, "profile", "main");
      obs_data_set_string(settings, "rate_control", "CBR");
      obs_data_set_bool(settings, "bframes", false);
      // x264-only keys are ignored by VideoToolbox.
      obs_data_set_string(settings, "preset", "veryfast");
      obs_data_set_string(settings, "tune", "zerolatency");
      obs_encoder_t *encoder = obs_video_encoder_create(candidate.c_str(), "proto-preview-video", settings, nullptr);
      obs_data_release(settings);
      if (encoder) {
        actual_id = candidate;
        return encoder;
      }
    }
    return nullptr;
  }

  void on_packet(struct encoder_packet *packet) {
    if (!packet || packet->type != OBS_ENCODER_VIDEO) return;
    frames_in_++;
    frames_encoded_++;
    std::vector<uint8_t> payload;
    if (packet->keyframe && encoder_) {
      uint8_t *extra = nullptr;
      size_t extra_size = 0;
      if (obs_encoder_get_extra_data(encoder_, &extra, &extra_size) && extra && extra_size) {
        payload.insert(payload.end(), extra, extra + extra_size);
      }
    }
    payload.insert(payload.end(), packet->data, packet->data + packet->size);
    const uint64_t pts_ns = packet->timebase_den > 0
        ? static_cast<uint64_t>(packet->pts) * 1000000000ULL * static_cast<uint64_t>(packet->timebase_num) / static_cast<uint64_t>(packet->timebase_den)
        : 0;
    std::vector<uint8_t> header;
    header.reserve(32);
    header.push_back('S');
    header.push_back(2);
    header.push_back(packet->keyframe ? 1 : 0);
    header.push_back(0);
    put_u32(header, static_cast<uint32_t>(seq_++));
    put_u64(header, wall_ms());
    put_u64(header, pts_ns);
    put_u16(header, static_cast<uint16_t>(width_));
    put_u16(header, static_cast<uint16_t>(height_));
    put_u32(header, 0);
    const uint64_t s0 = mono_us();
    if (!send_binary_ws(fd_, header, payload.data(), payload.size())) {
      send_failures_++;
      return;
    }
    send_us_total_ += (mono_us() - s0);
    frames_sent_++;
    bytes_sent_ += payload.size();
    if (packet->keyframe) keyframes_++;
  }

  // ---- bookkeeping -------------------------------------------------------------------------
  void reset_stats() {
    seq_ = 0;
    frames_in_ = frames_encoded_ = frames_sent_ = dropped_busy_ = dropped_backpressure_ = 0;
    encode_failures_ = send_failures_ = keyframes_ = 0;
    bytes_sent_ = encode_us_total_ = encode_us_max_ = send_us_total_ = pipeline_us_total_ = copy_us_total_ = 0;
    snapshot_count_ = snapshot_render_us_total_ = snapshot_encode_us_total_ = 0;
    obs_delay_us_total_ = 0;
    render_us_total_ = 0;
    encoder_id_.clear();
  }

  std::string status_json() const {
    return "{\"ok\":true,\"running\":" + std::string(running_ ? "true" : "false") + ",\"codec\":\"" + codec_ +
           "\",\"width\":" + std::to_string(width_) + ",\"height\":" + std::to_string(height_) +
           ",\"fpsDivisor\":" + std::to_string(divisor_) + ",\"quality\":" + std::to_string(quality_) +
           ",\"pixfmt\":\"" + pixfmt_ + "\",\"capture\":\"" + capture_ + "\",\"gpuPipeline\":" + std::string(gpu_pipeline_ ? "true" : "false") +
           ",\"bitrateKbps\":" + std::to_string(bitrate_kbps_) +
           ",\"encoderId\":\"" + json_escape(encoder_id_) + "\"}";
  }

  std::string stats_json() const {
    rusage now{};
    getrusage(RUSAGE_SELF, &now);
    auto tv_ms = [](const timeval &tv) { return static_cast<uint64_t>(tv.tv_sec) * 1000 + static_cast<uint64_t>(tv.tv_usec) / 1000; };
    const uint64_t cpu_user_ms = tv_ms(now.ru_utime) - tv_ms(rusage_start_.ru_utime);
    const uint64_t cpu_sys_ms = tv_ms(now.ru_stime) - tv_ms(rusage_start_.ru_stime);
    const uint64_t elapsed_ms = running_ ? (mono_us() - started_us_) / 1000 : 0;
    const uint64_t encoded = frames_encoded_.load();
    const uint64_t sent = frames_sent_.load();
    const uint64_t in = frames_in_.load();
    std::string status = status_json();
    status.pop_back();  // strip '}'
    return status + ",\"elapsedMs\":" + std::to_string(elapsed_ms) + ",\"framesIn\":" + std::to_string(in) +
           ",\"framesEncoded\":" + std::to_string(encoded) + ",\"framesSent\":" + std::to_string(sent) +
           ",\"droppedBusy\":" + std::to_string(dropped_busy_.load()) +
           ",\"droppedBackpressure\":" + std::to_string(dropped_backpressure_.load()) +
           ",\"encodeFailures\":" + std::to_string(encode_failures_.load()) +
           ",\"sendFailures\":" + std::to_string(send_failures_.load()) + ",\"keyframes\":" + std::to_string(keyframes_.load()) +
           ",\"bytesSent\":" + std::to_string(bytes_sent_.load()) +
           ",\"avgCopyUs\":" + std::to_string(in ? copy_us_total_.load() / in : 0) +
           ",\"avgEncodeUs\":" + std::to_string(encoded ? encode_us_total_.load() / encoded : 0) +
           ",\"maxEncodeUs\":" + std::to_string(encode_us_max_.load()) +
           ",\"avgSendUs\":" + std::to_string(sent ? send_us_total_.load() / sent : 0) +
           ",\"avgCaptureToSentUs\":" + std::to_string(sent ? pipeline_us_total_.load() / sent : 0) +
           ",\"cpuUserMs\":" + std::to_string(cpu_user_ms) + ",\"cpuSysMs\":" + std::to_string(cpu_sys_ms) +
           ",\"cpuTotalMs\":" + std::to_string(tv_ms(now.ru_utime) + tv_ms(now.ru_stime)) +
           ",\"avgObsDelayUs\":" + std::to_string(in ? obs_delay_us_total_.load() / in : 0) +
           ",\"avgRenderUs\":" + std::to_string(in ? render_us_total_.load() / in : 0) +
           ",\"snapshots\":" + std::to_string(snapshot_count_.load()) +
           ",\"avgSnapshotRenderUs\":" + std::to_string(snapshot_count_ ? snapshot_render_us_total_.load() / snapshot_count_.load() : 0) +
           ",\"avgSnapshotEncodeUs\":" + std::to_string(snapshot_count_ ? snapshot_encode_us_total_.load() / snapshot_count_.load() : 0) + "}";
  }

  std::mutex api_mutex_;
  bool running_ = false;
  int fd_ = -1;
  std::string codec_ = "jpeg";
  std::string pixfmt_ = "i420";
  std::string encoder_id_;
  uint32_t width_ = 0, height_ = 0, divisor_ = 1;
  int quality_ = 75;
  int bitrate_kbps_ = 2500;
  uint64_t started_us_ = 0;
  rusage rusage_start_{};

  tjhandle tj_ = nullptr;
  std::thread worker_;
  std::mutex slot_mutex_;
  std::condition_variable slot_cv_;
  Frame slot_;
  bool slot_full_ = false;
  bool stop_worker_ = false;

  obs_encoder_t *encoder_ = nullptr;
  obs_output_t *output_ = nullptr;

  std::atomic<uint64_t> seq_{0}, frames_in_{0}, frames_encoded_{0}, frames_sent_{0}, dropped_busy_{0}, dropped_backpressure_{0};
  std::atomic<uint64_t> encode_failures_{0}, send_failures_{0}, keyframes_{0}, bytes_sent_{0};
  std::atomic<uint64_t> encode_us_total_{0}, encode_us_max_{0}, send_us_total_{0}, pipeline_us_total_{0}, copy_us_total_{0};
  std::atomic<uint64_t> snapshot_count_{0}, snapshot_render_us_total_{0}, snapshot_encode_us_total_{0};
  std::atomic<uint64_t> obs_delay_us_total_{0}, render_us_total_{0};
  std::string capture_ = "raw";
  bool gpu_pipeline_ = false;
  uint64_t gpu_interval_us_ = 33333;
};

}  // namespace proto_preview
#endif  // STREAMMATE_HAS_LIBOBS
