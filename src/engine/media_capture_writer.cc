// What a capture is made into. Packets come as the player demuxes them:
// in decode order within a run, but a seek starts another run anywhere,
// and Media Source Extensions players append what they like (the same
// time again in another quality, the next segment of audio before the
// video's). So they are kept in a file beside the destination as they
// come, with an index, and only put in order once the capture stops:
//
// - each track's packets in groups from a keyframe to the next, the groups
//   sorted by time, and a group that overlaps one before it dropped: the
//   time it covers is in the file once;
// - gaps longer than CaptureOptions::close_gaps between what was captured
//   (a seek forward) closed up in every track at once, so that the file
//   plays through and keeps audio and video together;
// - decode times from the stream where the demuxer has them (Media Source
//   Extensions), else made from the presentation times (FFmpeg's demuxer
//   gives the decoders those only), as an MP4 muxer would;
// - the codec's configuration from its keyframes where the stream has it
//   in band (H.264, HEVC and AV1 through Media Source Extensions), AAC
//   without the ADTS headers Chromium adds to it.
//
// A track whose configuration changes starts another file (a part), since
// a track of MP4 and Matroska has one configuration.

#include "zebkit/engine/media_capture_writer.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "media/base/audio_codecs.h"
#include "media/base/video_codecs.h"
#include "media/ffmpeg/ffmpeg_common.h"

