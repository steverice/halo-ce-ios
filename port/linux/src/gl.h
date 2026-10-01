/*
GL.H

OpenGL entry points used by the renderer, resolved at run time through
SDL_GL_GetProcAddress once the context exists (gl_functions_load).
*/

#ifndef __HALO_LINUX_GL_H
#define __HALO_LINUX_GL_H

/* prototypes are declared only to give each pointer its exact type */
#define GL_GLEXT_PROTOTYPES 1
/* the XDK defines APIENTRY as __stdcall; OpenGL on Linux uses cdecl (on
Windows it is __stdcall too, and SDL would include windows.h without it) */
#pragma push_macro("APIENTRY")
#ifndef _WIN32
#undef APIENTRY
#endif
#ifdef HALO_ILP32
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#define GLAPIENTRY GL_APIENTRY
#else
#include <SDL3/SDL_opengl.h>
#endif
#pragma pop_macro("APIENTRY")

#ifdef HALO_ILP32
/* OpenGL ES 3.2 (port/ios/README.md); tools/guest_gl_stubs.py reads
this list to generate the guest's entry points */
/* GLES_FUNCTIONS_BEGIN */
#define GL_FUNCTIONS(X) \
	X(glGetString) \
	X(glGetIntegerv) \
	X(glCopyImageSubData) \
	X(glGenerateMipmap) \
	X(glGetError) \
	X(glEnable) \
	X(glDisable) \
	X(glViewport) \
	X(glDepthRangef) \
	X(glScissor) \
	X(glClearColor) \
	X(glClearDepthf) \
	X(glClearStencil) \
	X(glClear) \
	X(glColorMask) \
	X(glDepthMask) \
	X(glDepthFunc) \
	X(glStencilFunc) \
	X(glStencilOp) \
	X(glStencilMask) \
	X(glBlendFunc) \
	X(glBlendEquation) \
	X(glBlendColor) \
	X(glCullFace) \
	X(glFrontFace) \
	X(glPolygonOffset) \
	X(glLineWidth) \
	X(glPixelStorei) \
	X(glReadPixels) \
	X(glFinish) \
	X(glFlush) \
	X(glGenTextures) \
	X(glDeleteTextures) \
	X(glBindTexture) \
	X(glActiveTexture) \
	X(glTexImage2D) \
	X(glTexImage3D) \
	X(glTexSubImage2D) \
	X(glCompressedTexImage2D) \
	X(glCompressedTexImage3D) \
	X(glTexParameteri) \
	X(glTexParameteriv) \
	X(glTexParameterf) \
	X(glTexParameterfv) \
	X(glGenSamplers) \
	X(glBindSampler) \
	X(glSamplerParameteri) \
	X(glSamplerParameterf) \
	X(glSamplerParameterfv) \
	X(glGenFramebuffers) \
	X(glDeleteFramebuffers) \
	X(glBindFramebuffer) \
	X(glFramebufferTexture2D) \
	X(glCheckFramebufferStatus) \
	X(glBlitFramebuffer) \
	X(glDrawBuffers) \
	X(glReadBuffer) \
	X(glInvalidateFramebuffer) \
	X(glGenBuffers) \
	X(glDeleteBuffers) \
	X(glBindBuffer) \
	X(glBufferData) \
	X(glBufferSubData) \
	X(glBindBufferBase) \
	X(glBindBufferRange) \
	X(glGenVertexArrays) \
	X(glBindVertexArray) \
	X(glEnableVertexAttribArray) \
	X(glDisableVertexAttribArray) \
	X(glVertexAttribPointer) \
	X(glVertexAttribIPointer) \
	X(glVertexAttrib4fv) \
	X(glVertexAttribI4ui) \
	X(glDrawArrays) \
	X(glDrawElements) \
	X(glDrawElementsBaseVertex) \
	X(glCreateShader) \
	X(glShaderSource) \
	X(glCompileShader) \
	X(glGetShaderiv) \
	X(glGetShaderInfoLog) \
	X(glDeleteShader) \
	X(glCreateProgram) \
	X(glAttachShader) \
	X(glBindAttribLocation) \
	X(glLinkProgram) \
	X(glGetProgramiv) \
	X(glGetProgramInfoLog) \
	X(glUseProgram) \
	X(glGetUniformLocation) \
	X(glUniform1i) \
	X(glUniform1iv) \
	X(glUniform1f) \
	X(glUniform4fv) \
	X(glUniform2f) \
	X(glGenQueries) \
	X(glBeginQuery) \
	X(glEndQuery) \
	X(glGetQueryObjectuiv)
