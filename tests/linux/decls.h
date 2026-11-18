struct _RECT { int l,t,r,b; }; struct RGNDATA { int x; };
enum _D3DFORMAT { F0 }; enum _D3DPOOL { P0 }; enum _D3DTEXTUREFILTERTYPE { T0, T1 };
enum _D3DMULTISAMPLE_TYPE { M0 }; enum _D3DDEVTYPE { D0 }; enum _D3DPRIMITIVETYPE { PT0 };
struct _D3DPRESENT_PARAMETERS_ { int x; };
struct IDirect3DSurface9 { unsigned gl_type; unsigned gl_name; int w, h; };
struct IDirect3DTexture9 { unsigned gl_name; int w, h; long GetSurfaceLevel(unsigned, IDirect3DSurface9**); };
struct IDirect3DDevice9;
struct IDirect3D9 { long CreateDevice(unsigned, _D3DDEVTYPE, void*, unsigned, _D3DPRESENT_PARAMETERS_*, IDirect3DDevice9**); };
struct IDirect3DDevice9 {
  long Present(const _RECT*, const _RECT*, void*, const RGNDATA*);
  long SetRenderTarget(unsigned, IDirect3DSurface9*);
  long StretchRect(IDirect3DSurface9*, const _RECT*, IDirect3DSurface9*, const _RECT*, _D3DTEXTUREFILTERTYPE);
  long CreateTexture(unsigned,unsigned,unsigned,unsigned,_D3DFORMAT,_D3DPOOL,IDirect3DTexture9**,void**,char*);
  long CreateRenderTarget(unsigned,unsigned,_D3DFORMAT,_D3DMULTISAMPLE_TYPE,unsigned,int,IDirect3DSurface9**,void**,char*);
  long Reset(_D3DPRESENT_PARAMETERS_*);
  long DrawPrimitive(_D3DPRIMITIVETYPE, unsigned, unsigned);
};
extern "C" {
  IDirect3D9* Direct3DCreate9(unsigned);
  unsigned mock_make_texture(int,int); unsigned mock_make_renderbuffer(int,int,int); unsigned mock_make_fbo(int,unsigned);
  void mock_bind_draw(unsigned); void mock_set_state(unsigned,unsigned,unsigned,unsigned,int);
  int mock_state_ok(unsigned,unsigned,unsigned,unsigned,int); void mock_trace(const char*); unsigned mock_draw(void);
}
