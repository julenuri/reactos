/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS Win32k subsystem
 * PURPOSE:          Layered window support
 * FILE:             win32ss/user/ntuser/layered.c
 * PROGRAMER:
 */

#include <win32k.h>
DBG_DEFAULT_CHANNEL(UserMisc);


typedef struct _LRD_PROP
{
   COLORREF Key;
   BYTE     Alpha;
   BYTE     is_Layered;
   BYTE     NoUsed[2];
   DWORD    Flags;
   /*
    * The content that is behind the window. ReactOS has no compositor, so
    * the window manager does not know what is behind a layered window and
    * the old content of the layer would stay on the screen ("onion skin")
    * wherever the new content of the layer is transparent.
    *
    * It is therefore saved in a bitmap, which is restored before the layer
    * is updated.
    */
   HBITMAP  hBackground;
   HDC      hdcBackground;
   RECTL    BackgroundRect;   /* screen coordinates the bitmap holds */
   SIZE     BackgroundSize;
   BOOL     HasBackground;
} LRD_PROP, *PLRD_PROP;

BOOL FASTCALL
GetLayeredStatus(PWND pWnd)
{
   PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);
   if (pLrdProp)
   {
      return pLrdProp->is_Layered;
   }
   return FALSE;
}

BOOL FASTCALL
SetLayeredStatus(PWND pWnd, BYTE set)
{
   PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);
   if (pLrdProp)
   {
      pLrdProp->is_Layered = set;
      return TRUE;
   }
   return FALSE;
}

BOOL FASTCALL
IntSetLayeredWindowAttributes(PWND pWnd,
                              COLORREF crKey,
                              BYTE bAlpha,
                              DWORD dwFlags)
{
   PLRD_PROP pLrdProp;
   INT was_Layered;

   if (!(pWnd->ExStyle & WS_EX_LAYERED) )
   {
      ERR("Not a Layered Window!\n");
      return FALSE;
   }

   pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);

   if (!pLrdProp)
   {
      pLrdProp = ExAllocatePoolWithTag(PagedPool, sizeof(LRD_PROP), USERTAG_REDIRECT);
      if (pLrdProp == NULL)
      {
         ERR("failed to allocate LRD_PROP\n");
         return FALSE;
      }
      RtlZeroMemory(pLrdProp, sizeof(LRD_PROP));
      UserSetProp(pWnd, AtomLayer, (HANDLE)pLrdProp, TRUE);
   }

   if (pLrdProp)
   {
      was_Layered = pLrdProp->is_Layered;

      pLrdProp->Key = crKey;

      if (dwFlags & LWA_ALPHA)
      {
         pLrdProp->Alpha = bAlpha;
      }
      else
      {
         if (!pLrdProp->is_Layered) pLrdProp->Alpha = 0;
      }

      pLrdProp->Flags = dwFlags;

      pLrdProp->is_Layered = 1;
  
      if (!was_Layered)
         co_UserRedrawWindow(pWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME );
   }
   // FIXME: Now set some bits to the Window DC!!!!
   return TRUE;
}

/*
 * Returns the properties of the layered window, creating them if they do not
 * exist yet. They are only created by SetLayeredWindowAttributes(), but a
 * window can be layered without ever calling it.
 */
static PLRD_PROP
IntGetLayeredProperties(PWND pWnd)
{
    PLRD_PROP pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);

    if (!pLrdProp)
    {
        pLrdProp = ExAllocatePoolWithTag(PagedPool, sizeof(LRD_PROP), USERTAG_REDIRECT);
        if (!pLrdProp) return NULL;

        RtlZeroMemory(pLrdProp, sizeof(LRD_PROP));
        UserSetProp(pWnd, AtomLayer, (HANDLE)pLrdProp, TRUE);
    }

    return pLrdProp;
}

/*
 * Returns the bitmap that holds the content that is behind the layered
 * window, creating it if needed.
 */