namespace zebkit {

namespace {

constexpr int64_t kUnknown = mojom::kUnknown;
// Packets' times are microseconds.
constexpr AVRational kMicroseconds = {1, 1000000};

double ToSeconds(int64_t us) {
  return static_cast<double>(us) / 1e6;
}

base::ListValue RangesValue(const media::Ranges<base::TimeDelta>& ranges) {
  base::ListValue list;
  for (size_t i = 0; i < ranges.size(); ++i) {
    list.Append(base::ListValue()
                    .Append(ranges.start(i).InSecondsF())
                    .Append(ranges.end(i).InSecondsF()));
  }
  return list;
}

double Length(const media::Ranges<base::TimeDelta>& ranges) {
  double total = 0;
  for (size_t i = 0; i < ranges.size(); ++i) {
    total += (ranges.end(i) - ranges.start(i)).InSecondsF();
  }
  return total;
}

// ---- Bitstreams ---------------------------------------------------------

bool IsAnnexB(base::span<const uint8_t> data) {
  return (data.size() > 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
         (data.size() > 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
          data[3] == 1);
}

// The NAL units of an Annex B stream, without their start codes.
std::vector<base::span<const uint8_t>> AnnexBUnits(
    base::span<const uint8_t> data) {
  std::vector<base::span<const uint8_t>> units;
  const size_t none = std::numeric_limits<size_t>::max();
  size_t start = none;
  size_t i = 0;
  auto close = [&](size_t end) {
    if (start == none) {
      return;
    }
    while (end > start && data[end - 1] == 0) {
      --end;
    }
    if (end > start) {
      units.push_back(data.subspan(start, end - start));
    }
  };
  while (i + 2 < data.size()) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      close(i);
      i += 3;
      start = i;
      continue;
    }
    ++i;
  }
  close(data.size());
  return units;
}

// The parameter sets of an H.264 or HEVC keyframe in Annex B, as Annex B:
// what FFmpeg's muxers take as the codec's configuration.
std::vector<uint8_t> ParameterSets(base::span<const uint8_t> keyframe,
                                   bool hevc) {
  std::vector<uint8_t> out;
  for (base::span<const uint8_t> unit : AnnexBUnits(keyframe)) {
    const int type = hevc ? (unit[0] >> 1) & 0x3f : unit[0] & 0x1f;
    const bool wanted = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
    if (wanted) {
      out.insert(out.end(), {0, 0, 0, 1});
      out.insert(out.end(), unit.begin(), unit.end());
    }
  }
  return out;
}

// Annex B to the length-prefixed units an avcC or hvcC configuration says.
std::vector<uint8_t> ToLengthPrefixed(base::span<const uint8_t> data,
                                      int length_size) {
  std::vector<uint8_t> out;
  out.reserve(data.size() + 16);
  for (base::span<const uint8_t> unit : AnnexBUnits(data)) {
    for (int i = length_size - 1; i >= 0; --i) {
      out.push_back(static_cast<uint8_t>(unit.size() >> (8 * i)));
    }
    out.insert(out.end(), unit.begin(), unit.end());
  }
  return out;
}

// The sequence header OBU of an AV1 temporal unit, which is what FFmpeg's
// muxers make the av1C configuration of.
std::vector<uint8_t> Av1SequenceHeader(base::span<const uint8_t> data) {
  size_t i = 0;
  while (i < data.size()) {
    const uint8_t header = data[i];
    const int type = (header >> 3) & 0x0f;
    const bool extension = header & 0x04;
    const bool has_size = header & 0x02;
    size_t pos = i + 1 + (extension ? 1 : 0);
    uint64_t size = 0;
    if (has_size) {
      int shift = 0;
      while (pos < data.size()) {
        const uint8_t byte = data[pos++];
        size |= uint64_t{byte & 0x7fu} << shift;
        shift += 7;
        if (!(byte & 0x80) || shift > 56) {
          break;
        }
      }
    } else {
      size = data.size() - pos;
    }
    if (pos + size > data.size()) {
      break;
    }
    if (type == 1) {
      auto obu = data.subspan(i, pos + size - i);
      return std::vector<uint8_t>(obu.begin(), obu.end());
    }
    i = pos + size;
  }
  return {};
}

// The length of an ADTS header at the start of @data, or 0.
size_t AdtsHeaderSize(base::span<const uint8_t> data) {
  if (data.size() < 7 || data[0] != 0xff || (data[1] & 0xf6) != 0xf0) {
    return 0;
  }
  const size_t frame = ((data[3] & 0x03u) << 11) | (data[4] << 3) |
                       (data[5] >> 5);
  const size_t header = (data[1] & 0x01) ? 7 : 9;
  return frame == data.size() && header < data.size() ? header : 0;
}

// AudioSpecificConfig from an ADTS header.
std::vector<uint8_t> AdtsToConfig(base::span<const uint8_t> data) {
  const int object = (data[2] >> 6) + 1;
  const int rate = (data[2] >> 2) & 0x0f;
  const int channels = ((data[2] & 0x01) << 2) | (data[3] >> 6);
  const int config = (object << 11) | (rate << 7) | (channels << 3);
  return {static_cast<uint8_t>(config >> 8), static_cast<uint8_t>(config)};
}

// ---- Containers ---------------------------------------------------------

struct Container {
  const char* format;      // FFmpeg's muxer
  const char* name;        // as the JSON says it
  const char* extension;
  const char* audio_extension;
};

constexpr Container kMp4 = {"mp4", "mp4", ".mp4", ".m4a"};
constexpr Container kWebm = {"webm", "webm", ".webm", ".webm"};
constexpr Container kMatroska = {"matroska", "mkv", ".mkv", ".mka"};

bool Holds(const Container& container, const std::vector<AVCodecID>& codecs) {
  const AVOutputFormat* format =
      av_guess_format(container.format, nullptr, nullptr);
  if (!format) {
    return false;
  }
  for (AVCodecID codec : codecs) {
    if (avformat_query_codec(format, codec, FF_COMPLIANCE_NORMAL) != 1) {
      return false;
    }
  }
  return true;
}

const Container* ContainerNamed(std::string name) {
  name = base::ToLowerASCII(name);
  if (name == "mp4" || name == ".mp4" || name == ".m4a" || name == ".m4v" ||
      name == ".mov") {
    return &kMp4;
  }
  if (name == "webm" || name == ".webm" || name == ".weba") {
    return &kWebm;
  }
  if (name == "mkv" || name == "matroska" || name == ".mkv" ||
      name == ".mka") {
    return &kMatroska;
  }
  return nullptr;
}

// The container asked for, else the path's, when it holds the codecs; else
// WebM for WebM's codecs, MP4 for MP4's, Matroska for the rest.
const Container& ChooseContainer(const std::string& requested,
                                 const base::FilePath& path,
                                 const std::vector<AVCodecID>& codecs) {
  for (const Container* wanted :
       {ContainerNamed(requested), ContainerNamed(path.Extension())}) {
    if (wanted && Holds(*wanted, codecs)) {
      return *wanted;
    }
  }
  bool webm = true;
  for (AVCodecID codec : codecs) {
    webm &= codec == AV_CODEC_ID_VP8 || codec == AV_CODEC_ID_VP9 ||
            codec == AV_CODEC_ID_AV1 || codec == AV_CODEC_ID_OPUS ||
            codec == AV_CODEC_ID_VORBIS;
  }
  if (webm && Holds(kWebm, codecs)) {
    return kWebm;
  }
  if (Holds(kMp4, codecs)) {
    return kMp4;
  }
  return kMatroska;
}

// @path with the container's extension, unless it has one that names it.
base::FilePath WithExtension(const base::FilePath& path,
                             const Container& container,
                             bool audio_only) {
  if (ContainerNamed(path.Extension()) == &container) {
    return path;
  }
  return path.ReplaceExtension(audio_only ? container.audio_extension
                                          : container.extension);
}

// The @index'th file of a capture (0 is the path itself): "name-2.mp4".
base::FilePath PartPath(const base::FilePath& path, size_t index) {
  if (index == 0) {
    return path;
  }
  return path.InsertBeforeExtensionASCII("-" + base::NumberToString(index + 1));
}

struct FormatContextDeleter {
  void operator()(AVFormatContext* context) const {
    if (!context) {
      return;
    }
    if (context->pb) {
      avio_closep(&context->pb);
    }
    avformat_free_context(context);
  }
};

}  // namespace

