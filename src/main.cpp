// wwe2k15 - ReXGlue Recompiled Project

#include "generated/default/wwe2k15_init.h"

#include "wwe2k15_app.h"

REXCVAR_DEFINE_STRING(wwe2k15_graphics_backend, "any", "GPU",
                      "Graphics backend: any, d3d12, vulkan");
REX_DEFINE_APP(wwe2k15, Wwe2k15App::Create)
