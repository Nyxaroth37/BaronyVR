#pragma once

#include <algorithm>
#include <cmath>

// GL declarations and SDL_Rect are supplied by the caller. Keeping this
// renderer independent of OpenXR allows it to be exercised in a real GL test.
namespace VrNativeHud
{
struct Surface
{
    GLuint texture = 0, framebuffer = 0;
    int width = 0, height = 0;

    // Call only while the owning GL context is current.
    void release()
    {
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (texture) glDeleteTextures(1, &texture);
        framebuffer = texture = 0;
        width = height = 0;
    }
};

// Widget drawing uses 2D shaders, meshes and blend state. Restore the eye
// render state even on incomplete FBOs or failed allocations.
struct GlState
{
    GLint readFbo, drawFbo, readBuffer, drawBuffer, activeTexture, texture0;
    GLint viewport[4], scissor[4], program, vao, arrayBuffer;
    GLint blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha;
    GLint blendEquationRgb, blendEquationAlpha, depthFunc;
    GLboolean blend, depth, cull, scissorEnabled, depthMask, colorMask[4];
    GLfloat clearColor[4];
    GLdouble depthRange[2];

    GlState()
    {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        glGetIntegerv(GL_DRAW_BUFFER, &drawBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        glGetDoublev(GL_DEPTH_RANGE, depthRange);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    }

    ~GlState()
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        glReadBuffer(readBuffer);
        glDrawBuffer(drawBuffer);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glDepthMask(depthMask);
        glDepthFunc(depthFunc);
        glDepthRange(depthRange[0], depthRange[1]);
        glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
        glBlendEquationSeparate(blendEquationRgb, blendEquationAlpha);
        setEnabled(GL_BLEND, blend);
        setEnabled(GL_DEPTH_TEST, depth);
        setEnabled(GL_CULL_FACE, cull);
        setEnabled(GL_SCISSOR_TEST, scissorEnabled);
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture0);
        glActiveTexture(activeTexture);
    }

    static void setEnabled(GLenum cap, GLboolean enabled)
    {
        if (enabled) glEnable(cap); else glDisable(cap);
    }
};

inline bool allocate(Surface& surface, int width, int height)
{
    if (!surface.texture) glGenTextures(1, &surface.texture);
    if (!surface.framebuffer) glGenFramebuffers(1, &surface.framebuffer);
    if (!surface.texture || !surface.framebuffer) return false;
    glBindTexture(GL_TEXTURE_2D, surface.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (surface.width != width || surface.height != height)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // A failed resize can leave the old allocation attached and complete.
        GLint actualWidth = 0, actualHeight = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actualWidth);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actualHeight);
        if (actualWidth != width || actualHeight != height) return false;
        surface.width = width;
        surface.height = height;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, surface.framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, surface.texture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    return glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// Compose only the requested native widgets onto a transparent canvas. The
// desktop GUI texture is never read. The final blit packs this component's
// own canvas into a texture and converts its Y origin for Barony's UI mesh.
// Source/destination X always increase, preserving native left/right order.
template<class Draw>
bool render(Surface& scratch, Surface& target, SDL_Rect area,
    int canvasWidth, int canvasHeight, Draw draw)
{
    if (canvasWidth <= 0 || canvasHeight <= 0 || area.w <= 0 || area.h <= 0 ||
        area.x < 0 || area.y < 0 || area.x > canvasWidth - area.w ||
        area.y > canvasHeight - area.h) return false;
    GlState state;
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (canvasWidth > maximum || canvasHeight > maximum) return false;
    if (!allocate(scratch, canvasWidth, canvasHeight)) return false;

    glViewport(0, 0, canvasWidth, canvasHeight);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x, canvasHeight - area.y - area.h, area.w, area.h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    draw();

    const float fit = std::min(1.f, std::min(1024.f / area.w, 1024.f / area.h));
    const int width = std::max(1, static_cast<int>(std::lround(area.w * fit)));
    const int height = std::max(1, static_cast<int>(std::lround(area.h * fit)));
    if (!allocate(target, width, height)) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scratch.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(area.x, canvasHeight - area.y,
        area.x + area.w, canvasHeight - area.y - area.h,
        0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    return true;
}
} // namespace VrNativeHud
