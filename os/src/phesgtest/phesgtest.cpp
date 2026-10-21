// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// phesgtest.cpp -- OURS (GPU plan G6b): RSgImage <-> GLES2 on the Mali, zero copy.
//   1. RSgImage::Create (ARGB_8888, GLES2 surface + texture usage); the CPU pre-fills it with a pattern
//   2. eglCreatePixmapSurface(RSgImage*) + GLES2 context: clear + a shader-drawn quad render INTO the image
//   3. glFinish + eglWaitClient, then the CPU reads the image (MSgImage_Sw::DataAddress) and compares with the
//      expected colours and with glReadPixels
//   4. eglCreateImageKHR(EGL_NATIVE_PIXMAP_KHR, &sgImage2) -> glEGLImageTargetTexture2DOES: a second RSgImage the CPU
//      filled is sampled by the GPU into the first image; the CPU compares again
//   5. the same for XRGB_8888 and RGB_565 images (the 16 bit config)
// "PH: sg" lines; "PH: sg RESULT pass=N fail=M" is the verdict. SSM_TEST=phesgtest stages it at boot.
#include <e32base.h>
#include <e32debug.h>
#include <sgresource/sgresource.h>
#include <sgresource/sgimage.h>
#include <sgresource/sgimage_sw.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace
	{
	TInt gPass = 0, gFail = 0;

	void Check(TBool aOk, const char* aWhat)
		{
		if (aOk)
			++gPass;
		else
			++gFail;
		RDebug::Printf("PH: sg %s: %s", aOk ? "ok  " : "FAIL", aWhat);
		}

	// ---- pixel helpers (the CPU's view of an RSgImage) -------------------------------------------------------------
	const TInt KW = 128, KH = 96;

	struct TCpuView
		{
		TUint8* iData;
		TInt iStride;
		};

	TBool CpuView(RSgImage& aImage, TCpuView& aView)
		{
		TAny* itf = NULL;
		if (aImage.GetInterface(TUid::Uid(MSgImage_Sw::EInterfaceUid), itf) != KErrNone || !itf)
			return EFalse;
		const MSgImage_Sw* sw = static_cast<const MSgImage_Sw*>(itf);
		aView.iData = static_cast<TUint8*>(sw->DataAddress());
		aView.iStride = sw->DataStride();
		return aView.iData != NULL;
		}

	// 8888 images: byte order B, G, R, A (little endian ARGB)
	void Put8888(const TCpuView& aV, TInt aX, TInt aY, TUint8 aR, TUint8 aG, TUint8 aB, TUint8 aA)
		{
		TUint8* p = aV.iData + aY * aV.iStride + aX * 4;
		p[0] = aB; p[1] = aG; p[2] = aR; p[3] = aA;
		}

	// a progress line before each step: the last one in the log is where a crash happened (boot 612: a data abort, far=0x10,
	// in the GLSL compiler after eglMakeCurrent)
	void Step(const char* aWhat) { RDebug::Printf("PH: sg step: %s", aWhat); }

	TBool Near(TInt aA, TInt aB, TInt aTol) { return aA - aB <= aTol && aB - aA <= aTol; }

	// compare a pixel of an 8888 image (alpha only when aCheckAlpha)
	TBool Is8888(const TCpuView& aV, TInt aX, TInt aY, TInt aR, TInt aG, TInt aB, TInt aA)
		{
		const TUint8* p = aV.iData + aY * aV.iStride + aX * 4;
		return Near(p[2], aR, 2) && Near(p[1], aG, 2) && Near(p[0], aB, 2) && (aA < 0 || Near(p[3], aA, 2));
		}

	void Fill8888(const TCpuView& aV, TInt aW, TInt aH, TUint8 aR, TUint8 aG, TUint8 aB)
		{
		for (TInt y = 0; y < aH; ++y)
			for (TInt x = 0; x < aW; ++x)
				Put8888(aV, x, y, aR, aG, aB, 255);
		}

	// ---- GL ----------------------------------------------------------------------------------------------------------
	GLuint Shader(GLenum aType, const char* aSrc)
		{
		GLuint s = glCreateShader(aType);
		glShaderSource(s, 1, &aSrc, NULL);
		glCompileShader(s);
		GLint ok = 0;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok)
			{
			char log[200];
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			RDebug::Printf("PH: sg shader error: %s", log);
			}
		return s;
		}

	GLuint Program(const char* aVs, const char* aFs)
		{
		GLuint p = glCreateProgram();
		glAttachShader(p, Shader(GL_VERTEX_SHADER, aVs));
		glAttachShader(p, Shader(GL_FRAGMENT_SHADER, aFs));
		glBindAttribLocation(p, 0, "p");
		glLinkProgram(p);
		GLint linked = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &linked);
		if (!linked)
			RDebug::Printf("PH: sg program link failed");
		return p;
		}

	const char KVs[] = "attribute vec2 p; varying vec2 t; uniform vec4 box;\n"
		"void main() { t = p * 0.5 + 0.5; gl_Position = vec4(box.xy + t * (box.zw - box.xy), 0.0, 1.0); }\n";
	const char KFsSolid[] = "precision mediump float; uniform vec4 col; varying vec2 t;\n"
		"void main() { gl_FragColor = col; }\n";
	const char KFsTex[] = "precision mediump float; uniform sampler2D s; varying vec2 t;\n"
		"void main() { gl_FragColor = texture2D(s, t); }\n";
	const GLfloat KQuad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };

	// the display, config choice and entry points shared by the cases
	struct TEgl
		{
		EGLDisplay iDpy;
		PFNEGLCREATEIMAGEKHRPROC iCreateImage;
		PFNEGLDESTROYIMAGEKHRPROC iDestroyImage;
		PFNGLEGLIMAGETARGETTEXTURE2DOESPROC iTarget;
		GLuint iSolid, iTex;
		};

	TBool EglInit(TEgl& aE)
		{
		aE.iDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		eglInitialize(aE.iDpy, NULL, NULL);
		const char* ext = eglQueryString(aE.iDpy, EGL_EXTENSIONS);
		RDebug::Printf("PH: sg egl extensions: %s", ext ? ext : "(none)");
		TPtrC8 e((const TUint8*)(ext ? ext : ""));
		Check(e.Find(_L8("EGL_KHR_image_pixmap")) >= 0, "EGL_KHR_image_pixmap advertised");
		Check(e.Find(_L8("EGL_NOK_pixmap_type_rsgimage")) >= 0, "EGL_NOK_pixmap_type_rsgimage advertised");
		aE.iCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		aE.iDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		aE.iTarget = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		return aE.iCreateImage && aE.iDestroyImage && aE.iTarget;
		}

	// eglChooseConfig's "at least" sizes would hand a 565 image the 888 config (larger sizes sort first, and a request for
	// EGL_ALPHA_SIZE 0 accepts alpha): take the config whose red and alpha sizes are exactly the image's
	EGLConfig ChooseConfig(EGLDisplay aDpy, TInt aRed, TInt aAlpha)
		{
		const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PIXMAP_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, aRed, EGL_ALPHA_SIZE, aAlpha, EGL_NONE };
		EGLConfig cfgs[16];
		EGLint n = 0;
		eglChooseConfig(aDpy, attr, cfgs, 16, &n);
		for (TInt i = 0; i < n; ++i)
			{
			EGLint red = 0, alpha = 0;
			eglGetConfigAttrib(aDpy, cfgs[i], EGL_RED_SIZE, &red);
			eglGetConfigAttrib(aDpy, cfgs[i], EGL_ALPHA_SIZE, &alpha);
			if (red == aRed && alpha == aAlpha)
				return cfgs[i];
			}
		return NULL;
		}

	void SolidQuad(const TEgl& aE, GLfloat aX0, GLfloat aY0, GLfloat aX1, GLfloat aY1, GLfloat aR, GLfloat aG, GLfloat aB)
		{
		glUseProgram(aE.iSolid);
		glUniform4f(glGetUniformLocation(aE.iSolid, "box"), aX0, aY0, aX1, aY1);
		glUniform4f(glGetUniformLocation(aE.iSolid, "col"), aR, aG, aB, 1.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}

	TInt NowMs() { return (TInt)(User::NTickCount()); }

	// One render target of pixel format aFormat (ESgPixelFormat*): clear blue, a red quad over the left half (shader),
	// finish, read back with the CPU; if aSampleFrom, a second image filled by the CPU is sampled over the right half.
	void RunFormat(TEgl& aE, TInt aFormat, const char* aName)
		{
		const TBool is565 = aFormat == ESgPixelFormatRGB_565;
		const TBool hasAlpha = aFormat == ESgPixelFormatARGB_8888;
		const TUint32 usage = ESgUsageBitOpenGles2Surface | ESgUsageBitOpenGles2Texture2D;
		RDebug::Printf("PH: sg ---- %s ----", aName);

		RSgImage target;
		TInt r = target.Create(TSgImageInfo(TSize(KW, KH), aFormat, usage), NULL, 0);
		Check(r == KErrNone, "RSgImage::Create (render target)");
		if (r != KErrNone)
			return;
		TCpuView tv;
		Check(CpuView(target, tv), "MSgImage_Sw::DataAddress of the target");
		RDebug::Printf("PH: sg target data %08x stride %d", (TUint)tv.iData, tv.iStride);
		// the CPU's pattern the GPU must overwrite (all 0xA5: neither of the expected colours)
		for (TInt y = 0; y < KH; ++y)
			Mem::Fill(tv.iData + y * tv.iStride, KW * (is565 ? 2 : 4), 0xA5);

		EGLConfig cfg = ChooseConfig(aE.iDpy, is565 ? 5 : 8, hasAlpha ? 8 : 0);
		Check(cfg != NULL, "eglChooseConfig (EGL_PIXMAP_BIT)");
		if (!cfg)
			{
			target.Close();
			return;
			}
		const TInt t0 = NowMs();
		EGLSurface surf = eglCreatePixmapSurface(aE.iDpy, cfg, (EGLNativePixmapType)&target, NULL);
		RDebug::Printf("PH: sg eglCreatePixmapSurface %08x egl error %x (%d ms)", (TUint)surf, eglGetError(), NowMs() - t0);
		Check(surf != EGL_NO_SURFACE, "eglCreatePixmapSurface(RSgImage*)");
		EGLint w = 0, h = 0;
		if (surf != EGL_NO_SURFACE)
			{
			eglQuerySurface(aE.iDpy, surf, EGL_WIDTH, &w);
			eglQuerySurface(aE.iDpy, surf, EGL_HEIGHT, &h);
			}
		Check(w == KW && h == KH, "pixmap surface size = image size");
		const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
		EGLContext ctx = surf != EGL_NO_SURFACE ? eglCreateContext(aE.iDpy, cfg, EGL_NO_CONTEXT, ctxAttr) : EGL_NO_CONTEXT;
		const EGLBoolean cur = ctx != EGL_NO_CONTEXT && eglMakeCurrent(aE.iDpy, surf, surf, ctx);
		Check(cur, "eglMakeCurrent(pixmap surface)");
		if (cur)
			{
			Step("compile the solid program");
			aE.iSolid = Program(KVs, KFsSolid);
			Step("compile the texture program");
			aE.iTex = Program(KVs, KFsTex);
			Step("vertex attrib setup");
			glEnableVertexAttribArray(0);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, KQuad);
			Step("glViewport");
			glViewport(0, 0, KW, KH);
			Step("glClear (first use of the pixmap framebuffer)");
			glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			Step("left quad");
			SolidQuad(aE, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);       // left half red
			// top-right quarter (GL +y) yellow: tells which end of memory is the top
			Step("top-right quad");
			SolidQuad(aE, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f);
			Step("glFinish");
			glFinish();
			Check(glGetError() == GL_NO_ERROR, "GL error free after the draw");
			Step("glReadPixels");
			GLubyte px[4] = { 0, 0, 0, 0 };
			glReadPixels(KW * 3 / 4, KH / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);   // GL bottom-right quarter: blue
			RDebug::Printf("PH: sg glReadPixels (3/4 w, 1/4 h) = %02x %02x %02x %02x", px[0], px[1], px[2], px[3]);
			Check(px[0] < 4 && px[1] < 4 && px[2] > 250, "glReadPixels sees the clear colour");
			Step("eglWaitClient");
			eglWaitClient();
			Step("CPU compare");

			if (!is565)
				{
				// the CPU reads what the GPU wrote: left half red, right half blue except the top-right (GL) quarter yellow
				Check(Is8888(tv, KW / 4, KH / 2, 255, 0, 0, hasAlpha ? 255 : -1), "CPU read: left half is the GPU's red");
				const TBool glTopIsRow0 = Is8888(tv, KW * 3 / 4, KH / 4, 255, 255, 0, -1);
				const TBool glTopIsLast = Is8888(tv, KW * 3 / 4, KH * 3 / 4, 255, 255, 0, -1);
				RDebug::Printf("PH: sg orientation: GL top is memory %s", glTopIsRow0 ? "row 0 (top-down, the CFbsBitmap way)"
					: glTopIsLast ? "the LAST row (bottom-up)" : "?");
				Check(glTopIsRow0 || glTopIsLast, "CPU read: the yellow quarter is where a GL top-right quarter must be");
				Check(Is8888(tv, KW * 3 / 4, glTopIsRow0 ? KH * 3 / 4 : KH / 4, 0, 0, 255, -1), "CPU read: the other right quarter is the clear blue");
				Check(glTopIsRow0, "orientation: memory row 0 is the GL top (Symbian pixmap convention)");
				// GL and CPU agree
				Check(Is8888(tv, KW * 3 / 4, glTopIsRow0 ? KH * 3 / 4 : KH / 4, px[0], px[1], px[2], -1),
					"CPU read = glReadPixels at the same pixel");
				}
			else
				{
				const TUint16* row = reinterpret_cast<const TUint16*>(tv.iData + (KH / 2) * tv.iStride);
				RDebug::Printf("PH: sg 565 left %04x right %04x", row[KW / 4], row[KW * 3 / 4]);
				Check(row[KW / 4] == 0xF800, "CPU read (RGB565): left half is red");
				Check(row[KW * 3 / 4] == 0x001F || row[KW * 3 / 4] == 0xFFE0, "CPU read (RGB565): right half is blue / yellow");
				}

			// ---- an EGLImage over a CPU-filled RSgImage, sampled by the GPU into the target (8888 only) -----------------
			if (!is565)
				{
				RSgImage src;
				r = src.Create(TSgImageInfo(TSize(64, 64), ESgPixelFormatARGB_8888, ESgUsageBitOpenGles2Texture2D), NULL, 0);
				Check(r == KErrNone, "RSgImage::Create (texture source)");
				TCpuView sv;
				if (r == KErrNone && CpuView(src, sv))
					{
					for (TInt y = 0; y < 64; ++y)
						for (TInt x = 0; x < 64; ++x)
							if (x < 32)
								Put8888(sv, x, y, 0, 255, 0, 255);        // left half green
							else
								Put8888(sv, x, y, 255, 0, 255, 255);      // right half magenta
					Step("texture source filled; eglWaitNative");
					eglWaitNative(EGL_CORE_NATIVE_ENGINE);      // the CPU's lines go to memory before the GPU reads them
					const EGLint imgAttr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
					const TInt t1 = NowMs();
					EGLImageKHR image = aE.iCreateImage(aE.iDpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)&src, imgAttr);
					RDebug::Printf("PH: sg eglCreateImageKHR %08x egl error %x (%d ms)", (TUint)image, eglGetError(), NowMs() - t1);
					Check(image != EGL_NO_IMAGE_KHR, "eglCreateImageKHR(EGL_NATIVE_PIXMAP_KHR, RSgImage*)");
					if (image != EGL_NO_IMAGE_KHR)
						{
						GLuint tex = 0;
						glGenTextures(1, &tex);
						Step("glEGLImageTargetTexture2DOES");
						glBindTexture(GL_TEXTURE_2D, tex);
						aE.iTarget(GL_TEXTURE_2D, (GLeglImageOES)image);
						Check(glGetError() == GL_NO_ERROR, "glEGLImageTargetTexture2DOES");
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
						// the CPU is about to read the target again, the GPU about to write it
						eglWaitNative(EGL_CORE_NATIVE_ENGINE);
						glClearColor(0, 0, 0, 1);
						glClear(GL_COLOR_BUFFER_BIT);
						Step("draw the textured quad");
						glUseProgram(aE.iTex);
						glUniform1i(glGetUniformLocation(aE.iTex, "s"), 0);
						glUniform4f(glGetUniformLocation(aE.iTex, "box"), -1.0f, -1.0f, 1.0f, 1.0f);
						glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
						glFinish();
						eglWaitClient();
						Check(glGetError() == GL_NO_ERROR, "GL error free after sampling the EGLImage");
						Check(Is8888(tv, KW / 4, KH / 2, 0, 255, 0, hasAlpha ? 255 : -1), "CPU read: left half = the CPU-written green, via the GPU");
						Check(Is8888(tv, KW * 3 / 4, KH / 2, 255, 0, 255, hasAlpha ? 255 : -1), "CPU read: right half = the CPU-written magenta, via the GPU");
						glDeleteTextures(1, &tex);
						Check(aE.iDestroyImage(aE.iDpy, image), "eglDestroyImageKHR");
						}
					}
				src.Close();
				}
			}
		eglMakeCurrent(aE.iDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (ctx != EGL_NO_CONTEXT)
			eglDestroyContext(aE.iDpy, ctx);
		if (surf != EGL_NO_SURFACE)
			Check(eglDestroySurface(aE.iDpy, surf), "eglDestroySurface(pixmap)");
		target.Close();
		}

	void DoTests()
		{
		RSgDriver driver;
		const TInt r = driver.Open();
		RDebug::Printf("PH: sg RSgDriver::Open = %d", r);
		Check(r == KErrNone, "RSgDriver::Open");
		if (r != KErrNone)
			return;
		TEgl e;
		Mem::FillZ(&e, sizeof(e));
		if (EglInit(e))
			{
			RunFormat(e, ESgPixelFormatARGB_8888, "ARGB_8888");
			RunFormat(e, ESgPixelFormatXRGB_8888, "XRGB_8888");
			RunFormat(e, ESgPixelFormatRGB_565, "RGB_565");
			}
		else
			Check(EFalse, "eglCreateImageKHR / glEGLImageTargetTexture2DOES entry points");
		eglTerminate(e.iDpy);
		driver.Close();
		}
	}

TInt E32Main()
	{
	__UHEAP_MARK;
	RDebug::Printf("PH: sg start (G6b: RSgImage <-> GLES2, zero copy)");
	DoTests();
	RDebug::Printf("PH: sg RESULT pass=%d fail=%d", gPass, gFail);
	RDebug::Printf("PH: sg done");
	return gFail ? KErrGeneral : KErrNone;
	}
