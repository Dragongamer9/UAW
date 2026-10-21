#include "gl.hpp"

#define UAW_GL_DEFINE(ret, name, ...) PFN_##name name = nullptr;
UAW_GL_FUNCS(UAW_GL_DEFINE)
#undef UAW_GL_DEFINE

namespace uaw::gl {

const char* load(ProcLoader loader) {
#define UAW_GL_LOAD(ret, name, ...)                                    \
    name = reinterpret_cast<PFN_##name>(loader(#name));               \
    if (!name) return #name;
    UAW_GL_FUNCS(UAW_GL_LOAD)
#undef UAW_GL_LOAD
    return nullptr;
}

} // namespace uaw::gl
