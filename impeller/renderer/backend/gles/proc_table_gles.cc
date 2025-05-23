// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/proc_table_gles.h"

#include <sstream>

#include "impeller/base/allocation.h"
#include "impeller/base/comparable.h"
#include "impeller/base/validation.h"
#include "impeller/renderer/backend/gles/capabilities_gles.h"
#include "impeller/renderer/capabilities.h"

namespace impeller {

const char* GLErrorToString(GLenum value) {
  switch (value) {
    case GL_NO_ERROR:
      return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
      return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
      return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
      return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
      return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_FRAMEBUFFER_COMPLETE:
      return "GL_FRAMEBUFFER_COMPLETE";
    case GL_OUT_OF_MEMORY:
      return "GL_OUT_OF_MEMORY";
  }
  return "Unknown.";
}

bool GLErrorIsFatal(GLenum value) {
  switch (value) {
    case GL_NO_ERROR:
      return false;
    case GL_INVALID_ENUM:
    case GL_INVALID_VALUE:
    case GL_INVALID_OPERATION:
    case GL_INVALID_FRAMEBUFFER_OPERATION:
    case GL_OUT_OF_MEMORY:
      return true;
  }
  return false;
}

ProcTableGLES::Resolver WrappedResolver(
    const ProcTableGLES::Resolver& resolver) {
  return [resolver](const char* function_name) -> void* {
    auto resolved = resolver(function_name);
    if (resolved) {
      return resolved;
    }
    // If there are certain known suffixes (usually for extensions), strip them
    // out and try to resolve the same proc addresses again.
    auto function = std::string{function_name};
    if (function.find("KHR", function.size() - 3) != std::string::npos) {
      auto truncated = function.substr(0u, function.size() - 3);
      return resolver(truncated.c_str());
    }
    if (function.find("EXT", function.size() - 3) != std::string::npos) {
      auto truncated = function.substr(0u, function.size() - 3);
      return resolver(truncated.c_str());
    }
    return nullptr;
  };
}

ProcTableGLES::ProcTableGLES(Resolver resolver) {
  if (!resolver) {
    return;
  }

  resolver = WrappedResolver(resolver);

  auto error_fn = reinterpret_cast<PFNGLGETERRORPROC>(resolver("glGetError"));
  if (!error_fn) {
    VALIDATION_LOG << "Could not resolve "
                   << "glGetError";
    return;
  }

#define IMPELLER_PROC(proc_ivar)                                \
  if (auto fn_ptr = resolver(proc_ivar.name)) {                 \
    proc_ivar.function =                                        \
        reinterpret_cast<decltype(proc_ivar.function)>(fn_ptr); \
    proc_ivar.error_fn = error_fn;                              \
  } else {                                                      \
    VALIDATION_LOG << "Could not resolve " << proc_ivar.name;   \
    return;                                                     \
  }

  FOR_EACH_IMPELLER_PROC(IMPELLER_PROC);

#undef IMPELLER_PROC

#define IMPELLER_PROC(proc_ivar)                                \
  if (auto fn_ptr = resolver(proc_ivar.name)) {                 \
    proc_ivar.function =                                        \
        reinterpret_cast<decltype(proc_ivar.function)>(fn_ptr); \
    proc_ivar.error_fn = error_fn;                              \
  }
  FOR_EACH_IMPELLER_GLES3_PROC(IMPELLER_PROC);
  FOR_EACH_IMPELLER_EXT_PROC(IMPELLER_PROC);

#undef IMPELLER_PROC

  description_ = std::make_unique<DescriptionGLES>(*this);

  if (!description_->IsValid()) {
    return;
  }

  if (!description_->HasDebugExtension()) {
    PushDebugGroupKHR.Reset();
    PopDebugGroupKHR.Reset();
    ObjectLabelKHR.Reset();
  } else {
    GetIntegerv(GL_MAX_LABEL_LENGTH_KHR, &debug_label_max_length_);
  }

  if (!description_->HasExtension("GL_EXT_discard_framebuffer")) {
    DiscardFramebufferEXT.Reset();
  }

  capabilities_ = std::make_shared<CapabilitiesGLES>(*this);

  is_valid_ = true;
}

ProcTableGLES::~ProcTableGLES() = default;

bool ProcTableGLES::IsValid() const {
  return is_valid_;
}

void ProcTableGLES::ShaderSourceMapping(GLuint shader,
                                        const fml::Mapping& mapping) const {
  const GLchar* sources[] = {
      reinterpret_cast<const GLchar*>(mapping.GetMapping())};
  const GLint lengths[] = {static_cast<GLint>(mapping.GetSize())};
  ShaderSource(shader, 1u, sources, lengths);
}

const DescriptionGLES* ProcTableGLES::GetDescription() const {
  return description_.get();
}

const std::shared_ptr<const CapabilitiesGLES>& ProcTableGLES::GetCapabilities()
    const {
  return capabilities_;
}

static const char* FramebufferStatusToString(GLenum status) {
  switch (status) {
    case GL_FRAMEBUFFER_COMPLETE:
      return "GL_FRAMEBUFFER_COMPLETE";
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
      return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
#if GL_ES_VERSION_2_0
    case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
      return "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
#endif
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
      return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
    case GL_FRAMEBUFFER_UNSUPPORTED:
      return "GL_FRAMEBUFFER_UNSUPPORTED";
    case GL_INVALID_ENUM:
      return "GL_INVALID_ENUM";
  }

  return "Unknown FBO Error Status";
}

static const char* AttachmentTypeString(GLint type) {
  switch (type) {
    case GL_RENDERBUFFER:
      return "GL_RENDERBUFFER";
    case GL_TEXTURE:
      return "GL_TEXTURE";
    case GL_NONE:
      return "GL_NONE";
  }

  return "Unknown Type";
}

static std::string DescribeFramebufferAttachment(const ProcTableGLES& gl,
                                                 GLenum attachment) {
  GLint param = GL_NONE;
  gl.GetFramebufferAttachmentParameteriv(
      GL_FRAMEBUFFER,                         // target
      attachment,                             // attachment
      GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,  // parameter name
      &param                                  // parameter
  );

  if (param != GL_NONE) {
    param = GL_NONE;
    gl.GetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER,                         // target
        attachment,                             // attachment
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,  // parameter name
        &param                                  // parameter
    );
    std::stringstream stream;
    stream << AttachmentTypeString(param) << "(" << param << ")";
    return stream.str();
  }