/* GLES_FUNCTIONS_END */
#else
#define GL_FUNCTIONS(X) \
	X(glGetString) \
	X(glGetIntegerv) \
	X(glGetTexImage) \
	X(glCopyImageSubData) \
	X(glGenerateMipmap) \
	X(glGetError) \
	X(glEnable) \
	X(glDisable) \
	X(glViewport) \
	X(glDepthRange) \
	X(glScissor) \
	X(glClearColor) \
	X(glClearDepth) \
	X(glClearStencil) \
	X(glClear) \
	X(glColorMask) \
	X(glDepthMask) \
	X(glDepthFunc) \
	X(glStencilFunc) \
	X(glStencilOp) \
	X(glStencilMask) \
	X(glBlendFunc) \
	X(glBlendEquation) \
	X(glBlendColor) \
	X(glCullFace) \
	X(glFrontFace) \
	X(glPolygonMode) \
	X(glPolygonOffset) \
	X(glLineWidth) \
	X(glPixelStorei) \
	X(glReadPixels) \
	X(glFinish) \
	X(glFlush) \
	X(glClipControl) \
	X(glGenTextures) \
	X(glDeleteTextures) \
	X(glBindTexture) \
	X(glActiveTexture) \
	X(glTexImage2D) \
	X(glTexImage3D) \
	X(glTexSubImage2D) \
	X(glCompressedTexImage2D) \
	X(glCompressedTexImage3D) \
	X(glTexParameteri) \
	X(glTexParameteriv) \
	X(glTexParameterf) \
	X(glTexParameterfv) \
	X(glGenSamplers) \
	X(glBindSampler) \
	X(glSamplerParameteri) \
	X(glSamplerParameterf) \
	X(glSamplerParameterfv) \
	X(glGenFramebuffers) \
	X(glDeleteFramebuffers) \
	X(glBindFramebuffer) \
	X(glFramebufferTexture2D) \
	X(glCheckFramebufferStatus) \
	X(glBlitFramebuffer) \
	X(glDrawBuffers) \
	X(glGenBuffers) \
	X(glDeleteBuffers) \
	X(glBindBuffer) \
	X(glBufferData) \
	X(glBufferSubData) \
	X(glBufferStorage) \
	X(glMapBufferRange) \
	X(glBindBufferBase) \
	X(glGenVertexArrays) \
	X(glBindVertexArray) \
	X(glEnableVertexAttribArray) \
	X(glDisableVertexAttribArray) \
	X(glVertexAttribPointer) \
	X(glVertexAttribIPointer) \
	X(glVertexAttrib4fv) \
	X(glVertexAttribI4ui) \
	X(glDrawArrays) \
	X(glDrawElements) \
	X(glDrawElementsBaseVertex) \
	X(glCreateShader) \
	X(glShaderSource) \
	X(glCompileShader) \
	X(glGetShaderiv) \
	X(glGetShaderInfoLog) \
	X(glDeleteShader) \
	X(glCreateProgram) \
	X(glAttachShader) \
	X(glBindAttribLocation) \
	X(glBindFragDataLocation) \
	X(glLinkProgram) \
	X(glGetProgramiv) \
	X(glGetProgramInfoLog) \
	X(glUseProgram) \
	X(glGetUniformLocation) \
	X(glUniform1i) \
	X(glUniform1iv) \
	X(glUniform1f) \
	X(glUniform4fv) \
	X(glUniform2f) \
	X(glGenQueries) \
	X(glBeginQuery) \
	X(glEndQuery) \
	X(glGetQueryObjectuiv) \
	X(glDebugMessageCallback)
#endif

#define GL_DECLARE_FUNCTION(name) extern __typeof__(&name) halo_##name;
GL_FUNCTIONS(GL_DECLARE_FUNCTION)
#undef GL_DECLARE_FUNCTION

/* every GL call made through the aliases below is counted, for
debug.gpu_stats (d3d8_gl.c): the renderer's refactors must not add calls */
extern unsigned long halo_gl_call_count;
#define HALO_GL_COUNTED(function) (halo_gl_call_count++, function)

