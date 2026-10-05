/* gl_hooks.h: the engine's GL calls, noted.
 *
 * Included after wasmcart.h by the files that issue GL (render2d_gl.c,
 * render3d_gl.c, runtime.c). Each state-changing GL function becomes a macro
 * that first runs wcl__gl_pre(): it carries out a frame clear that is still
 * pending (the frame-start clear is deferred to the first GL call, see
 * render2d_gl.c) and records that the engine touched GL. An outside renderer
 * sharing the context (three.c, three_c.c) reads that flag to know whether
 * its own cached GL state is still true, instead of re-issuing all of it.
 *
 * glClear, glClearColor and the queries are left alone: the deferred clear
 * issues the first two itself, and the rest change no state.
 *
 * Generated from wasmcart.h's _GL_IMPORT list; regenerate if it grows. A
 * function-like macro does not expand inside its own replacement, so each
 * one calls the real import. */
#ifndef WCL_GL_HOOKS_H
#define WCL_GL_HOOKS_H

extern int wcl_gl_touched;
extern int wcl_clear_pending;
void wcl__run_pending_clear(void);
static inline void wcl__gl_pre(void) {
    if (wcl_clear_pending) wcl__run_pending_clear();
    wcl_gl_touched = 1;
}

#define glActiveTexture(...) (wcl__gl_pre(), glActiveTexture(__VA_ARGS__))
#define glAttachShader(...) (wcl__gl_pre(), glAttachShader(__VA_ARGS__))
#define glBindAttribLocation(...) (wcl__gl_pre(), glBindAttribLocation(__VA_ARGS__))
#define glBindBuffer(...) (wcl__gl_pre(), glBindBuffer(__VA_ARGS__))
#define glBindBufferBase(...) (wcl__gl_pre(), glBindBufferBase(__VA_ARGS__))
#define glBindBufferRange(...) (wcl__gl_pre(), glBindBufferRange(__VA_ARGS__))
#define glBindFramebuffer(...) (wcl__gl_pre(), glBindFramebuffer(__VA_ARGS__))
#define glBindRenderbuffer(...) (wcl__gl_pre(), glBindRenderbuffer(__VA_ARGS__))
#define glBindTexture(...) (wcl__gl_pre(), glBindTexture(__VA_ARGS__))
#define glBindVertexArray(...) (wcl__gl_pre(), glBindVertexArray(__VA_ARGS__))
#define glBlendColor(...) (wcl__gl_pre(), glBlendColor(__VA_ARGS__))
#define glBlendEquation(...) (wcl__gl_pre(), glBlendEquation(__VA_ARGS__))
#define glBlendEquationSeparate(...) (wcl__gl_pre(), glBlendEquationSeparate(__VA_ARGS__))
#define glBlendFunc(...) (wcl__gl_pre(), glBlendFunc(__VA_ARGS__))
#define glBlendFuncSeparate(...) (wcl__gl_pre(), glBlendFuncSeparate(__VA_ARGS__))
#define glBlitFramebuffer(...) (wcl__gl_pre(), glBlitFramebuffer(__VA_ARGS__))
#define glBufferData(...) (wcl__gl_pre(), glBufferData(__VA_ARGS__))
#define glBufferSubData(...) (wcl__gl_pre(), glBufferSubData(__VA_ARGS__))
#define glClearDepthf(...) (wcl__gl_pre(), glClearDepthf(__VA_ARGS__))
#define glClearStencil(...) (wcl__gl_pre(), glClearStencil(__VA_ARGS__))
#define glColorMask(...) (wcl__gl_pre(), glColorMask(__VA_ARGS__))
#define glCompileShader(...) (wcl__gl_pre(), glCompileShader(__VA_ARGS__))
#define glCompressedTexImage2D(...) (wcl__gl_pre(), glCompressedTexImage2D(__VA_ARGS__))
#define glCreateProgram(...) (wcl__gl_pre(), glCreateProgram(__VA_ARGS__))
#define glCreateShader(...) (wcl__gl_pre(), glCreateShader(__VA_ARGS__))
#define glCullFace(...) (wcl__gl_pre(), glCullFace(__VA_ARGS__))
#define glDeleteBuffers(...) (wcl__gl_pre(), glDeleteBuffers(__VA_ARGS__))
#define glDeleteFramebuffers(...) (wcl__gl_pre(), glDeleteFramebuffers(__VA_ARGS__))
#define glDeleteProgram(...) (wcl__gl_pre(), glDeleteProgram(__VA_ARGS__))
#define glDeleteRenderbuffers(...) (wcl__gl_pre(), glDeleteRenderbuffers(__VA_ARGS__))
#define glDeleteShader(...) (wcl__gl_pre(), glDeleteShader(__VA_ARGS__))
#define glDeleteTextures(...) (wcl__gl_pre(), glDeleteTextures(__VA_ARGS__))
#define glDeleteVertexArrays(...) (wcl__gl_pre(), glDeleteVertexArrays(__VA_ARGS__))
#define glDepthFunc(...) (wcl__gl_pre(), glDepthFunc(__VA_ARGS__))
#define glDepthMask(...) (wcl__gl_pre(), glDepthMask(__VA_ARGS__))
#define glDepthRangef(...) (wcl__gl_pre(), glDepthRangef(__VA_ARGS__))
#define glDetachShader(...) (wcl__gl_pre(), glDetachShader(__VA_ARGS__))
#define glDisable(...) (wcl__gl_pre(), glDisable(__VA_ARGS__))
#define glDisableVertexAttribArray(...) (wcl__gl_pre(), glDisableVertexAttribArray(__VA_ARGS__))
#define glDrawArrays(...) (wcl__gl_pre(), glDrawArrays(__VA_ARGS__))
#define glDrawArraysInstanced(...) (wcl__gl_pre(), glDrawArraysInstanced(__VA_ARGS__))
#define glDrawBuffers(...) (wcl__gl_pre(), glDrawBuffers(__VA_ARGS__))
#define glDrawElements(...) (wcl__gl_pre(), glDrawElements(__VA_ARGS__))
#define glDrawElementsInstanced(...) (wcl__gl_pre(), glDrawElementsInstanced(__VA_ARGS__))
#define glEnable(...) (wcl__gl_pre(), glEnable(__VA_ARGS__))
#define glEnableVertexAttribArray(...) (wcl__gl_pre(), glEnableVertexAttribArray(__VA_ARGS__))
#define glFramebufferRenderbuffer(...) (wcl__gl_pre(), glFramebufferRenderbuffer(__VA_ARGS__))
#define glFramebufferTexture2D(...) (wcl__gl_pre(), glFramebufferTexture2D(__VA_ARGS__))
#define glFramebufferTextureLayer(...) (wcl__gl_pre(), glFramebufferTextureLayer(__VA_ARGS__))
#define glFrontFace(...) (wcl__gl_pre(), glFrontFace(__VA_ARGS__))
#define glGenBuffers(...) (wcl__gl_pre(), glGenBuffers(__VA_ARGS__))
#define glGenerateMipmap(...) (wcl__gl_pre(), glGenerateMipmap(__VA_ARGS__))
#define glGenFramebuffers(...) (wcl__gl_pre(), glGenFramebuffers(__VA_ARGS__))
#define glGenRenderbuffers(...) (wcl__gl_pre(), glGenRenderbuffers(__VA_ARGS__))
#define glGenTextures(...) (wcl__gl_pre(), glGenTextures(__VA_ARGS__))
#define glGenVertexArrays(...) (wcl__gl_pre(), glGenVertexArrays(__VA_ARGS__))
#define glHint(...) (wcl__gl_pre(), glHint(__VA_ARGS__))
#define glLineWidth(...) (wcl__gl_pre(), glLineWidth(__VA_ARGS__))
#define glLinkProgram(...) (wcl__gl_pre(), glLinkProgram(__VA_ARGS__))
#define glPixelStorei(...) (wcl__gl_pre(), glPixelStorei(__VA_ARGS__))
#define glPolygonOffset(...) (wcl__gl_pre(), glPolygonOffset(__VA_ARGS__))
#define glRenderbufferStorage(...) (wcl__gl_pre(), glRenderbufferStorage(__VA_ARGS__))
#define glScissor(...) (wcl__gl_pre(), glScissor(__VA_ARGS__))
#define glShaderSource(...) (wcl__gl_pre(), glShaderSource(__VA_ARGS__))
#define glStencilFunc(...) (wcl__gl_pre(), glStencilFunc(__VA_ARGS__))
#define glStencilFuncSeparate(...) (wcl__gl_pre(), glStencilFuncSeparate(__VA_ARGS__))
#define glStencilMask(...) (wcl__gl_pre(), glStencilMask(__VA_ARGS__))
#define glStencilMaskSeparate(...) (wcl__gl_pre(), glStencilMaskSeparate(__VA_ARGS__))
#define glStencilOp(...) (wcl__gl_pre(), glStencilOp(__VA_ARGS__))
#define glStencilOpSeparate(...) (wcl__gl_pre(), glStencilOpSeparate(__VA_ARGS__))
#define glTexImage2D(...) (wcl__gl_pre(), glTexImage2D(__VA_ARGS__))
#define glTexImage3D(...) (wcl__gl_pre(), glTexImage3D(__VA_ARGS__))
#define glTexParameterf(...) (wcl__gl_pre(), glTexParameterf(__VA_ARGS__))
#define glTexParameteri(...) (wcl__gl_pre(), glTexParameteri(__VA_ARGS__))
#define glTexStorage2D(...) (wcl__gl_pre(), glTexStorage2D(__VA_ARGS__))
#define glTexStorage3D(...) (wcl__gl_pre(), glTexStorage3D(__VA_ARGS__))
#define glTexSubImage2D(...) (wcl__gl_pre(), glTexSubImage2D(__VA_ARGS__))
#define glTexSubImage3D(...) (wcl__gl_pre(), glTexSubImage3D(__VA_ARGS__))
#define glUniform1f(...) (wcl__gl_pre(), glUniform1f(__VA_ARGS__))
#define glUniform1fv(...) (wcl__gl_pre(), glUniform1fv(__VA_ARGS__))
#define glUniform1i(...) (wcl__gl_pre(), glUniform1i(__VA_ARGS__))
#define glUniform1iv(...) (wcl__gl_pre(), glUniform1iv(__VA_ARGS__))
#define glUniform2f(...) (wcl__gl_pre(), glUniform2f(__VA_ARGS__))
#define glUniform2fv(...) (wcl__gl_pre(), glUniform2fv(__VA_ARGS__))
#define glUniform2i(...) (wcl__gl_pre(), glUniform2i(__VA_ARGS__))
#define glUniform2iv(...) (wcl__gl_pre(), glUniform2iv(__VA_ARGS__))
#define glUniform3f(...) (wcl__gl_pre(), glUniform3f(__VA_ARGS__))
#define glUniform3fv(...) (wcl__gl_pre(), glUniform3fv(__VA_ARGS__))
#define glUniform3i(...) (wcl__gl_pre(), glUniform3i(__VA_ARGS__))
#define glUniform3iv(...) (wcl__gl_pre(), glUniform3iv(__VA_ARGS__))
#define glUniform4f(...) (wcl__gl_pre(), glUniform4f(__VA_ARGS__))
#define glUniform4fv(...) (wcl__gl_pre(), glUniform4fv(__VA_ARGS__))
#define glUniform4i(...) (wcl__gl_pre(), glUniform4i(__VA_ARGS__))
#define glUniform4iv(...) (wcl__gl_pre(), glUniform4iv(__VA_ARGS__))
#define glUniformBlockBinding(...) (wcl__gl_pre(), glUniformBlockBinding(__VA_ARGS__))
#define glUniformMatrix2fv(...) (wcl__gl_pre(), glUniformMatrix2fv(__VA_ARGS__))
#define glUniformMatrix3fv(...) (wcl__gl_pre(), glUniformMatrix3fv(__VA_ARGS__))
#define glUniformMatrix4fv(...) (wcl__gl_pre(), glUniformMatrix4fv(__VA_ARGS__))
#define glUseProgram(...) (wcl__gl_pre(), glUseProgram(__VA_ARGS__))
#define glValidateProgram(...) (wcl__gl_pre(), glValidateProgram(__VA_ARGS__))
#define glVertexAttribDivisor(...) (wcl__gl_pre(), glVertexAttribDivisor(__VA_ARGS__))
#define glVertexAttribPointer(...) (wcl__gl_pre(), glVertexAttribPointer(__VA_ARGS__))
#define glViewport(...) (wcl__gl_pre(), glViewport(__VA_ARGS__))

#endif
