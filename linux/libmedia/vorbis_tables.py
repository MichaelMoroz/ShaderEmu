"""stb_vorbis.c: a block size's twiddle factors and window in floats (m_vorbis_sinpi, m_vorbis_cospi in
m_audio_vorbis.c), not by sin and cos in doubles.

    python3 vorbis_tables.py stb_vorbis.c
A file's setup makes them for its two block sizes: 2,560 sines and cosines and 2,304 window sines for
256 and 2048, in soft doubles some 4,000 instructions each, 18 million of nxplay's start (a third of
the Music player's first 22 s). In floats they are some 60. The angles are pi times t, t a fraction
whose denominator is a power of two, so exact as a float: the sine is what a float polynomial makes
of it, within a unit or two of the last place of the double's sine rounded. Idempotent.
"""
import sys

path = sys.argv[1]
s = open(path).read()
if 'ShaderEmu: tables in floats' in s:
    sys.exit()
old = '''   for (k=k2=0; k < n4; ++k,k2+=2) {
      A[k2  ] = (float)  cos(4*k*M_PI/n);
      A[k2+1] = (float) -sin(4*k*M_PI/n);
      B[k2  ] = (float)  cos((k2+1)*M_PI/n/2) * 0.5f;
      B[k2+1] = (float)  sin((k2+1)*M_PI/n/2) * 0.5f;
   }
   for (k=k2=0; k < n8; ++k,k2+=2) {
      C[k2  ] = (float)  cos(2*(k2+1)*M_PI/n);
      C[k2+1] = (float) -sin(2*(k2+1)*M_PI/n);
   }
}

static void compute_window(int n, float *window)
{
   int n2 = n >> 1, i;
   for (i=0; i < n2; ++i)
      window[i] = (float) sin(0.5 * M_PI * square((float) sin((i - 0 + 0.5) / n2 * 0.5 * M_PI)));
}
'''
new = '''   /* ShaderEmu: tables in floats (vorbis_tables.py): sin and cos of pi t, t = 4k/n, (2k+1)/2n, 2(2k+1)/n */
   float fn = (float) n;
   for (k=k2=0; k < n4; ++k,k2+=2) {
      A[k2  ] =  m_vorbis_cospi((float) (4*k) / fn);
      A[k2+1] = -m_vorbis_sinpi((float) (4*k) / fn);
      B[k2  ] =  m_vorbis_cospi((float) (k2+1) / (2*fn)) * 0.5f;
      B[k2+1] =  m_vorbis_sinpi((float) (k2+1) / (2*fn)) * 0.5f;
   }
   for (k=k2=0; k < n8; ++k,k2+=2) {
      C[k2  ] =  m_vorbis_cospi((float) (2*(k2+1)) / fn);
      C[k2+1] = -m_vorbis_sinpi((float) (2*(k2+1)) / fn);
   }
}

static void compute_window(int n, float *window)
{
   int n2 = n >> 1, i;
   for (i=0; i < n2; ++i) {
      /* sin(pi/2 sin(pi/2 (i + 1/2) / n2)^2) */
      float s = m_vorbis_sinpi((float) (2*i + 1) / (float) (4*n2));
      window[i] = m_vorbis_sinpi(0.5f * square(s));
   }
}
'''
assert s.count(old) == 1, 'stb_vorbis.c: the tables to change are not there'
open(path, 'w').write(s.replace(old, new))
print('vorbis_tables.py: done')
