// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "jxl/decode.h"

#include "lib/jxl/base/byte_order.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/dec_external_image.h"
#include "lib/jxl/dec_frame.h"
#include "lib/jxl/dec_modular.h"
#include "lib/jxl/dec_reconstruct.h"
#include "lib/jxl/decode_to_jpeg.h"
#include "lib/jxl/fields.h"
#include "lib/jxl/headers.h"
#include "lib/jxl/icc_codec.h"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/loop_filter.h"
#include "lib/jxl/memory_manager_internal.h"
#include "lib/jxl/toc.h"

namespace {

// If set (by fuzzer) then some operations will fail, if those would require
// allocating large objects. Actual memory usage might be two orders of
// magnitude bigger.
// TODO(eustas): this is a poor-mans replacement for memory-manager approach;
//               remove, once memory-manager actually works.
size_t memory_limit_base_ = 0;
size_t cpu_limit_base_ = 0;
size_t used_cpu_base_ = 0;

bool CheckSizeLimit(size_t xsize, size_t ysize) {
  if (!memory_limit_base_) return true;
  if (xsize == 0 || ysize == 0) return true;
  size_t num_pixels = xsize * ysize;
  if (num_pixels / xsize != ysize) return false;  // overflow
  if (num_pixels > memory_limit_base_) return false;
  return true;
}

// Checks if a + b > size, taking possible integer overflow into account.
bool OutOfBounds(size_t a, size_t b, size_t size) {
  size_t pos = a + b;
  if (pos > size) return true;
  if (pos < a) return true;  // overflow happened
  return false;
}

// Checks if a + b + c > size, taking possible integer overflow into account.
bool OutOfBounds(size_t a, size_t b, size_t c, size_t size) {
  size_t pos = a + b;
  if (pos < b) return true;  // overflow happened
  pos += c;
  if (pos < c) return true;  // overflow happened
  if (pos > size) return true;
  return false;
}

bool SumOverflows(size_t a, size_t b, size_t c) {
  size_t sum = a + b;
  if (sum < b) return true;
  sum += c;
  if (sum < c) return true;
  return false;
}

JXL_INLINE size_t InitialBasicInfoSizeHint() {
  // Amount of bytes before the start of the codestream in the container format,
  // assuming that the codestream is the first box after the signature and
  // filetype boxes. 12 bytes signature box + 20 bytes filetype box + 16 bytes
  // codestream box length + name + optional XLBox length.
  const size_t container_header_size = 48;

  // Worst-case amount of bytes for basic info of the JPEG XL codestream header,
  // that is all information up to and including extra_channel_bits. Up to
  // around 2 bytes signature + 8 bytes SizeHeader + 31 bytes ColorEncoding + 4
  // bytes rest of ImageMetadata + 5 bytes part of ImageMetadata2.
  // TODO(lode): recompute and update this value when alpha_bits is moved to
  // extra channels info.
  const size_t max_codestream_basic_info_size = 50;

  return container_header_size + max_codestream_basic_info_size;
}

// Debug-printing failure macro similar to JXL_FAILURE, but for the status code
// JXL_DEC_ERROR
#ifdef JXL_CRASH_ON_ERROR
#define JXL_API_ERROR(format, ...)                                           \
  (::jxl::Debug(("%s:%d: " format "\n"), __FILE__, __LINE__, ##__VA_ARGS__), \
   ::jxl::Abort(), JXL_DEC_ERROR)
#else  // JXL_CRASH_ON_ERROR
#define JXL_API_ERROR(format, ...)                                             \
  (((JXL_DEBUG_ON_ERROR) &&                                                    \
    ::jxl::Debug(("%s:%d: " format "\n"), __FILE__, __LINE__, ##__VA_ARGS__)), \
   JXL_DEC_ERROR)
#endif  // JXL_CRASH_ON_ERROR

JxlDecoderStatus ConvertStatus(JxlDecoderStatus status) { return status; }

JxlDecoderStatus ConvertStatus(jxl::Status status) {
  return status ? JXL_DEC_SUCCESS : JXL_DEC_ERROR;
}

JxlSignature ReadSignature(const uint8_t* buf, size_t len, size_t* pos) {
  if (*pos >= len) return JXL_SIG_NOT_ENOUGH_BYTES;

  buf += *pos;
  len -= *pos;

  // JPEG XL codestream: 0xff 0x0a
  if (len >= 1 && buf[0] == 0xff) {
    if (len < 2) {
      return JXL_SIG_NOT_ENOUGH_BYTES;
    } else if (buf[1] == jxl::kCodestreamMarker) {
      *pos += 2;
      return JXL_SIG_CODESTREAM;
    } else {
      return JXL_SIG_INVALID;
    }
  }

  // JPEG XL container
  if (len >= 1 && buf[0] == 0) {
    if (len < 12) {
      return JXL_SIG_NOT_ENOUGH_BYTES;
    } else if (buf[1] == 0 && buf[2] == 0 && buf[3] == 0xC && buf[4] == 'J' &&
               buf[5] == 'X' && buf[6] == 'L' && buf[7] == ' ' &&
               buf[8] == 0xD && buf[9] == 0xA && buf[10] == 0x87 &&
               buf[11] == 0xA) {
      *pos += 12;
      return JXL_SIG_CONTAINER;
    } else {
      return JXL_SIG_INVALID;
    }
  }

  return JXL_SIG_INVALID;
}

}  // namespace

uint32_t JxlDecoderVersion(void) {
  return JPEGXL_MAJOR_VERSION * 1000000 + JPEGXL_MINOR_VERSION * 1000 +
         JPEGXL_PATCH_VERSION;
}

JxlSignature JxlSignatureCheck(const uint8_t* buf, size_t len) {
  size_t pos = 0;
  return ReadSignature(buf, len, &pos);
}

namespace {

size_t BitsPerChannel(JxlDataType data_type) {
  switch (data_type) {
    case JXL_TYPE_BOOLEAN:
      return 1;
    case JXL_TYPE_UINT8:
      return 8;
    case JXL_TYPE_UINT16:
      return 16;
    case JXL_TYPE_UINT32:
      return 32;
    case JXL_TYPE_FLOAT:
      return 32;
    case JXL_TYPE_FLOAT16:
      return 16;
      // No default, give compiler error if new type not handled.
  }
  return 0;  // Indicate invalid data type.
}

enum class DecoderStage : uint32_t {
  kInited,    // Decoder created, no JxlDecoderProcessInput called yet
  kStarted,   // Running JxlDecoderProcessInput calls
  kFinished,  // Everything done, nothing left to process
  kError,     // Error occurred, decoder object no longer usable
};

enum class FrameStage : uint32_t {
  kHeader,      // Must parse frame header. dec->frame_start must be set up
                // correctly already.
  kTOC,         // Must parse TOC
  kFull,        // Must parse full pixels
  kFullOutput,  // Must output full pixels
};

// Manages the sections for the FrameDecoder based on input bytes received.
struct Sections {
  // sections_begin = position in the frame where the sections begin, after
  // the frame header and TOC, so sections_begin = sum of frame header size and
  // TOC size.
  Sections(jxl::FrameDecoder* frame_dec, size_t frame_size,
           size_t sections_begin)
      : frame_dec_(frame_dec),
        frame_size_(frame_size),
        sections_begin_(sections_begin) {}

  Sections(const Sections&) = delete;
  Sections& operator=(const Sections&) = delete;
  Sections(Sections&&) = delete;
  Sections& operator=(Sections&&) = delete;

  ~Sections() {
    // Avoid memory leaks if the JXL decoder quits early and doesn't end up
    // calling CloseInput().
    CloseInput();
  }

  // frame_dec_ must have been Inited already, but not yet done ProcessSections.
  JxlDecoderStatus Init() {
    section_received.resize(frame_dec_->NumSections(), 0);

    const auto& offsets = frame_dec_->SectionOffsets();
    const auto& sizes = frame_dec_->SectionSizes();

    // Ensure none of the sums of section offset and size overflow.
    for (size_t i = 0; i < frame_dec_->NumSections(); i++) {
      if (OutOfBounds(sections_begin_, offsets[i], sizes[i], frame_size_)) {
        return JXL_API_ERROR("section out of bounds");
      }
    }

    return JXL_DEC_SUCCESS;
  }

  // Sets the input data for the frame. The frame pointer must point to the
  // beginning of the frame, size is the amount of bytes gotten so far and
  // should increase with next calls until the full frame is loaded.
  // TODO(lode): allow caller to provide only later chunks of memory when
  // earlier sections are fully processed already.
  void SetInput(const uint8_t* frame, size_t size) {
    const auto& offsets = frame_dec_->SectionOffsets();
    const auto& sizes = frame_dec_->SectionSizes();

    for (size_t i = 0; i < frame_dec_->NumSections(); i++) {
      if (section_received[i]) continue;
      if (!OutOfBounds(sections_begin_, offsets[i], sizes[i], size)) {
        section_received[i] = 1;
        section_info.emplace_back(jxl::FrameDecoder::SectionInfo{nullptr, i});
        section_status.emplace_back();
      }
    }
    // Reset all the bitreaders, because the address of the frame pointer may
    // change, even if it always represents the same frame start.
    for (size_t i = 0; i < section_info.size(); i++) {
      size_t id = section_info[i].id;
      JXL_ASSERT(section_info[i].br == nullptr);
      section_info[i].br = new jxl::BitReader(jxl::Span<const uint8_t>(
          frame + sections_begin_ + offsets[id], sizes[id]));
    }
  }

  JxlDecoderStatus CloseInput() {
    bool out_of_bounds = false;
    for (size_t i = 0; i < section_info.size(); i++) {
      if (!section_info[i].br) continue;
      if (!section_info[i].br->AllReadsWithinBounds()) {
        // Mark out of bounds section, but keep closing and deleting the next
        // ones as well.
        out_of_bounds = true;
      }
      JXL_ASSERT(section_info[i].br->Close());
      delete section_info[i].br;
      section_info[i].br = nullptr;
    }
    if (out_of_bounds) {
      // If any bit reader indicates out of bounds, it's an error, not just
      // needing more input, since we ensure only bit readers containing
      // a complete section are provided to the FrameDecoder.
      return JXL_API_ERROR("frame out of bounds");
    }
    return JXL_DEC_SUCCESS;
  }

  // Not managed by us.
  jxl::FrameDecoder* frame_dec_;

  size_t frame_size_;
  size_t sections_begin_;

  std::vector<jxl::FrameDecoder::SectionInfo> section_info;
  std::vector<jxl::FrameDecoder::SectionStatus> section_status;
  std::vector<char> section_received;
};

/*
Given list of frame references to storage slots, and storage slots in which this
frame is saved, computes which frames are required to decode the frame at the
given index and any frames after it. The frames on which this depends are
returned as a vector of their indices, in no particular order. The given index
must be smaller than saved_as.size(), and references.size() must equal
saved_as.size(). Any frames beyond saved_as and references are considered
unknown future frames and must be treated as if something depends on them.
*/
std::vector<size_t> GetFrameDependencies(size_t index,
                                         const std::vector<int>& saved_as,
                                         const std::vector<int>& references) {
  JXL_ASSERT(references.size() == saved_as.size());
  JXL_ASSERT(index < references.size());

  std::vector<size_t> result;

  constexpr size_t kNumStorage = 8;

  // value which indicates nothing is stored in this storage slot
  const size_t invalid = references.size();
  // for each of the 8 storage slots, a vector that translates frame index to
  // frame stored in this storage slot at this point, that is, the last
  // frame that was stored in this slot before or at this index.
  std::array<std::vector<size_t>, kNumStorage> storage;
  for (size_t s = 0; s < kNumStorage; ++s) {
    storage[s].resize(saved_as.size());
    int mask = 1 << s;
    size_t id = invalid;
    for (size_t i = 0; i < saved_as.size(); ++i) {
      if (saved_as[i] & mask) {
        id = i;
      }
      storage[s][i] = id;
    }
  }

  std::vector<char> seen(index + 1, 0);
  std::vector<size_t> stack;
  stack.push_back(index);
  seen[index] = 1;

  // For frames after index, assume they can depend on any of the 8 storage
  // slots, so push the frame for each stored reference to the stack and result.
  // All frames after index are treated as having unknown references and with
  // the possibility that there are more frames after the last known.
  // TODO(lode): take values of saved_as and references after index, and a
  // input flag indicating if they are all frames of the image, to further
  // optimize this.
  for (size_t s = 0; s < kNumStorage; ++s) {
    size_t frame_ref = storage[s][index];
    if (frame_ref == invalid) continue;
    if (seen[frame_ref]) continue;
    stack.push_back(frame_ref);
    seen[frame_ref] = 1;
    result.push_back(frame_ref);
  }

  while (!stack.empty()) {
    size_t frame_index = stack.back();
    stack.pop_back();
    if (frame_index == 0) continue;  // first frame cannot have references
    for (size_t s = 0; s < kNumStorage; ++s) {
      int mask = 1 << s;
      if (!(references[frame_index] & mask)) continue;
      size_t frame_ref = storage[s][frame_index - 1];
      if (frame_ref == invalid) continue;
      if (seen[frame_ref]) continue;
      stack.push_back(frame_ref);
      seen[frame_ref] = 1;
      result.push_back(frame_ref);
    }
  }

  return result;
}

// Parameters for user-requested extra channel output.
struct ExtraChannelOutput {
  JxlPixelFormat format;
  void* buffer;
  size_t buffer_size;
};

}  // namespace

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct JxlDecoderStruct {
  JxlDecoderStruct() = default;

  JxlMemoryManager memory_manager;
  std::unique_ptr<jxl::ThreadPool> thread_pool;

  DecoderStage stage;

  // Status of progression, internal.
  bool got_signature;
  bool first_codestream_seen;
  // Indicates we know that we've seen the last codestream, however this is not
  // guaranteed to be true for the last box because a jxl file may have multiple
  // "jxlp" boxes and it is possible (and permitted) that the last one is not a
  // final box that uses size 0 to indicate the end.
  bool last_codestream_seen;
  bool got_basic_info;
  size_t header_except_icc_bits = 0;  // To skip everything before ICC.
  bool got_all_headers;               // Codestream metadata headers.
  bool post_headers;                  // Already decoding pixels.
  jxl::ICCReader icc_reader;

  // This means either we actually got the preview image, or determined we
  // cannot get it or there is none.
  bool got_preview_image;

  // Position of next_in in the original file including box format if present
  // (as opposed to position in the codestream)
  size_t file_pos;
  size_t box_begin;
  size_t box_end;
  bool skip_box;
  // Begin and end of the content of the current codestream box. This could be
  // a partial codestream box.
  // codestream_begin 0 is used to indicate the begin is not yet known.
  // codestream_end 0 is used to indicate uncapped (until end of file, for the
  // last box if this box doesn't indicate its actual size).
  // Not used if the file is a direct codestream.
  size_t codestream_begin;
  size_t codestream_end;

  // Settings
  bool keep_orientation;

  // Bitfield, for which informative events (JXL_DEC_BASIC_INFO, etc...) the
  // decoder returns a status. By default, do not return for any of the events,
  // only return when the decoder cannot continue because it needs more input or
  // output data.
  int events_wanted;
  int orig_events_wanted;

  // Fields for reading the basic info from the header.
  size_t basic_info_size_hint;
  bool have_container;

  // Whether the preview out buffer was set. It is possible for the buffer to
  // be nullptr and buffer_set to be true, indicating it was deliberately
  // set to nullptr.
  bool preview_out_buffer_set;
  // Idem for the image buffer.
  bool image_out_buffer_set;

  // Owned by the caller, buffers for DC image and full resolution images
  void* preview_out_buffer;
  void* image_out_buffer;
  JxlImageOutCallback image_out_callback;
  void* image_out_opaque;

  size_t preview_out_size;
  size_t image_out_size;

  JxlPixelFormat preview_out_format;
  JxlPixelFormat image_out_format;

  // For extra channels. Empty if no extra channels are requested, and they are
  // reset each frame
  std::vector<ExtraChannelOutput> extra_channel_output;

  jxl::CodecMetadata metadata;
  std::unique_ptr<jxl::ImageBundle> ib;
  // ColorEncoding to use for xyb encoded image with ICC profile.
  jxl::ColorEncoding default_enc;

  std::unique_ptr<jxl::PassesDecoderState> passes_state;
  std::unique_ptr<jxl::FrameDecoder> frame_dec;
  std::unique_ptr<Sections> sections;
  // The FrameDecoder is initialized, and not yet finalized
  bool frame_dec_in_progress;

  // headers and TOC for the current frame. When got_toc is true, this is
  // always the frame header of the last frame of the current still series,
  // that is, the displayed frame.
  std::unique_ptr<jxl::FrameHeader> frame_header;

  // Start of the current frame being processed, as offset from the beginning of
  // the codestream.
  size_t frame_start;
  size_t frame_size;
  FrameStage frame_stage;
  // The currently processed frame is the last of the current composite still,
  // and so must be returned as pixels
  bool is_last_of_still;
  // The currently processed frame is the last of the codestream
  bool is_last_total;
  // How many frames to skip.
  size_t skip_frames;
  // Skipping the current frame. May be false if skip_frames was just set to
  // a positive value while already processing a current frame, then
  // skipping_frame will be enabled only for the next frame.
  bool skipping_frame;

  // Amount of internal frames and external frames started. External frames are
  // user-visible frames, internal frames includes all external frames and
  // also invisible frames such as patches, blending-only and dc_level frames.
  size_t internal_frames;
  size_t external_frames;

  // For each internal frame, which storage locations it references, and which
  // storage locations it is stored in, using the bit mask as defined in
  // FrameDecoder::References and FrameDecoder::SaveAs.
  std::vector<int> frame_references;
  std::vector<int> frame_saved_as;

  // Translates external frame index to internal frame index. The external
  // index is the index of user-visible frames. The internal index can be larger
  // since non-visible frames (such as frames with patches, ...) are included.
  std::vector<size_t> frame_external_to_internal;

  // Whether the frame with internal index is required to decode the frame
  // being skipped to or any frames after that. If no skipping is active,
  // this vector is ignored. If the current internal frame index is beyond this
  // vector, it must be treated as a required frame.
  std::vector<char> frame_required;

  // Codestream input data is stored here, when the decoder takes in and stores
  // the user input bytes. If the decoder does not do that (e.g. in one-shot
  // case), this field is unused.
  // TODO(lode): avoid needing this field once the C++ decoder doesn't need
  // all bytes at once, to save memory. Find alternative to std::vector doubling
  // strategy to prevent some memory usage.
  std::vector<uint8_t> codestream;

  jxl::JxlToJpegDecoder jpeg_decoder;

  // Position in the actual codestream, which codestream.begin() points to.
  // Non-zero once earlier parts of the codestream vector have been erased.
  size_t codestream_pos;

  // Statistics which CodecInOut can keep
  uint64_t dec_pixels;

  const uint8_t* next_in;
  size_t avail_in;
};

// TODO(zond): Make this depend on the data loaded into the decoder.
JxlDecoderStatus JxlDecoderDefaultPixelFormat(const JxlDecoder* dec,
                                              JxlPixelFormat* format) {
  if (!dec->got_basic_info) return JXL_DEC_NEED_MORE_INPUT;
  *format = {4, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
  return JXL_DEC_SUCCESS;
}

void JxlDecoderReset(JxlDecoder* dec) {
  dec->thread_pool.reset();
  dec->stage = DecoderStage::kInited;
  dec->got_signature = false;
  dec->first_codestream_seen = false;
  dec->last_codestream_seen = false;
  dec->got_basic_info = false;
  dec->header_except_icc_bits = 0;
  dec->got_all_headers = false;
  dec->post_headers = false;
  dec->icc_reader.Reset();
  dec->got_preview_image = false;
  dec->file_pos = 0;
  dec->box_begin = 0;
  dec->box_end = 0;
  dec->skip_box = false;
  dec->codestream_pos = 0;
  dec->codestream_begin = 0;
  dec->codestream_end = 0;
  dec->keep_orientation = false;
  dec->events_wanted = 0;
  dec->orig_events_wanted = 0;
  dec->basic_info_size_hint = InitialBasicInfoSizeHint();
  dec->have_container = 0;
  dec->preview_out_buffer_set = false;
  dec->image_out_buffer_set = false;
  dec->preview_out_buffer = nullptr;
  dec->image_out_buffer = nullptr;
  dec->image_out_callback = nullptr;
  dec->image_out_opaque = nullptr;
  dec->preview_out_size = 0;
  dec->image_out_size = 0;
  dec->extra_channel_output.clear();
  dec->dec_pixels = 0;
  dec->next_in = 0;
  dec->avail_in = 0;

  dec->passes_state.reset(nullptr);
  dec->frame_dec.reset(nullptr);
  dec->sections.reset(nullptr);
  dec->frame_dec_in_progress = false;

  dec->ib.reset();
  dec->metadata = jxl::CodecMetadata();
  dec->frame_header.reset(new jxl::FrameHeader(&dec->metadata));
  dec->codestream.clear();

  dec->frame_stage = FrameStage::kHeader;
  dec->frame_start = 0;
  dec->frame_size = 0;
  dec->is_last_of_still = false;
  dec->is_last_total = false;
  dec->skip_frames = 0;
  dec->skipping_frame = false;
  dec->internal_frames = 0;
  dec->external_frames = 0;
  dec->frame_references.clear();
  dec->frame_saved_as.clear();
  dec->frame_external_to_internal.clear();
  dec->frame_required.clear();
}

JxlDecoder* JxlDecoderCreate(const JxlMemoryManager* memory_manager) {
  JxlMemoryManager local_memory_manager;
  if (!jxl::MemoryManagerInit(&local_memory_manager, memory_manager))
    return nullptr;

  void* alloc =
      jxl::MemoryManagerAlloc(&local_memory_manager, sizeof(JxlDecoder));
  if (!alloc) return nullptr;
  // Placement new constructor on allocated memory
  JxlDecoder* dec = new (alloc) JxlDecoder();
  dec->memory_manager = local_memory_manager;

  JxlDecoderReset(dec);

  return dec;
}

void JxlDecoderDestroy(JxlDecoder* dec) {
  if (dec) {
    // Call destructor directly since custom free function is used.
    dec->~JxlDecoder();
    jxl::MemoryManagerFree(&dec->memory_manager, dec);
  }
}

void JxlDecoderRewind(JxlDecoder* dec) {
  int keep_orientation = dec->keep_orientation;
  int events_wanted = dec->orig_events_wanted;
  std::vector<int> frame_references;
  std::vector<int> frame_saved_as;
  std::vector<size_t> frame_external_to_internal;
  std::vector<char> frame_required;
  frame_references.swap(dec->frame_references);
  frame_saved_as.swap(dec->frame_saved_as);
  frame_external_to_internal.swap(dec->frame_external_to_internal);
  frame_required.swap(dec->frame_required);

  JxlDecoderReset(dec);
  dec->keep_orientation = keep_orientation;
  dec->events_wanted = events_wanted;
  dec->orig_events_wanted = events_wanted;
  frame_references.swap(dec->frame_references);
  frame_saved_as.swap(dec->frame_saved_as);
  frame_external_to_internal.swap(dec->frame_external_to_internal);
  frame_required.swap(dec->frame_required);
}

void JxlDecoderSkipFrames(JxlDecoder* dec, size_t amount) {
  // Increment amount, rather than set it: making the amount smaller is
  // impossible because the decoder may already have skipped frames required to
  // decode earlier frames, and making the amount larger compared to an existing
  // amount is impossible because if JxlDecoderSkipFrames is called in the
  // middle of already skipping frames, the user cannot know how many frames
  // have already been skipped internally so far so an absolute value cannot
  // be defined.
  dec->skip_frames += amount;

  dec->frame_required.clear();
  size_t next_frame = dec->external_frames + dec->skip_frames;

  // A frame that has been seen before a rewind
  if (next_frame < dec->frame_external_to_internal.size()) {
    size_t internal_index = dec->frame_external_to_internal[next_frame];
    if (internal_index < dec->frame_saved_as.size()) {
      std::vector<size_t> deps = GetFrameDependencies(
          internal_index, dec->frame_saved_as, dec->frame_references);

      dec->frame_required.resize(internal_index + 1, 0);
      for (size_t i = 0; i < deps.size(); i++) {
        JXL_ASSERT(deps[i] < dec->frame_required.size());
        dec->frame_required[deps[i]] = 1;
      }
    }
  }
}

JXL_EXPORT JxlDecoderStatus
JxlDecoderSetParallelRunner(JxlDecoder* dec, JxlParallelRunner parallel_runner,
                            void* parallel_runner_opaque) {
  if (dec->thread_pool) return JXL_API_ERROR("parallel runner already set");
  dec->thread_pool.reset(
      new jxl::ThreadPool(parallel_runner, parallel_runner_opaque));
  return JXL_DEC_SUCCESS;
}

size_t JxlDecoderSizeHintBasicInfo(const JxlDecoder* dec) {
  if (dec->got_basic_info) return 0;
  return dec->basic_info_size_hint;
}

JxlDecoderStatus JxlDecoderSubscribeEvents(JxlDecoder* dec, int events_wanted) {
  if (dec->stage != DecoderStage::kInited) {
    return JXL_DEC_ERROR;  // Cannot subscribe to events after having started.
  }
  if (events_wanted & 63) {
    return JXL_DEC_ERROR;  // Can only subscribe to informative events.
  }
  dec->events_wanted = events_wanted;
  dec->orig_events_wanted = events_wanted;
  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderSetKeepOrientation(JxlDecoder* dec,
                                              JXL_BOOL keep_orientation) {
  if (dec->stage != DecoderStage::kInited) {
    return JXL_API_ERROR("Must set keep_orientation option before starting");
  }
  dec->keep_orientation = !!keep_orientation;
  return JXL_DEC_SUCCESS;
}

namespace jxl {
namespace {

template <class T>
bool CanRead(Span<const uint8_t> data, BitReader* reader, T* JXL_RESTRICT t) {
  // Use a copy of the bit reader because CanRead advances bits.
  BitReader reader2(data);
  reader2.SkipBits(reader->TotalBitsConsumed());
  bool result = Bundle::CanRead(&reader2, t);
  JXL_ASSERT(reader2.Close());
  return result;
}

// Returns JXL_DEC_SUCCESS if the full bundle was successfully read, status
// indicating either error or need more input otherwise.
template <class T>
JxlDecoderStatus ReadBundle(Span<const uint8_t> data, BitReader* reader,
                            T* JXL_RESTRICT t) {
  if (!CanRead(data, reader, t)) {
    return JXL_DEC_NEED_MORE_INPUT;
  }
  if (!Bundle::Read(reader, t)) {
    return JXL_DEC_ERROR;
  }
  return JXL_DEC_SUCCESS;
}

#define JXL_API_RETURN_IF_ERROR(expr)               \
  {                                                 \
    JxlDecoderStatus status_ = ConvertStatus(expr); \
    if (status_ != JXL_DEC_SUCCESS) return status_; \
  }

std::unique_ptr<BitReader, std::function<void(BitReader*)>> GetBitReader(
    Span<const uint8_t> span) {
  BitReader* reader = new BitReader(span);
  return std::unique_ptr<BitReader, std::function<void(BitReader*)>>(
      reader, [](BitReader* reader) {
        // We can't allow Close to abort the program if the reader is out of
        // bounds, or all return paths in the code, even those that already
        // return failure, would have to manually call AllReadsWithinBounds().
        // Invalid JXL codestream should not cause program to quit.
        (void)reader->AllReadsWithinBounds();
        (void)reader->Close();
        delete reader;
      });
}

JxlDecoderStatus JxlDecoderReadBasicInfo(JxlDecoder* dec, const uint8_t* in,
                                         size_t size) {
  size_t pos = 0;

  // Check and skip the codestream signature
  JxlSignature signature = ReadSignature(in, size, &pos);
  if (signature == JXL_SIG_NOT_ENOUGH_BYTES) {
    return JXL_DEC_NEED_MORE_INPUT;
  }
  if (signature == JXL_SIG_CONTAINER) {
    // There is a container signature where we expect a codestream, container
    // is handled at a higher level already.
    return JXL_API_ERROR("invalid: nested container");
  }
  if (signature != JXL_SIG_CODESTREAM) {
    return JXL_API_ERROR("invalid signature");
  }

  Span<const uint8_t> span(in + pos, size - pos);
  auto reader = GetBitReader(span);
  JXL_API_RETURN_IF_ERROR(ReadBundle(span, reader.get(), &dec->metadata.size));

  dec->metadata.m.nonserialized_only_parse_basic_info = true;
  JXL_API_RETURN_IF_ERROR(ReadBundle(span, reader.get(), &dec->metadata.m));
  dec->metadata.m.nonserialized_only_parse_basic_info = false;
  dec->got_basic_info = true;
  dec->basic_info_size_hint = 0;

  if (!CheckSizeLimit(dec->metadata.size.xsize(), dec->metadata.size.ysize())) {
    return JXL_API_ERROR("image is too large");
  }

  return JXL_DEC_SUCCESS;
}

// Reads all codestream headers (but not frame headers)
JxlDecoderStatus JxlDecoderReadAllHeaders(JxlDecoder* dec, const uint8_t* in,
                                          size_t size) {
  size_t pos = 0;

  // Check and skip the codestream signature
  JxlSignature signature = ReadSignature(in, size, &pos);
  if (signature == JXL_SIG_CONTAINER) {
    return JXL_API_ERROR("invalid: nested container");
  }
  if (signature != JXL_SIG_CODESTREAM) {
    return JXL_API_ERROR("invalid signature");
  }

  Span<const uint8_t> span(in + pos, size - pos);
  auto reader = GetBitReader(span);

  if (dec->header_except_icc_bits != 0) {
    // Headers were decoded already.
    reader->SkipBits(dec->header_except_icc_bits);
  } else {
    SizeHeader dummy_size_header;
    JXL_API_RETURN_IF_ERROR(ReadBundle(span, reader.get(), &dummy_size_header));

    // We already decoded the metadata to dec->metadata.m, no reason to
    // overwrite it, use a dummy metadata instead.
    ImageMetadata dummy_metadata;
    JXL_API_RETURN_IF_ERROR(ReadBundle(span, reader.get(), &dummy_metadata));

    JXL_API_RETURN_IF_ERROR(
        ReadBundle(span, reader.get(), &dec->metadata.transform_data));
  }

  dec->header_except_icc_bits = reader->TotalBitsConsumed();

  if (dec->metadata.m.color_encoding.WantICC()) {
    jxl::Status status = dec->icc_reader.Init(reader.get(), memory_limit_base_);
    // Always check AllReadsWithinBounds, not all the C++ decoder implementation
    // handles reader out of bounds correctly  yet (e.g. context map). Not
    // checking AllReadsWithinBounds can cause reader->Close() to trigger an
    // assert, but we don't want library to quit program for invalid codestream.
    if (!reader->AllReadsWithinBounds()) {
      return JXL_DEC_NEED_MORE_INPUT;
    }
    if (!status) {
      if (status.code() == StatusCode::kNotEnoughBytes) {
        return JXL_DEC_NEED_MORE_INPUT;
      }
      // Other non-successful status is an error
      return JXL_DEC_ERROR;
    }
    PaddedBytes icc;
    status = dec->icc_reader.Process(reader.get(), &icc);
    if (!status) {
      if (status.code() == StatusCode::kNotEnoughBytes) {
        return JXL_DEC_NEED_MORE_INPUT;
      }
      // Other non-successful status is an error
      return JXL_DEC_ERROR;
    }
    if (!dec->metadata.m.color_encoding.SetICCRaw(std::move(icc))) {
      return JXL_DEC_ERROR;
    }
  }

  dec->got_all_headers = true;
  JXL_API_RETURN_IF_ERROR(reader->JumpToByteBoundary());

  dec->frame_start = pos + reader->TotalBitsConsumed() / jxl::kBitsPerByte;

  if (!dec->passes_state) {
    dec->passes_state.reset(new jxl::PassesDecoderState());
  }

  dec->default_enc =
      ColorEncoding::LinearSRGB(dec->metadata.m.color_encoding.IsGray());

  JXL_API_RETURN_IF_ERROR(dec->passes_state->output_encoding_info.Set(
      dec->metadata, dec->default_enc));

  return JXL_DEC_SUCCESS;
}

static size_t GetStride(const JxlDecoder* dec, const JxlPixelFormat& format,
                        const jxl::ImageBundle* frame = nullptr) {
  size_t xsize = dec->metadata.xsize();
  if (!dec->keep_orientation && dec->metadata.m.orientation > 4) {
    xsize = dec->metadata.ysize();
  }
  if (frame) {
    xsize = dec->keep_orientation ? frame->xsize() : frame->oriented_xsize();
  }
  size_t stride = xsize * (BitsPerChannel(format.data_type) *
                           format.num_channels / jxl::kBitsPerByte);
  if (format.align > 1) {
    stride = jxl::DivCeil(stride, format.align) * format.align;
  }
  return stride;
}

// Internal wrapper around jxl::ConvertToExternal which converts the stride,
// format and orientation and allows to choose whether to get all RGB(A)
// channels or alternatively get a single extra channel.
// If want_extra_channel, a valid index to a single extra channel must be
// given, the output must be single-channel, and format.num_channels is ignored
// and treated as if it is 1.
static JxlDecoderStatus ConvertImageInternal(
    const JxlDecoder* dec, const jxl::ImageBundle& frame,
    const JxlPixelFormat& format, bool want_extra_channel,
    size_t extra_channel_index, void* out_image, size_t out_size,
    JxlImageOutCallback out_callback, void* out_opaque) {
  // TODO(lode): handle mismatch of RGB/grayscale color profiles and pixel data
  // color/grayscale format
  const size_t stride = GetStride(dec, format, &frame);

  bool float_format = format.data_type == JXL_TYPE_FLOAT ||
                      format.data_type == JXL_TYPE_FLOAT16;

  jxl::Orientation undo_orientation = dec->keep_orientation
                                          ? jxl::Orientation::kIdentity
                                          : dec->metadata.m.GetOrientation();

  jxl::Status status(true);
  if (want_extra_channel) {
    status = jxl::ConvertToExternal(
        frame.extra_channels()[extra_channel_index],
        BitsPerChannel(format.data_type), float_format, format.endianness,
        stride, dec->thread_pool.get(), out_image, out_size,
        /*out_callback=*/out_callback,
        /*out_opaque=*/out_opaque, undo_orientation);
  } else {
    status = jxl::ConvertToExternal(
        frame, BitsPerChannel(format.data_type), float_format,
        format.num_channels, format.endianness, stride, dec->thread_pool.get(),
        out_image, out_size,
        /*out_callback=*/out_callback,
        /*out_opaque=*/out_opaque, undo_orientation);
  }

  return status ? JXL_DEC_SUCCESS : JXL_DEC_ERROR;
}

// Parses the FrameHeader and the total frame_size, given the initial bytes
// of the frame up to and including the TOC.
// TODO(lode): merge this with FrameDecoder
JxlDecoderStatus ParseFrameHeader(jxl::FrameHeader* frame_header,
                                  const uint8_t* in, size_t size, size_t pos,
                                  bool is_preview, size_t* frame_size,
                                  int* saved_as) {
  if (pos >= size) {
    return JXL_DEC_NEED_MORE_INPUT;
  }
  Span<const uint8_t> span(in + pos, size - pos);
  auto reader = GetBitReader(span);

  frame_header->nonserialized_is_preview = is_preview;
  jxl::Status status = DecodeFrameHeader(reader.get(), frame_header);
  jxl::FrameDimensions frame_dim = frame_header->ToFrameDimensions();
  if (!CheckSizeLimit(frame_dim.xsize_upsampled_padded,
                      frame_dim.ysize_upsampled_padded)) {
    return JXL_API_ERROR("frame is too large");
  }

  if (status.code() == StatusCode::kNotEnoughBytes) {
    // TODO(lode): prevent asking for way too much input bytes in case of
    // invalid header that the decoder thinks is a very long user extension
    // instead. Example: fields can currently print something like this:
    // "../lib/jxl/fields.cc:416: Skipping 71467322-bit extension(s)"
    // Maybe fields.cc should return error in the above case rather than
    // print a message.
    return JXL_DEC_NEED_MORE_INPUT;
  } else if (!status) {
    return JXL_API_ERROR("invalid frame header");
  }

  // Read TOC.
  uint64_t groups_total_size;
  const bool has_ac_global = true;
  const size_t toc_entries =
      NumTocEntries(frame_dim.num_groups, frame_dim.num_dc_groups,
                    frame_header->passes.num_passes, has_ac_global);

  std::vector<uint64_t> group_offsets;
  std::vector<uint32_t> group_sizes;
  status = ReadGroupOffsets(toc_entries, reader.get(), &group_offsets,
                            &group_sizes, &groups_total_size);

  // TODO(lode): we're actually relying on AllReadsWithinBounds() here
  // instead of on status.code(), change the internal TOC C++ code to
  // correctly set the status.code() instead so we can rely on that one.
  if (!reader->AllReadsWithinBounds() ||
      status.code() == StatusCode::kNotEnoughBytes) {
    return JXL_DEC_NEED_MORE_INPUT;
  } else if (!status) {
    return JXL_API_ERROR("invalid toc entries");
  }

  JXL_DASSERT((reader->TotalBitsConsumed() % kBitsPerByte) == 0);
  JXL_API_RETURN_IF_ERROR(reader->JumpToByteBoundary());
  size_t header_size = (reader->TotalBitsConsumed() >> 3);
  *frame_size = header_size + groups_total_size;

  if (saved_as != nullptr) {
    *saved_as = FrameDecoder::SavedAs(*frame_header);
  }

  return JXL_DEC_SUCCESS;
}

// TODO(eustas): no CodecInOut -> no image size reinforcement -> possible OOM.
JxlDecoderStatus JxlDecoderProcessInternal(JxlDecoder* dec, const uint8_t* in,
                                           size_t size) {
  // If no parallel runner is set, use the default
  // TODO(lode): move this initialization to an appropriate location once the
  // runner is used to decode pixels.
  if (!dec->thread_pool) {
    dec->thread_pool.reset(new jxl::ThreadPool(nullptr, nullptr));
  }

  // No matter what events are wanted, the basic info is always required.
  if (!dec->got_basic_info) {
    JxlDecoderStatus status = JxlDecoderReadBasicInfo(dec, in, size);
    if (status != JXL_DEC_SUCCESS) return status;
  }

  if (dec->events_wanted & JXL_DEC_BASIC_INFO) {
    dec->events_wanted &= ~JXL_DEC_BASIC_INFO;
    return JXL_DEC_BASIC_INFO;
  }

  if (!dec->got_all_headers) {
    JxlDecoderStatus status = JxlDecoderReadAllHeaders(dec, in, size);
    if (status != JXL_DEC_SUCCESS) return status;
  }

  if (dec->events_wanted & JXL_DEC_EXTENSIONS) {
    dec->events_wanted &= ~JXL_DEC_EXTENSIONS;
    if (dec->metadata.m.extensions != 0) {
      return JXL_DEC_EXTENSIONS;
    }
  }

  if (dec->events_wanted & JXL_DEC_COLOR_ENCODING) {
    dec->events_wanted &= ~JXL_DEC_COLOR_ENCODING;
    return JXL_DEC_COLOR_ENCODING;
  }

  dec->post_headers = true;

  // Decode to pixels, only if required for the events the user wants.
  if (!dec->got_preview_image) {
    // Parse the preview, or at least its TOC to be able to skip the frame, if
    // any frame or image decoding is desired.
    bool parse_preview =
        (dec->events_wanted &
         (JXL_DEC_PREVIEW_IMAGE | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));

    if (!dec->metadata.m.have_preview) {
      // There is no preview, mark this as done and go to next step
      dec->got_preview_image = true;
    } else if (!parse_preview) {
      // No preview parsing needed, mark this step as done
      dec->got_preview_image = true;
    } else {
      // Want to decode the preview, not just skip the frame
      bool want_preview = (dec->events_wanted & JXL_DEC_PREVIEW_IMAGE);
      size_t frame_size;
      size_t pos = dec->frame_start;
      dec->frame_header.reset(new FrameHeader(&dec->metadata));
      JxlDecoderStatus status = ParseFrameHeader(dec->frame_header.get(), in,
                                                 size, pos, true, &frame_size,
                                                 /*saved_as=*/nullptr);
      if (status != JXL_DEC_SUCCESS) return status;
      if (OutOfBounds(pos, frame_size, size)) {
        return JXL_DEC_NEED_MORE_INPUT;
      }

      if (want_preview && !dec->preview_out_buffer_set) {
        return JXL_DEC_NEED_PREVIEW_OUT_BUFFER;
      }

      jxl::Span<const uint8_t> compressed(in + dec->frame_start,
                                          size - dec->frame_start);
      auto reader = GetBitReader(compressed);
      jxl::DecompressParams dparams;
      dparams.preview = want_preview ? jxl::Override::kOn : jxl::Override::kOff;
      jxl::ImageBundle ib(&dec->metadata.m);
      PassesDecoderState preview_dec_state;
      JXL_API_RETURN_IF_ERROR(preview_dec_state.output_encoding_info.Set(
          dec->metadata,
          ColorEncoding::LinearSRGB(dec->metadata.m.color_encoding.IsGray())));
      if (!DecodeFrame(dparams, &preview_dec_state, dec->thread_pool.get(),
                       reader.get(), &ib, dec->metadata,
                       /*constraints=*/nullptr,
                       /*is_preview=*/true)) {
        return JXL_API_ERROR("decoding preview failed");
      }

      // Set frame_start to the first non-preview frame.
      dec->frame_start += DivCeil(reader->TotalBitsConsumed(), kBitsPerByte);
      dec->got_preview_image = true;

      if (want_preview) {
        if (dec->preview_out_buffer) {
          JxlDecoderStatus status = ConvertImageInternal(
              dec, ib, dec->preview_out_format, /*want_extra_channel=*/false,
              /*extra_channel_index=*/0, dec->preview_out_buffer,
              dec->preview_out_size, /*out_callback=*/nullptr,
              /*out_opaque=*/nullptr);
          if (status != JXL_DEC_SUCCESS) return status;
        }
        return JXL_DEC_PREVIEW_IMAGE;
      }
    }
  }

  // Handle frames
  for (;;) {
    if (!(dec->events_wanted & (JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME))) {
      break;
    }
    if (dec->frame_stage == FrameStage::kHeader && dec->is_last_total) {
      break;
    }

    if (dec->frame_stage == FrameStage::kHeader) {
      size_t pos = dec->frame_start - dec->codestream_pos;
      if (pos >= size) {
        return JXL_DEC_NEED_MORE_INPUT;
      }
      dec->frame_header.reset(new FrameHeader(&dec->metadata));
      int saved_as = 0;
      JxlDecoderStatus status =
          ParseFrameHeader(dec->frame_header.get(), in, size, pos,
                           /*is_preview=*/false, &dec->frame_size, &saved_as);
      if (status != JXL_DEC_SUCCESS) return status;

      // is last in entire codestream
      dec->is_last_total = dec->frame_header->is_last;
      // is last of current still
      dec->is_last_of_still =
          dec->is_last_total || dec->frame_header->animation_frame.duration > 0;

      const size_t internal_frame_index = dec->internal_frames;
      const size_t external_frame_index = dec->external_frames;
      if (dec->is_last_of_still) dec->external_frames++;
      dec->internal_frames++;

      dec->frame_stage = FrameStage::kTOC;

      if (dec->skip_frames > 0) {
        dec->skipping_frame = true;
        if (dec->is_last_of_still) {
          dec->skip_frames--;
        }
      } else {
        dec->skipping_frame = false;
      }

      if (external_frame_index >= dec->frame_external_to_internal.size()) {
        dec->frame_external_to_internal.push_back(internal_frame_index);
        JXL_ASSERT(dec->frame_external_to_internal.size() ==
                   external_frame_index + 1);
      }

      if (internal_frame_index >= dec->frame_saved_as.size()) {
        dec->frame_saved_as.push_back(saved_as);
        JXL_ASSERT(dec->frame_saved_as.size() == internal_frame_index + 1);

        // add the value 0xff (which means all references) to new slots: we only
        // know the references of the frame at FinalizeFrame, and fill in the
        // correct values there. As long as this information is not known, the
        // worst case where the frame depends on all storage slots is assumed.
        dec->frame_references.push_back(0xff);
        JXL_ASSERT(dec->frame_references.size() == internal_frame_index + 1);
      }

      if (dec->skipping_frame) {
        // Whether this frame could be referenced by any future frame: either
        // because it's a frame saved for blending or patches, or because it's
        // a DC frame.
        bool referenceable =
            dec->frame_header->CanBeReferenced() ||
            dec->frame_header->frame_type == FrameType::kDCFrame;
        if (internal_frame_index < dec->frame_required.size() &&
            !dec->frame_required[internal_frame_index]) {
          referenceable = false;
        }
        if (!referenceable) {
          // Skip all decoding for this frame, since the user is skipping this
          // frame and no future frames can reference it.
          dec->frame_stage = FrameStage::kHeader;
          dec->frame_start += dec->frame_size;
          continue;
        }
      }

      if ((dec->events_wanted & JXL_DEC_FRAME) && dec->is_last_of_still) {
        // Only return this for the last of a series of stills: patches frames
        // etc... before this one do not contain the correct information such
        // as animation timing, ...
        if (!dec->skipping_frame) {
          return JXL_DEC_FRAME;
        }
      }
    }

    if (dec->frame_stage == FrameStage::kTOC) {
      size_t pos = dec->frame_start - dec->codestream_pos;
      if (pos >= size) {
        return JXL_DEC_NEED_MORE_INPUT;
      }
      Span<const uint8_t> span(in + pos, size - pos);
      auto reader = GetBitReader(span);

      if (!dec->passes_state) {
        dec->passes_state.reset(new jxl::PassesDecoderState());
      }
      if (!dec->ib) {
        dec->ib.reset(new jxl::ImageBundle(&dec->metadata.m));
      }

      dec->frame_dec.reset(new FrameDecoder(
          dec->passes_state.get(), dec->metadata, dec->thread_pool.get()));

      // If JPEG reconstruction is wanted and possible, set the jpeg_data of
      // the ImageBundle.
      if (!dec->jpeg_decoder.SetImageBundleJpegData(dec->ib.get()))
        return JXL_DEC_ERROR;

      jxl::Status status = dec->frame_dec->InitFrame(
          reader.get(), dec->ib.get(), /*is_preview=*/false,
          /*allow_partial_frames=*/false, /*allow_partial_dc_global=*/false);
      if (!status) JXL_API_RETURN_IF_ERROR(status);

      size_t sections_begin =
          DivCeil(reader->TotalBitsConsumed(), kBitsPerByte);

      dec->sections.reset(
          new Sections(dec->frame_dec.get(), dec->frame_size, sections_begin));
      JXL_API_RETURN_IF_ERROR(dec->sections->Init());

      // If we don't need pixels, we can skip actually decoding the frames
      // (kFull / kFullOut). By not updating frame_stage, none of
      // these stages will execute, and the loop will continue from the next
      // frame.
      if (dec->events_wanted & JXL_DEC_FULL_IMAGE) {
        dec->frame_dec_in_progress = true;
        dec->frame_stage = FrameStage::kFull;
      }
    }

    bool return_full_image = false;

    if (dec->frame_stage == FrameStage::kFull) {
      if (dec->events_wanted & JXL_DEC_FULL_IMAGE) {
        if (!dec->image_out_buffer_set &&
            (!dec->jpeg_decoder.IsOutputSet() ||
             dec->ib->jpeg_data == nullptr) &&
            dec->is_last_of_still) {
          // TODO(lode): remove the dec->is_last_of_still condition if the
          // frame decoder needs the image buffer as working space for decoding
          // non-visible or blending frames too
          if (!dec->skipping_frame) {
            return JXL_DEC_NEED_IMAGE_OUT_BUFFER;
          }
        }
      }

      if (dec->image_out_buffer_set && !!dec->image_out_buffer &&
          dec->image_out_format.data_type == JXL_TYPE_UINT8 &&
          dec->image_out_format.num_channels >= 3 &&
          dec->extra_channel_output.empty()) {
        bool is_rgba = dec->image_out_format.num_channels == 4;
        dec->frame_dec->MaybeSetRGB8OutputBuffer(
            reinterpret_cast<uint8_t*>(dec->image_out_buffer),
            GetStride(dec, dec->image_out_format), is_rgba,
            !dec->keep_orientation);
      }

      const bool little_endian =
          dec->image_out_format.endianness == JXL_LITTLE_ENDIAN ||
          (dec->image_out_format.endianness == JXL_NATIVE_ENDIAN &&
           IsLittleEndian());
      bool swap_endianness = little_endian != IsLittleEndian();

      // TODO(lode): Support more formats than just native endian float32 for
      // the low-memory callback path
      if (dec->image_out_buffer_set && !!dec->image_out_callback &&
          dec->image_out_format.data_type == JXL_TYPE_FLOAT &&
          dec->image_out_format.num_channels >= 3 && !swap_endianness &&
          dec->frame_dec_in_progress) {
        bool is_rgba = dec->image_out_format.num_channels == 4;
        dec->frame_dec->MaybeSetFloatCallback(
            [dec](const float* pixels, size_t x, size_t y, size_t num_pixels) {
              dec->image_out_callback(dec->image_out_opaque, x, y, num_pixels,
                                      pixels);
            },
            is_rgba, !dec->keep_orientation);
      }

      size_t pos = dec->frame_start - dec->codestream_pos;
      if (pos >= size) {
        return JXL_DEC_NEED_MORE_INPUT;
      }
      dec->sections->SetInput(in + pos, size - pos);

      if (cpu_limit_base_ != 0) {
        FrameDimensions frame_dim = dec->frame_header->ToFrameDimensions();
        // No overflow, checked in ParseHeader.
        size_t num_pixels = frame_dim.xsize * frame_dim.ysize;
        if (used_cpu_base_ + num_pixels < used_cpu_base_) {
          return JXL_API_ERROR("used too much CPU");
        }
        used_cpu_base_ += num_pixels;
        if (used_cpu_base_ > cpu_limit_base_) {
          return JXL_API_ERROR("used too much CPU");
        }
      }

      jxl::Status status =
          dec->frame_dec->ProcessSections(dec->sections->section_info.data(),
                                          dec->sections->section_info.size(),
                                          dec->sections->section_status.data());
      JXL_API_RETURN_IF_ERROR(dec->sections->CloseInput());
      if (status.IsFatalError()) {
        return JXL_API_ERROR("decoding frame failed");
      }

      // TODO(lode): allow next_in to move forward if sections from the
      // beginning of the stream have been processed

      if (status.code() == StatusCode::kNotEnoughBytes ||
          dec->sections->section_info.size() < dec->frame_dec->NumSections()) {
        // Not all sections have been processed yet
        return JXL_DEC_NEED_MORE_INPUT;
      }

      size_t internal_index = dec->internal_frames - 1;
      JXL_ASSERT(dec->frame_references.size() > internal_index);
      // Always fill this in, even if it was already written, it could be that
      // this frame was skipped before and set to 255, while only now we know
      // the true value.
      dec->frame_references[internal_index] = dec->frame_dec->References();
      if (!dec->frame_dec->FinalizeFrame()) {
        return JXL_API_ERROR("decoding frame failed");
      }
      dec->frame_dec_in_progress = false;
      dec->frame_stage = FrameStage::kFullOutput;
    }

    if (dec->frame_stage == FrameStage::kFullOutput) {
      if (dec->is_last_of_still) {
        if (dec->events_wanted & JXL_DEC_FULL_IMAGE) {
          dec->events_wanted &= ~JXL_DEC_FULL_IMAGE;
          return_full_image = true;
        }

        // Frame finished, restore the events_wanted with the per-frame events
        // from orig_events_wanted, in case there is a next frame.
        dec->events_wanted |=
            (dec->orig_events_wanted & (JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME));

        // If no output buffer was set, we merely return the JXL_DEC_FULL_IMAGE
        // status without outputting pixels.
        if (dec->jpeg_decoder.IsOutputSet() && dec->ib->jpeg_data != nullptr) {
          JxlDecoderStatus status =
              dec->jpeg_decoder.WriteOutput(*dec->ib->jpeg_data);
          if (status != JXL_DEC_SUCCESS) return status;
        } else if (return_full_image && dec->image_out_buffer_set) {
          if (!dec->frame_dec->HasRGBBuffer()) {
            // Copy pixels if desired.
            JxlDecoderStatus status = ConvertImageInternal(
                dec, *dec->ib, dec->image_out_format,
                /*want_extra_channel=*/false,
                /*extra_channel_index=*/0, dec->image_out_buffer,
                dec->image_out_size, dec->image_out_callback,
                dec->image_out_opaque);
            if (status != JXL_DEC_SUCCESS) return status;
          }
          dec->image_out_buffer_set = false;

          for (size_t i = 0; i < dec->extra_channel_output.size(); ++i) {
            void* buffer = dec->extra_channel_output[i].buffer;
            // buffer nullptr indicates this extra channel is not requested
            if (!buffer) continue;
            const JxlPixelFormat* format = &dec->extra_channel_output[i].format;
            JxlDecoderStatus status = ConvertImageInternal(
                dec, *dec->ib, *format,
                /*want_extra_channel=*/true, i, buffer,
                dec->extra_channel_output[i].buffer_size, nullptr, nullptr);
            if (status != JXL_DEC_SUCCESS) return status;
          }

          dec->extra_channel_output.clear();
        }
      }
    }

    // The pixels have been output or are not needed, do not keep them in
    // memory here.
    dec->ib.reset();
    dec->frame_stage = FrameStage::kHeader;
    dec->frame_start += dec->frame_size;
    if (return_full_image && !dec->skipping_frame) {
      return JXL_DEC_FULL_IMAGE;
    }
  }

  dec->stage = DecoderStage::kFinished;
  // Return success, this means there is nothing more to do.
  return JXL_DEC_SUCCESS;
}

}  // namespace
}  // namespace jxl

JxlDecoderStatus JxlDecoderSetInput(JxlDecoder* dec, const uint8_t* data,
                                    size_t size) {
  if (dec->next_in) return JXL_DEC_ERROR;

  dec->next_in = data;
  dec->avail_in = size;
  return JXL_DEC_SUCCESS;
}

size_t JxlDecoderReleaseInput(JxlDecoder* dec) {
  size_t result = dec->avail_in;
  dec->next_in = nullptr;
  dec->avail_in = 0;
  return result;
}

JxlDecoderStatus JxlDecoderSetJPEGBuffer(JxlDecoder* dec, uint8_t* data,
                                         size_t size) {
  return dec->jpeg_decoder.SetOutputBuffer(data, size);
}

size_t JxlDecoderReleaseJPEGBuffer(JxlDecoder* dec) {
  return dec->jpeg_decoder.ReleaseOutputBuffer();
}

JxlDecoderStatus JxlDecoderProcessInput(JxlDecoder* dec) {
  const uint8_t** next_in = &dec->next_in;
  size_t* avail_in = &dec->avail_in;
  if (dec->stage == DecoderStage::kInited) {
    dec->stage = DecoderStage::kStarted;
  }
  if (dec->stage == DecoderStage::kError) {
    return JXL_API_ERROR(
        "Cannot keep using decoder after it encountered an error, use "
        "JxlDecoderReset to reset it");
  }
  if (dec->stage == DecoderStage::kFinished) {
    return JXL_API_ERROR(
        "Cannot keep using decoder after it finished, use JxlDecoderReset to "
        "reset it");
  }

  if (!dec->got_signature) {
    JxlSignature sig = JxlSignatureCheck(*next_in, *avail_in);
    if (sig == JXL_SIG_INVALID) return JXL_API_ERROR("invalid signature");
    if (sig == JXL_SIG_NOT_ENOUGH_BYTES) return JXL_DEC_NEED_MORE_INPUT;

    dec->got_signature = true;

    if (sig == JXL_SIG_CONTAINER) {
      dec->have_container = 1;
    }
  }

  // Available codestream bytes, may differ from *avail_in if there is another
  // box behind the current position, in the dec->have_container case.
  size_t csize = *avail_in;

  if (dec->have_container) {
    /*
    Process bytes as follows:
    *) find the box(es) containing the codestream
    *) support codestream split over multiple partial boxes
    *) avoid copying bytes to the codestream vector if the decoding will be
     one-shot, when the user already provided everything contiguously in
     memory
    *) copy to codestream vector, and update next_in so user can delete the data
    on their side, once we know it's not oneshot. This relieves the user from
    continuing to store the data.
    *) also copy to codestream if one-shot but the codestream is split across
    multiple boxes: this copying can be avoided in the future if the C++
    decoder is updated for streaming, but for now it requires all consecutive
    data at once.
    */

    if (dec->skip_box) {
      // Amount of remaining bytes in the box that is being skipped.
      size_t remaining = dec->box_end - dec->file_pos;
      if (*avail_in < remaining) {
        // Don't have the full box yet, skip all we have so far
        dec->file_pos += *avail_in;
        *next_in += *avail_in;
        *avail_in -= *avail_in;
        return JXL_DEC_NEED_MORE_INPUT;
      } else {
        // Full box available, skip all its remaining bytes
        dec->file_pos += remaining;
        *next_in += remaining;
        *avail_in -= remaining;
        dec->skip_box = false;
      }
    }

    if (dec->first_codestream_seen && !dec->last_codestream_seen &&
        dec->codestream_end != 0 && dec->file_pos < dec->codestream_end &&
        dec->file_pos + *avail_in >= dec->codestream_end &&
        !dec->codestream.empty()) {
      // dec->file_pos in a codestream, not in surrounding box format bytes, but
      // the end of the current codestream part is in the current input, and
      // boxes that can contain a next part of the codestream could be present.
      // Therefore, store the known codestream part, and ensure processing of
      // boxes below will trigger. This is only done if
      // !dec->codestream.empty(), that is, we're already streaming.

      // Size of the codestream, excluding potential boxes that come after it.
      csize = *avail_in;
      if (dec->codestream_end && csize > dec->codestream_end - dec->file_pos) {
        csize = dec->codestream_end - dec->file_pos;
      }
      dec->codestream.insert(dec->codestream.end(), *next_in, *next_in + csize);
      dec->file_pos += csize;
      *next_in += csize;
      *avail_in -= csize;
    }

    if (dec->jpeg_decoder.IsParsingBox()) {
      // We are inside a JPEG reconstruction box.
      JxlDecoderStatus recon_result =
          dec->jpeg_decoder.Process(next_in, avail_in);
      if (recon_result == JXL_DEC_JPEG_RECONSTRUCTION) {
        // If successful JPEG reconstruction, return the success if the user
        // cares about it, otherwise continue.
        if (dec->events_wanted & recon_result) {
          dec->events_wanted &= ~recon_result;
          return recon_result;
        }
      } else {
        // If anything else, return the result.
        return recon_result;
      }
    }

    if (!dec->last_codestream_seen &&
        (dec->codestream_begin == 0 ||
         (dec->codestream_end != 0 && dec->file_pos >= dec->codestream_end))) {
      size_t pos = 0;
      // after this for loop, either we should be in a part of the data that is
      // codestream (not boxes), or have returned that we need more input.
      for (;;) {
        const uint8_t* in = *next_in;
        size_t size = *avail_in;
        if (size == pos) {
          // If the remaining size is 0, we are exactly after a full box. We
          // can't know for sure if this is the last box or not since more bytes
          // can follow, but do not return NEED_MORE_INPUT, instead break and
          // let the codestream-handling code determine if we need more.
          break;
        }
        if (OutOfBounds(pos, 8, size)) {
          dec->basic_info_size_hint =
              InitialBasicInfoSizeHint() + pos + 8 - dec->file_pos;
          return JXL_DEC_NEED_MORE_INPUT;
        }
        size_t box_start = pos;
        // Box size, including this header itself.
        uint64_t box_size = LoadBE32(in + pos);
        char type[5] = {0};
        memcpy(type, in + pos + 4, 4);
        pos += 8;
        if (box_size == 1) {
          if (OutOfBounds(pos, 8, size)) return JXL_DEC_NEED_MORE_INPUT;
          box_size = LoadBE64(in + pos);
          pos += 8;
        }
        size_t header_size = pos - box_start;
        if (box_size > 0 && box_size < header_size) {
          return JXL_API_ERROR("invalid box size");
        }
        if (SumOverflows(dec->file_pos, pos, box_size)) {
          return JXL_API_ERROR("Box size overflow");
        }
        size_t contents_size =
            (box_size == 0) ? 0 : (box_size - pos + box_start);

        dec->box_begin = box_start;
        dec->box_end = dec->file_pos + box_start + box_size;
        if (strcmp(type, "jxlc") == 0 || strcmp(type, "jxlp") == 0) {
          size_t codestream_size = contents_size;
          // Whether this is the last codestream box, either when it is a jxlc
          // box, or when it is a jxlp box that has the final bit set.
          // The codestream is either contained within a single jxlc box, or
          // within one or more jxlp boxes. The final jxlp box is marked as last
          // by setting the high bit of its 4-byte box-index value.
          bool last_codestream = false;
          if (strcmp(type, "jxlp") == 0) {
            if (OutOfBounds(pos, 4, size)) return JXL_DEC_NEED_MORE_INPUT;
            if (box_size != 0 && contents_size < 4) {
              return JXL_API_ERROR("jxlp box too small to contain index");
            }
            codestream_size -= 4;
            size_t jxlp_index = LoadBE32(in + pos);
            pos += 4;
            // The high bit of jxlp_index indicates whether this is the last
            // jxlp box.
            if (jxlp_index & 0x80000000) last_codestream = true;
          } else if (strcmp(type, "jxlc") == 0) {
            last_codestream = true;
          }
          if (!last_codestream && box_size == 0) {
            return JXL_API_ERROR(
                "final box has unbounded size, but is a non-final codestream "
                "box");
          }
          dec->first_codestream_seen = true;
          if (last_codestream) dec->last_codestream_seen = true;
          if (dec->codestream_begin != 0 && dec->codestream.empty()) {
            // We've already seen a codestream part, so it's a stream spanning
            // multiple boxes.
            // We have no choice but to copy contents to the codestream
            // vector to make it a contiguous stream for the C++ decoder.
            // This appends the previous codestream box that we had seen to
            // dec->codestream.
            if (dec->codestream_begin < dec->file_pos) {
              return JXL_API_ERROR("earlier codestream box out of range");
            }
            size_t begin = dec->codestream_begin - dec->file_pos;
            size_t end = dec->codestream_end - dec->file_pos;
            JXL_ASSERT(end <= *avail_in);
            dec->codestream.insert(dec->codestream.end(), *next_in + begin,
                                   *next_in + end);
          }
          dec->codestream_begin = dec->file_pos + pos;
          dec->codestream_end =
              (box_size == 0) ? 0 : (dec->codestream_begin + codestream_size);
          size_t avail_codestream_size =
              (box_size == 0)
                  ? (size - pos)
                  : std::min<size_t>(size - pos, box_size - pos + box_start);
          // If already appending codestream, append what we have here too
          if (!dec->codestream.empty()) {
            size_t begin = pos;
            size_t end =
                std::min<size_t>(*avail_in, begin + avail_codestream_size);
            dec->codestream.insert(dec->codestream.end(), *next_in + begin,
                                   *next_in + end);
            pos += (end - begin);
            dec->file_pos += pos;
            *next_in += pos;
            *avail_in -= pos;
            pos = 0;
            // TODO(lode): check if this should break always instead, and
            // process what we have of the codestream so far, to support
            // progressive decoding, and get events such as basic info faster.
            // The user could have given 1.5 boxes here, and the first one could
            // contain useful parts of codestream that can already be processed.
            // Similar to several other exact avail_size checks. This may not
            // need to be changed here, but instead at the point in this for
            // loop where it returns "NEED_MORE_INPUT", it could instead break
            // and allow decoding what we have of the codestream so far.
            if (*avail_in == 0) break;
          } else {
            // skip only the header, so next_in points to the start of this new
            // codestream part, for the one-shot case where user data is not
            // (yet) copied to dec->codestream.
            dec->file_pos += pos;
            *next_in += pos;
            *avail_in -= pos;
            pos = 0;
            // Update pos to be after the box contents with codestream
            if (avail_codestream_size == *avail_in) {
              break;  // the rest is codestream, this loop is done
            }
            pos += avail_codestream_size;
          }
        } else if ((JPEGXL_ENABLE_TRANSCODE_JPEG) &&
                   (dec->orig_events_wanted & JXL_DEC_JPEG_RECONSTRUCTION) &&
                   strcmp(type, "jbrd") == 0) {
          // This is a new JPEG reconstruction metadata box.
          dec->jpeg_decoder.StartBox(box_size, contents_size);
          dec->file_pos += pos;
          *next_in += pos;
          *avail_in -= pos;
          pos = 0;
          JxlDecoderStatus recon_result =
              dec->jpeg_decoder.Process(next_in, avail_in);
          if (recon_result == JXL_DEC_JPEG_RECONSTRUCTION) {
            // If successful JPEG reconstruction, return the success if the user
            // cares about it, otherwise continue.
            if (dec->events_wanted & recon_result) {
              dec->events_wanted &= ~recon_result;
              return recon_result;
            }
          } else {
            // If anything else, return the result.
            return recon_result;
          }
        } else {
          if (box_size == 0) {
            // Final box with unknown size, but it's not a codestream box, so
            // nothing more to do.
            if (!dec->first_codestream_seen) {
              return JXL_API_ERROR("didn't find any codestream box");
            }
            break;
          }
          if (OutOfBounds(pos, contents_size, size)) {
            dec->skip_box = true;
            dec->file_pos += pos;
            *next_in += pos;
            *avail_in -= pos;
            // Indicate how many more bytes needed starting from *next_in.
            dec->basic_info_size_hint = InitialBasicInfoSizeHint() + pos +
                                        contents_size - dec->file_pos;
            return JXL_DEC_NEED_MORE_INPUT;
          }
          pos += contents_size;
          if (!(dec->codestream.empty() && dec->first_codestream_seen)) {
            // Last box no longer needed since we have copied the codestream
            // buffer, remove from input so user can release memory.
            dec->file_pos += pos;
            *next_in += pos;
            *avail_in -= pos;
            pos = 0;
          }
        }
      }
    }

    // Size of the codestream, excluding potential boxes that come after it.
    csize = *avail_in;
    if (dec->codestream_end && csize > dec->codestream_end - dec->file_pos) {
      csize = dec->codestream_end - dec->file_pos;
    }
  }

  // Whether we are taking the input directly from the user (oneshot case,
  // without copying bytes), or appending parts of input to dec->codestream
  // (streaming)
  bool detected_streaming = !dec->codestream.empty();
  JxlDecoderStatus result;
  JXL_DASSERT(csize <= *avail_in);

  if (detected_streaming) {
    dec->codestream.insert(dec->codestream.end(), *next_in, *next_in + csize);
    dec->file_pos += csize;
    *next_in += csize;
    *avail_in -= csize;
    result = jxl::JxlDecoderProcessInternal(dec, dec->codestream.data(),
                                            dec->codestream.size());
  } else {
    // No data copied to codestream buffer yet, the user input may contain the
    // full codestream.
    result = jxl::JxlDecoderProcessInternal(dec, *next_in, csize);
    // Copy the user's input bytes to the codestream once we are able to and
    // it is needed. Before we got the basic info, we're still parsing the box
    // format instead. If the result is not JXL_DEC_NEED_MORE_INPUT, then
    // there is no reason yet to copy since the user may have a full buffer
    // allowing one-shot. Once JXL_DEC_NEED_MORE_INPUT occurred at least once,
    // start copying over the codestream bytes and allow user to free them
    // instead. Next call, detected_streaming will be true.
    if (dec->got_basic_info && result == JXL_DEC_NEED_MORE_INPUT) {
      dec->codestream.insert(dec->codestream.end(), *next_in, *next_in + csize);
      dec->file_pos += csize;
      *next_in += csize;
      *avail_in -= csize;
    }
  }

  return result;
}

JxlDecoderStatus JxlDecoderGetBasicInfo(const JxlDecoder* dec,
                                        JxlBasicInfo* info) {
  if (!dec->got_basic_info) return JXL_DEC_NEED_MORE_INPUT;

  if (info) {
    const jxl::ImageMetadata& meta = dec->metadata.m;

    info->have_container = dec->have_container;
    info->xsize = dec->metadata.size.xsize();
    info->ysize = dec->metadata.size.ysize();
    info->uses_original_profile = !meta.xyb_encoded;

    info->bits_per_sample = meta.bit_depth.bits_per_sample;
    info->exponent_bits_per_sample = meta.bit_depth.exponent_bits_per_sample;

    info->have_preview = meta.have_preview;
    info->have_animation = meta.have_animation;
    // TODO(janwas): intrinsic_size
    info->orientation = static_cast<JxlOrientation>(meta.orientation);

    if (!dec->keep_orientation) {
      if (info->orientation >= JXL_ORIENT_TRANSPOSE) {
        std::swap(info->xsize, info->ysize);
      }
      info->orientation = JXL_ORIENT_IDENTITY;
    }

    info->intensity_target = meta.IntensityTarget();
    info->min_nits = meta.tone_mapping.min_nits;
    info->relative_to_max_display = meta.tone_mapping.relative_to_max_display;
    info->linear_below = meta.tone_mapping.linear_below;

    const jxl::ExtraChannelInfo* alpha = meta.Find(jxl::ExtraChannel::kAlpha);
    if (alpha != nullptr) {
      info->alpha_bits = alpha->bit_depth.bits_per_sample;
      info->alpha_exponent_bits = alpha->bit_depth.exponent_bits_per_sample;
      info->alpha_premultiplied = alpha->alpha_associated;
    } else {
      info->alpha_bits = 0;
      info->alpha_exponent_bits = 0;
      info->alpha_premultiplied = 0;
    }

    info->num_color_channels =
        meta.color_encoding.GetColorSpace() == jxl::ColorSpace::kGray ? 1 : 3;

    info->num_extra_channels = meta.num_extra_channels;

    if (info->have_preview) {
      info->preview.xsize = dec->metadata.m.preview_size.xsize();
      info->preview.ysize = dec->metadata.m.preview_size.ysize();
    }

    if (info->have_animation) {
      info->animation.tps_numerator = dec->metadata.m.animation.tps_numerator;
      info->animation.tps_denominator =
          dec->metadata.m.animation.tps_denominator;
      info->animation.num_loops = dec->metadata.m.animation.num_loops;
      info->animation.have_timecodes = dec->metadata.m.animation.have_timecodes;
    }
  }

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderGetExtraChannelInfo(const JxlDecoder* dec,
                                               size_t index,
                                               JxlExtraChannelInfo* info) {
  if (!dec->got_basic_info) return JXL_DEC_NEED_MORE_INPUT;

  const std::vector<jxl::ExtraChannelInfo>& channels =
      dec->metadata.m.extra_channel_info;

  if (index >= channels.size()) return JXL_DEC_ERROR;  // out of bounds
  const jxl::ExtraChannelInfo& channel = channels[index];

  info->type = static_cast<JxlExtraChannelType>(channel.type);
  info->bits_per_sample = channel.bit_depth.bits_per_sample;
  info->exponent_bits_per_sample =
      channel.bit_depth.floating_point_sample
          ? channel.bit_depth.exponent_bits_per_sample
          : 0;
  info->dim_shift = channel.dim_shift;
  info->name_length = channel.name.size();
  info->alpha_premultiplied = channel.alpha_associated;
  info->spot_color[0] = channel.spot_color[0];
  info->spot_color[1] = channel.spot_color[1];
  info->spot_color[2] = channel.spot_color[2];
  info->spot_color[3] = channel.spot_color[3];
  info->cfa_channel = channel.cfa_channel;

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderGetExtraChannelName(const JxlDecoder* dec,
                                               size_t index, char* name,
                                               size_t size) {
  if (!dec->got_basic_info) return JXL_DEC_NEED_MORE_INPUT;

  const std::vector<jxl::ExtraChannelInfo>& channels =
      dec->metadata.m.extra_channel_info;

  if (index >= channels.size()) return JXL_DEC_ERROR;  // out of bounds
  const jxl::ExtraChannelInfo& channel = channels[index];

  // Also need null-termination character
  if (channel.name.size() + 1 > size) return JXL_DEC_ERROR;

  memcpy(name, channel.name.c_str(), channel.name.size() + 1);

  return JXL_DEC_SUCCESS;
}

namespace {

// Gets the jxl::ColorEncoding for the desired target, and checks errors.
// Returns the object regardless of whether the actual color space is in ICC,
// but ensures that if the color encoding is not the encoding from the
// codestream header metadata, it cannot require ICC profile.
JxlDecoderStatus GetColorEncodingForTarget(
    const JxlDecoder* dec, const JxlPixelFormat* format,
    JxlColorProfileTarget target, const jxl::ColorEncoding** encoding) {
  if (!dec->got_all_headers) return JXL_DEC_NEED_MORE_INPUT;
  *encoding = nullptr;
  if (target == JXL_COLOR_PROFILE_TARGET_DATA && dec->metadata.m.xyb_encoded) {
    *encoding = &dec->passes_state->output_encoding_info.color_encoding;
  } else {
    *encoding = &dec->metadata.m.color_encoding;
  }
  return JXL_DEC_SUCCESS;
}
}  // namespace

JxlDecoderStatus JxlDecoderGetColorAsEncodedProfile(
    const JxlDecoder* dec, const JxlPixelFormat* format,
    JxlColorProfileTarget target, JxlColorEncoding* color_encoding) {
  const jxl::ColorEncoding* jxl_color_encoding = nullptr;
  JxlDecoderStatus status =
      GetColorEncodingForTarget(dec, format, target, &jxl_color_encoding);
  if (status) return status;

  if (jxl_color_encoding->WantICC())
    return JXL_DEC_ERROR;  // Indicate no encoded profile available.

  if (color_encoding) {
    ConvertInternalToExternalColorEncoding(*jxl_color_encoding, color_encoding);
  }

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderGetICCProfileSize(const JxlDecoder* dec,
                                             const JxlPixelFormat* format,
                                             JxlColorProfileTarget target,
                                             size_t* size) {
  const jxl::ColorEncoding* jxl_color_encoding = nullptr;
  JxlDecoderStatus status =
      GetColorEncodingForTarget(dec, format, target, &jxl_color_encoding);
  if (status != JXL_DEC_SUCCESS) return status;

  if (jxl_color_encoding->WantICC()) {
    jxl::ColorSpace color_space =
        dec->metadata.m.color_encoding.GetColorSpace();
    if (color_space == jxl::ColorSpace::kUnknown ||
        color_space == jxl::ColorSpace::kXYB) {
      // This indicates there's no ICC profile available
      // TODO(lode): for the XYB case, do we want to craft an ICC profile that
      // represents XYB as an RGB profile? It may be possible, but not with
      // only 1D transfer functions.
      return JXL_DEC_ERROR;
    }
  }

  if (size) {
    *size = jxl_color_encoding->ICC().size();
  }

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderGetColorAsICCProfile(const JxlDecoder* dec,
                                                const JxlPixelFormat* format,
                                                JxlColorProfileTarget target,
                                                uint8_t* icc_profile,
                                                size_t size) {
  size_t wanted_size;
  // This also checks the NEED_MORE_INPUT and the unknown/xyb cases
  JxlDecoderStatus status =
      JxlDecoderGetICCProfileSize(dec, format, target, &wanted_size);
  if (status != JXL_DEC_SUCCESS) return status;
  if (size < wanted_size) return JXL_API_ERROR("ICC profile output too small");

  const jxl::ColorEncoding* jxl_color_encoding = nullptr;
  status = GetColorEncodingForTarget(dec, format, target, &jxl_color_encoding);
  if (status != JXL_DEC_SUCCESS) return status;

  memcpy(icc_profile, jxl_color_encoding->ICC().data(),
         jxl_color_encoding->ICC().size());

  return JXL_DEC_SUCCESS;
}

namespace {

// Returns the amount of bits needed for getting memory buffer size, and does
// all error checking required for size checking and format validity.
JxlDecoderStatus PrepareSizeCheck(const JxlDecoder* dec,
                                  const JxlPixelFormat* format, size_t* bits) {
  if (!dec->got_basic_info) {
    // Don't know image dimensions yet, cannot check for valid size.
    return JXL_DEC_NEED_MORE_INPUT;
  }
  if (format->num_channels > 4) {
    return JXL_API_ERROR("More than 4 channels not supported");
  }
  if (format->data_type == JXL_TYPE_BOOLEAN) {
    return JXL_API_ERROR("Boolean data type not yet supported");
  }
  if (format->data_type == JXL_TYPE_UINT32) {
    return JXL_API_ERROR("uint32 data type not yet supported");
  }

  *bits = BitsPerChannel(format->data_type);

  if (*bits == 0) {
    return JXL_API_ERROR("Invalid data type");
  }

  return JXL_DEC_SUCCESS;
}
}  // namespace

JxlDecoderStatus JxlDecoderFlushImage(JxlDecoder* dec) {
  if (!dec->image_out_buffer) return JXL_DEC_ERROR;
  if (!dec->sections || dec->sections->section_info.empty()) {
    return JXL_DEC_ERROR;
  }
  if (!dec->frame_dec || !dec->frame_dec_in_progress) {
    return JXL_DEC_ERROR;
  }
  if (!dec->frame_dec->HasDecodedDC()) {
    // FrameDecoder::Fush currently requires DC to have been decoded already
    // to work correctly.
    return JXL_DEC_ERROR;
  }
  if (dec->frame_header->encoding != jxl::FrameEncoding::kVarDCT) {
    // Flushing does not yet work correctly if the frame uses modular encoding.
    return JXL_DEC_ERROR;
  }
  if (dec->metadata.m.num_extra_channels > 0) {
    // Flushing does not yet work correctly if there are extra channels, which
    // use modular
    return JXL_DEC_ERROR;
  }

  if (!dec->frame_dec->Flush()) {
    return JXL_DEC_ERROR;
  }

  if (dec->frame_dec->HasRGBBuffer()) {
    return JXL_DEC_SUCCESS;
  }

  // Temporarily shrink `dec->ib` to the actual size of the full image to call
  // ConvertImageInternal.
  size_t xsize = dec->ib->xsize();
  size_t ysize = dec->ib->ysize();
  dec->ib->ShrinkTo(dec->metadata.size.xsize(), dec->metadata.size.ysize());
  JxlDecoderStatus status = jxl::ConvertImageInternal(
      dec, *dec->ib, dec->image_out_format, /*want_extra_channel=*/false,
      /*extra_channel_index=*/0, dec->image_out_buffer, dec->image_out_size,
      /*out_callback=*/nullptr, /*out_opaque=*/nullptr);
  dec->ib->ShrinkTo(xsize, ysize);
  if (status != JXL_DEC_SUCCESS) return status;
  return JXL_DEC_SUCCESS;
}

JXL_EXPORT JxlDecoderStatus JxlDecoderPreviewOutBufferSize(
    const JxlDecoder* dec, const JxlPixelFormat* format, size_t* size) {
  size_t bits;
  JxlDecoderStatus status = PrepareSizeCheck(dec, format, &bits);
  if (status != JXL_DEC_SUCCESS) return status;
  if (format->num_channels < 3 && !dec->metadata.m.color_encoding.IsGray()) {
    return JXL_API_ERROR("Grayscale output not possible for color image");
  }

  size_t xsize = dec->metadata.oriented_preview_xsize(dec->keep_orientation);
  size_t ysize = dec->metadata.oriented_preview_ysize(dec->keep_orientation);

  size_t row_size =
      jxl::DivCeil(xsize * format->num_channels * bits, jxl::kBitsPerByte);
  if (format->align > 1) {
    row_size = jxl::DivCeil(row_size, format->align) * format->align;
  }
  *size = row_size * ysize;
  return JXL_DEC_SUCCESS;
}

JXL_EXPORT JxlDecoderStatus JxlDecoderSetPreviewOutBuffer(
    JxlDecoder* dec, const JxlPixelFormat* format, void* buffer, size_t size) {
  if (!dec->got_basic_info || !dec->metadata.m.have_preview ||
      !(dec->orig_events_wanted & JXL_DEC_PREVIEW_IMAGE)) {
    return JXL_API_ERROR("No preview out buffer needed at this time");
  }
  if (format->num_channels < 3 && !dec->metadata.m.color_encoding.IsGray()) {
    return JXL_API_ERROR("Grayscale output not possible for color image");
  }

  size_t min_size;
  // This also checks whether the format is valid and supported and basic info
  // is available.
  JxlDecoderStatus status =
      JxlDecoderPreviewOutBufferSize(dec, format, &min_size);
  if (status != JXL_DEC_SUCCESS) return status;

  if (size < min_size) return JXL_DEC_ERROR;

  dec->preview_out_buffer_set = true;
  dec->preview_out_buffer = buffer;
  dec->preview_out_size = size;
  dec->preview_out_format = *format;

  return JXL_DEC_SUCCESS;
}

JXL_EXPORT JxlDecoderStatus JxlDecoderDCOutBufferSize(
    const JxlDecoder* dec, const JxlPixelFormat* format, size_t* size) {
  size_t bits;
  JxlDecoderStatus status = PrepareSizeCheck(dec, format, &bits);
  if (status != JXL_DEC_SUCCESS) return status;

  size_t xsize = jxl::DivCeil(
      dec->metadata.oriented_xsize(dec->keep_orientation), jxl::kBlockDim);
  size_t ysize = jxl::DivCeil(
      dec->metadata.oriented_ysize(dec->keep_orientation), jxl::kBlockDim);

  size_t row_size =
      jxl::DivCeil(xsize * format->num_channels * bits, jxl::kBitsPerByte);
  if (format->align > 1) {
    row_size = jxl::DivCeil(row_size, format->align) * format->align;
  }
  *size = row_size * ysize;
  return JXL_DEC_SUCCESS;
}

JXL_EXPORT JxlDecoderStatus JxlDecoderSetDCOutBuffer(
    JxlDecoder* dec, const JxlPixelFormat* format, void* buffer, size_t size) {
  // No buffer set: this feature is deprecated
  return JXL_DEC_SUCCESS;
}

JXL_EXPORT JxlDecoderStatus JxlDecoderImageOutBufferSize(
    const JxlDecoder* dec, const JxlPixelFormat* format, size_t* size) {
  size_t bits;
  JxlDecoderStatus status = PrepareSizeCheck(dec, format, &bits);
  if (status != JXL_DEC_SUCCESS) return status;
  if (format->num_channels < 3 && !dec->metadata.m.color_encoding.IsGray()) {
    return JXL_API_ERROR("Grayscale output not possible for color image");
  }

  size_t row_size =
      jxl::DivCeil(dec->metadata.oriented_xsize(dec->keep_orientation) *
                       format->num_channels * bits,
                   jxl::kBitsPerByte);
  if (format->align > 1) {
    row_size = jxl::DivCeil(row_size, format->align) * format->align;
  }
  *size = row_size * dec->metadata.oriented_ysize(dec->keep_orientation);

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderSetImageOutBuffer(JxlDecoder* dec,
                                             const JxlPixelFormat* format,
                                             void* buffer, size_t size) {
  if (!dec->got_basic_info || !(dec->orig_events_wanted & JXL_DEC_FULL_IMAGE)) {
    return JXL_API_ERROR("No image out buffer needed at this time");
  }
  if (dec->image_out_buffer_set && !!dec->image_out_callback) {
    return JXL_API_ERROR(
        "Cannot change from image out callback to image out buffer");
  }
  if (format->num_channels < 3 && !dec->metadata.m.color_encoding.IsGray()) {
    return JXL_API_ERROR("Grayscale output not possible for color image");
  }
  size_t min_size;
  // This also checks whether the format is valid and supported and basic info
  // is available.
  JxlDecoderStatus status =
      JxlDecoderImageOutBufferSize(dec, format, &min_size);
  if (status != JXL_DEC_SUCCESS) return status;

  if (size < min_size) return JXL_DEC_ERROR;

  dec->image_out_buffer_set = true;
  dec->image_out_buffer = buffer;
  dec->image_out_size = size;
  dec->image_out_format = *format;

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderExtraChannelBufferSize(const JxlDecoder* dec,
                                                  const JxlPixelFormat* format,
                                                  size_t* size,
                                                  uint32_t index) {
  if (!dec->got_basic_info || !(dec->orig_events_wanted & JXL_DEC_FULL_IMAGE)) {
    return JXL_API_ERROR("No extra channel buffer needed at this time");
  }

  if (index >= dec->metadata.m.num_extra_channels) {
    return JXL_API_ERROR("Invalid extra channel index");
  }

  size_t num_channels = 1;  // Do not use format's num_channels

  size_t bits;
  JxlDecoderStatus status = PrepareSizeCheck(dec, format, &bits);
  if (status != JXL_DEC_SUCCESS) return status;

  size_t row_size = jxl::DivCeil(
      dec->metadata.oriented_xsize(dec->keep_orientation) * num_channels * bits,
      jxl::kBitsPerByte);
  if (format->align > 1) {
    row_size = jxl::DivCeil(row_size, format->align) * format->align;
  }
  *size = row_size * dec->metadata.oriented_ysize(dec->keep_orientation);

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderSetExtraChannelBuffer(JxlDecoder* dec,
                                                 const JxlPixelFormat* format,
                                                 void* buffer, size_t size,
                                                 uint32_t index) {
  size_t min_size;
  // This also checks whether the format and index are valid and supported and
  // basic info is available.
  JxlDecoderStatus status =
      JxlDecoderExtraChannelBufferSize(dec, format, &min_size, index);
  if (status != JXL_DEC_SUCCESS) return status;

  if (size < min_size) return JXL_DEC_ERROR;

  if (dec->extra_channel_output.size() <= index) {
    dec->extra_channel_output.resize(dec->metadata.m.num_extra_channels,
                                     {{}, nullptr, 0});
  }
  // Guaranteed correct thanks to check in JxlDecoderExtraChannelBufferSize.
  JXL_ASSERT(index < dec->extra_channel_output.size());

  dec->extra_channel_output[index].format = *format;
  dec->extra_channel_output[index].format.num_channels = 1;
  dec->extra_channel_output[index].buffer = buffer;
  dec->extra_channel_output[index].buffer_size = size;

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderSetImageOutCallback(JxlDecoder* dec,
                                               const JxlPixelFormat* format,
                                               JxlImageOutCallback callback,
                                               void* opaque) {
  if (dec->image_out_buffer_set && !!dec->image_out_buffer) {
    return JXL_API_ERROR(
        "Cannot change from image out buffer to image out callback");
  }

  // Perform error checking for invalid format.
  size_t bits_dummy;
  JxlDecoderStatus status = PrepareSizeCheck(dec, format, &bits_dummy);
  if (status != JXL_DEC_SUCCESS) return status;

  dec->image_out_buffer_set = true;
  dec->image_out_callback = callback;
  dec->image_out_opaque = opaque;
  dec->image_out_format = *format;

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderGetFrameHeader(const JxlDecoder* dec,
                                          JxlFrameHeader* header) {
  if (!dec->frame_header || dec->frame_stage == FrameStage::kHeader) {
    return JXL_API_ERROR("no frame header available");
  }
  const auto& metadata = dec->metadata.m;
  if (metadata.have_animation) {
    header->duration = dec->frame_header->animation_frame.duration;
    if (metadata.animation.have_timecodes) {
      header->timecode = dec->frame_header->animation_frame.timecode;
    }
  }
  header->name_length = dec->frame_header->name.size();
  header->is_last = dec->frame_header->is_last;

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderGetFrameName(const JxlDecoder* dec, char* name,
                                        size_t size) {
  if (!dec->frame_header || dec->frame_stage == FrameStage::kHeader) {
    return JXL_API_ERROR("no frame header available");
  }
  if (size < dec->frame_header->name.size() + 1) {
    return JXL_API_ERROR("too small frame name output buffer");
  }
  memcpy(name, dec->frame_header->name.c_str(),
         dec->frame_header->name.size() + 1);

  return JXL_DEC_SUCCESS;
}

JxlDecoderStatus JxlDecoderSetPreferredColorProfile(
    JxlDecoder* dec, const JxlColorEncoding* color_encoding) {
  if (!dec->got_all_headers) {
    return JXL_API_ERROR("color info not yet available");
  }
  if (dec->post_headers) {
    return JXL_API_ERROR("too late to set the color encoding");
  }
  if (dec->metadata.m.color_encoding.IsGray() !=
      (color_encoding->color_space == JXL_COLOR_SPACE_GRAY)) {
    return JXL_API_ERROR("grayscale mismatch");
  }
  if (color_encoding->color_space == JXL_COLOR_SPACE_UNKNOWN ||
      color_encoding->color_space == JXL_COLOR_SPACE_XYB) {
    return JXL_API_ERROR("only RGB or grayscale output supported");
  }

  JXL_API_RETURN_IF_ERROR(ConvertExternalToInternalColorEncoding(
      *color_encoding, &dec->default_enc));
  JXL_API_RETURN_IF_ERROR(dec->passes_state->output_encoding_info.Set(
      dec->metadata, dec->default_enc));
  return JXL_DEC_SUCCESS;
}

// This function is "package-private". It is only used by fuzzer to avoid
// running cases that are too memory / CPU hungry. Limitations are applied
// at mid-level API. In the future high-level API would also include the
// means of limiting / throttling memory / CPU usage.
void SetDecoderMemoryLimitBase_(size_t memory_limit_base) {
  memory_limit_base_ = memory_limit_base;
  // Allow 5 x max_image_size processing units; every frame is accounted
  // as W x H CPU processing units, so there could be numerous small frames
  // or few larger ones.
  cpu_limit_base_ = 5 * memory_limit_base;
}
