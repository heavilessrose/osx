/*
 Copyright (c) 2009 Apple Inc.
 
 Permission is hereby granted, free of charge, to any person
 obtaining a copy of this software and associated documentation files
 (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge,
 publish, distribute, sublicense, and/or sell copies of the Software,
 and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:
 
 The above copyright notice and this permission notice shall be
 included in all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT.  IN NO EVENT SHALL THE ABOVE LISTED COPYRIGHT
 HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 DEALINGS IN THE SOFTWARE.
 
 Except as contained in this notice, the name(s) of the above
 copyright holders shall not be used in advertising or otherwise to
 promote the sale, use or other dealings in this Software without
 prior written authorization.
*/
#include <stdbool.h>
#include <X11/Xlibint.h>
#include <X11/extensions/extutil.h>
#include <X11/extensions/Xext.h>
#include "glxclient.h"
#include "glx_error.h"

extern XExtDisplayInfo *__glXFindDisplay(Display * dpy);

void
__glXSendError(Display * dpy, int errorCode, unsigned long resourceID,
               unsigned long minorCode, bool coreX11error)
{
   XExtDisplayInfo *info = __glXFindDisplay(dpy);
   GLXContext gc = __glXGetCurrentContext();
   xError error;

   LockDisplay(dpy);

   error.type = X_Error;

   if (coreX11error) {
      error.errorCode = errorCode;
   }
   else {
      error.errorCode = info->codes->first_error + errorCode;
   }

   error.sequenceNumber = dpy->request;
   error.resourceID = resourceID;
   error.minorCode = minorCode;
   error.majorCode = gc ? gc->majorOpcode : 0;

   _XError(dpy, &error);

   UnlockDisplay(dpy);
}
