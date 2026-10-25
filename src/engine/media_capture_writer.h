// Media capture, the browser's side: what a player's demuxer gives its
// decoders, kept as it comes and made into files once the capture stops,
// with the engine's FFmpeg muxers and without decoding anything
// (media_capture_writer.cc).

#ifndef ZEBKIT_ENGINE_MEDIA_CAPTURE_WRITER_H_
#define ZEBKIT_ENGINE_MEDIA_CAPTURE_WRITER_H_

#include <map>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/time/time.h"
#include "base/values.h"
#include "media/base/audio_decoder_config.h"
#include "media/base/ranges.h"
#include "media/base/video_decoder_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "zebkit/engine/media_capture.mojom.h"

namespace zebkit {

struct CaptureOptions {
  // "mp4", "webm", "mkv", or "" to choose by the codecs and the path.
  std::string format;
  // Gaps longer than this between what was captured (a seek forward) are
  // closed up in the file; zero keeps them.
  base::TimeDelta close_gaps = base::Seconds(1);
  // The capture ends, with an error and the files of what it has, once it
  // holds this much, or this much of the media's time; zero: no limit of
  // time.
  int64_t max_bytes = int64_t{16} << 30;
  base::TimeDelta max_duration;
};

// Parses zebkit_media_capture_start()'s options; false with @error.
bool ParseCaptureOptions(const std::string& json,
                         CaptureOptions* options,
                         bool* include_buffered,
                         std::string* error);

// On a sequence that may block; the sink is bound there.
class CaptureWriter : public mojom::MediaCaptureSink {
 public:
  // @report gets the capture's state as JSON: while it captures, and once,
  // with @done, when its files are written.
  using Report = base::RepeatingCallback<void(std::string json, bool done)>;

  CaptureWriter(base::FilePath path,
                CaptureOptions options,
                mojo::PendingReceiver<mojom::MediaCaptureSink> receiver,
                Report report);
  ~CaptureWriter() override;

  // Stops taking packets and writes the files; @error, if any, is reported
  // with them.
  void Finish(const std::string& reason, const std::string& error);

  // mojom::MediaCaptureSink
  void OnAudioConfig(uint32_t track,
                     const media::AudioDecoderConfig& config) override;
  void OnVideoConfig(uint32_t track,
                     const media::VideoDecoderConfig& config) override;
  void OnPacket(mojom::MediaPacketPtr packet) override;
  void OnEnded(const std::string& reason) override;

 private:
  struct Packet {
    uint32_t track;
    int part;
    int64_t pts;
    int64_t dts;
    int64_t duration;
    bool keyframe;
    int64_t offset;
    uint32_t size;
  };
  // A track is audio or video from its first configuration on.
  struct Track {
    bool audio = false;
    std::vector<media::AudioDecoderConfig> audio_configs;
    std::vector<media::VideoDecoderConfig> video_configs;
    int64_t last_pts = mojom::kUnknown;
    media::Ranges<base::TimeDelta> captured;
  };
  // A file of the capture: the configuration of each track in it. A track
  // whose configuration changes (another resolution, another codec) starts
  // another file, since a track of MP4 or Matroska has one.
  using Part = std::map<uint32_t, int>;

  // The track @id of the kind given, made if it is new; nullptr, the
  // capture failed, when the renderer goes beyond what a capture has.
  Track* TrackOf(uint32_t id, bool audio);
  // The track's configuration @index of its own kind, or nullptr.
  static const media::AudioDecoderConfig* AudioConfig(const Track& track,
                                                      int index);
  static const media::VideoDecoderConfig* VideoConfig(const Track& track,
                                                      int index);
  void SwitchConfig(uint32_t track, int config);
  // Closes the spool, and deletes it where it still has a name (Windows).
  void CloseSpool();
  void Fail(const std::string& error);
  media::Ranges<base::TimeDelta> Captured() const;
  base::DictValue State(const char* state) const;
  void ReportProgress(bool now);
  // Writes part @part; false with @error. @file gets its description.
  bool WritePart(int part,
                 const base::FilePath& path,
                 base::DictValue* file,
                 std::string* error);

  const base::FilePath path_;
  const CaptureOptions options_;
  mojo::Receiver<mojom::MediaCaptureSink> receiver_;
  Report report_;
  base::FilePath spool_path_;
  base::File spool_;
  int64_t spool_size_ = 0;
  std::map<uint32_t, Track> tracks_;
  std::vector<Part> parts_;
  int current_part_ = 0;
  std::vector<Packet> packets_;
  bool finished_ = false;
  std::string error_;
  base::TimeTicks last_report_;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_MEDIA_CAPTURE_WRITER_H_