static BOOL
IntLayerBackground_Get(PLRD_PROP pLrdProp, HDC hdc, LONG Width, LONG Height,
                       const RECTL *ScreenRect)
{
    if (pLrdProp->hBackground &&
        (pLrdProp->BackgroundSize.cx != Width || pLrdProp->BackgroundSize.cy != Height ||
         pLrdProp->BackgroundRect.left != ScreenRect->left ||
         pLrdProp->BackgroundRect.top != ScreenRect->top))
    {
        /* The window has changed its size or its position, the old background
           is useless. */
        if (pLrdProp->hdcBackground)
        {
            IntGdiDeleteDC(pLrdProp->hdcBackground, FALSE);
            pLrdProp->hdcBackground = NULL;
        }
        if (pLrdProp->hBackground)
        {
            GreDeleteObject(pLrdProp->hBackground);
            pLrdProp->hBackground = NULL;
        }
        pLrdProp->HasBackground = FALSE;
    }

    if (!pLrdProp->hBackground)
    {
        BITMAPINFO bi;

        pLrdProp->hdcBackground = NtGdiCreateCompatibleDC(hdc);
        if (!pLrdProp->hdcBackground) return FALSE;

        RtlZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = Width;
        bi.bmiHeader.biHeight = -Height;    /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        pLrdProp->hBackground = NtGdiCreateDIBSection(hdc, NULL, 0, &bi,
                                                     DIB_RGB_COLORS,
                                                     sizeof(BITMAPINFOHEADER),
                                                     0, 0, NULL);
        if (!pLrdProp->hBackground)
        {
            IntGdiDeleteDC(pLrdProp->hdcBackground, FALSE);
            pLrdProp->hdcBackground = NULL;
            return FALSE;
        }

        NtGdiSelectBitmap(pLrdProp->hdcBackground, pLrdProp->hBackground);

        pLrdProp->BackgroundSize.cx = Width;
        pLrdProp->BackgroundSize.cy = Height;
        pLrdProp->BackgroundRect = *ScreenRect;
        pLrdProp->HasBackground = FALSE;
    }

    return TRUE;
}

/*
 * Restores the content that was behind the layered window and saves the
 * content that is behind it now.
 *
 * ReactOS has no compositor, so the window manager does not know what is
 * behind a layered window.  The old content of the layer would therefore stay
 * on the screen ("onion skin") wherever the new content of the layer is
 * transparent, and the content that is behind the window (the desktop for
 * instance) would never be seen.
 *
 * The content that is behind the window is therefore saved in a bitmap, which
 * is restored before the layer is updated.  This is the equivalent of the
 * "redraw below" that Windows performs, but it does not depend on the windows
 * below repainting themselves (which is asynchronous and would therefore
 * repaint over the new content of the layer).
 *
 * The rectangle is given in screen coordinates, and the content is saved in
 * the same coordinates, so that it does not matter where the window is.
 */
static VOID
IntLayerBackground_Update(PLRD_PROP pLrdProp, PWND pWnd, const RECTL *ScreenRect)
{
    LONG Width = ScreenRect->right - ScreenRect->left;
    LONG Height = ScreenRect->bottom - ScreenRect->top;
    HDC hdcScreen;
    UNICODE_STRING DriverName = RTL_CONSTANT_STRING(L"DISPLAY");

    if (Width <= 0 || Height <= 0) return;

    hdcScreen = IntGdiCreateDC(&DriverName, NULL, NULL, NULL, FALSE);
    if (!hdcScreen) return;

    if (!IntLayerBackground_Get(pLrdProp, hdcScreen, Width, Height, ScreenRect))
    {
        IntGdiDeleteDC(hdcScreen, FALSE);
        return;
    }

    if (pLrdProp->HasBackground)
    {
        /* Restore the content that was behind the window. */
        NtGdiBitBlt(hdcScreen, ScreenRect->left, ScreenRect->top, Width, Height,
                    pLrdProp->hdcBackground,
                    ScreenRect->left - pLrdProp->BackgroundRect.left,
                    ScreenRect->top - pLrdProp->BackgroundRect.top,
                    SRCCOPY, CLR_INVALID, 0);
    }

    /* Save the content that is behind the window now. */
    NtGdiBitBlt(pLrdProp->hdcBackground,
                ScreenRect->left - pLrdProp->BackgroundRect.left,
                ScreenRect->top - pLrdProp->BackgroundRect.top,
                Width, Height,
                hdcScreen, ScreenRect->left, ScreenRect->top,
                SRCCOPY, CLR_INVALID, 0);

    pLrdProp->HasBackground = TRUE;

    IntGdiDeleteDC(hdcScreen, FALSE);
}