/* call sites use the ordinary names; gl_functions.c, which defines the
pointers, sees the declarations without these aliases */
#ifndef GL_FUNCTIONS_DEFINE
#ifdef HALO_ILP32
#define glGetString HALO_GL_COUNTED(halo_glGetString)
#define glGetIntegerv HALO_GL_COUNTED(halo_glGetIntegerv)
#define glCopyImageSubData HALO_GL_COUNTED(halo_glCopyImageSubData)
#define glGenerateMipmap HALO_GL_COUNTED(halo_glGenerateMipmap)
#define glGetError HALO_GL_COUNTED(halo_glGetError)
#define glEnable HALO_GL_COUNTED(halo_glEnable)
#define glDisable HALO_GL_COUNTED(halo_glDisable)
#define glViewport HALO_GL_COUNTED(halo_glViewport)
#define glDepthRangef HALO_GL_COUNTED(halo_glDepthRangef)
#define glScissor HALO_GL_COUNTED(halo_glScissor)
#define glClearColor HALO_GL_COUNTED(halo_glClearColor)
#define glClearDepthf HALO_GL_COUNTED(halo_glClearDepthf)
#define glClearStencil HALO_GL_COUNTED(halo_glClearStencil)
#define glClear HALO_GL_COUNTED(halo_glClear)
#define glColorMask HALO_GL_COUNTED(halo_glColorMask)
#define glDepthMask HALO_GL_COUNTED(halo_glDepthMask)
#define glDepthFunc HALO_GL_COUNTED(halo_glDepthFunc)
#define glStencilFunc HALO_GL_COUNTED(halo_glStencilFunc)
#define glStencilOp HALO_GL_COUNTED(halo_glStencilOp)
#define glStencilMask HALO_GL_COUNTED(halo_glStencilMask)
#define glBlendFunc HALO_GL_COUNTED(halo_glBlendFunc)
#define glBlendEquation HALO_GL_COUNTED(halo_glBlendEquation)
#define glBlendColor HALO_GL_COUNTED(halo_glBlendColor)
#define glCullFace HALO_GL_COUNTED(halo_glCullFace)
#define glFrontFace HALO_GL_COUNTED(halo_glFrontFace)
#define glPolygonOffset HALO_GL_COUNTED(halo_glPolygonOffset)
#define glLineWidth HALO_GL_COUNTED(halo_glLineWidth)
#define glPixelStorei HALO_GL_COUNTED(halo_glPixelStorei)
#define glReadPixels HALO_GL_COUNTED(halo_glReadPixels)
#define glFinish HALO_GL_COUNTED(halo_glFinish)
#define glFlush HALO_GL_COUNTED(halo_glFlush)
#define glGenTextures HALO_GL_COUNTED(halo_glGenTextures)
#define glDeleteTextures HALO_GL_COUNTED(halo_glDeleteTextures)
#define glBindTexture HALO_GL_COUNTED(halo_glBindTexture)
#define glActiveTexture HALO_GL_COUNTED(halo_glActiveTexture)
#define glTexImage2D HALO_GL_COUNTED(halo_glTexImage2D)
#define glTexImage3D HALO_GL_COUNTED(halo_glTexImage3D)
#define glTexSubImage2D HALO_GL_COUNTED(halo_glTexSubImage2D)
#define glCompressedTexImage2D HALO_GL_COUNTED(halo_glCompressedTexImage2D)
#define glCompressedTexImage3D HALO_GL_COUNTED(halo_glCompressedTexImage3D)
#define glTexParameteri HALO_GL_COUNTED(halo_glTexParameteri)
#define glTexParameteriv HALO_GL_COUNTED(halo_glTexParameteriv)
#define glTexParameterf HALO_GL_COUNTED(halo_glTexParameterf)
#define glTexParameterfv HALO_GL_COUNTED(halo_glTexParameterfv)
#define glGenSamplers HALO_GL_COUNTED(halo_glGenSamplers)
#define glBindSampler HALO_GL_COUNTED(halo_glBindSampler)
#define glSamplerParameteri HALO_GL_COUNTED(halo_glSamplerParameteri)
#define glSamplerParameterf HALO_GL_COUNTED(halo_glSamplerParameterf)
#define glSamplerParameterfv HALO_GL_COUNTED(halo_glSamplerParameterfv)
#define glGenFramebuffers HALO_GL_COUNTED(halo_glGenFramebuffers)
#define glDeleteFramebuffers HALO_GL_COUNTED(halo_glDeleteFramebuffers)
#define glBindFramebuffer HALO_GL_COUNTED(halo_glBindFramebuffer)
#define glFramebufferTexture2D HALO_GL_COUNTED(halo_glFramebufferTexture2D)
#define glCheckFramebufferStatus HALO_GL_COUNTED(halo_glCheckFramebufferStatus)
#define glBlitFramebuffer HALO_GL_COUNTED(halo_glBlitFramebuffer)
#define glDrawBuffers HALO_GL_COUNTED(halo_glDrawBuffers)
#define glReadBuffer HALO_GL_COUNTED(halo_glReadBuffer)
#define glInvalidateFramebuffer HALO_GL_COUNTED(halo_glInvalidateFramebuffer)
#define glGenBuffers HALO_GL_COUNTED(halo_glGenBuffers)
#define glDeleteBuffers HALO_GL_COUNTED(halo_glDeleteBuffers)
#define glBindBuffer HALO_GL_COUNTED(halo_glBindBuffer)
#define glBufferData HALO_GL_COUNTED(halo_glBufferData)
#define glBufferSubData HALO_GL_COUNTED(halo_glBufferSubData)
#define glBindBufferBase HALO_GL_COUNTED(halo_glBindBufferBase)
#define glBindBufferRange HALO_GL_COUNTED(halo_glBindBufferRange)
#define glGenVertexArrays HALO_GL_COUNTED(halo_glGenVertexArrays)
#define glBindVertexArray HALO_GL_COUNTED(halo_glBindVertexArray)
#define glEnableVertexAttribArray HALO_GL_COUNTED(halo_glEnableVertexAttribArray)
#define glDisableVertexAttribArray HALO_GL_COUNTED(halo_glDisableVertexAttribArray)
#define glVertexAttribPointer HALO_GL_COUNTED(halo_glVertexAttribPointer)
#define glVertexAttribIPointer HALO_GL_COUNTED(halo_glVertexAttribIPointer)
#define glVertexAttrib4fv HALO_GL_COUNTED(halo_glVertexAttrib4fv)
#define glVertexAttribI4ui HALO_GL_COUNTED(halo_glVertexAttribI4ui)
#define glDrawArrays HALO_GL_COUNTED(halo_glDrawArrays)
#define glDrawElements HALO_GL_COUNTED(halo_glDrawElements)
#define glDrawElementsBaseVertex HALO_GL_COUNTED(halo_glDrawElementsBaseVertex)
#define glCreateShader HALO_GL_COUNTED(halo_glCreateShader)
#define glShaderSource HALO_GL_COUNTED(halo_glShaderSource)
#define glCompileShader HALO_GL_COUNTED(halo_glCompileShader)
#define glGetShaderiv HALO_GL_COUNTED(halo_glGetShaderiv)
#define glGetShaderInfoLog HALO_GL_COUNTED(halo_glGetShaderInfoLog)
#define glDeleteShader HALO_GL_COUNTED(halo_glDeleteShader)
#define glCreateProgram HALO_GL_COUNTED(halo_glCreateProgram)
#define glAttachShader HALO_GL_COUNTED(halo_glAttachShader)
#define glBindAttribLocation HALO_GL_COUNTED(halo_glBindAttribLocation)
#define glLinkProgram HALO_GL_COUNTED(halo_glLinkProgram)
#define glGetProgramiv HALO_GL_COUNTED(halo_glGetProgramiv)
#define glGetProgramInfoLog HALO_GL_COUNTED(halo_glGetProgramInfoLog)
#define glUseProgram HALO_GL_COUNTED(halo_glUseProgram)
#define glGetUniformLocation HALO_GL_COUNTED(halo_glGetUniformLocation)
#define glUniform1i HALO_GL_COUNTED(halo_glUniform1i)
#define glUniform1iv HALO_GL_COUNTED(halo_glUniform1iv)
#define glUniform1f HALO_GL_COUNTED(halo_glUniform1f)
#define glUniform4fv HALO_GL_COUNTED(halo_glUniform4fv)
#define glUniform2f HALO_GL_COUNTED(halo_glUniform2f)
#define glGenQueries HALO_GL_COUNTED(halo_glGenQueries)
#define glBeginQuery HALO_GL_COUNTED(halo_glBeginQuery)
#define glEndQuery HALO_GL_COUNTED(halo_glEndQuery)
#define glGetQueryObjectuiv HALO_GL_COUNTED(halo_glGetQueryObjectuiv)
#else
#define glGetString HALO_GL_COUNTED(halo_glGetString)
#define glGetIntegerv HALO_GL_COUNTED(halo_glGetIntegerv)
#define glGetTexImage HALO_GL_COUNTED(halo_glGetTexImage)
#define glCopyImageSubData HALO_GL_COUNTED(halo_glCopyImageSubData)
#define glGenerateMipmap HALO_GL_COUNTED(halo_glGenerateMipmap)
#define glGetError HALO_GL_COUNTED(halo_glGetError)
#define glEnable HALO_GL_COUNTED(halo_glEnable)
#define glDisable HALO_GL_COUNTED(halo_glDisable)
#define glViewport HALO_GL_COUNTED(halo_glViewport)
#define glDepthRange HALO_GL_COUNTED(halo_glDepthRange)
#define glScissor HALO_GL_COUNTED(halo_glScissor)
#define glClearColor HALO_GL_COUNTED(halo_glClearColor)
#define glClearDepth HALO_GL_COUNTED(halo_glClearDepth)
#define glClearStencil HALO_GL_COUNTED(halo_glClearStencil)
#define glClear HALO_GL_COUNTED(halo_glClear)
#define glColorMask HALO_GL_COUNTED(halo_glColorMask)
#define glDepthMask HALO_GL_COUNTED(halo_glDepthMask)
#define glDepthFunc HALO_GL_COUNTED(halo_glDepthFunc)
#define glStencilFunc HALO_GL_COUNTED(halo_glStencilFunc)
#define glStencilOp HALO_GL_COUNTED(halo_glStencilOp)
#define glStencilMask HALO_GL_COUNTED(halo_glStencilMask)
#define glBlendFunc HALO_GL_COUNTED(halo_glBlendFunc)
#define glBlendEquation HALO_GL_COUNTED(halo_glBlendEquation)
#define glBlendColor HALO_GL_COUNTED(halo_glBlendColor)
#define glCullFace HALO_GL_COUNTED(halo_glCullFace)
#define glFrontFace HALO_GL_COUNTED(halo_glFrontFace)
#define glPolygonMode HALO_GL_COUNTED(halo_glPolygonMode)
#define glPolygonOffset HALO_GL_COUNTED(halo_glPolygonOffset)
#define glLineWidth HALO_GL_COUNTED(halo_glLineWidth)
#define glPixelStorei HALO_GL_COUNTED(halo_glPixelStorei)
#define glReadPixels HALO_GL_COUNTED(halo_glReadPixels)
#define glFinish HALO_GL_COUNTED(halo_glFinish)
#define glFlush HALO_GL_COUNTED(halo_glFlush)
#define glClipControl HALO_GL_COUNTED(halo_glClipControl)
#define glGenTextures HALO_GL_COUNTED(halo_glGenTextures)
#define glDeleteTextures HALO_GL_COUNTED(halo_glDeleteTextures)
#define glBindTexture HALO_GL_COUNTED(halo_glBindTexture)
#define glActiveTexture HALO_GL_COUNTED(halo_glActiveTexture)
#define glTexImage2D HALO_GL_COUNTED(halo_glTexImage2D)
#define glTexImage3D HALO_GL_COUNTED(halo_glTexImage3D)
#define glTexSubImage2D HALO_GL_COUNTED(halo_glTexSubImage2D)
#define glCompressedTexImage2D HALO_GL_COUNTED(halo_glCompressedTexImage2D)
#define glCompressedTexImage3D HALO_GL_COUNTED(halo_glCompressedTexImage3D)
#define glTexParameteri HALO_GL_COUNTED(halo_glTexParameteri)
#define glTexParameteriv HALO_GL_COUNTED(halo_glTexParameteriv)
#define glTexParameterf HALO_GL_COUNTED(halo_glTexParameterf)
#define glTexParameterfv HALO_GL_COUNTED(halo_glTexParameterfv)
#define glGenSamplers HALO_GL_COUNTED(halo_glGenSamplers)
#define glBindSampler HALO_GL_COUNTED(halo_glBindSampler)
#define glSamplerParameteri HALO_GL_COUNTED(halo_glSamplerParameteri)
#define glSamplerParameterf HALO_GL_COUNTED(halo_glSamplerParameterf)
#define glSamplerParameterfv HALO_GL_COUNTED(halo_glSamplerParameterfv)
#define glGenFramebuffers HALO_GL_COUNTED(halo_glGenFramebuffers)
#define glDeleteFramebuffers HALO_GL_COUNTED(halo_glDeleteFramebuffers)
#define glBindFramebuffer HALO_GL_COUNTED(halo_glBindFramebuffer)
#define glFramebufferTexture2D HALO_GL_COUNTED(halo_glFramebufferTexture2D)
#define glCheckFramebufferStatus HALO_GL_COUNTED(halo_glCheckFramebufferStatus)
#define glBlitFramebuffer HALO_GL_COUNTED(halo_glBlitFramebuffer)
#define glDrawBuffers HALO_GL_COUNTED(halo_glDrawBuffers)
#define glGenBuffers HALO_GL_COUNTED(halo_glGenBuffers)
#define glDeleteBuffers HALO_GL_COUNTED(halo_glDeleteBuffers)
#define glBindBuffer HALO_GL_COUNTED(halo_glBindBuffer)
#define glBufferData HALO_GL_COUNTED(halo_glBufferData)
#define glBufferSubData HALO_GL_COUNTED(halo_glBufferSubData)
#define glBufferStorage HALO_GL_COUNTED(halo_glBufferStorage)
#define glMapBufferRange HALO_GL_COUNTED(halo_glMapBufferRange)
#define glBindBufferBase HALO_GL_COUNTED(halo_glBindBufferBase)
#define glGenVertexArrays HALO_GL_COUNTED(halo_glGenVertexArrays)
#define glBindVertexArray HALO_GL_COUNTED(halo_glBindVertexArray)
#define glEnableVertexAttribArray HALO_GL_COUNTED(halo_glEnableVertexAttribArray)
#define glDisableVertexAttribArray HALO_GL_COUNTED(halo_glDisableVertexAttribArray)
#define glVertexAttribPointer HALO_GL_COUNTED(halo_glVertexAttribPointer)
#define glVertexAttribIPointer HALO_GL_COUNTED(halo_glVertexAttribIPointer)
#define glVertexAttrib4fv HALO_GL_COUNTED(halo_glVertexAttrib4fv)
#define glVertexAttribI4ui HALO_GL_COUNTED(halo_glVertexAttribI4ui)
#define glDrawArrays HALO_GL_COUNTED(halo_glDrawArrays)
#define glDrawElements HALO_GL_COUNTED(halo_glDrawElements)
#define glDrawElementsBaseVertex HALO_GL_COUNTED(halo_glDrawElementsBaseVertex)
#define glCreateShader HALO_GL_COUNTED(halo_glCreateShader)
#define glShaderSource HALO_GL_COUNTED(halo_glShaderSource)
#define glCompileShader HALO_GL_COUNTED(halo_glCompileShader)
#define glGetShaderiv HALO_GL_COUNTED(halo_glGetShaderiv)
#define glGetShaderInfoLog HALO_GL_COUNTED(halo_glGetShaderInfoLog)
#define glDeleteShader HALO_GL_COUNTED(halo_glDeleteShader)
#define glCreateProgram HALO_GL_COUNTED(halo_glCreateProgram)
#define glAttachShader HALO_GL_COUNTED(halo_glAttachShader)
#define glBindAttribLocation HALO_GL_COUNTED(halo_glBindAttribLocation)
#define glBindFragDataLocation HALO_GL_COUNTED(halo_glBindFragDataLocation)
#define glLinkProgram HALO_GL_COUNTED(halo_glLinkProgram)
#define glGetProgramiv HALO_GL_COUNTED(halo_glGetProgramiv)
#define glGetProgramInfoLog HALO_GL_COUNTED(halo_glGetProgramInfoLog)
#define glUseProgram HALO_GL_COUNTED(halo_glUseProgram)
#define glGetUniformLocation HALO_GL_COUNTED(halo_glGetUniformLocation)
#define glUniform1i HALO_GL_COUNTED(halo_glUniform1i)
#define glUniform1iv HALO_GL_COUNTED(halo_glUniform1iv)
#define glUniform1f HALO_GL_COUNTED(halo_glUniform1f)
#define glUniform4fv HALO_GL_COUNTED(halo_glUniform4fv)
#define glUniform2f HALO_GL_COUNTED(halo_glUniform2f)
#define glGenQueries HALO_GL_COUNTED(halo_glGenQueries)
#define glBeginQuery HALO_GL_COUNTED(halo_glBeginQuery)
#define glEndQuery HALO_GL_COUNTED(halo_glEndQuery)
#define glGetQueryObjectuiv HALO_GL_COUNTED(halo_glGetQueryObjectuiv)
#define glDebugMessageCallback HALO_GL_COUNTED(halo_glDebugMessageCallback)

#endif
#endif

/* returns FALSE (and logs) if a required function is missing */
int gl_functions_load(void);

#endif
