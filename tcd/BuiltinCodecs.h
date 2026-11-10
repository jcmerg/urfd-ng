#pragma once

// BuiltinCodecs -- tcd's own vocoders as codec plugins without a shared object:
// "op25" (P25 IMBE, the OP25 vocoder in imbe/) and "codec2" (M17, Codec2 3200
// and 1600 in codec2/). CCodecPlugins puts them before the plugins of the
// CodecPlugin lines, so every vocoder in software goes through CSoftCodec.
// Two plugins, not one: each has a lock of its own, so P25 and M17 do not
// wait for each other.
//
// Copyright (C) 2026
// Licensed under the GNU General Public License v3 or later

#include "CodecPlugin.h"

extern const tcd_codec_plugin g_BuiltinOp25;
extern const tcd_codec_plugin g_BuiltinCodec2;