BOOL FASTCALL
IntUpdateLayeredWindowI( PWND pWnd,
                         UPDATELAYEREDWINDOWINFO *info )
{
   PWND Parent;
   RECT Window, Client;
   SIZE Offset;
   BOOL ret = FALSE;
   DWORD flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;

   Window = pWnd->rcWindow;
   Client = pWnd->rcClient;

   Parent = pWnd->spwndParent;
   if (pWnd->style & WS_CHILD && Parent)
   {
      RECTL_vOffsetRect(&Window, - Parent->rcClient.left, - Parent->rcClient.top);
      RECTL_vOffsetRect(&Client, - Parent->rcClient.left, - Parent->rcClient.top);
   }

   if (info->pptDst)
   {
      Offset.cx = info->pptDst->x - Window.left;
      Offset.cy = info->pptDst->y - Window.top;
      RECTL_vOffsetRect( &Client, Offset.cx, Offset.cy );
      RECTL_vOffsetRect( &Window, Offset.cx, Offset.cy );
      flags &= ~SWP_NOMOVE;
   }
   if (info->psize)
   {
      Offset.cx = info->psize->cx - (Window.right  - Window.left);
      Offset.cy = info->psize->cy - (Window.bottom - Window.top);
      if (info->psize->cx <= 0 || info->psize->cy <= 0)
      {
          ERR("Update Layered Invalid Parameters\n");
          EngSetLastError( ERROR_INVALID_PARAMETER );
          return FALSE;
      }
      if ((info->dwFlags & ULW_EX_NORESIZE) && (Offset.cx || Offset.cy))
      {
          ERR("Wrong Size\n");
          EngSetLastError( ERROR_INCORRECT_SIZE );
          return FALSE;
      }
      Client.right  += Offset.cx;
      Client.bottom += Offset.cy;
      Window.right  += Offset.cx;
      Window.bottom += Offset.cy;
      flags &= ~SWP_NOSIZE;
   }

   if (info->hdcSrc)
   {
      HDC hdc, hdcBuffer;
      RECT Rect;
      BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
      COLORREF color_key = (info->dwFlags & ULW_COLORKEY) ? info->crKey : CLR_INVALID;
      HBITMAP hOldBitmap, hbmSrc;
      DIBSECTION dibs;
      LONG Width, Height;

      Rect = Window;

      RECTL_vOffsetRect( &Rect, -Window.left, -Window.top );

      TRACE("H %d W %d\n",Rect.bottom - Rect.top,Rect.right - Rect.left);

      Width  = Rect.right  - Rect.left;
      Height = Rect.bottom - Rect.top;

      /* The dirty rectangle is given in window coordinates, just like Rect.
         Only that part of the window has to be updated. */
      if (info->prcDirty)
      {
         RECT DirtyRect;

         TRACE("prcDirty %d %d %d %d\n",
               info->prcDirty->left, info->prcDirty->top,
               info->prcDirty->right, info->prcDirty->bottom);

         RECTL_bIntersectRect( &DirtyRect, &Rect, info->prcDirty );
         Rect = DirtyRect;
      }

      {
         RECTL ScreenRect;

         ScreenRect = Rect;
         RECTL_vOffsetRect(&ScreenRect, Window.left, Window.top);

         /*
          * Restore the content that was behind the window and save the
          * content that is behind it now, so that the old content of the
          * layer does not stay on the screen where the new content of the
          * layer is transparent ("onion skin").
          */
         if (!info->hdcDst)
         {
             PLRD_PROP pLrdProp = IntGetLayeredProperties(pWnd);

             if (pLrdProp)
             {
                 IntLayerBackground_Update(pLrdProp, pWnd, &ScreenRect);
             }
         }
      }

      if (!info->hdcDst) hdc = UserGetDCEx(pWnd, NULL, DCX_USESTYLE);
      else hdc = info->hdcDst;

      hbmSrc = NtGdiCreateCompatibleBitmap(info->hdcSrc, Width, Height);

      if (hbmSrc)
      {
         GreGetObject(hbmSrc, sizeof(DIBSECTION), &dibs);

         TRACE("Source Bitmap bc %d\n",dibs.dsBmih.biBitCount);
      }

      hdcBuffer = NtGdiCreateCompatibleDC(hdc);

      hOldBitmap = (HBITMAP)NtGdiSelectBitmap(hdcBuffer, hbmSrc);

      /* Copy the source bitmap into our own buffer, so that we can blend it
         over the destination without disturbing the source device context. */
      NtGdiStretchBlt( hdcBuffer,
                       Rect.left,
                       Rect.top,
                       Width,
                       Height,
                       info->hdcSrc,
                       Rect.left + (info->pptSrc ? info->pptSrc->x : 0),
                       Rect.top  + (info->pptSrc ? info->pptSrc->y : 0),
                       Width,
                       Height,
                       SRCCOPY,
                       color_key );

      if (info->dwFlags & ULW_ALPHA)
      {
         blend = *info->pblend;
         TRACE("ULW_ALPHA bop %d Alpha %d aF %d\n", blend.BlendOp, blend.SourceConstantAlpha, blend.AlphaFormat);
      }

      /* Blend the new content over the destination.
         With ULW_ALPHA the per pixel alpha value of the source tells how much
         of the destination is preserved: a fully transparent pixel leaves the
         destination untouched, so whatever is behind the window (the desktop
         for a translucent window) stays visible.
         Note that the destination must not be cleared beforehand, otherwise
         the content behind the window would be destroyed. */
      ret = NtGdiAlphaBlend( hdc,
                             Rect.left,
                             Rect.top,
                             Rect.right - Rect.left,
                             Rect.bottom - Rect.top,
                             hdcBuffer,
                             Rect.left,
                             Rect.top,
                             Rect.right - Rect.left,
                             Rect.bottom - Rect.top,
                             blend,
                             0);

      NtGdiSelectBitmap(hdcBuffer, hOldBitmap);
      if (hbmSrc) GreDeleteObject(hbmSrc);
      if (hdcBuffer) IntGdiDeleteDC(hdcBuffer, FALSE);
      if (!info->hdcDst) UserReleaseDC(pWnd, hdc, FALSE);
   }
   else
      ret = TRUE;

   co_WinPosSetWindowPos(pWnd, 0, Window.left, Window.top, Window.right - Window.left, Window.bottom - Window.top, flags);
   return ret;
}


