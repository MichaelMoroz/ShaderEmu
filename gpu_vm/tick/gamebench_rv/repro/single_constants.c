float half_plus(float x) { return x * 0.5 + 1.0; }          /* unsuffixed constants: double math under clang */
float half_plus_f(float x) { return x * 0.5f + 1.0f; }     /* what gcc's -fsingle-precision-constant makes of it */