  return "No Attachment";
}

std::string ProcTableGLES::DescribeCurrentFramebuffer() const {
  GLint framebuffer = GL_NONE;
  GetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
  if (IsFramebuffer(framebuffer) == GL_FALSE) {
    return "No framebuffer or the default window framebuffer is bound.";
  }

  GLenum status = CheckFramebufferStatus(framebuffer);
  std::stringstream stream;
  stream << "FBO "
         << ((framebuffer == GL_NONE) ? "(Default)"
                                      : std::to_string(framebuffer))
         << ": " << FramebufferStatusToString(status) << std::endl;
  if (IsCurrentFramebufferComplete()) {
    stream << "Framebuffer is complete." << std::endl;
  } else {
    stream << "Framebuffer is incomplete." << std::endl;
  }
  stream << "Description: " << std::endl;
  stream << "Color Attachment: "
         << DescribeFramebufferAttachment(*this, GL_COLOR_ATTACHMENT0)
         << std::endl;
  stream << "Color Attachment: "
         << DescribeFramebufferAttachment(*this, GL_DEPTH_ATTACHMENT)
         << std::endl;
  stream << "Color Attachment: "
         << DescribeFramebufferAttachment(*this, GL_STENCIL_ATTACHMENT)
         << std::endl;
  return stream.str();
}

