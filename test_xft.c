#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>

int main() {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open display\n"); return 1; }
    
    int screen = DefaultScreen(dpy);
    
    FcInit();
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (FcChar8 *)"Noto Sans Mono CJK SC");
    FcPatternAddDouble(pat, FC_SIZE, 12.0);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcChar8 *family = NULL;
    FcPatternGetString(match, FC_FAMILY, 0, &family);
    printf("Font: %s\n", family);
    
    XftFont *font = XftFontOpenPattern(dpy, match);
    printf("Font loaded: %s\n", font ? "yes" : "no");
    
    return 0;
}