bool ParseCaptureOptions(const std::string& json,
                         CaptureOptions* options,
                         bool* include_buffered,
                         std::string* error) {
  *include_buffered = false;
  if (json.empty()) {
    return true;
  }
  std::optional<base::Value> value = base::JSONReader::Read(json, 0);
  if (!value || !value->is_dict()) {
    *error = "the options are not a JSON object";
    return false;
  }
  const base::DictValue& dict = value->GetDict();
  if (const std::string* format = dict.FindString("format")) {
    if (!format->empty() && *format != "auto") {
      if (!ContainerNamed(*format)) {
        *error = "format is one of \"mp4\", \"webm\", \"mkv\", \"auto\"";
        return false;
      }
      options->format = *format;
    }
  }
  *include_buffered = dict.FindBool("includeBuffered").value_or(false);
  if (std::optional<double> gaps = dict.FindDouble("closeGaps")) {
    options->close_gaps = base::Seconds(std::max(0.0, *gaps));
  } else if (std::optional<bool> close = dict.FindBool("closeGaps")) {
    options->close_gaps = *close ? base::Seconds(1) : base::TimeDelta();
  }
  return true;
}

CaptureWriter::CaptureWriter(
    base::FilePath path,
    CaptureOptions options,
    mojo::PendingReceiver<mojom::MediaCaptureSink> receiver,
    Report report)
    : path_(std::move(path)),
      options_(std::move(options)),
      receiver_(this, std::move(receiver)),
      report_(std::move(report)),
      parts_(1) {
  receiver_.set_disconnect_handler(base::BindOnce(
      &CaptureWriter::Finish, base::Unretained(this),
      std::string("the page went away"), std::string()));
  spool_path_ = path_.AddExtensionASCII("capture");
  base::CreateDirectory(path_.DirName());
  spool_.Initialize(spool_path_, base::File::FLAG_CREATE_ALWAYS |
                                     base::File::FLAG_READ |
                                     base::File::FLAG_WRITE);
  if (!spool_.IsValid()) {
    error_ = "cannot write " + spool_path_.AsUTF8Unsafe() + ": " +
             base::File::ErrorToString(spool_.error_details());
  }
}

CaptureWriter::~CaptureWriter() {
  if (spool_.IsValid()) {
    spool_.Close();
    base::DeleteFile(spool_path_);
  }
}

void CaptureWriter::Fail(const std::string& error) {
  if (error_.empty()) {
    error_ = error;
  }
  Finish("error", std::string());
}

void CaptureWriter::OnAudioConfig(uint32_t track,
                                  const media::AudioDecoderConfig& config) {
  if (finished_) {
    return;
  }
  if (config.is_encrypted()) {
    Fail("the media is encrypted (Encrypted Media Extensions) and cannot be "
         "captured");
    return;
  }
  Track& entry = tracks_[track];
  entry.audio = true;
  int index = -1;
  for (size_t i = 0; i < entry.audio_configs.size(); ++i) {
    if (entry.audio_configs[i].Matches(config)) {
      index = static_cast<int>(i);
    }
  }
  if (index < 0) {
    entry.audio_configs.push_back(config);
    index = static_cast<int>(entry.audio_configs.size()) - 1;
  }
  SwitchConfig(track, index);
}

void CaptureWriter::OnVideoConfig(uint32_t track,
                                  const media::VideoDecoderConfig& config) {
  if (finished_) {
    return;
  }
  if (config.is_encrypted()) {
    Fail("the media is encrypted (Encrypted Media Extensions) and cannot be "
         "captured");
    return;
  }
  Track& entry = tracks_[track];
  int index = -1;
  for (size_t i = 0; i < entry.video_configs.size(); ++i) {
    if (entry.video_configs[i].Matches(config)) {
      index = static_cast<int>(i);
    }
  }
  if (index < 0) {
    entry.video_configs.push_back(config);
    index = static_cast<int>(entry.video_configs.size()) - 1;
  }
  SwitchConfig(track, index);
}

