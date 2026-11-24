// FFmpeg's OpenH264 encoder wrapper (libavcodec/libopenh264enc.c) is C, and
// calls the encoder through OpenH264's C interface: an object whose first
// member points at a table of function pointers, which is how C++ lays out
// ISVCEncoder's vtable in the usual ABI. zebkit's Linux build uses Clang's
// relative vtables (use_relative_vtables_abi, profiles/slim.gn), whose slots
// are 32-bit offsets rather than pointers, and the C calls would jump through
// them. patches/1090 has the wrapper create and destroy its encoder here: an
// object of the C layout whose functions call the C++ encoder, under any
// vtable ABI, so on every system alike.

#include <new>

#include "wels/codec_api.h"

namespace {

// OpenH264's ISVCEncoderVtbl, as its C interface declares it (codec_api.h,
// without __cplusplus): each function takes the C object, a pointer to the
// table's pointer.
struct Table;
using Object = const Table*;
struct Table {
  int (*Initialize)(Object*, const SEncParamBase*);
  int (*InitializeExt)(Object*, const SEncParamExt*);
  int (*GetDefaultParams)(Object*, SEncParamExt*);
  int (*Uninitialize)(Object*);
  int (*EncodeFrame)(Object*, const SSourcePicture*, SFrameBSInfo*);
  int (*EncodeParameterSets)(Object*, SFrameBSInfo*);
  int (*ForceIntraFrame)(Object*, bool);
  int (*SetOption)(Object*, ENCODER_OPTION, void*);
  int (*GetOption)(Object*, ENCODER_OPTION, void*);
};

// The C object, and the C++ encoder it calls.
struct Encoder {
  Object table;
  ISVCEncoder* encoder;
};

ISVCEncoder* Of(Object* object) {
  return reinterpret_cast<Encoder*>(object)->encoder;
}

const Table kTable = {
    [](Object* o, const SEncParamBase* p) { return Of(o)->Initialize(p); },
    [](Object* o, const SEncParamExt* p) { return Of(o)->InitializeExt(p); },
    [](Object* o, SEncParamExt* p) { return Of(o)->GetDefaultParams(p); },
    [](Object* o) { return Of(o)->Uninitialize(); },
    [](Object* o, const SSourcePicture* p, SFrameBSInfo* i) { return Of(o)->EncodeFrame(p, i); },
    [](Object* o, SFrameBSInfo* i) { return Of(o)->EncodeParameterSets(i); },
    [](Object* o, bool idr) { return Of(o)->ForceIntraFrame(idr); },
    [](Object* o, ENCODER_OPTION id, void* v) { return Of(o)->SetOption(id, v); },
    [](Object* o, ENCODER_OPTION id, void* v) { return Of(o)->GetOption(id, v); },
};

}  // namespace

// WelsCreateSVCEncoder() and WelsDestroySVCEncoder() for the C wrapper, which
// patches/1090 renames to these (patches/1093: outside the exports' zebkit_*).
extern "C" int openh264_zebkit_create_encoder(Object** out) {
  *out = nullptr;
  ISVCEncoder* encoder = nullptr;
  if (WelsCreateSVCEncoder(&encoder) || !encoder)
    return 1;
  Encoder* object = new (std::nothrow) Encoder{&kTable, encoder};
  if (!object) {
    WelsDestroySVCEncoder(encoder);
    return 1;
  }
  *out = &object->table;
  return 0;
}

extern "C" void openh264_zebkit_destroy_encoder(Object* object) {
  if (!object)
    return;
  Encoder* encoder = reinterpret_cast<Encoder*>(object);
  WelsDestroySVCEncoder(encoder->encoder);
  delete encoder;
}