/*
 * @implemented
 */
BOOL
APIENTRY
NtUserGetLayeredWindowAttributes(
    HWND hwnd,
    COLORREF *pcrKey,
    BYTE *pbAlpha,
    DWORD *pdwFlags)
{
   PLRD_PROP pLrdProp;
   PWND pWnd;
   BOOL Ret = FALSE;

   TRACE("Enter NtUserGetLayeredWindowAttributes\n");
   UserEnterShared();

   if (!(pWnd = UserGetWindowObject(hwnd)) ||
       !(pWnd->ExStyle & WS_EX_LAYERED) )
   {
      ERR("Not a Layered Window!\n");
      goto Exit;
   }

   pLrdProp = UserGetProp(pWnd, AtomLayer, TRUE);

   if (!pLrdProp)
   {
      TRACE("No Prop!\n");
      goto Exit;
   }

   if (pLrdProp->is_Layered == 0)
   {
      goto Exit;
   }

   _SEH2_TRY
   {
      if (pcrKey)
      {
          ProbeForWrite(pcrKey, sizeof(*pcrKey), 1);
          *pcrKey = pLrdProp->Key;
      }
      if (pbAlpha)
      {
          ProbeForWrite(pbAlpha, sizeof(*pbAlpha), 1);
          *pbAlpha = pLrdProp->Alpha;
      }
      if (pdwFlags)
      {
          ProbeForWrite(pdwFlags, sizeof(*pdwFlags), 1);
          *pdwFlags = pLrdProp->Flags;
      }
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
      SetLastNtError(_SEH2_GetExceptionCode());
      _SEH2_YIELD(goto Exit);
   }
   _SEH2_END;

   Ret = TRUE;

Exit:
   TRACE("Leave NtUserGetLayeredWindowAttributes, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserSetLayeredWindowAttributes(HWND hwnd,
			   COLORREF crKey,
			   BYTE bAlpha,
			   DWORD dwFlags)
{
   PWND pWnd;
   BOOL Ret = FALSE;

   TRACE("Enter NtUserSetLayeredWindowAttributes\n");
   UserEnterExclusive();

   if (!(pWnd = UserGetWindowObject(hwnd)) ||
       !(pWnd->ExStyle & WS_EX_LAYERED) )
   {
      ERR("Not a Layered Window!\n");
      goto Exit;
   }

   Ret = IntSetLayeredWindowAttributes(pWnd, crKey, bAlpha, dwFlags);
Exit:
   TRACE("Leave NtUserSetLayeredWindowAttributes, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/*
 * @implemented
 */
BOOL
APIENTRY
NtUserUpdateLayeredWindow(
   HWND hwnd,
   HDC hdcDst,
   POINT *pptDst,
   SIZE *psize,
   HDC hdcSrc,
   POINT *pptSrc,
   COLORREF crKey,
   BLENDFUNCTION *pblend,
   DWORD dwFlags,
   RECT *prcDirty)
{
   UPDATELAYEREDWINDOWINFO info;
   POINT Dst, Src; 
   SIZE Size;
   RECT Dirty;
   BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
   PWND pWnd;
   BOOL Ret = FALSE;

   TRACE("Enter NtUserUpdateLayeredWindow\n");
   UserEnterExclusive();

   if (!(pWnd = UserGetWindowObject(hwnd)))
   {
      goto Exit;
   }

   _SEH2_TRY
   {
      if (pptDst)
      {
         ProbeForRead(pptDst, sizeof(POINT), 1);
         Dst = *pptDst;
      }
      if (pptSrc)
      {
         ProbeForRead(pptSrc, sizeof(POINT), 1);
         Src = *pptSrc;
      }
      ProbeForRead(psize, sizeof(SIZE), 1);
      Size = *psize;
      if (pblend)
      {
         ProbeForRead(pblend, sizeof(BLENDFUNCTION), 1);
         blend = *pblend;
      }
      if (prcDirty)
      {
         ProbeForRead(prcDirty, sizeof(RECT), 1);
         Dirty = *prcDirty;
      }
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
      EngSetLastError( ERROR_INVALID_PARAMETER );
      _SEH2_YIELD(goto Exit);
   }
   _SEH2_END;

   if ( GetLayeredStatus(pWnd) ||
        dwFlags & ~(ULW_COLORKEY | ULW_ALPHA | ULW_OPAQUE | ULW_EX_NORESIZE) ||
       !(pWnd->ExStyle & WS_EX_LAYERED) )
   {
      ERR("Layered Window Invalid Parameters\n");
      EngSetLastError( ERROR_INVALID_PARAMETER );
      goto Exit;
   }

   info.cbSize   = sizeof(info);
   info.hdcDst   = hdcDst;
   info.pptDst   = pptDst? &Dst : NULL;
   info.psize    = &Size;
   info.hdcSrc   = hdcSrc;
   info.pptSrc   = pptSrc ? &Src : NULL;
   info.crKey    = crKey;
   info.pblend   = &blend;
   info.dwFlags  = dwFlags;
   info.prcDirty = prcDirty ? &Dirty : NULL;
   Ret = IntUpdateLayeredWindowI( pWnd, &info );
Exit:
   TRACE("Leave NtUserUpdateLayeredWindow, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/* EOF */