void CaptureWriter::SwitchConfig(uint32_t track, int config) {
  Part& current = parts_[current_part_];
  auto it = current.find(track);
  if (it == current.end()) {
    // A track the file did not have yet joins it.
    current[track] = config;
    return;
  }
  if (it->second == config) {
    return;
  }
  Part next = current;
  next[track] = config;
  for (size_t i = 0; i < parts_.size(); ++i) {
    if (parts_[i] == next) {
      current_part_ = static_cast<int>(i);
      return;
    }
  }
  parts_.push_back(std::move(next));
  current_part_ = static_cast<int>(parts_.size()) - 1;
}

void CaptureWriter::OnPacket(mojom::MediaPacketPtr packet) {
  if (finished_ || !spool_.IsValid() || packet->pts_us == kUnknown) {
    return;
  }
  auto track = tracks_.find(packet->track);
  if (track == tracks_.end() ||
      !parts_[current_part_].contains(packet->track)) {
    return;
  }
  base::span<const uint8_t> data(packet->data);
  if (data.empty()) {
    return;
  }
  if (!spool_.WriteAtCurrentPos(data)) {
    Fail("cannot write " + spool_path_.AsUTF8Unsafe());
    return;
  }
  packets_.push_back({packet->track, current_part_, packet->pts_us,
                      packet->dts_us, packet->duration_us, packet->keyframe,
                      spool_size_, static_cast<uint32_t>(data.size())});
  spool_size_ += data.size();

  // What is captured, for the application: from each packet to the next
  // of the track, unless they are far apart (a seek).
  Track& entry = track->second;
  const int64_t pts = packet->pts_us;
  int64_t end = pts + std::max<int64_t>(packet->duration_us, 1000);
  if (packet->duration_us == kUnknown) {
    end = pts + 1000;
  }
  if (entry.last_pts != kUnknown && pts > entry.last_pts &&
      pts - entry.last_pts < 2'000'000) {
    entry.captured.Add(base::Microseconds(entry.last_pts),
                       base::Microseconds(end));
  } else {
    entry.captured.Add(base::Microseconds(pts), base::Microseconds(end));
  }
  if (entry.last_pts == kUnknown || pts > entry.last_pts) {
    entry.last_pts = pts;
  }
  ReportProgress(false);
}

void CaptureWriter::OnEnded(const std::string& reason) {
  Finish(reason, std::string());
}

media::Ranges<base::TimeDelta> CaptureWriter::Captured() const {
  media::Ranges<base::TimeDelta> all;
  for (const auto& [id, track] : tracks_) {
    for (size_t i = 0; i < track.captured.size(); ++i) {
      all.Add(track.captured.start(i), track.captured.end(i));
    }
  }
  return all;
}

base::DictValue CaptureWriter::State(const char* state) const {
  base::DictValue value;
  value.Set("state", state);
  value.Set("bytes", static_cast<double>(spool_size_));
  value.Set("packets", static_cast<int>(packets_.size()));
  const media::Ranges<base::TimeDelta> captured = Captured();
  value.Set("duration", Length(captured));
  value.Set("ranges", RangesValue(captured));
  base::ListValue tracks;
  for (const auto& [id, track] : tracks_) {
    base::DictValue entry;
    entry.Set("track", static_cast<int>(id));
    if (track.audio && !track.audio_configs.empty()) {
      const media::AudioDecoderConfig& config = track.audio_configs.back();
      entry.Set("type", "audio");
      entry.Set("codec", media::GetCodecName(config.codec()));
      entry.Set("sampleRate", config.samples_per_second());
      entry.Set("channels", config.channels());
    } else if (!track.video_configs.empty()) {
      const media::VideoDecoderConfig& config = track.video_configs.back();
      entry.Set("type", "video");
      entry.Set("codec", media::GetCodecName(config.codec()));
      entry.Set("width", config.visible_rect().width());
      entry.Set("height", config.visible_rect().height());
    }
    entry.Set("ranges", RangesValue(track.captured));
    tracks.Append(std::move(entry));
  }
  value.Set("tracks", std::move(tracks));
  if (!error_.empty()) {
    value.Set("error", error_);
  }
  return value;
}

void CaptureWriter::ReportProgress(bool now) {
  const base::TimeTicks time = base::TimeTicks::Now();
  if (!now && time - last_report_ < base::Milliseconds(500)) {
    return;
  }
  last_report_ = time;
  report_.Run(base::WriteJson(State("capturing")).value_or("{}"), false);
}

