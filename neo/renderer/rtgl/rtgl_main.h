#ifndef __D3_RTGL_MAIN_H__
#define __D3_RTGL_MAIN_H__

#ifdef D3_RTGL

// sys/glimp.cpp: native Win32 HWND of the SDL window, so RTGL1 can create its
// own Vulkan surface on the same window. Returns NULL on non-Win32 / failure.
void *GLimp_GetWin32WindowHandle( void );

// Lifecycle (R_InitOpenGL / ShutdownOpenGL). RTGL_Init() is a no-op when the
// r_rtgl_enable cvar is 0, keeping the stock ARB2/OpenGL renderer unchanged.
bool RTGL_IsActive( void );
bool RTGL_Init( void );
void RTGL_Shutdown( void );

// Per-frame hooks:
//   BeginFrame          -> rgStartFrame (P1)
//   SubmitFrameCommands -> walk RC_DRAW_VIEW, upload camera (P2) + geometry
//                          (P2) + original textures (P3); called from
//                          R_IssueRenderCommands() before the chain is cleared
//   EndFrame            -> rgDrawFrame (ray trace + present)
void RTGL_BeginFrame( int windowWidth, int windowHeight );
void RTGL_SubmitFrameCommands( const void *commandChainHead );
void RTGL_EndFrame( void );

#endif // D3_RTGL
#endif // __D3_RTGL_MAIN_H__
