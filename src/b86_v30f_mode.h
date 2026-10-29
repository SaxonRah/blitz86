/* V30F guarded v2.1: 0=control; 2=fixed 2-hit; 3=adaptive; 4=guarded. */
#ifndef B86_V30F_MODE
#define B86_V30F_MODE 4
#endif
#if B86_V30F_MODE != 0 && B86_V30F_MODE != 2 && B86_V30F_MODE != 3 && B86_V30F_MODE != 4
#error "Unsupported B86_V30F_MODE"
#endif