void CaptureWriter::Finish(const std::string& reason,
                           const std::string& error) {
  if (finished_) {
    return;
  }
  finished_ = true;
  receiver_.reset();
  if (!error.empty() && error_.empty()) {
    error_ = error;
  }

  base::DictValue done = State("done");
  done.Set("reason", reason);
  base::ListValue files;
  double bytes = 0;
  std::string write_error;
  size_t index = 0;
  for (size_t part = 0; part < parts_.size(); ++part) {
    bool any = false;
    for (const Packet& packet : packets_) {
      if (packet.part == static_cast<int>(part)) {
        any = true;
        break;
      }
    }
    if (!any) {
      continue;
    }
    base::DictValue file;
    std::string part_error;
    if (WritePart(static_cast<int>(part), PartPath(path_, index), &file,
                  &part_error)) {
      bytes += file.FindDouble("bytes").value_or(0);
      files.Append(std::move(file));
      ++index;
    } else if (write_error.empty()) {
      write_error = part_error;
    }
  }
  if (error_.empty() && !write_error.empty()) {
    error_ = write_error;
  }
  if (error_.empty() && files.empty()) {
    error_ = packets_.empty() ? "nothing was captured: the player demuxed "
                                "nothing while the capture ran"
                              : "nothing that plays was captured (no "
                                "keyframe)";
  }
  if (!error_.empty()) {
    done.Set("error", error_);
  }
  done.Set("bytes", bytes);
  done.Set("files", std::move(files));
  if (spool_.IsValid()) {
    spool_.Close();
    base::DeleteFile(spool_path_);
  }
  report_.Run(base::WriteJson(done).value_or("{}"), true);
}