bool ProcTableGLES::IsCurrentFramebufferComplete() const {
  GLint framebuffer = GL_NONE;
  GetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
  if (IsFramebuffer(framebuffer) == GL_FALSE) {
    // The default framebuffer is always complete.
    return true;
  }
  GLenum status = CheckFramebufferStatus(framebuffer);
  return status == GL_FRAMEBUFFER_COMPLETE;
}

static std::optional<GLenum> ToDebugIdentifier(DebugResourceType type) {
  switch (type) {
    case DebugResourceType::kTexture:
      return GL_TEXTURE;
    case DebugResourceType::kBuffer:
      return GL_BUFFER_KHR;
    case DebugResourceType::kProgram:
      return GL_PROGRAM_KHR;
    case DebugResourceType::kShader:
      return GL_SHADER_KHR;
    case DebugResourceType::kRenderBuffer:
      return GL_RENDERBUFFER;
    case DebugResourceType::kFrameBuffer:
      return GL_FRAMEBUFFER;
  }
  FML_UNREACHABLE();
}

static bool ResourceIsLive(const ProcTableGLES& gl,
                           DebugResourceType type,
                           GLint name) {
  switch (type) {
    case DebugResourceType::kTexture:
      return gl.IsTexture(name);
    case DebugResourceType::kBuffer:
      return gl.IsBuffer(name);
    case DebugResourceType::kProgram:
      return gl.IsProgram(name);
    case DebugResourceType::kShader:
      return gl.IsShader(name);
    case DebugResourceType::kRenderBuffer:
      return gl.IsRenderbuffer(name);
    case DebugResourceType::kFrameBuffer:
      return gl.IsFramebuffer(name);
  }
  FML_UNREACHABLE();
}

bool ProcTableGLES::SetDebugLabel(DebugResourceType type,
                                  GLint name,
                                  const std::string& label) const {
  if (debug_label_max_length_ <= 0) {
    return true;
  }
  if (!ObjectLabelKHR.IsAvailable()) {
    return true;
  }
  if (!ResourceIsLive(*this, type, name)) {
    return false;
  }
  const auto identifier = ToDebugIdentifier(type);
  const auto label_length =
      std::min<GLsizei>(debug_label_max_length_ - 1, label.size());
  if (!identifier.has_value()) {
    return true;
  }
  ObjectLabelKHR(identifier.value(),  // identifier
                 name,                // name
                 label_length,        // length
                 label.data()         // label
  );
  return true;
}

void ProcTableGLES::PushDebugGroup(const std::string& label) const {
  if (debug_label_max_length_ <= 0) {
    return;
  }

  UniqueID id;
  const auto label_length =
      std::min<GLsizei>(debug_label_max_length_ - 1, label.size());
  PushDebugGroupKHR(GL_DEBUG_SOURCE_APPLICATION_KHR,  // source
                    static_cast<GLuint>(id.id),       // id
                    label_length,                     // length
                    label.data()                      // message
  );
}

void ProcTableGLES::PopDebugGroup() const {
  if (debug_label_max_length_ <= 0) {
    return;
  }

  PopDebugGroupKHR();
}

std::string ProcTableGLES::GetProgramInfoLogString(GLuint program) const {
  GLint length = 0;
  GetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
  if (length <= 0) {
    return "";
  }

  length = std::min<GLint>(length, 1024);
  Allocation allocation;
  if (!allocation.Truncate(length, false)) {
    return "";
  }
  GetProgramInfoLog(program,  // program
                    length,   // max length
                    &length,  // length written (excluding NULL terminator)
                    reinterpret_cast<GLchar*>(allocation.GetBuffer())  // buffer
  );
  if (length <= 0) {
    return "";
  }
  return std::string{reinterpret_cast<const char*>(allocation.GetBuffer()),
                     static_cast<size_t>(length)};
}

}  // namespace impeller