bool CaptureWriter::WritePart(int part,
                              const base::FilePath& requested_path,
                              base::DictValue* file,
                              std::string* error) {
  const Part& configs = parts_[part];

  // What each track has in this part, in the order it came.
  std::map<uint32_t, std::vector<size_t>> arrived;
  for (size_t i = 0; i < packets_.size(); ++i) {
    if (packets_[i].part == part) {
      arrived[packets_[i].track].push_back(i);
    }
  }

  // A track whose configuration never changed came in the part current
  // when it arrived, which for what a player appends ahead is not always
  // the part of its time: its packets go with the time of the tracks that
  // changed.
  if (parts_.size() > 1) {
    std::vector<int64_t> times;
    for (const Packet& packet : packets_) {
      const Track& track = tracks_.at(packet.track);
      const size_t count = track.audio ? track.audio_configs.size()
                                       : track.video_configs.size();
      if (packet.part == part && count > 1) {
        times.push_back(packet.pts);
      }
    }
    std::sort(times.begin(), times.end());
    // Packets a few seconds apart are the same stretch.
    media::Ranges<base::TimeDelta> stretch;
    for (size_t i = 0; i < times.size();) {
      size_t j = i;
      while (j + 1 < times.size() && times[j + 1] - times[j] < 3'000'000) {
        ++j;
      }
      stretch.Add(base::Microseconds(times[i]),
                  base::Microseconds(times[j] + 1));
      i = j + 1;
    }
    if (stretch.size()) {
      for (const auto& [id, config] : configs) {
        const Track& track = tracks_.at(id);
        const size_t count = track.audio ? track.audio_configs.size()
                                         : track.video_configs.size();
        if (count != 1) {
          continue;
        }
        std::vector<size_t>& indices = arrived[id];
        indices.clear();
        for (size_t i = 0; i < packets_.size(); ++i) {
          const Packet& packet = packets_[i];
          if (packet.track != id) {
            continue;
          }
          const base::TimeDelta t = base::Microseconds(packet.pts);
          for (size_t r = 0; r < stretch.size(); ++r) {
            if (t >= stretch.start(r) &&
                t < stretch.end(r) + base::Milliseconds(500)) {
              indices.push_back(i);
              break;
            }
          }
        }
      }
    }
  }

  struct Group {
    int64_t key_pts;
    int64_t start;
    int64_t end;
    std::vector<size_t> packets;
  };
  struct Output {
    uint32_t track;
    bool audio;
    AVCodecID codec;
    std::vector<Group> groups;
    // The packets as written, with their times in the file.
    std::vector<size_t> packets;
    std::vector<int64_t> pts;
    std::vector<int64_t> dts;
    int64_t base_shift = 0;
    AVStream* stream = nullptr;
    // The length of an avcC's or hvcC's units, for Annex B packets.
    int length_size = 0;
    bool strip_adts = false;
  };
  std::vector<Output> outputs;
  std::vector<AVCodecID> codecs;
  bool audio_only = true;

  for (auto& [id, indices] : arrived) {
    const Track& track = tracks_.at(id);
    auto config = configs.find(id);
    if (config == configs.end()) {
      continue;
    }
    Output output;
    output.track = id;
    output.audio = track.audio;
    output.codec =
        track.audio
            ? media::AudioCodecToCodecID(
                  track.audio_configs[config->second].codec(),
                  track.audio_configs[config->second].sample_format())
            : media::VideoCodecToCodecID(
                  track.video_configs[config->second].codec());
    if (output.codec == AV_CODEC_ID_NONE) {
      continue;
    }
    // From a keyframe to the next.
    for (size_t i : indices) {
      const Packet& packet = packets_[i];
      if (packet.keyframe || track.audio) {
        output.groups.push_back({packet.pts, packet.pts, packet.pts, {}});
      } else if (output.groups.empty()) {
        continue;
      }
      Group& group = output.groups.back();
      group.packets.push_back(i);
      group.start = std::min(group.start, packet.pts);
      const int64_t length = packet.duration > 0 ? packet.duration : 0;
      group.end = std::max(group.end, packet.pts + length);
    }
    std::stable_sort(
        output.groups.begin(), output.groups.end(),
        [](const Group& a, const Group& b) { return a.key_pts < b.key_pts; });
    // The time a group covers once: one that starts before the end of the
    // one kept before it is the same time again.
    std::vector<Group> kept;
    for (Group& group : output.groups) {
      if (!kept.empty()) {
        Group& last = kept.back();
        // The same group again, whole where the first was cut short (a
        // player that suspends and seeks back to where it was).
        if (group.key_pts == last.key_pts) {
          if (group.packets.size() > last.packets.size()) {
            last = std::move(group);
          }
          continue;
        }
        const int64_t slack = track.audio ? 2000 : 1000;
        if (group.key_pts < last.key_pts || group.start < last.end - slack) {
          continue;
        }
      }
      kept.push_back(std::move(group));
    }
    output.groups = std::move(kept);
    if (output.groups.empty()) {
      continue;
    }
    codecs.push_back(output.codec);
    audio_only &= track.audio;
    outputs.push_back(std::move(output));
  }
  if (outputs.empty()) {
    *error = "nothing that plays was captured (no keyframe)";
    return false;
  }

  // Gaps between what was captured, in every track at once.
  std::vector<std::pair<int64_t, int64_t>> gaps;  // (at, length)
  if (options_.close_gaps.is_positive()) {
    std::vector<std::pair<int64_t, int64_t>> spans;
    for (const Output& output : outputs) {
      for (const Group& group : output.groups) {
        spans.emplace_back(group.start, std::max(group.end, group.start));
      }
    }
    std::sort(spans.begin(), spans.end());
    int64_t reach = spans.front().second;
    const int64_t limit = options_.close_gaps.InMicroseconds();
    for (size_t i = 1; i < spans.size(); ++i) {
      if (spans[i].first - reach > limit) {
        gaps.emplace_back(spans[i].first, spans[i].first - reach);
      }
      reach = std::max(reach, spans[i].second);
    }
  }
  auto shift_at = [&](int64_t t) {
    int64_t shift = 0;
    for (const auto& [at, length] : gaps) {
      if (t >= at) {
        shift += length;
      }
    }
    return shift;
  };

  // Times in the file: the gaps closed, decode times made where the
  // demuxer did not give them.
  for (Output& output : outputs) {
    bool known = true;
    for (const Group& group : output.groups) {
      for (size_t i : group.packets) {
        output.packets.push_back(i);
        known &= packets_[i].dts != kUnknown;
      }
    }
    std::vector<int64_t> shifts;
    for (const Group& group : output.groups) {
      const int64_t shift = shift_at(group.start);
      for (size_t n = 0; n < group.packets.size(); ++n) {
        shifts.push_back(shift);
      }
    }
    const size_t count = output.packets.size();
    output.pts.resize(count);
    output.dts.resize(count);
    for (size_t n = 0; n < count; ++n) {
      output.pts[n] = packets_[output.packets[n]].pts - shifts[n];
    }
    if (output.audio) {
      output.dts = output.pts;
    } else if (known) {
      for (size_t n = 0; n < count; ++n) {
        output.dts[n] = packets_[output.packets[n]].dts - shifts[n];
      }
    } else {
      // The n'th decode time is the n'th presentation time in order, as
      // early as the frames shown before their decode allow.
      std::vector<int64_t> sorted = output.pts;
      std::sort(sorted.begin(), sorted.end());
      int64_t delay = 0;
      for (size_t n = 0; n < count; ++n) {
        delay = std::max(delay, sorted[n] - output.pts[n]);
      }
      for (size_t n = 0; n < count; ++n) {
        output.dts[n] = sorted[n] - delay;
      }
    }
    // Strictly increasing, and never after the presentation time.
    for (int pass = 0; pass < 2; ++pass) {
      int64_t excess = 0;
      for (size_t n = 1; n < count; ++n) {
        output.dts[n] = std::max(output.dts[n], output.dts[n - 1] + 1);
      }
      for (size_t n = 0; n < count; ++n) {
        excess = std::max(excess, output.dts[n] - output.pts[n]);
      }
      if (!excess) {
        break;
      }
      for (int64_t& dts : output.dts) {
        dts -= excess;
      }
    }
  }
  // The file starts at zero; where it started in the stream is reported.
  int64_t first_dts = std::numeric_limits<int64_t>::max();
  int64_t first_pts = std::numeric_limits<int64_t>::max();
  int64_t last_end = std::numeric_limits<int64_t>::min();
  for (const Output& output : outputs) {
    first_dts = std::min(first_dts, output.dts.front());
    for (size_t n = 0; n < output.pts.size(); ++n) {
      first_pts = std::min(first_pts, output.pts[n]);
      const int64_t length =
          std::max<int64_t>(packets_[output.packets[n]].duration, 0);
      last_end = std::max(last_end, output.pts[n] + length);
    }
  }

  const Container& container =
      ChooseContainer(options_.format, requested_path, codecs);
  const base::FilePath path =
      WithExtension(requested_path, container, audio_only);

  AVFormatContext* raw = nullptr;
  if (avformat_alloc_output_context2(&raw, nullptr, container.format,
                                     path.value().c_str()) < 0 ||
      !raw) {
    *error = std::string("FFmpeg has no ") + container.format + " muxer";
    return false;
  }
  std::unique_ptr<AVFormatContext, FormatContextDeleter> context(raw);

  std::vector<uint8_t> buffer;
  auto read = [&](const Packet& packet) -> base::span<const uint8_t> {
    buffer.resize(packet.size);
    if (spool_.Read(packet.offset, buffer) != packet.size) {
      return {};
    }
    return buffer;
  };

  base::ListValue tracks_value;
  for (Output& output : outputs) {
    const Track& track = tracks_.at(output.track);
    const int config = configs.at(output.track);
    AVStream* stream = avformat_new_stream(context.get(), nullptr);
    if (!stream) {
      *error = "FFmpeg could not add a stream";
      return false;
    }
    output.stream = stream;
    AVCodecParameters* parameters = stream->codecpar;
    parameters->codec_id = output.codec;
    std::vector<uint8_t> extra;
    base::DictValue track_value;
    if (output.audio) {
      const media::AudioDecoderConfig& audio = track.audio_configs[config];
      parameters->codec_type = AVMEDIA_TYPE_AUDIO;
      parameters->sample_rate = audio.samples_per_second();
      av_channel_layout_default(&parameters->ch_layout, audio.channels());
      extra = audio.extra_data();
      if (output.codec == AV_CODEC_ID_OPUS && audio.codec_delay() > 0) {
        parameters->initial_padding = audio.codec_delay();
      }
      if (output.codec == AV_CODEC_ID_AAC) {
        base::span<const uint8_t> first = read(packets_[output.packets[0]]);
        if (AdtsHeaderSize(first)) {
          output.strip_adts = true;
          if (extra.empty()) {
            extra = AdtsToConfig(first);
          }
        }
      }
      track_value.Set("type", "audio");
      track_value.Set("codec", media::GetCodecName(audio.codec()));
      track_value.Set("sampleRate", audio.samples_per_second());
      track_value.Set("channels", audio.channels());
    } else {
      const media::VideoDecoderConfig& video = track.video_configs[config];
      parameters->codec_type = AVMEDIA_TYPE_VIDEO;
      parameters->width = video.visible_rect().width();
      parameters->height = video.visible_rect().height();
      const gfx::Size natural = video.natural_size();
      if (natural.width() > 0 && natural.height() > 0 &&
          natural != video.visible_rect().size()) {
        av_reduce(&parameters->sample_aspect_ratio.num,
                  &parameters->sample_aspect_ratio.den,
                  int64_t{natural.width()} * parameters->height,
                  int64_t{parameters->width} * natural.height(), 1 << 30);
      }
      switch (video.profile()) {
        case media::VP9PROFILE_PROFILE0:
          parameters->profile = 0;
          break;
        case media::VP9PROFILE_PROFILE1:
          parameters->profile = 1;
          break;
        case media::VP9PROFILE_PROFILE2:
          parameters->profile = 2;
          break;
        case media::VP9PROFILE_PROFILE3:
          parameters->profile = 3;
          break;
        default:
          break;
      }
      extra = video.extra_data();
      const bool hevc = output.codec == AV_CODEC_ID_HEVC;
      if (output.codec == AV_CODEC_ID_H264 || hevc) {
        base::span<const uint8_t> first = read(packets_[output.packets[0]]);
        if (IsAnnexB(first)) {
          if (extra.empty()) {
            // Annex B: the muxers make avcC (hvcC) of it, and convert the
            // packets.
            extra = ParameterSets(first, hevc);
          } else if (extra[0] == 1 && extra.size() > (hevc ? 22u : 5u)) {
            output.length_size = (extra[hevc ? 21 : 4] & 0x03) + 1;
          }
        }
      } else if (output.codec == AV_CODEC_ID_AV1 && extra.empty()) {
        extra = Av1SequenceHeader(read(packets_[output.packets[0]]));
      }
      track_value.Set("type", "video");
      track_value.Set("codec", media::GetCodecName(video.codec()));
      track_value.Set("width", parameters->width);
      track_value.Set("height", parameters->height);
    }
    if (!extra.empty()) {
      parameters->extradata = static_cast<uint8_t*>(
          av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      std::copy(extra.begin(), extra.end(), parameters->extradata);
      parameters->extradata_size = static_cast<int>(extra.size());
    }
    stream->time_base = kMicroseconds;
    track_value.Set("packets", static_cast<int>(output.packets.size()));
    tracks_value.Append(std::move(track_value));
  }

  if (avio_open(&context->pb, path.value().c_str(), AVIO_FLAG_WRITE) < 0) {
    *error = "cannot write " + path.AsUTF8Unsafe();
    return false;
  }
  AVDictionary* header_options = nullptr;
  if (&container == &kMp4) {
    // The index first, so that a browser plays it as it downloads.
    av_dict_set(&header_options, "movflags", "+faststart", 0);
  }
  int result = avformat_write_header(context.get(), &header_options);
  av_dict_free(&header_options);
  if (result < 0) {
    *error = "FFmpeg could not start the " + std::string(container.name) +
             " file: " + media::AVErrorToString(result);
    base::DeleteFile(path);
    return false;
  }

  // All tracks in decode order.
  struct Next {
    int64_t dts;
    size_t output;
    size_t n;
  };
  std::vector<Next> order;
  for (size_t o = 0; o < outputs.size(); ++o) {
    for (size_t n = 0; n < outputs[o].packets.size(); ++n) {
      order.push_back({outputs[o].dts[n], o, n});
    }
  }
  std::stable_sort(order.begin(), order.end(),
                   [](const Next& a, const Next& b) { return a.dts < b.dts; });
  AVPacket* av_packet = av_packet_alloc();
  for (const Next& next : order) {
    Output& output = outputs[next.output];
    const Packet& packet = packets_[output.packets[next.n]];
    base::span<const uint8_t> data = read(packet);
    if (data.empty()) {
      continue;
    }
    std::vector<uint8_t> converted;
    if (output.strip_adts) {
      const size_t header = AdtsHeaderSize(data);
      data = data.subspan(header);
    } else if (output.length_size && IsAnnexB(data)) {
      converted = ToLengthPrefixed(data, output.length_size);
      data = converted;
    }
    if (data.empty() || av_new_packet(av_packet, data.size()) < 0) {
      continue;
    }
    std::copy(data.begin(), data.end(), av_packet->data);
    av_packet->stream_index = output.stream->index;
    av_packet->pts = output.pts[next.n] - first_dts;
    av_packet->dts = output.dts[next.n] - first_dts;
    av_packet->duration = packet.duration > 0 ? packet.duration : 0;
    if (packet.keyframe) {
      av_packet->flags |= AV_PKT_FLAG_KEY;
    }
    av_packet_rescale_ts(av_packet, kMicroseconds, output.stream->time_base);
    result = av_interleaved_write_frame(context.get(), av_packet);
    av_packet_unref(av_packet);
    if (result < 0) {
      break;
    }
  }
  av_packet_free(&av_packet);
  if (result >= 0) {
    result = av_write_trailer(context.get());
  }
  context.reset();
  if (result < 0) {
    *error = "FFmpeg could not write the " + std::string(container.name) +
             " file: " + media::AVErrorToString(result);
    base::DeleteFile(path);
    return false;
  }

  // Where the file's start and its gaps are in the stream.
  file->Set("path", path.AsUTF8Unsafe());
  file->Set("format", container.name);
  file->Set("start", ToSeconds(first_pts));
  file->Set("duration", ToSeconds(last_end - first_pts));
  base::ListValue gaps_value;
  for (const auto& [at, length] : gaps) {
    gaps_value.Append(base::ListValue()
                          .Append(ToSeconds(at - length))
                          .Append(ToSeconds(at)));
  }
  file->Set("gaps", std::move(gaps_value));
  file->Set("tracks", std::move(tracks_value));
  file->Set("bytes", static_cast<double>(base::GetFileSize(path).value_or(0)));
  return true;
}

}  // namespace zebkit
